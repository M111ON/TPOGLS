/*
 * bench9_real.cu  POGLS38  22K → 27K+ stable
 *
 * Root cause of peak→sus gap: DRAM write pressure
 *   output 12 MB > T4 L2 (4 MB) → every rep = full DRAM round-trip
 *
 * Fix: bitpack hilbert(20b)+lane(6b)+audit(1b) → 1 uint32
 *   12 MB → 8 MB = 33% fewer DRAM writes
 *
 * Kernels:
 *   A: baseline      (3 separate arrays, 12 MB)
 *   B: bitpack        (1 uint32, 8 MB)
 *   C: bitpack+stcs   (streaming store, skip L2)
 *   D: bitpack+ILP2   (2 ops/iter, hide latency)
 */
#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>

#define PHI_UP   1696631u
#define PHI_MASK ((1u<<20)-1u)

#define PACK(hil,ln,au) \
    (((uint32_t)(hil)&0xFFFFFu)|(((uint32_t)(ln)&0x3Fu)<<20)|(((uint32_t)(au)&1u)<<26))
#define UNPACK_HIL(p)   ((p)&0xFFFFFu)
#define UNPACK_LANE(p)  (((p)>>20)&0x3Fu)
#define UNPACK_AUDIT(p) (((p)>>26)&1u)

__device__ __constant__ uint8_t d_lut[16] = {
    0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10
};

__device__ __forceinline__ uint8_t fast_iso(uint64_t a) {
    uint32_t lo = (uint32_t)(a & 0xFFFFFFFFu);
    uint32_t q  = (uint32_t)(((uint64_t)lo * 4042322161ULL) >> 36);
    return (lo - q*17u == 1u) ? 0u : 1u;
}

__device__ __forceinline__ uint32_t phi_hilbert(uint64_t a) {
    uint32_t t  = (uint32_t)(((a & PHI_MASK)*(uint64_t)PHI_UP)>>20);
    uint16_t x  = t & 0x3FFu, y = (t>>10) & 0x3FFu;
    uint32_t rx=x, ry=y;
    rx=(rx|(rx<<8))&0x00FF00FFu; rx=(rx|(rx<<4))&0x0F0F0F0Fu;
    rx=(rx|(rx<<2))&0x33333333u; rx=(rx|(rx<<1))&0x55555555u;
    ry=(ry|(ry<<8))&0x00FF00FFu; ry=(ry|(ry<<4))&0x0F0F0F0Fu;
    ry=(ry|(ry<<2))&0x33333333u; ry=(ry|(ry<<1))&0x55555555u;
    uint32_t mor = rx|(ry<<1);
    return ((mor>>4)<<4)|d_lut[mor&0xFu];
}

/* ── A: baseline 3 arrays ── */
__global__ __launch_bounds__(256,2)
void kA_baseline(uint32_t* __restrict__ h, uint8_t* __restrict__ l,
                 uint8_t* __restrict__ a,
                 uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t g=gridDim.x*blockDim.x, i0=blockIdx.x*blockDim.x+threadIdx.x;
    for(uint32_t r=0;r<reps;r++)
        for(uint32_t i=i0;i<N;i+=g){
            uint64_t addr=base+sv*(uint64_t)i;
            uint32_t hil=phi_hilbert(addr);
            h[i]=hil; l[i]=(uint8_t)(hil%54u); a[i]=fast_iso(addr);
        }
}

/* ── B: bitpack 1 uint32 ── */
__global__ __launch_bounds__(256,2)
void kB_bitpack(uint32_t* __restrict__ pk,
                uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t g=gridDim.x*blockDim.x, i0=blockIdx.x*blockDim.x+threadIdx.x;
    for(uint32_t r=0;r<reps;r++)
        for(uint32_t i=i0;i<N;i+=g){
            uint64_t addr=base+sv*(uint64_t)i;
            uint32_t hil=phi_hilbert(addr);
            pk[i]=PACK(hil, hil%54u, fast_iso(addr));
        }
}

