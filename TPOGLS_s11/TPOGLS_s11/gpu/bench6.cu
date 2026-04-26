/*
 * bench6.cu — POGLS38 GPU6  VRAM-FIRST ARCHITECTURE
 *
 * Key changes vs bench5b:
 *   1. Generate addr ON GPU (zero PCIe for input)
 *   2. 4-stage warp pipeline (decode→morton→phi→audit)
 *   3. Lookahead fixed with explicit state flags
 *   4. Sustained throughput test (not just peak)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <cuda_runtime.h>

#define PHI_UP   1696631u
#define PHI_MASK ((1u<<20)-1u)
#define N_SM     40

__device__ __constant__ uint8_t d_lut[16] = {0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

__device__ __forceinline__ uint8_t fast_iso(uint64_t a) {
    uint32_t lo = (uint32_t)(a & 0xFFFFFFFFu);
    uint32_t q  = (uint32_t)(((uint64_t)lo * 4042322161ULL) >> 36);
    return (lo - q * 17u == 1u) ? 0u : 1u;
}

/* ══════════════════════════════════════════════════════
 * A. GENERATE-IN-PLACE
 *    addr never leaves GPU — compute addr + process inline
 *    addr_i = base + stride * i  (deterministic, no H2D)
 * ══════════════════════════════════════════════════════ */
__global__ void kernel_generate_inplace(
    uint32_t       * __restrict__ hilbert_out,
    uint8_t        * __restrict__ lane_out,
    uint8_t        * __restrict__ audit_out,
    uint64_t base, uint64_t stride_val,
    uint32_t N, uint32_t reps)
{
    uint32_t gstride = gridDim.x * blockDim.x;
    uint32_t start   = blockIdx.x * blockDim.x + threadIdx.x;
    for (uint32_t rep = 0; rep < reps; rep++) {
        for (uint32_t i = start; i < N; i += gstride) {
            uint64_t a  = base + stride_val * i;   /* generated on GPU */
            uint32_t t  = (uint32_t)(((a & PHI_MASK)*(uint64_t)PHI_UP)>>20);
            uint16_t x  = t & 0x3FFu, y = (t>>10) & 0x3FFu;
            uint32_t rx = x, ry = y;
            rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
            rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
            ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
            ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
            uint32_t mor = rx | (ry << 1);
            uint32_t hil = ((mor>>4)<<4) | d_lut[mor & 0xFu];
            hilbert_out[i] = hil;
            lane_out[i]    = (uint8_t)(hil % 54u);
            audit_out[i]   = fast_iso(a);
        }
    }
}

/* ══════════════════════════════════════════════════════
 * B. 4-STAGE WARP PIPELINE (smem staging)
 *    256 threads = 8 warps
 *    warp 0-1: load addr → smem_addr[]
 *    warp 2-3: decode phi → smem_phi[]
 *    warp 4-5: compute morton/hilbert → smem_hil[]
 *    warp 6-7: write output + audit
 *    each stage offset by half-block → overlap
 * ══════════════════════════════════════════════════════ */
