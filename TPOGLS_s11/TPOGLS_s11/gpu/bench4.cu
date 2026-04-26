/*
 * bench4.cu — POGLS38 GPU4
 *
 * Target: 1500 → 3000+ M/s
 *
 * 3 approaches (ทดสอบแยกกัน เปรียบชัด):
 *
 *   A. Zero-copy (mapped pinned memory)
 *      GPU อ่าน host addr[] ตรงผ่าน PCIe
 *      ไม่มี memcpy overhead, ไม่มี launch latency
 *      ceiling = PCIe BW ÷ 8B = ~16GB/s ÷ 8 = ~2000 M/s
 *
 *   B. Persistent kernel (one launch, kernel loops internally)
 *      ตัด cudaMemcpyAsync + stream sync + event overhead
 *      kernel ดึงงานเองจาก atomic counter
 *      ceiling = GPU compute = ~9900 M/s (ถ้า data อยู่ใน VRAM แล้ว)
 *
 *   C. Managed memory + prefetch
 *      cudaMallocManaged + cudaMemPrefetchAsync
 *      ให้ driver migrate อัตโนมัติ
 *
 * Kill %17: เปลี่ยนเป็น multiply-shift (compile-time constant opt)
 * __ldg: ใช้ read-only cache บน T4
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <cuda_runtime.h>

/* ── constants ── */
#define PHI_UP   1696631u
#define PHI_MASK ((1u<<20)-1u)

/* ── Hilbert LUT in device constant memory ── */
__device__ __constant__ uint8_t d_lut[16] = {0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

/* ── fast mod17: compiler will optimize, but explicit magic helps ──
 * a % 17 == 1  →  (a - 1) divisible by 17
 * use: ((uint32_t)((uint64_t)(a-1) * 0x0F0F0F0FULL >> 32)) ... too complex
 * just use: (a & 0xF) ... no, 17 is not power of 2
 * BEST: rely on nvcc -O3 to optimize constant modulo, add __attribute__
 * Real trick: since we only need == 1: use subtraction + check
 */
__device__ __forceinline__ uint8_t fast_iso(uint64_t a) {
    /* a % 17 == 1  same as  (a - 1) % 17 == 0
     * For GPU: nvcc -O3 converts constant modulo to multiply-shift
     * Explicitly: mul-high trick for 17
     * r = a - 17 * (a / 17)
     * a / 17 ≈ (a * 3848290697ULL) >> 36  (magic for div by 17, 32-bit a)
     */
    uint32_t lo = (uint32_t)(a & 0xFFFFFFFFu);
    uint32_t q  = (uint32_t)(((uint64_t)lo * 3848290697ULL) >> 36);
    return (lo - q * 17u == 1u) ? 0u : 1u;
}

/* ── core compute (SoA, __ldg, fused morton+hilbert) ── */
__device__ __forceinline__ void compute_one(
    const uint64_t * __restrict__ addr_in,
    uint32_t * __restrict__ hilbert_out,
    uint8_t  * __restrict__ lane_out,
    uint8_t  * __restrict__ audit_out,
    uint32_t i)
{
    uint64_t a = __ldg(addr_in + i);        /* read-only cache */
    uint32_t t = (uint32_t)(((a & PHI_MASK) * (uint64_t)PHI_UP) >> 20);
    uint16_t x = t & 0x3FFu, y = (t >> 10) & 0x3FFu;
    uint32_t rx = x, ry = y;
    rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
    rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
    ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
    ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
    uint32_t mor = rx | (ry << 1);
    /* fused: no store for morton (write only hilbert) */
    uint32_t hil = ((mor >> 4) << 4) | d_lut[mor & 0xFu];
    hilbert_out[i] = hil;
    lane_out[i]    = (uint8_t)(hil % 54u);
    audit_out[i]   = fast_iso(a);
}

/* ══════════════════════════════════════════════════════
 * A. ZERO-COPY kernel
 *    addr_in lives in host pinned memory, GPU reads via PCIe
 * ══════════════════════════════════════════════════════ */
__global__ void kernel_zerocopy(
    const uint64_t * __restrict__ addr_in,  /* mapped host ptr */
    uint32_t * __restrict__ hilbert_out,    /* device */
    uint8_t  * __restrict__ lane_out,
    uint8_t  * __restrict__ audit_out,
    uint32_t N)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < N) compute_one(addr_in, hilbert_out, lane_out, audit_out, i);
}

/* ══════════════════════════════════════════════════════
 * B. PERSISTENT kernel
 *    one launch, loop over chunks via atomic counter
 * ══════════════════════════════════════════════════════ */