/* ── C: bitpack + streaming store (bypass L2 on write) ── */
__global__ __launch_bounds__(256,2)
void kC_stream(uint32_t* __restrict__ pk,
               uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t g=gridDim.x*blockDim.x, i0=blockIdx.x*blockDim.x+threadIdx.x;
    for(uint32_t r=0;r<reps;r++)
        for(uint32_t i=i0;i<N;i+=g){
            uint64_t addr=base+sv*(uint64_t)i;
            uint32_t hil=phi_hilbert(addr);
            __stcs(&pk[i], PACK(hil, hil%54u, fast_iso(addr)));
        }
}

/* ── D: bitpack + ILP-2 ── */
__global__ __launch_bounds__(256,2)
void kD_ilp2(uint32_t* __restrict__ pk,
             uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t g=gridDim.x*blockDim.x, i0=blockIdx.x*blockDim.x+threadIdx.x;
    for(uint32_t r=0;r<reps;r++){
        uint32_t i=i0;
        for(;i+g<N;i+=g*2){
            uint64_t a0=base+sv*(uint64_t)i,    a1=base+sv*(uint64_t)(i+g);
            uint32_t h0=phi_hilbert(a0),          h1=phi_hilbert(a1);
            pk[i]  =PACK(h0,h0%54u,fast_iso(a0));
            pk[i+g]=PACK(h1,h1%54u,fast_iso(a1));
        }
        if(i<N){
            uint64_t a=base+sv*(uint64_t)i;
            uint32_t h=phi_hilbert(a);
            pk[i]=PACK(h,h%54u,fast_iso(a));
        }
    }
}

/* ══ timing helpers ══ */
static double elapsed_ms(cudaEvent_t t0, cudaEvent_t t1) {
    float ms; cudaEventElapsedTime(&ms,t0,t1); return ms;
}

typedef void(*KA_t)(uint32_t*,uint8_t*,uint8_t*,uint64_t,uint64_t,uint32_t,uint32_t);
typedef void(*KB_t)(uint32_t*,uint64_t,uint64_t,uint32_t,uint32_t);

static void bench_sep(const char *lbl, KA_t fn,
                      uint32_t blk, uint32_t tpb,
                      uint32_t *dh, uint8_t *dl, uint8_t *da,
                      uint64_t base, uint64_t sv, uint32_t N, int rp, int rs)
{
    fn<<<blk,tpb>>>(dh,dl,da,base,sv,N,2); cudaDeviceSynchronize();
    cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0); fn<<<blk,tpb>>>(dh,dl,da,base,sv,N,rp);
    cudaEventRecord(t1); cudaEventSynchronize(t1); double mp=elapsed_ms(t0,t1);
    cudaEventRecord(t0); fn<<<blk,tpb>>>(dh,dl,da,base,sv,N,rs);
    cudaEventRecord(t1); cudaEventSynchronize(t1); double ms=elapsed_ms(t0,t1);
    printf("  %-32s  peak %6.0f  sus %6.0f M/s\n",lbl,
           (double)N*rp/(mp/1e3)/1e6, (double)N*rs/(ms/1e3)/1e6);
    cudaEventDestroy(t0); cudaEventDestroy(t1);
}

static void bench_pk(const char *lbl, KB_t fn,
                     uint32_t blk, uint32_t tpb, uint32_t *dp,
                     uint64_t base, uint64_t sv, uint32_t N, int rp, int rs)
{
    fn<<<blk,tpb>>>(dp,base,sv,N,2); cudaDeviceSynchronize();
    cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0); fn<<<blk,tpb>>>(dp,base,sv,N,rp);
    cudaEventRecord(t1); cudaEventSynchronize(t1); double mp=elapsed_ms(t0,t1);
    cudaEventRecord(t0); fn<<<blk,tpb>>>(dp,base,sv,N,rs);
    cudaEventRecord(t1); cudaEventSynchronize(t1); double ms=elapsed_ms(t0,t1);
    printf("  %-32s  peak %6.0f  sus %6.0f M/s\n",lbl,
           (double)N*rp/(mp/1e3)/1e6, (double)N*rs/(ms/1e3)/1e6);
    cudaEventDestroy(t0); cudaEventDestroy(t1);
}

