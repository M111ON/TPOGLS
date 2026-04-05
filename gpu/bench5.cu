/*
 * bench5.cu — POGLS38 GPU5  FINAL ARCH
 *
 * Persistent kernel + Managed memory + Prefetch + SoA
 *
 * Tests:
 *   A. Occupancy sweep: blocks = SM×1, ×2, ×4, ×8
 *   B. Warp specialization (load/compute split)
 *   C. Prefetch lookahead (CPU prefetch next batch while GPU runs)
 *   D. Register pressure check (--ptxas-options=-v output embedded)
 *
 * Compile:
 *   nvcc -O3 -arch=sm_75 --ptxas-options=-v bench5.cu -o bench5
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <cuda_runtime.h>

#define PHI_UP   1696631u
#define PHI_MASK ((1u<<20)-1u)

__device__ __constant__ uint8_t d_lut[16] = {0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

__device__ __forceinline__ uint8_t fast_iso(uint64_t a) {
    uint32_t lo = (uint32_t)(a & 0xFFFFFFFFu);
    uint32_t q  = (uint32_t)(((uint64_t)lo * 4042322161ULL) >> 36);
    return (lo - q * 17u == 1u) ? 0u : 1u;
}

__device__ __forceinline__ void compute_one(
    const uint64_t * __restrict__ addr_in,
    uint32_t       * __restrict__ hilbert_out,
    uint8_t        * __restrict__ lane_out,
    uint8_t        * __restrict__ audit_out,
    uint32_t i)
{
    uint64_t a  = __ldg(addr_in + i);
    uint32_t t  = (uint32_t)(((a & PHI_MASK) * (uint64_t)PHI_UP) >> 20);
    uint16_t x  = t & 0x3FFu, y = (t >> 10) & 0x3FFu;
    uint32_t rx = x, ry = y;
    rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
    rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
    ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
    ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
    uint32_t mor = rx | (ry << 1);
    uint32_t hil = ((mor >> 4) << 4) | d_lut[mor & 0xFu];
    hilbert_out[i] = hil;
    lane_out[i]    = (uint8_t)(hil % 54u);
    audit_out[i]   = fast_iso(a);
}

/* ══════════════════════════════════════════════════════
 * PERSISTENT KERNEL — standard (stride loop, no sync)
 * ══════════════════════════════════════════════════════ */
__global__ void kernel_persistent(
    const uint64_t * __restrict__ addr_in,
    uint32_t       * __restrict__ hilbert_out,
    uint8_t        * __restrict__ lane_out,
    uint8_t        * __restrict__ audit_out,
    uint32_t N, uint32_t reps)
{
    uint32_t stride = gridDim.x * blockDim.x;
    uint32_t start  = blockIdx.x * blockDim.x + threadIdx.x;
    for (uint32_t rep = 0; rep < reps; rep++)
        for (uint32_t i = start; i < N; i += stride)
            compute_one(addr_in, hilbert_out, lane_out, audit_out, i);
}

/* ══════════════════════════════════════════════════════
 * WARP SPECIALIZATION
 * warp 0...(n/2-1) → prefetch (L1 warm)
 * warp n/2...n-1   → compute
 * uses shared memory staging
 * ══════════════════════════════════════════════════════ */
