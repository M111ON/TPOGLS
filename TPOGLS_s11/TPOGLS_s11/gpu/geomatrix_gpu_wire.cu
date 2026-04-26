/*
 * geomatrix_gpu_wire.cu — P3: fetch_bit wired to GeoSeed + Cylinder
 *
 * ต่างจาก kernel เดิมตรงนี้:
 *   1. d_bundle derive จาก GeoSeed บน CPU → upload
 *   2. fetch_bit ใช้ CYL_FULL_N (3456) address space
 *   3. validate รู้จัก cylinder spoke mapping
 *
 * Colab: nvcc -O2 -arch=sm_75 -o geo_wire geomatrix_gpu_wire.cu
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ── */
#include "geo_config.h"
#define CYL_SPOKES  GEO_SPOKES   /* compat: kernel uses CYL_SPOKES directly */
#define CYL_SLOTS   GEO_SLOTS
#define CYL_FULL_N  GEO_FULL_N
#include "geo_pipeline_wire.h"  /* GeoNet+RH integration */
#define BLOCK_SIZE        256

/* ── Phase masks (v2, upper32-only) ── */
static __constant__ uint64_t d_phase_mask[4] = {
    0xAAAAAAAA00000000ULL,
    0x5555555500000000ULL,
    0xF0F0F0F000000000ULL,
    0x0F0F0F0F00000000ULL,
};

/* ── GeoSeed: defined in geo_thirdeye.h (via geo_pipeline_wire.h include chain) ── */

/* ── GeoPacketWire: defined in geo_pipeline_wire.h (GEOPKTWIRE_DEFINED guard) ── */

/* ── CPU: derive bundle from GeoSeed ─────────────────────────────────
 * pure function — same seed → same bundle always
 * matches handoff v2.0 derive_bundles() spec
 */
static inline void cpu_derive_bundle(const GeoSeed *s, uint64_t *b) {
    uint64_t mix = s->gen2 ^ s->gen3;
    b[0] = s->gen2;
    b[1] = s->gen3;
    b[2] = s->gen2 ^ s->gen3;
    b[3] = ~s->gen2;
    b[4] = ~s->gen3;
    b[5] = ~(s->gen2 ^ s->gen3);
    /* free shadow: rol(mix,12/18/24) — b[8] covers center face slot 512..575 (word=8) */
    b[6] = (mix << 12) | (mix >> (64-12));
    b[7] = (mix << 18) | (mix >> (64-18));
    b[8] = (mix << 24) | (mix >> (64-24));   /* center face word */
}

/* ── GPU: fetch_bit with cylinder address space ───────────────────────
 * idx = slot (0..575 per-spoke) — already from geo_pipeline_fill()
 * word = slot >> 6   → bundle word 0..8 (8 = center face)
 * off  = slot & 63   → bit offset within word
 */
__global__ void fetch_bit_wire(
    const uint64_t* __restrict__ d_bundle,  /* 9 words (GEO_BUNDLE_WORDS) */
    const uint16_t* __restrict__ d_idx,     /* slot 0..575                */
    uint8_t*        __restrict__ d_out,
    int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;

    uint32_t slot = d_idx[i];               /* slot 0..575 (per-spoke)    */
    uint32_t word = slot >> 6;              /* 0..8  (8 = center face)    */
    uint32_t off  = slot & 63;

    d_out[i] = (uint8_t)((d_bundle[word] >> off) & 1ULL);
}

/* ── GPU: validate with GeoSeed sig ─────────────────────────────────── */
__global__ void validate_wire(
    const uint64_t*       __restrict__ d_bundle,
    const GeoPacketWire*  __restrict__ d_pkts,
    uint8_t*              __restrict__ d_result,
    int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;

    GeoPacketWire p = d_pkts[i];

    /* fold bundle → sig */
    uint64_t fold = 0;
    #pragma unroll
    for (int w = 0; w < GEO_BUNDLE_WORDS; w++) fold ^= d_bundle[w];

    uint8_t  ph  = p.phase < GEO_PHASE_COUNT ? p.phase : 0;
    uint64_t s64 = fold ^ d_phase_mask[ph];
    uint32_t s32 = (uint32_t)(s64 >> 32) ^ (uint32_t)(s64 & 0xFFFFFFFFU);

    if (p.sig32 == 0 || p.sig32 != s32) { d_result[i] = 0; return; }

    /* spoke invert check: spoke opposite = (spoke+3)%6 */
    uint8_t inv = (p.spoke + 3) % CYL_SPOKES;
    d_result[i] = (inv < CYL_SPOKES) ? 1 : 0;
}