static void iso_sep(uint64_t base, uint64_t sv, const uint8_t *da, uint32_t N) {
    uint8_t *h=(uint8_t*)malloc(N);
    cudaMemcpy(h,da,N,cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for(uint32_t i=0;i<N;i++){
        uint8_t ex=((base+sv*i)%17u==1u)?0u:1u;
        if(h[i]==ex)ok++;else fail++;
    }
    printf("    iso: %llu/%llu%s\n",(unsigned long long)ok,
           (unsigned long long)(ok+fail),fail?" ❌":" ✓");
    free(h);
}

static void iso_pk(uint64_t base, uint64_t sv, const uint32_t *dp, uint32_t N) {
    uint32_t *h=(uint32_t*)malloc(N*4);
    cudaMemcpy(h,dp,N*4,cudaMemcpyDeviceToHost);
    uint64_t ok=0,fail=0;
    for(uint32_t i=0;i<N;i++){
        uint64_t a=base+sv*(uint64_t)i;
        uint8_t ex=(a%17u==1u)?0u:1u;
        uint32_t hil=UNPACK_HIL(h[i]);
        uint8_t ln=UNPACK_LANE(h[i]), au=UNPACK_AUDIT(h[i]);
        if(au==ex && ln==(uint8_t)(hil%54u)) ok++; else fail++;
    }
    printf("    iso+lane: %llu/%llu%s\n",(unsigned long long)ok,
           (unsigned long long)(ok+fail),fail?" ❌":" ✓");
    free(h);
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    int nsm=p.multiProcessorCount;
    printf("GPU: %s  SMs=%d\n\n",p.name,nsm);

    const uint32_t N=2*1024*1024, TPB=256, BLK=(uint32_t)nsm*2;
    const uint64_t BASE=1, SV=34;
    const int RP=20, RS=200;

    uint32_t *dh,*dp; uint8_t *dl,*da;
    cudaMalloc(&dh,N*4); cudaMalloc(&dl,N); cudaMalloc(&da,N);
    cudaMalloc(&dp,N*4);

    printf("=== bench9_real: DRAM pressure fix ===\n");
    printf("    N=%dM  BLK=%d(SM×2)  TPB=%d\n",N>>20,BLK,TPB);
    printf("    baseline=12MB out  packed=8MB out\n\n");
    printf("  %-32s  %6s  %8s\n","kernel","peak","sus(200r)");
    printf("  %s\n","───────────────────────────────────────────────────");

    bench_sep("A baseline (12MB)",kA_baseline,BLK,TPB,dh,dl,da,BASE,SV,N,RP,RS);
    iso_sep(BASE,SV,da,N);

    bench_pk ("B bitpack  (8MB)",  kB_bitpack, BLK,TPB,dp,BASE,SV,N,RP,RS);
    iso_pk(BASE,SV,dp,N);

    bench_pk ("C bitpack+stcs",    kC_stream,  BLK,TPB,dp,BASE,SV,N,RP,RS);
    iso_pk(BASE,SV,dp,N);

    bench_pk ("D bitpack+ILP2",    kD_ilp2,    BLK,TPB,dp,BASE,SV,N,RP,RS);
    iso_pk(BASE,SV,dp,N);

    printf("\n=== Theory ceiling (T4) ===\n");
    printf("  DRAM BW ~300 GB/s,  L2 = 4 MB\n");
    printf("  packed 8MB × 200 reps @ 300GB/s = 5.3ms → max %.0f M/s\n",
           (double)N*RS/5.3e-3/1e6);
    printf("  SM compute ceiling ≈ 27000 M/s\n");
    printf("  → realistic sus target: 25000-27000 M/s\n");

    printf("\n=== bench journey ===\n");
    printf("  bench2 timing   :    200 M/s\n");
    printf("  bench3 SoA      :   9900 M/s\n");
    printf("  bench4 managed  :  17748 M/s\n");
    printf("  bench5 warpspec :  21800 M/s\n");
    printf("  bench6 gen-inp  :  28501 M/s peak\n");
    printf("  final sus       :  22658 M/s\n");
    printf("  bench9 target   : ~27000 M/s sus\n");

    cudaFree(dh); cudaFree(dl); cudaFree(da); cudaFree(dp);
    return 0;
}
