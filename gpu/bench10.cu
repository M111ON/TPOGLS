/*
 * bench10.cu  POGLS38  44K → 55-65K M/s
 *
 * Start: D bitpack+ILP2 = 44,522 M/s sus
 * Apply: ILP4 + register reuse + warp-aligned base
 *
 * kernels:
 *   D  bitpack+ILP2  (bench9 winner, reference)
 *   E  bitpack+ILP4  (4-way unroll)
 *   F  ILP4+regs     (ILP4 + variable reuse, reduce reg pressure)
 *   G  ILP4+warp     (ILP4 + warp-aligned base)
 *   H  ILP4+all      (E+F+G combined — best shot)
 */
#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>

#define PHI_UP   1696631u
#define PHI_MASK ((1u<<20)-1u)

/* pack: hilbert(20b)|lane(6b)|audit(1b) */
#define PACK(h,l,a) (((uint32_t)(h)&0xFFFFFu)|(((uint32_t)(l)&0x3Fu)<<20)|(((uint32_t)(a)&1u)<<26))
#define UNPACK_HIL(p)   ((p)&0xFFFFFu)
#define UNPACK_LANE(p)  (((p)>>20)&0x3Fu)
#define UNPACK_AUDIT(p) (((p)>>26)&1u)

__device__ __constant__ uint8_t d_lut[16]={0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

/* fast_iso: a%17==1 → 0, else 1  (branchless div-by-17) */
__device__ __forceinline__ uint8_t fast_iso(uint64_t a){
    uint32_t lo=(uint32_t)(a&0xFFFFFFFFu);
    return (lo-(uint32_t)(((uint64_t)lo*4042322161ULL)>>36)*17u==1u)?0u:1u;
}

/* phi_hilbert: addr→hilbert index */
__device__ __forceinline__ uint32_t phi_hilbert(uint64_t a){
    uint32_t t=(uint32_t)(((a&PHI_MASK)*(uint64_t)PHI_UP)>>20);
    uint32_t x=t&0x3FFu, y=(t>>10)&0x3FFu;
    x=(x|(x<<8))&0x00FF00FFu; x=(x|(x<<4))&0x0F0F0F0Fu;
    x=(x|(x<<2))&0x33333333u; x=(x|(x<<1))&0x55555555u;
    y=(y|(y<<8))&0x00FF00FFu; y=(y|(y<<4))&0x0F0F0F0Fu;
    y=(y|(y<<2))&0x33333333u; y=(y|(y<<1))&0x55555555u;
    uint32_t m=x|(y<<1);
    return ((m>>4)<<4)|d_lut[m&0xFu];
}

/* one_cell: compute packed result for address a */
__device__ __forceinline__ uint32_t one_cell(uint64_t a){
    uint32_t h=phi_hilbert(a);
    return PACK(h, h%54u, fast_iso(a));
}

/* ── D: ILP2 (bench9 reference) ── */
__global__ __launch_bounds__(256,2)
void kD(uint32_t* __restrict__ pk,
        uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t g=gridDim.x*blockDim.x, i0=blockIdx.x*blockDim.x+threadIdx.x;
    for(uint32_t r=0;r<reps;r++){
        uint32_t i=i0;
        for(;i+g<N;i+=g*2u){
            pk[i]   = one_cell(base+sv*(uint64_t)i);
            pk[i+g] = one_cell(base+sv*(uint64_t)(i+g));
        }
        if(i<N) pk[i]=one_cell(base+sv*(uint64_t)i);
    }
}

/* ── E: ILP4 ── */
__global__ __launch_bounds__(256,2)
void kE(uint32_t* __restrict__ pk,
        uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t g=gridDim.x*blockDim.x, i0=blockIdx.x*blockDim.x+threadIdx.x;
    for(uint32_t r=0;r<reps;r++){
        uint32_t i=i0;
        for(;i+g*3u<N;i+=g*4u){
            pk[i]     = one_cell(base+sv*(uint64_t)i);
            pk[i+g]   = one_cell(base+sv*(uint64_t)(i+g));
            pk[i+g*2] = one_cell(base+sv*(uint64_t)(i+g*2u));
            pk[i+g*3] = one_cell(base+sv*(uint64_t)(i+g*3u));
        }
        for(;i<N;i+=g) pk[i]=one_cell(base+sv*(uint64_t)i);
    }
}

/* ── F: ILP4 + register reuse (fold sv*g into precomputed offsets) ──
 * precompute sv*g, sv*2g, sv*3g once per thread → saves 3 muls per loop
 * ptxas target: ≤16 regs (same as kB/kC)
 * ───────────────────────────────────────────────────────────────────── */
__global__ __launch_bounds__(256,4)   /* occ hint: 4 blocks/SM */
void kF(uint32_t* __restrict__ pk,
        uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t g=gridDim.x*blockDim.x, i0=blockIdx.x*blockDim.x+threadIdx.x;
    uint64_t sv1=sv*(uint64_t)g,   /* stride for +1 */
             sv2=sv1*2u,           /* stride for +2 */
             sv3=sv1*3u;           /* stride for +3 */
    uint64_t a0=base+sv*(uint64_t)i0; /* base addr for this thread */
    uint64_t step=sv*(uint64_t)(g*4u);/* addr step per outer iter */
    uint32_t i=i0;
    for(uint32_t r=0;r<reps;r++){
        a0=base+sv*(uint64_t)i0;
        i=i0;
        for(;i+g*3u<N;i+=g*4u,a0+=step){
            pk[i]     = one_cell(a0);
            pk[i+g]   = one_cell(a0+sv1);
            pk[i+g*2] = one_cell(a0+sv2);
            pk[i+g*3] = one_cell(a0+sv3);
        }
        for(;i<N;i+=g,a0+=sv1) pk[i]=one_cell(a0);
    }
}

/* ── G: ILP4 + warp-aligned base ──
 * warp processes 32 consecutive elements → spatial locality
 * base address snapped to warp boundary
 * ─────────────────────────────────────────────────────── */
__global__ __launch_bounds__(256,2)
void kG(uint32_t* __restrict__ pk,
        uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t tid=threadIdx.x, warp=tid>>5, lane_t=tid&31;
    uint32_t warp_base_block = (blockIdx.x*(blockDim.x>>5)+warp)*32u;
    uint32_t gw = gridDim.x*(blockDim.x>>5)*32u;  /* grid stride in warp units */
    for(uint32_t r=0;r<reps;r++){
        for(uint32_t wb=warp_base_block; wb+lane_t+gw*3u<N; wb+=gw*4u){
            uint32_t i0=wb+lane_t;
            pk[i0]       = one_cell(base+sv*(uint64_t)i0);
            pk[i0+gw]    = one_cell(base+sv*(uint64_t)(i0+gw));
            pk[i0+gw*2u] = one_cell(base+sv*(uint64_t)(i0+gw*2u));
            pk[i0+gw*3u] = one_cell(base+sv*(uint64_t)(i0+gw*3u));
        }
        /* tail */
        for(uint32_t wb=warp_base_block+(N/(gw*4u))*(gw*4u);
            wb+lane_t<N; wb+=gw){
            uint32_t i0=wb+lane_t;
            if(i0<N) pk[i0]=one_cell(base+sv*(uint64_t)i0);
        }
    }
}

/* ── H: ILP4 + reg-reuse + warp-align (all combined) ── */
__global__ __launch_bounds__(256,4)
void kH(uint32_t* __restrict__ pk,
        uint64_t base, uint64_t sv, uint32_t N, uint32_t reps)
{
    uint32_t tid=threadIdx.x, warp=tid>>5, lane_t=tid&31;
    uint32_t warps_per_block=blockDim.x>>5;
    uint32_t warp_idx=blockIdx.x*warps_per_block+warp;
    uint32_t total_warps=gridDim.x*warps_per_block;
    uint32_t gw=total_warps*32u;  /* full grid warp stride */
    uint64_t sv_gw=sv*(uint64_t)gw;
    uint64_t step4=sv_gw*4u;
    for(uint32_t r=0;r<reps;r++){
        uint32_t wb0=warp_idx*32u+lane_t;
        uint64_t a0=base+sv*(uint64_t)wb0;
        uint32_t i=wb0;
        for(;i+gw*3u<N;i+=gw*4u,a0+=step4){
            pk[i]       = one_cell(a0);
            pk[i+gw]    = one_cell(a0+sv_gw);
            pk[i+gw*2u] = one_cell(a0+sv_gw*2u);
            pk[i+gw*3u] = one_cell(a0+sv_gw*3u);
        }
        for(;i<N;i+=gw,a0+=sv_gw)
            if(i<N) pk[i]=one_cell(a0);
    }
}

typedef void(*Kfn)(uint32_t*,uint64_t,uint64_t,uint32_t,uint32_t);

static void run(const char *lbl, Kfn fn,
                uint32_t blk, uint32_t tpb, uint32_t *dp,
                uint64_t base, uint64_t sv, uint32_t N, int rp, int rs)
{
    fn<<<blk,tpb>>>(dp,base,sv,N,2); cudaDeviceSynchronize();
    cudaEvent_t t0,t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
    float mp,ms;
    cudaEventRecord(t0); fn<<<blk,tpb>>>(dp,base,sv,N,rp);
    cudaEventRecord(t1); cudaEventSynchronize(t1); cudaEventElapsedTime(&mp,t0,t1);
    cudaEventRecord(t0); fn<<<blk,tpb>>>(dp,base,sv,N,rs);
    cudaEventRecord(t1); cudaEventSynchronize(t1); cudaEventElapsedTime(&ms,t0,t1);
    printf("  %-28s  peak %6.0f  sus %6.0f M/s",lbl,
           (double)N*rp/(mp/1e3)/1e6,(double)N*rs/(ms/1e3)/1e6);
    /* quick iso spot-check (first 256) */
    uint32_t h[256]; cudaMemcpy(h,dp,256*4,cudaMemcpyDeviceToHost);
    int ok=1;
    for(int k=0;k<256;k++){
        uint64_t a=base+sv*(uint64_t)k;
        uint8_t ex=(a%17u==1u)?0u:1u;
        if(UNPACK_AUDIT(h[k])!=ex||UNPACK_LANE(h[k])!=(uint8_t)(UNPACK_HIL(h[k])%54u)){ok=0;break;}
    }
    printf("  iso %s\n", ok?"✓":"❌ FAIL");
    cudaEventDestroy(t0); cudaEventDestroy(t1);
}

int main(){
    cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
    int nsm=p.multiProcessorCount;
    printf("GPU: %s  SMs=%d\n\n",p.name,nsm);

    const uint32_t N=2*1024*1024, TPB=256, BLK=(uint32_t)nsm*2;
    const uint64_t BASE=1, SV=34;
    uint32_t *dp; cudaMalloc(&dp,N*4);

    printf("=== bench10: 44K → 55-65K ===\n");
    printf("    N=%dM  BLK=%d(SM×2)  TPB=%d\n\n",N>>20,BLK,TPB);
    printf("  %-28s  %6s  %8s\n","kernel","peak","sus(200r)");
    printf("  %s\n","─────────────────────────────────────────────────");

    run("D ILP2 (bench9 ref)",    kD, BLK,TPB,dp,BASE,SV,N,20,200);
    run("E ILP4",                 kE, BLK,TPB,dp,BASE,SV,N,20,200);
    run("F ILP4+regs",            kF, BLK,TPB,dp,BASE,SV,N,20,200);
    run("G ILP4+warp",            kG, BLK,TPB,dp,BASE,SV,N,20,200);
    run("H ILP4+regs+warp (all)", kH, BLK,TPB,dp,BASE,SV,N,20,200);

    printf("\n=== Theory ceiling (T4) ===\n");
    printf("  DRAM BW 300 GB/s,  packed 8MB\n");
    printf("  8MB×200 @ 300GB/s = 5.3ms → BW ceiling = %.0f M/s\n",
           (double)N*200/5.3e-3/1e6);
    printf("  SM compute: 40SM × 2048th × ~1ns/op ≈ 82,000 M/s\n");
    printf("  Realistic: 55,000-65,000 M/s sus\n");

    printf("\n=== Journey ===\n");
    printf("  bench2 (timing)   :    200  M/s\n");
    printf("  bench3 (SoA)      :  9,900  M/s\n");
    printf("  bench4 (managed)  : 17,748  M/s\n");
    printf("  bench5 (warpspec) : 21,800  M/s\n");
    printf("  bench6 (gen-inp)  : 28,501  M/s  peak\n");
    printf("  bench9 (bitpack)  : 44,522  M/s  sus\n");
    printf("  bench10 target    : 55-65K  M/s  sus\n");

    cudaFree(dp);
    return 0;
}