#define CUDA_CHECK(x) do { \
    cudaError_t e=(x); \
    if(e!=cudaSuccess){fprintf(stderr,"CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));exit(1);} \
} while(0)

#define N_PACKETS (1<<20)

int main(void) {
    printf("=== P3: fetch_bit GPU Wire (GeoSeed + Cylinder) ===\n\n");

    /* ── 1. GeoSeed → derive bundle (CPU) ── */
    GeoSeed seed = {0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL};
    uint64_t h_bundle[GEO_BUNDLE_WORDS];
    cpu_derive_bundle(&seed, h_bundle);

    printf("[GeoSeed → bundle (CPU derive)]\n");
    for (int i = 0; i < GEO_BUNDLE_WORDS; i++)
        printf("  b[%d] = 0x%016llx\n", i, (unsigned long long)h_bundle[i]);

    /* verify invariant: b[i]^b[i+3] = 0xfff... */
    int inv_ok = 1;
    for (int i = 0; i < 3; i++)
        if ((h_bundle[i] ^ h_bundle[i+3]) != 0xFFFFFFFFFFFFFFFFULL) inv_ok = 0;
    printf("  b[i]^b[i+3]=0xfff invariant: %s\n\n", inv_ok?"✓":"✗");

    /* ── 2. Build packets ── */
    uint64_t fold = 0;
    for (int w = 0; w < GEO_BUNDLE_WORDS; w++) fold ^= h_bundle[w];
    uint64_t s64_probe = fold ^ 0xAAAAAAAA00000000ULL;
    uint32_t s32_probe = (uint32_t)(s64_probe>>32)^(uint32_t)(s64_probe&0xFFFFFFFFU);

    GeoPacketWire *h_pkts;
    uint16_t      *h_idx;
    uint8_t       *h_bits_out, *h_result;
    CUDA_CHECK(cudaMallocHost(&h_pkts,     N_PACKETS * sizeof(GeoPacketWire)));
    CUDA_CHECK(cudaMallocHost(&h_idx,      N_PACKETS * sizeof(uint16_t)));
    CUDA_CHECK(cudaMallocHost(&h_bits_out, N_PACKETS));
    CUDA_CHECK(cudaMallocHost(&h_result,   N_PACKETS));

    /* GeoNet + Radial Hilbert → spoke/slot/phase from geometry */
    static GeoPipeline geo_pipe;
    geo_pipeline_init(&geo_pipe, (GeoSeed){2, 3});
    geo_pipeline_fill(&geo_pipe, h_pkts, N_PACKETS, 0, 0, 0);
    /* sig32 override: stamp probe sig (GPU will validate) */
    for (int i = 0; i < N_PACKETS; i++) {
        h_pkts[i].sig32 = s32_probe;
        h_idx[i]        = h_pkts[i].idx;
    }

    /* ── 3. GPU alloc + transfer ── */
    uint64_t      *d_bundle;
    GeoPacketWire *d_pkts;
    uint16_t      *d_idx;
    uint8_t       *d_bits_out, *d_result;

    CUDA_CHECK(cudaMalloc(&d_bundle,   GEO_BUNDLE_WORDS * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_pkts,     N_PACKETS * sizeof(GeoPacketWire)));
    CUDA_CHECK(cudaMalloc(&d_idx,      N_PACKETS * sizeof(uint16_t)));
    CUDA_CHECK(cudaMalloc(&d_bits_out, N_PACKETS));
    CUDA_CHECK(cudaMalloc(&d_result,   N_PACKETS));

    CUDA_CHECK(cudaMemcpy(d_bundle, h_bundle, GEO_BUNDLE_WORDS*sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_pkts,   h_pkts,   N_PACKETS*sizeof(GeoPacketWire),   cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_idx,    h_idx,    N_PACKETS*sizeof(uint16_t),         cudaMemcpyHostToDevice));

    int grid = (N_PACKETS + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* ── 4. Warmup ── */
    fetch_bit_wire<<<grid,BLOCK_SIZE>>>(d_bundle, d_idx, d_bits_out, N_PACKETS);
    validate_wire<<<grid,BLOCK_SIZE>>>(d_bundle, d_pkts, d_result, N_PACKETS);
    CUDA_CHECK(cudaDeviceSynchronize());

    /* ── 5. Benchmark ── */
    cudaEvent_t t0, t1;
    CUDA_CHECK(cudaEventCreate(&t0));
    CUDA_CHECK(cudaEventCreate(&t1));

    CUDA_CHECK(cudaEventRecord(t0));
    for (int r=0;r<10;r++)
        fetch_bit_wire<<<grid,BLOCK_SIZE>>>(d_bundle,d_idx,d_bits_out,N_PACKETS);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));
    float ms_fetch; CUDA_CHECK(cudaEventElapsedTime(&ms_fetch,t0,t1)); ms_fetch/=10.0f;

    CUDA_CHECK(cudaEventRecord(t0));
    for (int r=0;r<10;r++)
        validate_wire<<<grid,BLOCK_SIZE>>>(d_bundle,d_pkts,d_result,N_PACKETS);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));
    float ms_val; CUDA_CHECK(cudaEventElapsedTime(&ms_val,t0,t1)); ms_val/=10.0f;

    /* ── 6. Verify ── */
    CUDA_CHECK(cudaMemcpy(h_result, d_result, N_PACKETS, cudaMemcpyDeviceToHost));
    int pass=0; for(int i=0;i<N_PACKETS;i++) pass+=h_result[i];

    /* ── 7. Results ── */
    printf("[fetch_bit_wire]\n");
    printf("  %.3f ms  |  %.1f M-pkt/s  |  %.2f ns/pkt\n",
           ms_fetch, N_PACKETS/(ms_fetch*1e-3)/1e6, ms_fetch*1e6/N_PACKETS);

    printf("[validate_wire]\n");
    printf("  %.3f ms  |  %.1f M-pkt/s  |  %.2f ns/pkt\n",
           ms_val, N_PACKETS/(ms_val*1e-3)/1e6, ms_val*1e6/N_PACKETS);
    printf("  pass: %d/%d (%.1f%%)\n", pass, N_PACKETS, 100.0*pass/N_PACKETS);

    printf("\n[cylinder address space]\n");
    printf("  CYL_FULL_N = %d (576*6 = 144*24)\n", CYL_FULL_N);
    printf("  idx = slot (0..575) → word = slot>>6 (0..8)\n");
    printf("  center face: slots 512..575 → word=8 = b[8]\n");
    printf("  spoke= idx%%6 → invert = (spoke+3)%%6\n");

    /* cleanup */
    cudaFree(d_bundle); cudaFree(d_pkts); cudaFree(d_idx);
    cudaFree(d_bits_out); cudaFree(d_result);
    cudaFreeHost(h_pkts); cudaFreeHost(h_idx);
    cudaFreeHost(h_bits_out); cudaFreeHost(h_result);
    return 0;
}
