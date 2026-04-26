/*
 * bench7.cu — POGLS38 GPU7  FINAL PRODUCTION CANDIDATE
 *
 * Winners confirmed:
 *   GenInPlace SM×2 = 28501 M/s (peak)
 *   Sustained gap   = -22%  (thermal/boost)
 *
 * bench7 goals:
 *   1. Confirm SM×2 best for sustained (vs SM×1/4)
 *   2. __launch_bounds__ hint → stable clock
 *   3. Large N test (8M, 16M) — real dataset size
 *   4. Final production number
 */
#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>

#define PHI_UP   1696631u
#define PHI_MASK ((1u<<20)-1u)

__device__ __constant__ uint8_t d_lut[16] = {0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

__device__ __forceinline__ uint8_t fast_iso(uint64_t a) {
    uint32_t lo = (uint32_t)(a & 0xFFFFFFFFu);
    uint32_t q  = (uint32_t)(((uint64_t)lo * 4042322161ULL) >> 36);
    return (lo - q * 17u == 1u) ? 0u : 1u;
}

/* __launch_bounds__(TPB, min_blocks_per_SM)
 * TPB=256, min=2 → compiler keeps registers ≤ 32 → full occupancy */
__global__ __launch_bounds__(256, 2)
void kernel_gen(
    uint32_t * __restrict__ hilbert_out,
    uint8_t  * __restrict__ lane_out,
    uint8_t  * __restrict__ audit_out,
    uint64_t base, uint64_t sv,
    uint32_t N, uint32_t reps)
{
    uint32_t gstride = gridDim.x * blockDim.x;
    uint32_t start   = blockIdx.x * blockDim.x + threadIdx.x;
    for (uint32_t rep = 0; rep < reps; rep++) {
        for (uint32_t i = start; i < N; i += gstride) {
            uint64_t a  = base + sv * (uint64_t)i;
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

static void iso_check(uint64_t base, uint64_t sv,
                      const uint8_t *d_a, uint32_t N, const char *tag) {
    uint8_t *h = (uint8_t*)malloc(N);
    cudaMemcpy(h, d_a, N, cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for(uint32_t i=0;i<N;i++){
        uint64_t a = base + sv*i;
        uint8_t ex = ((a%17u)==1u)?0u:1u;
        if(h[i]==ex) ok++; else fail++;
    }
    printf("    iso: %llu/%llu%s\n",(unsigned long long)ok,
           (unsigned long long)(ok+fail), fail?"  ❌":"  ✓");
    free(h);
}

static double run_gen(const char *label, uint32_t blk, uint32_t tpb,
                      uint32_t *d_hil, uint8_t *d_lane, uint8_t *d_audit,
                      uint64_t base, uint64_t sv, uint32_t N, int reps,
                      int do_iso) {
    /* warmup */
    kernel_gen<<<blk,tpb>>>(d_hil,d_lane,d_audit,base,sv,N,2);
    cudaDeviceSynchronize();
    cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0);
    kernel_gen<<<blk,tpb>>>(d_hil,d_lane,d_audit,base,sv,N,reps);
    cudaEventRecord(t1); cudaEventSynchronize(t1);
    float ms; cudaEventElapsedTime(&ms,t0,t1);
    double mops = (double)N*reps/(ms/1000.0)/1e6;
    printf("  %-42s %7.0f M/s  ms=%.2f\n", label, mops, ms);
    if (do_iso) iso_check(base,sv,d_audit,N,label);
    cudaEventDestroy(t0); cudaEventDestroy(t1);
    return mops;
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    int n_sm = p.multiProcessorCount;
    printf("GPU: %s SM%d.%d  SMs=%d\n\n",p.name,p.major,p.minor,n_sm);

    const uint32_t TPB = 256;
    const uint64_t BASE=1, SV=34;
    int dev; cudaGetDevice(&dev);

    /* alloc for largest N */
    const uint32_t NMAX = 16*1024*1024;
    uint32_t *d_hil; uint8_t *d_lane,*d_audit;
    cudaMalloc(&d_hil,NMAX*4); cudaMalloc(&d_lane,NMAX); cudaMalloc(&d_audit,NMAX);

    /* ── A: SM sweep sustained (20 & 100 reps) ── */
    printf("=== A. SM sweep — sustained (N=2M, 100 reps) ===\n");
    {
        uint32_t N = 2*1024*1024;
        int waves[] = {1,2,4,8};
        for(int w=0;w<4;w++){
            uint32_t blk = (uint32_t)n_sm * waves[w];
            char lbl[64]; sprintf(lbl,"GenInPlace SM×%d (%3u blk) 100r",waves[w],blk);
            run_gen(lbl,blk,TPB,d_hil,d_lane,d_audit,BASE,SV,N,100,0);
        }
    }

    /* ── B: Large N test (real dataset) ── */
    printf("\n=== B. Large N — find memory ceiling ===\n");
    {
        uint32_t blk = (uint32_t)n_sm * 2;  /* SM×2 winner */
        uint32_t Ns[] = {2*1024*1024, 4*1024*1024, 8*1024*1024, 16*1024*1024};
        const char *labels[] = {"N= 2M","N= 4M","N= 8M","N=16M"};
        for(int i=0;i<4;i++){
            char lbl[64]; sprintf(lbl,"GenInPlace SM×2 %s", labels[i]);
            run_gen(lbl,blk,TPB,d_hil,d_lane,d_audit,BASE,SV,Ns[i],20,
                    i==0?1:0);
        }
    }

    /* ── C: Production estimate ── */
    printf("\n=== C. Production estimate ===\n");
    {
        /* POGLS38 real workload: 289 cells × lanes × gate_18
         * estimate N per second */
        uint32_t N   = 8*1024*1024;
        uint32_t blk = (uint32_t)n_sm * 2;
        printf("  [setup] N=8M, SM×2 (%u blk), 50 reps\n", blk);
        run_gen("Production 8M×50r", blk,TPB,
                d_hil,d_lane,d_audit,BASE,SV,N,50,1);
        /* how many POGLS ops/s at this rate? */
        /* each 'op' = 1 addr processed (phi scatter + hilbert + iso) */
        printf("  ── at this M/s:\n");
        printf("     1B ops = %.2f ms\n", 1000.0/22000.0);
        printf("     POGLS38 289 cells × 54 lanes × 18 gate = %u ops/frame\n",
               289u*54u*18u);
        printf("     frames/s at 22000 M/s = %.0f\n",
               22000e6/(289.0*54.0*18.0));
    }

    printf("\n=== Journey (final) ===\n");
    printf("  bench2  (timing bug)         200 M/s\n");
    printf("  bench3  (SoA layout)        9900 M/s\n");
    printf("  bench4  (managed+prefetch) 17748 M/s\n");
    printf("  bench5  (warp spec)        21800 M/s\n");
    printf("  bench6  (generate-in-place) 28501 M/s  peak\n");
    printf("  bench7  ??? (sustained)         M/s\n");
    printf("  Total journey:  ×140+ from start\n");

    cudaFree(d_hil); cudaFree(d_lane); cudaFree(d_audit);
    return 0;
}