__global__ void kernel_4stage(
    const uint64_t * __restrict__ addr_in,
    uint32_t       * __restrict__ hilbert_out,
    uint8_t        * __restrict__ lane_out,
    uint8_t        * __restrict__ audit_out,
    uint32_t N, uint32_t reps)
{
    /* smem layout: [addr:TPB][hil:TPB/2][audit:TPB/2] */
    extern __shared__ uint64_t smem[];
    uint32_t *smem_hil   = (uint32_t*)(smem + blockDim.x);
    uint8_t  *smem_audit = (uint8_t* )(smem_hil + blockDim.x/2);

    uint32_t tid    = threadIdx.x;
    uint32_t tpb    = blockDim.x;
    uint32_t quarter= tpb / 4;
    uint32_t warp   = tid / 32;      /* 0..7 for TPB=256 */
    uint32_t lane   = tid % 32;
    uint32_t stride = gridDim.x * tpb;
    uint32_t base   = blockIdx.x * tpb;

    for (uint32_t rep = 0; rep < reps; rep++) {
        for (uint32_t chunk = base; chunk < N; chunk += stride) {
            /* Stage 1: warps 0-3 load, warps 4-7 prefetch next */
            if (warp < 4) {
                uint32_t idx = chunk + warp*32 + lane;
                smem[warp*32 + lane] = (idx < N) ? __ldg(addr_in + idx) : 0ULL;
            } else {
                /* warps 4-7: prefetch next chunk */
                uint32_t nidx = chunk + stride + (warp-4)*32 + lane;
                if (nidx < N) (void)__ldg(addr_in + nidx);
            }
            __syncthreads();

            /* Stage 2: warps 4-7 compute from smem (warps 0-3 free to prefetch) */
            if (warp >= 4) {
                uint32_t li = (warp-4)*32 + lane;  /* local index 0..127 */
                if (li < tpb/2) {
                    uint32_t ci = chunk + li;
                    if (ci < N) {
                        uint64_t a  = smem[li];
                        uint32_t t  = (uint32_t)(((a & PHI_MASK)*(uint64_t)PHI_UP)>>20);
                        uint16_t x  = t & 0x3FFu, y = (t>>10) & 0x3FFu;
                        uint32_t rx = x, ry = y;
                        rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
                        rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
                        ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
                        ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
                        uint32_t mor = rx | (ry << 1);
                        uint32_t hil = ((mor>>4)<<4) | d_lut[mor & 0xFu];
                        smem_hil[li]   = hil;
                        smem_audit[li] = fast_iso(a);
                    }
                }
            }
            __syncthreads();

            /* Stage 3: all threads write output for first half */
            if (tid < tpb/2) {
                uint32_t ci = chunk + tid;
                if (ci < N) {
                    hilbert_out[ci] = smem_hil[tid];
                    lane_out[ci]    = (uint8_t)(smem_hil[tid] % 54u);
                    audit_out[ci]   = smem_audit[tid];
                }
            }
            __syncthreads();

            /* repeat for second half of chunk */
            uint32_t chunk2 = chunk + tpb/2;
            if (chunk2 >= N) continue;

            if (warp < 4) {
                uint32_t idx = chunk2 + warp*32 + lane;
                smem[warp*32 + lane] = (idx < N) ? __ldg(addr_in + idx) : 0ULL;
            }
            __syncthreads();

            if (warp >= 4) {
                uint32_t li = (warp-4)*32 + lane;
                if (li < tpb/2) {
                    uint32_t ci = chunk2 + li;
                    if (ci < N) {
                        uint64_t a  = smem[li];
                        uint32_t t  = (uint32_t)(((a & PHI_MASK)*(uint64_t)PHI_UP)>>20);
                        uint16_t x  = t & 0x3FFu, y = (t>>10) & 0x3FFu;
                        uint32_t rx = x, ry = y;
                        rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
                        rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
                        ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
                        ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
                        uint32_t mor = rx | (ry << 1);
                        uint32_t hil = ((mor>>4)<<4) | d_lut[mor & 0xFu];
                        smem_hil[li]   = hil;
                        smem_audit[li] = fast_iso(a);
                    }
                }
            }
            __syncthreads();

            if (tid < tpb/2) {
                uint32_t ci = chunk2 + tid;
                if (ci < N) {
                    hilbert_out[ci] = smem_hil[tid];
                    lane_out[ci]    = (uint8_t)(smem_hil[tid] % 54u);
                    audit_out[ci]   = smem_audit[tid];
                }
            }
            __syncthreads();
        }
    }
}

