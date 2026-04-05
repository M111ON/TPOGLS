/*
 * geomatrix_fused.cu — fused fetch+invert+validate
 *
 * Key optimizations:
 *   1. multiply-shift แทน div/mod 6
 *   2. fused kernel: fetch+validate in 1 pass
 *   3. bitwise mask แทน branch
 *   4. inline everything → shift+xor only
 */

#include <stdint.h>
#include "geo_config.h"
#define CYL_FULL_N  GEO_FULL_N
#define CYL_SPOKES  GEO_SPOKES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE        256

static __constant__ uint64_t d_phase_mask[4] = {
    0xAAAAAAAA00000000ULL,
    0x5555555500000000ULL,
    0xF0F0F0F000000000ULL,
    0x0F0F0F0F00000000ULL,
};

typedef struct { uint64_t gen2; uint64_t gen3; } GeoSeed;

typedef struct __align__(8) {
    uint32_t sig32;
    uint16_t idx;
    uint8_t  spoke;
    uint8_t  phase;
} GeoPacketWire;

/* ── fast div6 / mod6 (multiply-shift, no division) ── */
__device__ __forceinline__ uint32_t fast_div6(uint32_t x) {
    /* __umulhi = upper 32 bits of 32x32 multiply — correct on GPU */
    return __umulhi(x, 0xAAAAAAABu) >> 2;
}
__device__ __forceinline__ uint32_t fast_mod6(uint32_t x) {
    return x - fast_div6(x) * 6;
}

/* ── FUSED kernel: fetch + invert + validate ─────────────────────────
 * 1 thread = 1 packet
 * ops per thread: mul-shift ×2, shift, xor × (8+2), compare
 * no branch on hot path
 */
__global__ void geo_fused(
    const uint64_t*       __restrict__ d_bundle,
    const GeoPacketWire*  __restrict__ d_pkts,
    uint8_t*              __restrict__ d_result,
    int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;

    GeoPacketWire p = d_pkts[i];

    /* ── idx → slot/spoke (no div/mod) ── */
    uint32_t cidx  = p.idx;
    uint32_t slot  = fast_div6(cidx);
    uint32_t spoke = cidx - slot * 6;        /* fast_mod6 inlined */

    /* ── fetch bit ── */
    uint32_t word   = slot >> 6;
    uint32_t bitpos = slot & 63;
    uint8_t  bit    = (uint8_t)((d_bundle[word] >> bitpos) & 1ULL);

    /* ── fold bundle → sig (unrolled) ── */
    uint64_t fold = 0;
    #pragma unroll
    for (int w = 0; w < GEO_BUNDLE_WORDS; w++) fold ^= d_bundle[w];

    uint8_t  ph  = p.phase & 3;
    uint64_t s64 = fold ^ d_phase_mask[ph];
    uint32_t s32 = (uint32_t)(s64 >> 32) ^ (uint32_t)(s64);

    /* ── invert spoke (bitwise, no branch) ── */
    uint32_t inv = spoke + 3;
    inv -= (inv >= 6) ? 6 : 0;   /* branchless mod6 for small n */

    /* ── validate: all bitwise ── */
    uint32_t sig_ok  = (p.sig32 != 0) & (p.sig32 == s32);
    uint32_t inv_ok  = (inv < CYL_SPOKES);
    d_result[i] = (uint8_t)((sig_ok & inv_ok) | (bit << 1));  /* bit in upper, valid in bit0 */

}

/* ── CPU derive (unchanged) ── */
static inline void cpu_derive_bundle(const GeoSeed *s, uint64_t *b) {
    uint64_t mix = s->gen2 ^ s->gen3;
    b[0]=s->gen2; b[1]=s->gen3; b[2]=mix;
    b[3]=~s->gen2; b[4]=~s->gen3; b[5]=~mix;
    b[6]=(mix<<12)|(mix>>(64-12));
    b[7]=(mix<<18)|(mix>>(64-18));
}

#define CUDA_CHECK(x) do { \
    cudaError_t e=(x); \
    if(e!=cudaSuccess){fprintf(stderr,"CUDA %s\n",cudaGetErrorString(e));exit(1);} \
} while(0)