__global__ void kernel_warp_spec(
    const uint64_t * __restrict__ addr_in,
    uint32_t       * __restrict__ hilbert_out,
    uint8_t        * __restrict__ lane_out,
    uint8_t        * __restrict__ audit_out,
    uint32_t N, uint32_t reps)
{
    extern __shared__ uint64_t smem[];  /* TPB elements */
    uint32_t tid    = threadIdx.x;
    uint32_t tpb    = blockDim.x;
    uint32_t half   = tpb / 2;
    uint32_t stride = gridDim.x * tpb;
    uint32_t base   = blockIdx.x * tpb;

    for (uint32_t rep = 0; rep < reps; rep++) {
        for (uint32_t chunk = base; chunk < N; chunk += stride) {
            /* all threads load into smem */
            uint32_t idx = chunk + tid;
            smem[tid] = (idx < N) ? __ldg(addr_in + idx) : 0ULL;
            __syncthreads();

            /* only upper half computes (lower half free to prefetch next) */
            if (tid >= half) {
                uint32_t ci = chunk + (tid - half);
                if (ci < N) {
                    /* compute from smem */
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
            }
            /* lower half: prefetch next chunk into L1 */
            else {
                uint32_t next_idx = chunk + stride + tid;
                if (next_idx < N) (void)__ldg(addr_in + next_idx);
            }
            __syncthreads();
        }
    }
}

static void verify(const uint64_t *h, const uint8_t *d_audit,
                   uint32_t N, const char *tag) {
    uint8_t *hout = (uint8_t*)malloc(N);
    cudaMemcpy(hout, d_audit, N, cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for (uint32_t i=0;i<N;i++) {
        uint8_t ex = ((h[i]%17u)==1u)?0u:1u;
        if(hout[i]==ex) ok++; else fail++;
    }
    printf("    iso [%s]: %llu/%llu%s\n",tag,
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

    /* managed memory (driver migrates after prefetch) */
    uint64_t *m_addr;
    cudaMallocManaged(&m_addr, N*8);
    for(uint32_t i=0;i<N;i++) m_addr[i]=(uint64_t)34*i+1;

    uint32_t *d_hil; uint8_t *d_lane,*d_audit;
    cudaMalloc(&d_hil,N*4); cudaMalloc(&d_lane,N); cudaMalloc(&d_audit,N);

    int dev; cudaGetDevice(&dev);
    cudaMemPrefetchAsync(m_addr, N*8, dev);
    cudaDeviceSynchronize();

    /* ── A. Occupancy sweep ── */
    printf("=== A. Occupancy sweep (persistent, managed+prefetch) ===\n");
    printf("    SM=%d  TPB=%d  N=%dM  reps=%d\n\n", n_sm, TPB, N/1024/1024, REPS);

    int waves[] = {1, 2, 4, 8};
    for(int w=0;w<4;w++) {
        uint32_t blk = (uint32_t)n_sm * waves[w];

        /* warmup */
        kernel_persistent<<<blk,TPB>>>(m_addr,d_hil,d_lane,d_audit,N,1);
        cudaDeviceSynchronize();

        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        kernel_persistent<<<blk,TPB>>>(m_addr,d_hil,d_lane,d_audit,N,REPS);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        double mops = (double)N*REPS/(ms/1000.0)/1e6;

        /* theoretical occupancy */
        int act_blk;
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(
            &act_blk, kernel_persistent, TPB, 0);
        float occ = (float)(act_blk*TPB) / p.maxThreadsPerMultiProcessor;

        printf("  blocks=SM×%-2d (%4u)  %7.0f M/s  ms=%.2f"
               "  occ=%.0f%%  active_blk/SM=%d\n",
               waves[w], blk, mops, ms, occ*100, act_blk);
        verify(m_addr, d_audit, N, "occ");
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    /* ── B. Warp specialization ── */
    printf("\n=== B. Warp specialization vs standard ===\n");
    {
        uint32_t blk = (uint32_t)n_sm * 4;
        size_t smem  = TPB * sizeof(uint64_t);  /* 2KB per block */

        /* standard */
        kernel_persistent<<<blk,TPB>>>(m_addr,d_hil,d_lane,d_audit,N,1);
        cudaDeviceSynchronize();
        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        kernel_persistent<<<blk,TPB>>>(m_addr,d_hil,d_lane,d_audit,N,REPS);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("  Standard persistent:    %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        verify(m_addr, d_audit, N, "standard");

        /* warp spec */
        kernel_warp_spec<<<blk,TPB,smem>>>(m_addr,d_hil,d_lane,d_audit,N,1);
        cudaDeviceSynchronize();
        cudaEventRecord(t0);
        kernel_warp_spec<<<blk,TPB,smem>>>(m_addr,d_hil,d_lane,d_audit,N,REPS);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        cudaEventElapsedTime(&ms,t0,t1);
        printf("  Warp specialization:    %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        verify(m_addr, d_audit, N, "warp-spec");
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    /* ── C. Prefetch lookahead (CPU side, double managed buffer) ── */
    printf("\n=== C. Prefetch lookahead (2 managed buffers, ping-pong) ===\n");
    {
        /* two managed buffers */
        uint64_t *buf[2];
        cudaMallocManaged(&buf[0], N*8);
        cudaMallocManaged(&buf[1], N*8);
        for(int b=0;b<2;b++) for(uint32_t i=0;i<N;i++) buf[b][i]=(uint64_t)34*i+b+1;

        /* prefetch buf[0] to GPU */
        cudaMemPrefetchAsync(buf[0], N*8, dev);

        uint32_t blk = (uint32_t)n_sm * 4;
        cudaStream_t s; cudaStreamCreate(&s);
        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);

        /* warmup */
        kernel_persistent<<<blk,TPB,0,s>>>(buf[0],d_hil,d_lane,d_audit,N,1);
        cudaStreamSynchronize(s);

        cudaEventRecord(t0);
        for(int rep=0; rep<REPS; rep++) {
            int cur  = rep & 1;
            int next = cur ^ 1;
            /* launch kernel on cur */
            kernel_persistent<<<blk,TPB,0,s>>>(buf[cur],d_hil,d_lane,d_audit,N,1);
            /* while GPU runs cur: CPU prefetches next to GPU */
            cudaMemPrefetchAsync(buf[next], N*8, dev);
        }
        cudaStreamSynchronize(s);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        printf("  Lookahead ping-pong:    %7.0f M/s  ms=%.2f\n",
               (double)N*REPS/(ms/1000.0)/1e6, ms);
        verify(buf[0], d_audit, N, "lookahead");
        cudaFree(buf[0]); cudaFree(buf[1]);
        cudaStreamDestroy(s);
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    cudaFree(m_addr); cudaFree(d_hil); cudaFree(d_lane); cudaFree(d_audit);
    return 0;
}