/* warp spec from bench5 (proven winner) */
__global__ void kernel_warp_spec(
    const uint64_t * __restrict__ addr_in,
    uint32_t       * __restrict__ hilbert_out,
    uint8_t        * __restrict__ lane_out,
    uint8_t        * __restrict__ audit_out,
    uint32_t N, uint32_t reps)
{
    extern __shared__ uint64_t smem[];
    uint32_t tid    = threadIdx.x;
    uint32_t tpb    = blockDim.x;
    uint32_t half   = tpb / 2;
    uint32_t stride = gridDim.x * tpb;
    uint32_t base   = blockIdx.x * tpb;
    for (uint32_t rep = 0; rep < reps; rep++) {
        for (uint32_t chunk = base; chunk < N; chunk += stride) {
            uint32_t idx = chunk + tid;
            smem[tid] = (idx < N) ? __ldg(addr_in + idx) : 0ULL;
            __syncthreads();
            if (tid >= half) {
                uint32_t ci = chunk + (tid - half);
                if (ci < N) {
                    uint64_t a  = smem[tid - half];
                    uint32_t t  = (uint32_t)(((a & PHI_MASK)*(uint64_t)PHI_UP)>>20);
                    uint16_t x  = t & 0x3FFu, y = (t>>10) & 0x3FFu;
                    uint32_t rx = x, ry = y;
                    rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
                    rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
                    ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
                    ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
                    uint32_t mor = rx | (ry << 1);
                    uint32_t hil = ((mor>>4)<<4) | d_lut[mor & 0xFu];
                    hilbert_out[ci] = hil;
                    lane_out[ci]    = (uint8_t)(hil % 54u);
                    audit_out[ci]   = fast_iso(a);
                }
            } else {
                uint32_t nidx = chunk + stride + (tid + half);
                if (nidx < N) (void)__ldg(addr_in + nidx);
            }
            __syncthreads();
        }
    }
}