#define N_PACKETS (1<<20)  /* 1M — same as before */

int main(void) {
    printf("=== Geomatrix Fused Kernel ===\n\n");

    GeoSeed seed = {0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL};
    uint64_t h_bundle[GEO_BUNDLE_WORDS];
    cpu_derive_bundle(&seed, h_bundle);

    uint64_t fold=0;
    for(int w=0;w<GEO_BUNDLE_WORDS;w++) fold^=h_bundle[w];
    uint64_t s64 = fold ^ 0xAAAAAAAA00000000ULL;
    uint32_t s32 = (uint32_t)(s64>>32)^(uint32_t)(s64);

    GeoPacketWire *h_pkts;
    uint8_t       *h_result;
    CUDA_CHECK(cudaMallocHost(&h_pkts,   N_PACKETS*sizeof(GeoPacketWire)));
    CUDA_CHECK(cudaMallocHost(&h_result, N_PACKETS));

    for(int i=0;i<N_PACKETS;i++){
        uint16_t cidx   = (uint16_t)(i % CYL_FULL_N);
        h_pkts[i].sig32 = s32;
        h_pkts[i].idx   = cidx;
        h_pkts[i].spoke = (uint8_t)(cidx % 6);
        h_pkts[i].phase = 0;
    }

    uint64_t      *d_bundle;
    GeoPacketWire *d_pkts;
    uint8_t       *d_result;
    CUDA_CHECK(cudaMalloc(&d_bundle, GEO_BUNDLE_WORDS*sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_pkts,   N_PACKETS*sizeof(GeoPacketWire)));
    CUDA_CHECK(cudaMalloc(&d_result, N_PACKETS));

    CUDA_CHECK(cudaMemcpy(d_bundle,h_bundle,GEO_BUNDLE_WORDS*sizeof(uint64_t),cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_pkts,  h_pkts,  N_PACKETS*sizeof(GeoPacketWire),  cudaMemcpyHostToDevice));

    int grid = (N_PACKETS+BLOCK_SIZE-1)/BLOCK_SIZE;

    /* warmup */
    geo_fused<<<grid,BLOCK_SIZE>>>(d_bundle,d_pkts,d_result,N_PACKETS);
    CUDA_CHECK(cudaDeviceSynchronize());

    /* bench x10 */
    cudaEvent_t t0,t1;
    CUDA_CHECK(cudaEventCreate(&t0));
    CUDA_CHECK(cudaEventCreate(&t1));
    CUDA_CHECK(cudaEventRecord(t0));
    for(int r=0;r<10;r++)
        geo_fused<<<grid,BLOCK_SIZE>>>(d_bundle,d_pkts,d_result,N_PACKETS);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));

    float ms; CUDA_CHECK(cudaEventElapsedTime(&ms,t0,t1)); ms/=10.0f;
    CUDA_CHECK(cudaMemcpy(h_result,d_result,N_PACKETS,cudaMemcpyDeviceToHost));

    int pass=0; for(int i=0;i<N_PACKETS;i++) pass+=(h_result[i]&1);

    printf("[geo_fused: fetch+invert+validate]\n");
    printf("  %.3f ms  |  %.1f M-pkt/s  |  %.3f ns/pkt\n",
           ms, N_PACKETS/(ms*1e-3)/1e6, ms*1e6/N_PACKETS);
    printf("  pass: %d/%d (%.1f%%)\n", pass, N_PACKETS, 100.0*pass/N_PACKETS);

    printf("\n[ops per thread]\n");
    printf("  idx→slot/spoke: multiply-shift (no div)\n");
    printf("  fetch:  shift + and\n");
    printf("  fold:   xor x8 (unrolled)\n");
    printf("  sig:    xor x2\n");
    printf("  invert: add + sub (branchless)\n");
    printf("  result: and x2 (no branch)\n");

    cudaFree(d_bundle); cudaFree(d_pkts); cudaFree(d_result);
    cudaFreeHost(h_pkts); cudaFreeHost(h_result);
    return 0;
}