__global__ void kernel_persistent(
    const uint64_t * __restrict__ addr_in,
    uint32_t * __restrict__ hilbert_out,
    uint8_t  * __restrict__ lane_out,
    uint8_t  * __restrict__ audit_out,
    uint32_t N,
    uint32_t reps,
    uint32_t * __restrict__ counter)  /* global atomic work counter */
{
    for (uint32_t rep = 0; rep < reps; rep++) {
        /* reset counter at start of each rep (only thread 0 of block 0) */
        if (blockIdx.x == 0 && threadIdx.x == 0)
            atomicExch(counter, 0u);
        __syncthreads(); /* all blocks wait */
        /* can't use grid sync cheaply, use chunk stride instead */
        uint32_t stride = gridDim.x * blockDim.x;
        uint32_t start  = blockIdx.x * blockDim.x + threadIdx.x;
        for (uint32_t i = start; i < N; i += stride)
            compute_one(addr_in, hilbert_out, lane_out, audit_out, i);
        __threadfence(); /* flush before next rep */
    }
}

/* standard kernel (for comparison) */
__global__ void kernel_soa(
    const uint64_t * __restrict__ addr_in,
    uint32_t * __restrict__ hilbert_out,
    uint8_t  * __restrict__ lane_out,
    uint8_t  * __restrict__ audit_out,
    uint32_t N)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < N) compute_one(addr_in, hilbert_out, lane_out, audit_out, i);
}