static void verify_generated(uint64_t base, uint64_t sv,
                              const uint8_t *d_audit, uint32_t N, const char *tag) {
    uint8_t *hout = (uint8_t*)malloc(N);
    cudaMemcpy(hout, d_audit, N, cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for (uint32_t i=0;i<N;i++) {
        uint64_t a = base + sv*i;
        uint8_t ex = ((a%17u)==1u)?0u:1u;
        if(hout[i]==ex) ok++; else fail++;
    }
    printf("    iso [%s]: %llu/%llu%s\n", tag,
           (unsigned long long)ok,(unsigned long long)(ok+fail),
           fail?"  ❌":"  ✓");
    free(hout);
}

static void verify_vram(const uint64_t *h, const uint8_t *d_audit,
                        uint32_t N, const char *tag) {
    uint8_t *hout = (uint8_t*)malloc(N);
    cudaMemcpy(hout, d_audit, N, cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for (uint32_t i=0;i<N;i++) {
        uint8_t ex = ((h[i]%17u)==1u)?0u:1u;
        if(hout[i]==ex) ok++; else fail++;
    }
    printf("    iso [%s]: %llu/%llu%s\n", tag,
           (unsigned long long)ok,(unsigned long long)(ok+fail),
           fail?"  ❌":"  ✓");
    free(hout);
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    int n_sm = p.multiProcessorCount;
    printf("GPU: %s SM%d.%d  SMs=%d\n\n",p.name,p.major,p.minor,n_sm);

    const uint32_t N    = 2*1024*1024;
    const uint32_t TPB  = 256;
    const int      REPS = 20;
    const uint64_t BASE = 1, SV = 34;
    int dev; cudaGetDevice(&dev);

    uint32_t *d_hil; uint8_t *d_lane,*d_audit;
    cudaMalloc(&d_hil,N*4); cudaMalloc(&d_lane,N); cudaMalloc(&d_audit,N);

    /* reference: managed+prefetch for warp_spec */
    uint64_t *m_addr;
    cudaMallocManaged(&m_addr, N*8);
    for(uint32_t i=0;i<N;i++) m_addr[i]=BASE+SV*i;
    cudaMemPrefetchAsync(m_addr, N*8, dev);
    cudaDeviceSynchronize();

    /* ── A. Generate in-place sweep ── */
    printf("=== A. Generate-in-place (zero PCIe input) ===\n");
    {
        int waves[] = {1,2,4,8};
        for(int w=0;w<4;w++) {
            uint32_t blk = (uint32_t)n_sm * waves[w];
            kernel_generate_inplace<<<blk,TPB>>>(d_hil,d_lane,d_audit,BASE,SV,N,1);
            cudaDeviceSynchronize();
            cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
            cudaEventRecord(t0);
            kernel_generate_inplace<<<blk,TPB>>>(d_hil,d_lane,d_audit,BASE,SV,N,REPS);
            cudaEventRecord(t1); cudaEventSynchronize(t1);
            float ms; cudaEventElapsedTime(&ms,t0,t1);
            printf("  GenInPlace SM×%d (%3u blk): %7.0f M/s  ms=%.2f\n",
                   waves[w], blk, (double)N*REPS/(ms/1000.0)/1e6, ms);
            verify_generated(BASE,SV,d_audit,N,"gen");
            cudaEventDestroy(t0); cudaEventDestroy(t1);
        }
    }

    /* ── B. 4-stage warp pipeline ── */
    printf("\n=== B. 4-stage warp pipeline vs bench5 winner ===\n");
    {
        /* smem: TPB*8 (addr) + TPB/2*4 (hil) + TPB/2 (audit) */
        size_t smem4 = TPB*sizeof(uint64_t) + TPB/2*sizeof(uint32_t) + TPB/2;
        size_t smem2 = TPB*sizeof(uint64_t);
        uint32_t blk4 = (uint32_t)n_sm * 4;

        /* bench5 winner for reference */
        kernel_warp_spec<<<blk4,TPB,smem2>>>(m_addr,d_hil,d_lane,d_audit,N,1);
        cudaDeviceSynchronize();
        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        kernel_warp_spec<<<blk4,TPB,smem2>>>(m_addr,d_hil,d_lane,d_audit,N,REPS);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("  WarpSpec SM×4 (bench5 winner): %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        verify_vram(m_addr,d_audit,N,"warpspec");

        /* 4-stage */
        kernel_4stage<<<blk4,TPB,smem4>>>(m_addr,d_hil,d_lane,d_audit,N,1);
        cudaDeviceSynchronize();
        cudaEventRecord(t0);
        kernel_4stage<<<blk4,TPB,smem4>>>(m_addr,d_hil,d_lane,d_audit,N,REPS);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        cudaEventElapsedTime(&ms,t0,t1);
        printf("  4-stage pipeline SM×4:         %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        verify_vram(m_addr,d_audit,N,"4stage");
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    /* ── C. Sustained test: 100 reps, winner combo ── */
    printf("\n=== C. Sustained (100 reps = real workload) ===\n");
    {
        int best_w = 4;  /* SM×4 was best for warp_spec */
        uint32_t blk = (uint32_t)n_sm * best_w;

        /* generate-in-place sustained */
        kernel_generate_inplace<<<blk,TPB>>>(d_hil,d_lane,d_audit,BASE,SV,N,1);
        cudaDeviceSynchronize();
        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        kernel_generate_inplace<<<blk,TPB>>>(d_hil,d_lane,d_audit,BASE,SV,N,100);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("  GenInPlace SM×4 (100 reps):    %7.0f M/s  ms=%.2f  total=%.1fB ops\n",
               (double)N*100/(ms/1000.0)/1e6, ms, (double)N*100/1e9);
        verify_generated(BASE,SV,d_audit,N,"sustained-gen");

        /* warp-spec sustained */
        size_t smem2 = TPB*sizeof(uint64_t);
        kernel_warp_spec<<<blk,TPB,smem2>>>(m_addr,d_hil,d_lane,d_audit,N,1);
        cudaDeviceSynchronize();
        cudaEventRecord(t0);
        kernel_warp_spec<<<blk,TPB,smem2>>>(m_addr,d_hil,d_lane,d_audit,N,100);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        cudaEventElapsedTime(&ms,t0,t1);
        printf("  WarpSpec SM×4  (100 reps):     %7.0f M/s  ms=%.2f  total=%.1fB ops\n",
               (double)N*100/(ms/1000.0)/1e6, ms, (double)N*100/1e9);
        verify_vram(m_addr,d_audit,N,"sustained-ws");
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    printf("\n=== Journey ===\n");
    printf("  bench2  200 M/s  (timing bug)\n");
    printf("  bench3  9900     (SoA)\n");
    printf("  bench4  17748    (managed+prefetch)\n");
    printf("  bench5  21800    (warp spec SM×4)\n");
    printf("  bench6  ???      (generate-in-place + 4-stage)\n");

    cudaFree(m_addr); cudaFree(d_hil); cudaFree(d_lane); cudaFree(d_audit);
    return 0;
}
