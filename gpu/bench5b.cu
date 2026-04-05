/*
 * bench5b.cu — POGLS38 GPU5b  FINAL COMBINATION
 *
 * Takes winners from bench5:
 *   A: SM×2 blocks (best occupancy)
 *   B: Warp specialization (21787 M/s, +58%)
 *   C: Lookahead fix (verify last_cur buffer)
 *   D: Combined: SM×2 + warp spec + lookahead
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

/* ── warp specialization kernel ── */
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
                uint32_t next_idx = chunk + stride + (tid + half);
                if (next_idx < N) (void)__ldg(addr_in + next_idx);
            }
            __syncthreads();
        }
    }
}

/* ── standard persistent (baseline comparison) ── */
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
        for (uint32_t i = start; i < N; i += stride) {
            uint64_t a  = __ldg(addr_in + i);
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

static void verify(const uint64_t *h, const uint8_t *d_audit,
                   uint32_t N, const char *tag) {
    uint8_t *hout = (uint8_t*)malloc(N);
    cudaMemcpy(hout, d_audit, N, cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for (uint32_t i=0;i<N;i++) {
        uint8_t ex = ((h[i]%17u)==1u) ? 0u : 1u;
        if(hout[i]==ex) ok++; else fail++;
    }
    printf("    iso [%s]: %llu/%llu%s\n", tag,
           (unsigned long long)ok, (unsigned long long)(ok+fail),
           fail ? "  ❌" : "  ✓");
    free(hout);
}

static double run(const char *label,
                  uint64_t *addr, uint32_t *d_hil,
                  uint8_t *d_lane, uint8_t *d_audit,
                  uint32_t N, uint32_t blk, uint32_t tpb,
                  int reps, int warp_spec) {
    size_t smem = warp_spec ? tpb * sizeof(uint64_t) : 0;
    /* warmup */
    if (warp_spec)
        kernel_warp_spec<<<blk,tpb,smem>>>(addr,d_hil,d_lane,d_audit,N,1);
    else
        kernel_persistent<<<blk,tpb>>>(addr,d_hil,d_lane,d_audit,N,1);
    cudaDeviceSynchronize();
    cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0);
    if (warp_spec)
        kernel_warp_spec<<<blk,tpb,smem>>>(addr,d_hil,d_lane,d_audit,N,reps);
    else
        kernel_persistent<<<blk,tpb>>>(addr,d_hil,d_lane,d_audit,N,reps);
    cudaEventRecord(t1); cudaEventSynchronize(t1);
    float ms; cudaEventElapsedTime(&ms,t0,t1);
    double mops = (double)N*reps/(ms/1000.0)/1e6;
    printf("  %-40s %7.0f M/s  ms=%.2f\n", label, mops, ms);
    verify(addr, d_audit, N, label);
    cudaEventDestroy(t0); cudaEventDestroy(t1);
    return mops;
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    int n_sm = p.multiProcessorCount;
    printf("GPU: %s SM%d.%d  SMs=%d\n\n",p.name,p.major,p.minor,n_sm);

    const uint32_t N    = 2*1024*1024;
    const uint32_t TPB  = 256;
    const int      REPS = 20;
    int dev; cudaGetDevice(&dev);

    uint64_t *m_addr;
    cudaMallocManaged(&m_addr, N*8);
    for(uint32_t i=0;i<N;i++) m_addr[i]=(uint64_t)34*i+1;
    cudaMemPrefetchAsync(m_addr, N*8, dev);

    uint32_t *d_hil; uint8_t *d_lane,*d_audit;
    cudaMalloc(&d_hil,N*4); cudaMalloc(&d_lane,N); cudaMalloc(&d_audit,N);
    cudaDeviceSynchronize();

    uint32_t blk1 = (uint32_t)n_sm * 1;
    uint32_t blk2 = (uint32_t)n_sm * 2;
    uint32_t blk4 = (uint32_t)n_sm * 4;

    printf("=== Head-to-head: all combinations ===\n");
    printf("    N=%dM  TPB=%d  reps=%d\n\n", N/1024/1024, TPB, REPS);

    run("Standard   SM×1",  m_addr,d_hil,d_lane,d_audit,N,blk1,TPB,REPS,0);
    run("Standard   SM×2",  m_addr,d_hil,d_lane,d_audit,N,blk2,TPB,REPS,0);
    run("Standard   SM×4",  m_addr,d_hil,d_lane,d_audit,N,blk4,TPB,REPS,0);
    printf("\n");
    run("WarpSpec   SM×1",  m_addr,d_hil,d_lane,d_audit,N,blk1,TPB,REPS,1);
    run("WarpSpec   SM×2",  m_addr,d_hil,d_lane,d_audit,N,blk2,TPB,REPS,1);
    run("WarpSpec   SM×4",  m_addr,d_hil,d_lane,d_audit,N,blk4,TPB,REPS,1);

    /* ── C. Lookahead fixed: track last buffer ── */
    printf("\n=== C. Lookahead ping-pong (FIXED) ===\n");
    {
        uint64_t *buf[2];
        cudaMallocManaged(&buf[0], N*8);
        cudaMallocManaged(&buf[1], N*8);
        for(int b=0;b<2;b++) for(uint32_t i=0;i<N;i++) buf[b][i]=(uint64_t)34*i+b+1;
        cudaMemPrefetchAsync(buf[0], N*8, dev);

        uint32_t blk = blk2;  /* SM×2 winner */
        size_t smem  = TPB * sizeof(uint64_t);
        cudaStream_t s; cudaStreamCreate(&s);

        /* warmup */
        kernel_warp_spec<<<blk,TPB,smem,s>>>(buf[0],d_hil,d_lane,d_audit,N,1);
        cudaStreamSynchronize(s);

        cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
        cudaEventRecord(t0);
        for(int rep=0; rep<REPS; rep++) {
            int cur  = rep & 1;
            int next = cur ^ 1;
            kernel_warp_spec<<<blk,TPB,smem,s>>>(
                buf[cur],d_hil,d_lane,d_audit,N,1);
            /* prefetch next while GPU runs cur */
            cudaMemPrefetchAsync(buf[next], N*8, dev);
        }
        cudaStreamSynchronize(s);
        cudaEventRecord(t1); cudaEventSynchronize(t1);
        float ms; cudaEventElapsedTime(&ms,t0,t1);
        double mops = (double)N*REPS/(ms/1000.0)/1e6;
        printf("  WarpSpec SM×2 + lookahead:              %7.0f M/s  ms=%.2f\n",
               mops, ms);
        /* FIX: verify last buffer used */
        int last_cur = (REPS - 1) & 1;
        verify(buf[last_cur], d_audit, N, "lookahead-fixed");

        cudaFree(buf[0]); cudaFree(buf[1]);
        cudaStreamDestroy(s);
        cudaEventDestroy(t0); cudaEventDestroy(t1);
    }

    printf("\n=== Journey summary ===\n");
    printf("  bench2 (timing bug)      :     200 M/s\n");
    printf("  bench3 (SoA fix)         :    9900 M/s\n");
    printf("  bench4 (managed+prefetch):   17748 M/s\n");
    printf("  bench5 (warp spec found) :   21787 M/s\n");
    printf("  bench5b target           :   ~22000+ M/s?\n");

    cudaFree(m_addr); cudaFree(d_hil); cudaFree(d_lane); cudaFree(d_audit);
    return 0;
}