static void verify(const uint64_t *h_addr, const uint8_t *h_audit, uint32_t N,
                   const char *tag) {
    uint64_t ok=0,fail=0;
    for (uint32_t i=0;i<N;i++) {
        uint8_t ex = ((h_addr[i]%17u)==1u)?0u:1u;
        if (h_audit[i]==ex) ok++; else fail++;
    }
    printf("    iso [%s]: %llu/%llu%s\n", tag,
           (unsigned long long)ok, (unsigned long long)(ok+fail),
           fail?"  ❌ FAIL":"  ✓");
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    printf("GPU: %s SM%d.%d  SMs=%d  PCIe %dx\n\n",
           p.name,p.major,p.minor,p.multiProcessorCount,p.pciDeviceID);

    const uint32_t N   = 2*1024*1024;  /* 2M batch */
    const uint32_t TPB = 256;
    const int REPS     = 20;
    uint32_t blk = (N + TPB - 1) / TPB;

    printf("batch=%uM  tpb=%u  reps=%d\n\n", N/1024/1024, TPB, REPS);

    /* ── baseline: SIMPLE SoA (all in VRAM) ── */
    {
        uint64_t *h; cudaMallocHost(&h, N*8);
        for(uint32_t i=0;i<N;i++) h[i]=(uint64_t)34*i+1;
        uint64_t *d_addr; uint32_t *d_hil; uint8_t *d_lane,*d_audit;
        cudaMalloc(&d_addr, N*8); cudaMalloc(&d_hil, N*4);
        cudaMalloc(&d_lane, N);   cudaMalloc(&d_audit, N);
        cudaMemcpy(d_addr, h, N*8, cudaMemcpyHostToDevice);
        /* warmup */
        kernel_soa<<<blk,TPB>>>(d_addr,d_hil,d_lane,d_audit,N);
        cudaDeviceSynchronize();
        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        for(int r=0;r<REPS;r++)
            kernel_soa<<<blk,TPB>>>(d_addr,d_hil,d_lane,d_audit,N);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("[Baseline] SIMPLE+SoA (data in VRAM): %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        uint8_t *hout=(uint8_t*)malloc(N);
        cudaMemcpy(hout,d_audit,N,cudaMemcpyDeviceToHost);
        verify(h,hout,N,"baseline"); free(hout);
        cudaFreeHost(h); cudaFree(d_addr); cudaFree(d_hil);
        cudaFree(d_lane); cudaFree(d_audit);
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    printf("\n");

    /* ── A. ZERO-COPY ── */
    {
        /* pinned mapped host memory */
        uint64_t *h_addr_mapped;
        cudaHostAlloc(&h_addr_mapped, N*8,
                      cudaHostAllocMapped | cudaHostAllocWriteCombined);
        for(uint32_t i=0;i<N;i++) h_addr_mapped[i]=(uint64_t)34*i+1;

        /* get device pointer to host memory */
        uint64_t *d_addr_mapped;
        cudaHostGetDevicePointer((void**)&d_addr_mapped, h_addr_mapped, 0);

        /* output stays in VRAM */
        uint32_t *d_hil; uint8_t *d_lane, *d_audit;
        cudaMalloc(&d_hil,  N*4);
        cudaMalloc(&d_lane, N);
        cudaMalloc(&d_audit,N);

        /* warmup */
        kernel_zerocopy<<<blk,TPB>>>(d_addr_mapped,d_hil,d_lane,d_audit,N);
        cudaDeviceSynchronize();

        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        for(int r=0;r<REPS;r++)
            kernel_zerocopy<<<blk,TPB>>>(d_addr_mapped,d_hil,d_lane,d_audit,N);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("[A] Zero-copy (PCIe direct):            %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        uint8_t *hout=(uint8_t*)malloc(N);
        cudaMemcpy(hout,d_audit,N,cudaMemcpyDeviceToHost);
        verify(h_addr_mapped,hout,N,"zero-copy"); free(hout);
        cudaFreeHost(h_addr_mapped);
        cudaFree(d_hil); cudaFree(d_lane); cudaFree(d_audit);
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    printf("\n");

    /* ── B. PERSISTENT kernel (data in VRAM) ── */
    {
        uint64_t *h; cudaMallocHost(&h,N*8);
        for(uint32_t i=0;i<N;i++) h[i]=(uint64_t)34*i+1;
        uint64_t *d_addr; uint32_t *d_hil; uint8_t *d_lane,*d_audit;
        uint32_t *d_counter;
        cudaMalloc(&d_addr,   N*8); cudaMalloc(&d_hil, N*4);
        cudaMalloc(&d_lane,   N);   cudaMalloc(&d_audit,N);
        cudaMalloc(&d_counter, 4);
        cudaMemcpy(d_addr,h,N*8,cudaMemcpyHostToDevice);

        /* persistent: launch exactly SM*TPB/4 blocks (4 waves per SM) */
        int n_sm = p.multiProcessorCount;
        uint32_t pers_blk = n_sm * 4;  /* tune: 2-8 waves per SM */

        /* warmup */
        kernel_persistent<<<pers_blk,TPB>>>(
            d_addr,d_hil,d_lane,d_audit,N,1,d_counter);
        cudaDeviceSynchronize();

        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        kernel_persistent<<<pers_blk,TPB>>>(
            d_addr,d_hil,d_lane,d_audit,N,REPS,d_counter);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("[B] Persistent kernel (%d blocks):      %7.0f M/s  ms=%.2f\n",
               pers_blk,
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        uint8_t *hout=(uint8_t*)malloc(N);
        cudaMemcpy(hout,d_audit,N,cudaMemcpyDeviceToHost);
        verify(h,hout,N,"persistent"); free(hout);
        cudaFreeHost(h); cudaFree(d_addr); cudaFree(d_hil);
        cudaFree(d_lane); cudaFree(d_audit); cudaFree(d_counter);
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    printf("\n");

    /* ── C. MANAGED MEMORY + prefetch ── */
    {
        uint64_t *m_addr;
        cudaMallocManaged(&m_addr, N*8);
        for(uint32_t i=0;i<N;i++) m_addr[i]=(uint64_t)34*i+1;
        uint32_t *d_hil; uint8_t *d_lane,*d_audit;
        cudaMalloc(&d_hil,N*4); cudaMalloc(&d_lane,N); cudaMalloc(&d_audit,N);

        /* prefetch to GPU before launch */
        int dev; cudaGetDevice(&dev);
        cudaMemPrefetchAsync(m_addr, N*8, dev);

        /* warmup */
        kernel_soa<<<blk,TPB>>>(m_addr,d_hil,d_lane,d_audit,N);
        cudaDeviceSynchronize();

        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        for(int r=0;r<REPS;r++)
            kernel_soa<<<blk,TPB>>>(m_addr,d_hil,d_lane,d_audit,N);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("[C] Managed+prefetch:                   %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        uint8_t *hout=(uint8_t*)malloc(N);
        cudaMemcpy(hout,d_audit,N,cudaMemcpyDeviceToHost);
        verify(m_addr,hout,N,"managed"); free(hout);
        cudaFree(m_addr); cudaFree(d_hil); cudaFree(d_lane); cudaFree(d_audit);
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    printf("\n=== Summary ===\n");
    printf("Baseline (VRAM data)    = compute ceiling  → target: ~9900 M/s\n");
    printf("Zero-copy (PCIe direct) = PCIe ceiling     → expect: ~2000 M/s\n");
    printf("Persistent (VRAM data)  = no launch overhead → expect: ~9800 M/s\n");
    printf("Managed+prefetch        = driver migration → expect: near baseline\n");
    return 0;
}
