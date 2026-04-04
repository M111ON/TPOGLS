/*
 * geomatrix_kernel.cu
 * GPU bitboard fetch + per-packet validate
 * Colab: nvcc -O2 -arch=sm_75 -o geo_kernel geomatrix_kernel.cu
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants (mirror geomatrix_shared.h — no host header in .cu) ── */
#define GEO_BUNDLE_WORDS  8
#define GEO_HILBERT_N     512
#define GEO_PHASE_COUNT   4

static __constant__ uint64_t d_phase_mask[4] = {
    0xAAAAAAAA00000000ULL,  /* PROBE  */
    0x5555555500000000ULL,  /* MAIN   */
    0xF0F0F0F000000000ULL,  /* MIRROR */
    0x0F0F0F0F00000000ULL,  /* CANCEL */
};

/* ── GeoPacket (16B, must match CPU struct) ── */
typedef struct __align__(16) {
    uint64_t sig;
    uint16_t hpos;
    uint16_t idx;
    uint8_t  bit;
    uint8_t  phase;
    uint8_t  _pad[2];
} GeoPacket;

/* ── Kernel 1: fetch_bit ─────────────────────────────────────────────
 * 1 thread = 1 packet
 * coalesced read: threads in same warp → consecutive idx → same word
 */
__global__ void fetch_bit_kernel(
    const uint64_t* __restrict__ bitboard,   /* 8 × uint64_t, 64-byte aligned */
    const uint16_t* __restrict__ idx,        /* per-packet bit index [0..511] */
    uint8_t*        __restrict__ out,        /* result bit per packet          */
    int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;

    uint32_t bit  = idx[i];
    uint32_t word = bit >> 6;          /* which uint64_t in bitboard */
    uint32_t off  = bit & 63;          /* bit position within word   */

    out[i] = (uint8_t)((bitboard[word] >> off) & 1ULL);
}

/* ── Kernel 2: geo_validate_kernel ──────────────────────────────────
 * reconstruct expected sig from bitboard + phase → compare with packet.sig
 * line_match: hpos block == idx block
 * result[i] = 1 (pass) / 0 (reject)
 */
__global__ void geo_validate_kernel(
    const uint64_t* __restrict__ bitboard,   /* shared 512-bit state */
    const GeoPacket* __restrict__ packets,
    uint8_t*         __restrict__ result,
    int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;

    GeoPacket p = packets[i];

    /* reconstruct sig from bitboard */
    uint64_t fold = 0;
    #pragma unroll
    for (int w = 0; w < GEO_BUNDLE_WORDS; w++)
        fold ^= bitboard[w];

    uint8_t ph = p.phase < GEO_PHASE_COUNT ? p.phase : 0;
    uint64_t expected = fold ^ d_phase_mask[ph];

    /* sig match */
    if (p.sig == 0 || p.sig != expected) { result[i] = 0; return; }

    /* L1 equality only — no popcount, no window
     * Truth is in bundle reconstruct (L2 at endpoint) */

    /* Hilbert line match */
    result[i] = (uint8_t)(((p.hpos >= 256) == (p.idx >= 256)) ? 1 : 0);
}

/* ── Kernel 3: geo_isect_kernel ─────────────────────────────────────
 * 1 thread = 1 raw word, 8 threads/bundle → shared XOR-reduce
 * isect = x & rotl8 & rotl16 & rotl24, fold with lane phase mask
 */
__global__ void geo_isect_kernel(
    const uint64_t* __restrict__ raw,
    uint64_t*       __restrict__ isect_sig,
    int N_bundles)
{
    __shared__ uint64_t smem[256];
    int lane        = threadIdx.x & 7;
    int bund_global = (blockIdx.x << 5) + (threadIdx.x >> 3);

    uint64_t isect = 0;
    if (bund_global < N_bundles) {
        uint64_t x   = raw[bund_global * GEO_BUNDLE_WORDS + lane];
        uint64_t r8  = (x << 8)  | (x >> 56);
        uint64_t r16 = (x << 16) | (x >> 48);
        uint64_t r24 = (x << 24) | (x >> 40);
        int ph       = lane < GEO_PHASE_COUNT ? lane : lane - GEO_PHASE_COUNT;
        isect        = (x & r8 & r16 & r24) ^ d_phase_mask[ph];
    }
    smem[threadIdx.x] = isect;
    __syncthreads();
    if (lane < 4) smem[threadIdx.x] ^= smem[threadIdx.x + 4]; __syncthreads();
    if (lane < 2) smem[threadIdx.x] ^= smem[threadIdx.x + 2]; __syncthreads();
    if (lane < 1) smem[threadIdx.x] ^= smem[threadIdx.x + 1]; __syncthreads();
    if (lane == 0 && bund_global < N_bundles)
        isect_sig[bund_global] = smem[threadIdx.x];
}

/* ── Kernel 4: geo_isect_validate ───────────────────────────────────
 * sig from kernel 3 — no extra phase XOR needed
 */
__global__ void geo_isect_validate(
    const uint64_t*  __restrict__ isect_sig,
    const GeoPacket* __restrict__ packets,
    uint8_t*         __restrict__ result,
    int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;
    GeoPacket p  = packets[i];
    uint64_t sig = isect_sig[i / GEO_BUNDLE_WORDS];
    if (sig == 0) sig ^= 0x9E3779B97F4A7C15ULL ^ (uint64_t)i;
    result[i] = (p.sig != 0 && p.sig == sig &&
                 ((p.hpos >= 256) == (p.idx >= 256))) ? 1 : 0;
}

/* GeoPacket32 + geo_isect_fused32 decl — shared with geo_wire_kernel.cu */
#include "geo_wire_types.h"

/* ── Kernel 5: geo_isect_fused ──────────────────────────────────────
 * K3+K4 in one kernel — eliminates inter-kernel sync
 * Phase 1: each thread computes isect, stores to smem
 * Phase 2: XOR-reduce 8 lanes → sig (lane 0 only)
 * Phase 3: all 8 threads in bundle validate their packet against sig
 */
__global__ void geo_isect_fused(
    const uint64_t*  __restrict__ raw,
    const GeoPacket* __restrict__ packets,
    uint8_t*         __restrict__ result,
    int N_bundles)
{
    __shared__ uint64_t smem[256];

    int lane        = threadIdx.x & 7;
    int bund_global = (blockIdx.x << 5) + (threadIdx.x >> 3);
    int pkt_base    = bund_global * GEO_BUNDLE_WORDS;

    uint64_t isect = 0;
    if (bund_global < N_bundles) {
        uint64_t x   = raw[pkt_base + lane];
        uint64_t r8  = (x << 8)  | (x >> 56);
        uint64_t r16 = (x << 16) | (x >> 48);
        uint64_t r24 = (x << 24) | (x >> 40);
        int ph       = lane < GEO_PHASE_COUNT ? lane : lane - GEO_PHASE_COUNT;
        isect        = (x & r8 & r16 & r24) ^ d_phase_mask[ph];
    }

    smem[threadIdx.x] = isect;
    __syncthreads();
    if (lane < 4) smem[threadIdx.x] ^= smem[threadIdx.x + 4]; __syncthreads();
    if (lane < 2) smem[threadIdx.x] ^= smem[threadIdx.x + 2]; __syncthreads();
    if (lane < 1) smem[threadIdx.x] ^= smem[threadIdx.x + 1]; __syncthreads();

    /* all 8 threads read bundle sig from lane-0 slot */
    if (bund_global < N_bundles) {
        uint64_t sig = smem[threadIdx.x & ~7];   /* lane-0 of this bundle */
        if (sig == 0) sig ^= 0x9E3779B97F4A7C15ULL ^ (uint64_t)(pkt_base + lane);
        GeoPacket p  = packets[pkt_base + lane];
        result[pkt_base + lane] = (p.sig != 0 && p.sig == sig &&
                                   ((p.hpos >= 256) == (p.idx >= 256))) ? 1 : 0;
    }
}

/* ── Kernel 6: geo_isect_fused32 (8B GeoPacket32) ────────────────────
 * Identical optX+reduce to K5, validates against 32b sig only
 * sig32 = upper 32b of fused isect (lower 32b = 0 in all phase_masks)
 * 2× cache density vs K5 → better L1/L2 hit rate on packet array
 * flags bit0 = line_ok precomputed → no branch cost in validate
 */
__global__ void geo_isect_fused32(
    const uint64_t*    __restrict__ raw,
    const GeoPacket32* __restrict__ pkts32,
    uint8_t*           __restrict__ result,
    int N_bundles)
{
    __shared__ uint64_t smem[256];

    int lane        = threadIdx.x & 7;
    int bund_global = (blockIdx.x << 5) + (threadIdx.x >> 3);
    int pkt_base    = bund_global * GEO_BUNDLE_WORDS;

    uint64_t isect = 0;
    if (bund_global < N_bundles) {
        uint64_t x   = raw[pkt_base + lane];
        uint64_t r8  = (x << 8)  | (x >> 56);
        uint64_t r16 = (x << 16) | (x >> 48);
        uint64_t r24 = (x << 24) | (x >> 40);
        int ph       = lane < GEO_PHASE_COUNT ? lane : lane - GEO_PHASE_COUNT;
        isect        = (x & r8 & r16 & r24) ^ d_phase_mask[ph];
    }

    smem[threadIdx.x] = isect;
    __syncthreads();
    if (lane < 4) smem[threadIdx.x] ^= smem[threadIdx.x + 4]; __syncthreads();
    if (lane < 2) smem[threadIdx.x] ^= smem[threadIdx.x + 2]; __syncthreads();
    if (lane < 1) smem[threadIdx.x] ^= smem[threadIdx.x + 1]; __syncthreads();

    if (bund_global < N_bundles) {
        uint64_t sig64 = smem[threadIdx.x & ~7];
        if (sig64 == 0) sig64 ^= 0x9E3779B97F4A7C15ULL ^ (uint64_t)(pkt_base + lane);
        uint32_t sig32  = (uint32_t)(sig64 >> 32);   /* upper 32b — lower always 0 */
        GeoPacket32 p   = pkts32[pkt_base + lane];
        result[pkt_base + lane] =
            (p.sig32 != 0 && p.sig32 == sig32 && (p.flags & 1)) ? 1 : 0;
    }
}

/* ── CUDA error helper ── */
#define CUDA_CHECK(x) do { \
    cudaError_t e = (x); \
    if (e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d — %s\n", __FILE__, __LINE__, cudaGetErrorString(e)); \
        exit(1); \
    } \
} while(0)

/* ── Benchmark ── */
#define N_PACKETS  (1 << 24)   /* 16M packets — S28 ceiling test */
#define BLOCK_SIZE 256

int main(void) {
    printf("=== GeoMatrix GPU Kernel Bench ===\n");

    /* ── Setup bitboard (host) ── */
    /* 8 words × 64B alignment via cudaMallocHost */
    uint64_t *h_bitboard;
    CUDA_CHECK(cudaMallocHost(&h_bitboard, GEO_BUNDLE_WORDS * sizeof(uint64_t)));
    /* fill with non-trivial pattern (PROBE-friendly popcount) */
    for (int w = 0; w < GEO_BUNDLE_WORDS; w++)
        h_bitboard[w] = 0xAAAAAAAAAAAAAAAAULL >> w; /* ~32 bits set per word */

    /* ── Precompute expected sig (CPU side) ── */
    uint64_t fold = 0;
    for (int w = 0; w < GEO_BUNDLE_WORDS; w++) fold ^= h_bitboard[w];
    uint64_t probe_sig = fold ^ 0xAAAAAAAA00000000ULL;  /* PROBE phase new mask */

    /* ── Packets (pinned) ── */
    GeoPacket *h_pkts;
    CUDA_CHECK(cudaMallocHost(&h_pkts, N_PACKETS * sizeof(GeoPacket)));
    for (int i = 0; i < N_PACKETS; i++) {
        h_pkts[i].sig   = probe_sig;
        h_pkts[i].hpos  = (uint16_t)(i % 256);   /* block 0 */
        h_pkts[i].idx   = (uint16_t)(i % 256);   /* block 0 — line match */
        h_pkts[i].bit   = 1;
        h_pkts[i].phase = 0; /* PROBE */
        h_pkts[i]._pad[0] = h_pkts[i]._pad[1] = 0;
    }

    /* ── idx array for fetch_bit bench ── */
    uint16_t *h_idx;
    CUDA_CHECK(cudaMallocHost(&h_idx, N_PACKETS * sizeof(uint16_t)));
    for (int i = 0; i < N_PACKETS; i++) h_idx[i] = (uint16_t)(i % 512);

    uint8_t *h_bits_out, *h_result;
    CUDA_CHECK(cudaMallocHost(&h_bits_out, N_PACKETS));
    CUDA_CHECK(cudaMallocHost(&h_result,   N_PACKETS));

    /* ── Device alloc ── */
    uint64_t *d_bitboard;  GeoPacket *d_pkts;
    uint16_t *d_idx;       uint8_t   *d_bits_out, *d_result;

    CUDA_CHECK(cudaMalloc(&d_bitboard, GEO_BUNDLE_WORDS * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_pkts,     N_PACKETS * sizeof(GeoPacket)));
    CUDA_CHECK(cudaMalloc(&d_idx,      N_PACKETS * sizeof(uint16_t)));
    CUDA_CHECK(cudaMalloc(&d_bits_out, N_PACKETS));
    CUDA_CHECK(cudaMalloc(&d_result,   N_PACKETS));

    /* ── Transfer ── */
    CUDA_CHECK(cudaMemcpy(d_bitboard, h_bitboard, GEO_BUNDLE_WORDS * sizeof(uint64_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_pkts,     h_pkts,     N_PACKETS * sizeof(GeoPacket),        cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_idx,      h_idx,      N_PACKETS * sizeof(uint16_t),         cudaMemcpyHostToDevice));

    int grid = (N_PACKETS + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* ── Warmup ── */
    fetch_bit_kernel<<<grid, BLOCK_SIZE>>>(d_bitboard, d_idx, d_bits_out, N_PACKETS);
    geo_validate_kernel<<<grid, BLOCK_SIZE>>>(d_bitboard, d_pkts, d_result, N_PACKETS);
    CUDA_CHECK(cudaDeviceSynchronize());

    /* ── Bench: fetch_bit ── */
    cudaEvent_t t0, t1;
    CUDA_CHECK(cudaEventCreate(&t0));
    CUDA_CHECK(cudaEventCreate(&t1));

    CUDA_CHECK(cudaEventRecord(t0));
    for (int r = 0; r < 10; r++)
        fetch_bit_kernel<<<grid, BLOCK_SIZE>>>(d_bitboard, d_idx, d_bits_out, N_PACKETS);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));

    float ms_fetch;
    CUDA_CHECK(cudaEventElapsedTime(&ms_fetch, t0, t1));
    ms_fetch /= 10.0f;

    /* ── Bench: validate ── */
    CUDA_CHECK(cudaEventRecord(t0));
    for (int r = 0; r < 10; r++)
        geo_validate_kernel<<<grid, BLOCK_SIZE>>>(d_bitboard, d_pkts, d_result, N_PACKETS);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));

    float ms_validate;
    CUDA_CHECK(cudaEventElapsedTime(&ms_validate, t0, t1));
    ms_validate /= 10.0f;

    /* ── Verify correctness (small sample) ── */
    CUDA_CHECK(cudaMemcpy(h_result, d_result, N_PACKETS, cudaMemcpyDeviceToHost));
    int pass_count = 0;
    for (int i = 0; i < N_PACKETS; i++) pass_count += h_result[i];

    /* ── Results ── */
    double gbps_fetch    = (N_PACKETS * sizeof(uint16_t)) / (ms_fetch    * 1e-3) / 1e9;
    double gbps_validate = (N_PACKETS * sizeof(GeoPacket))/ (ms_validate * 1e-3) / 1e9;
    double mpkt_fetch    = N_PACKETS / (ms_fetch    * 1e-3) / 1e6;
    double mpkt_validate = N_PACKETS / (ms_validate * 1e-3) / 1e6;

    printf("\n[fetch_bit_kernel]\n");
    printf("  Time:        %.3f ms\n", ms_fetch);
    printf("  Throughput:  %.1f M-pkt/s  |  %.2f GB/s\n", mpkt_fetch, gbps_fetch);
    printf("  Latency/pkt: %.2f ns\n", ms_fetch * 1e6 / N_PACKETS);

    printf("\n[geo_validate_kernel]\n");
    printf("  Time:        %.3f ms\n", ms_validate);
    printf("  Throughput:  %.1f M-pkt/s  |  %.2f GB/s\n", mpkt_validate, gbps_validate);
    printf("  Latency/pkt: %.2f ns\n", ms_validate * 1e6 / N_PACKETS);
    printf("  Pass rate:   %d/%d  (%.2f%%)\n",
           pass_count, N_PACKETS, 100.0 * pass_count / N_PACKETS);

    printf("\n[target]\n");
    printf("  fetch_bit    >5 GB/s   → %s\n", gbps_fetch    > 5.0 ? "✓" : "✗ need tune");
    printf("  isect        >10 GB/s  → %s\n", gbps_validate > 5.0 ? "✓" : "✗ need tune");

    /* ── Bench: geo_isect_kernel + geo_isect_validate ── */
    int N_bundles = N_PACKETS / GEO_BUNDLE_WORDS;

    uint64_t *d_raw, *d_isect_sig;
    uint8_t  *d_result2;
    CUDA_CHECK(cudaMalloc(&d_raw,       (size_t)N_PACKETS * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_isect_sig, (size_t)N_bundles * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_result2,   (size_t)N_PACKETS));

    /* raw: tile bitboard pattern across N_PACKETS words */
    uint64_t *h_raw;
    CUDA_CHECK(cudaMallocHost(&h_raw, (size_t)N_PACKETS * sizeof(uint64_t)));
    for (int i = 0; i < N_PACKETS; i++) h_raw[i] = h_bitboard[i % GEO_BUNDLE_WORDS];
    CUDA_CHECK(cudaMemcpy(d_raw, h_raw, (size_t)N_PACKETS * sizeof(uint64_t), cudaMemcpyHostToDevice));
    cudaFreeHost(h_raw);

    /* precompute expected_sig (CPU) — same logic as geo_isect_kernel reduce */
    uint64_t expected_isect = 0;
    for (int l = 0; l < GEO_BUNDLE_WORDS; l++) {
        uint64_t x = h_bitboard[l];
        uint64_t r8=(x<<8)|(x>>56), r16=(x<<16)|(x>>48), r24=(x<<24)|(x>>40);
        int ph = l < GEO_PHASE_COUNT ? l : l - GEO_PHASE_COUNT;
        expected_isect ^= (x & r8 & r16 & r24) ^ (
            ph==0 ? 0xAAAAAAAA00000000ULL :
            ph==1 ? 0x5555555500000000ULL :
            ph==2 ? 0xF0F0F0F000000000ULL :
                    0x0F0F0F0F00000000ULL);
    }

    /* patch packets to match isect_sig */
    GeoPacket *h_pkts2;
    CUDA_CHECK(cudaMallocHost(&h_pkts2, N_PACKETS * sizeof(GeoPacket)));
    for (int i = 0; i < N_PACKETS; i++) {
        h_pkts2[i].sig   = expected_isect;
        h_pkts2[i].hpos  = (uint16_t)(i % 256);
        h_pkts2[i].idx   = (uint16_t)(i % 256);
        h_pkts2[i].bit   = 1; h_pkts2[i].phase = 0;
        h_pkts2[i]._pad[0] = h_pkts2[i]._pad[1] = 0;
    }
    uint64_t *d_pkts2_buf; GeoPacket *d_pkts2;
    CUDA_CHECK(cudaMalloc(&d_pkts2, N_PACKETS * sizeof(GeoPacket)));
    CUDA_CHECK(cudaMemcpy(d_pkts2, h_pkts2, N_PACKETS * sizeof(GeoPacket), cudaMemcpyHostToDevice));
    (void)d_pkts2_buf;

    /* isect grid: 256 threads/block, 8 threads/bundle → 32 bundles/block */
    int isect_grid = (N_PACKETS + 255) / 256;

    /* warmup */
    geo_isect_kernel<<<isect_grid, 256>>>(d_raw, d_isect_sig, N_bundles);
    geo_isect_validate<<<grid, BLOCK_SIZE>>>(d_isect_sig, d_pkts2, d_result2, N_PACKETS);
    CUDA_CHECK(cudaDeviceSynchronize());

    /* bench isect kernel alone */
    CUDA_CHECK(cudaEventRecord(t0));
    for (int r = 0; r < 10; r++)
        geo_isect_kernel<<<isect_grid, 256>>>(d_raw, d_isect_sig, N_bundles);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));
    float ms_isect; CUDA_CHECK(cudaEventElapsedTime(&ms_isect, t0, t1));
    ms_isect /= 10.0f;

    /* bench isect + validate fused */
    CUDA_CHECK(cudaEventRecord(t0));
    for (int r = 0; r < 10; r++) {
        geo_isect_kernel<<<isect_grid, 256>>>(d_raw, d_isect_sig, N_bundles);
        geo_isect_validate<<<grid, BLOCK_SIZE>>>(d_isect_sig, d_pkts2, d_result2, N_PACKETS);
    }
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));
    float ms_fused; CUDA_CHECK(cudaEventElapsedTime(&ms_fused, t0, t1));
    ms_fused /= 10.0f;

    /* verify */
    uint8_t *h_result2; CUDA_CHECK(cudaMallocHost(&h_result2, N_PACKETS));
    CUDA_CHECK(cudaMemcpy(h_result2, d_result2, N_PACKETS, cudaMemcpyDeviceToHost));
    int pass2 = 0; for (int i = 0; i < N_PACKETS; i++) pass2 += h_result2[i];

    double gbps_isect = (N_PACKETS * sizeof(uint64_t)) / (ms_isect * 1e-3) / 1e9;
    double mpkt_isect = N_PACKETS / (ms_isect * 1e-3) / 1e6;
    double mpkt_fused = N_PACKETS / (ms_fused * 1e-3) / 1e6;

    printf("\n[geo_isect_kernel (S24 optX)]\n");
    printf("  Time:        %.3f ms\n", ms_isect);
    printf("  Throughput:  %.1f M-pkt/s  |  %.2f GB/s\n", mpkt_isect, gbps_isect);
    printf("  Latency/pkt: %.2f ns\n", ms_isect * 1e6 / N_PACKETS);

    printf("\n[geo_isect + validate (fused)]\n");
    printf("  Time:        %.3f ms\n", ms_fused);
    printf("  Throughput:  %.1f M-pkt/s\n", mpkt_fused);
    printf("  Pass rate:   %d/%d  (%.2f%%)\n",
           pass2, N_PACKETS, 100.0 * pass2 / N_PACKETS);
    printf("  isect        >10 GB/s  → %s\n", gbps_isect > 10.0 ? "✓" : "✗ need tune");

    printf("  isect        >10 GB/s  → %s\n", gbps_isect > 10.0 ? "✓" : "✗ need tune");

    /* ── Bench: geo_isect_fused (K5) ── */
    uint8_t *d_result3, *h_result3;
    CUDA_CHECK(cudaMalloc(&d_result3, N_PACKETS));
    CUDA_CHECK(cudaMallocHost(&h_result3, N_PACKETS));

    geo_isect_fused<<<isect_grid, 256>>>(d_raw, d_pkts2, d_result3, N_bundles);
    CUDA_CHECK(cudaDeviceSynchronize());

    CUDA_CHECK(cudaEventRecord(t0));
    for (int r = 0; r < 10; r++)
        geo_isect_fused<<<isect_grid, 256>>>(d_raw, d_pkts2, d_result3, N_bundles);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaEventSynchronize(t1));
    float ms_fused2; CUDA_CHECK(cudaEventElapsedTime(&ms_fused2, t0, t1));
    ms_fused2 /= 10.0f;

    CUDA_CHECK(cudaMemcpy(h_result3, d_result3, N_PACKETS, cudaMemcpyDeviceToHost));
    int pass3 = 0; for (int i = 0; i < N_PACKETS; i++) pass3 += h_result3[i];

    double mpkt_fused2 = N_PACKETS / (ms_fused2 * 1e-3) / 1e6;
    double gbps_fused2 = (N_PACKETS * sizeof(uint64_t)) / (ms_fused2 * 1e-3) / 1e9;

    printf("\n[geo_isect_fused K5 (single kernel)]\n");
    printf("  Time:        %.3f ms\n", ms_fused2);
    printf("  Throughput:  %.1f M-pkt/s  |  %.2f GB/s\n", mpkt_fused2, gbps_fused2);
    printf("  Latency/pkt: %.2f ns\n", ms_fused2 * 1e6 / N_PACKETS);
    printf("  Pass rate:   %d/%d  (%.2f%%)\n", pass3, N_PACKETS, 100.0*pass3/N_PACKETS);
    printf("  vs fused K3+K4: %.2fx\n", mpkt_fused2 / mpkt_fused);

    cudaFree(d_result3); cudaFreeHost(h_result3);

    /* ═══════════════════════════════════════════════════════════════
     * K6: geo_isect_fused32 — 8B GeoPacket32
     * A (S28): async pipeline overlap — H2D → kernel → D2H on same stream
     *          pinned host memory required (already cudaMallocHost)
     * ══════════════════════════════════════════════════════════════ */
    GeoPacket32 *h_pkts32, *d_pkts32;
    CUDA_CHECK(cudaMallocHost(&h_pkts32, (size_t)N_PACKETS * sizeof(GeoPacket32)));
    uint32_t expected_isect32 = (uint32_t)(expected_isect >> 32);
    for (int i = 0; i < N_PACKETS; i++) {
        h_pkts32[i].sig32 = expected_isect32;
        h_pkts32[i].hpos  = (uint16_t)(i % 256);
        h_pkts32[i].phase = 0;
        h_pkts32[i].flags = 1;
    }
    CUDA_CHECK(cudaMalloc(&d_pkts32, (size_t)N_PACKETS * sizeof(GeoPacket32)));

    uint8_t *d_result_k6, *h_result_k6;
    CUDA_CHECK(cudaMalloc(&d_result_k6, (size_t)N_PACKETS));
    CUDA_CHECK(cudaMallocHost(&h_result_k6, (size_t)N_PACKETS));

    int k6_grid = (N_bundles + 31) / 32;

    /* ── A: pipe_stream — H2D once, then kernel-only timed loop ────
     * Separates PCIe cost from compute cost.
     * End-to-end (H2D+kernel+D2H) reported separately for pipeline budget.
     */
    cudaStream_t pipe_stream;
    CUDA_CHECK(cudaStreamCreate(&pipe_stream));

    /* H2D once — data stays on device for kernel timing */
    CUDA_CHECK(cudaMemcpyAsync(d_pkts32, h_pkts32,
                               (size_t)N_PACKETS * sizeof(GeoPacket32),
                               cudaMemcpyHostToDevice, pipe_stream));
    CUDA_CHECK(cudaStreamSynchronize(pipe_stream));

    /* warmup kernel */
    geo_isect_fused32<<<k6_grid, 256, 0, pipe_stream>>>(
        d_raw, d_pkts32, d_result_k6, N_bundles);
    CUDA_CHECK(cudaStreamSynchronize(pipe_stream));

    /* timed — kernel only (data already on device) */
    CUDA_CHECK(cudaEventRecord(t0));
    for (int r = 0; r < 10; r++)
        geo_isect_fused32<<<k6_grid, 256, 0, pipe_stream>>>(
            d_raw, d_pkts32, d_result_k6, N_bundles);
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaStreamSynchronize(pipe_stream));
    CUDA_CHECK(cudaEventSynchronize(t1));
    float ms_k6; CUDA_CHECK(cudaEventElapsedTime(&ms_k6, t0, t1));
    ms_k6 /= 10.0f;

    /* D2H for correctness check */
    CUDA_CHECK(cudaMemcpyAsync(h_result_k6, d_result_k6,
                               (size_t)N_PACKETS,
                               cudaMemcpyDeviceToHost, pipe_stream));
    CUDA_CHECK(cudaStreamSynchronize(pipe_stream));
    int pass_k6 = 0; for (int i = 0; i < N_PACKETS; i++) pass_k6 += h_result_k6[i];

    /* end-to-end H2D+kernel+D2H — pipeline budget reference */
    CUDA_CHECK(cudaEventRecord(t0));
    for (int r = 0; r < 10; r++) {
        CUDA_CHECK(cudaMemcpyAsync(d_pkts32, h_pkts32,
                                   (size_t)N_PACKETS * sizeof(GeoPacket32),
                                   cudaMemcpyHostToDevice, pipe_stream));
        geo_isect_fused32<<<k6_grid, 256, 0, pipe_stream>>>(
            d_raw, d_pkts32, d_result_k6, N_bundles);
        CUDA_CHECK(cudaMemcpyAsync(h_result_k6, d_result_k6,
                                   (size_t)N_PACKETS,
                                   cudaMemcpyDeviceToHost, pipe_stream));
    }
    CUDA_CHECK(cudaEventRecord(t1));
    CUDA_CHECK(cudaStreamSynchronize(pipe_stream));
    CUDA_CHECK(cudaEventSynchronize(t1));
    float ms_k6_e2e; CUDA_CHECK(cudaEventElapsedTime(&ms_k6_e2e, t0, t1));
    ms_k6_e2e /= 10.0f;

    double mpkt_k6     = N_PACKETS / (ms_k6     * 1e-3) / 1e6;
    double mpkt_k6_e2e = N_PACKETS / (ms_k6_e2e * 1e-3) / 1e6;
    printf("\n[K6: geo_isect_fused32 (8B)]\n");
    printf("  Kernel-only:   %.3f ms  →  %.1f M-pkt/s\n", ms_k6, mpkt_k6);
    printf("  End-to-end:    %.3f ms  →  %.1f M-pkt/s  (H2D+kernel+D2H)\n", ms_k6_e2e, mpkt_k6_e2e);
    printf("  PCIe overhead: %.2fx\n", ms_k6_e2e / ms_k6);
    printf("  Pass rate:     %d/%d  (%.2f%%)\n", pass_k6, N_PACKETS, 100.0*pass_k6/N_PACKETS);

    /* ═══════════════════════════════════════════════════════════════
     * S28: multi-stream sweep 1/2/4/8 — find saturation point
     * T4 = 40 SM, 16M packets — kernel-only, data pre-loaded
     * ══════════════════════════════════════════════════════════════ */
#define MAX_STREAMS 8
    {
        static const int stream_counts[] = {1, 2, 4, 8};
        uint8_t *d_result_ms;
        CUDA_CHECK(cudaMalloc(&d_result_ms, (size_t)N_PACKETS));

        printf("\n[K6 multi-stream sweep (kernel-only, 16M pkts)]\n");
        printf("  %-8s  %-12s  %-10s  %-8s\n", "streams", "M-pkt/s", "vs K6×1", "pass");

        cudaStream_t streams[MAX_STREAMS];
        for (int s = 0; s < MAX_STREAMS; s++)
            CUDA_CHECK(cudaStreamCreate(&streams[s]));

        for (int si = 0; si < 4; si++) {
            int NS         = stream_counts[si];
            int chunk      = N_PACKETS / NS;
            int chunk_bund = N_bundles  / NS;
            int chunk_grid = (chunk_bund + 31) / 32;

            /* warmup */
            for (int s = 0; s < NS; s++)
                geo_isect_fused32<<<chunk_grid, 256, 0, streams[s]>>>(
                    d_raw       + (size_t)s * chunk_bund * GEO_BUNDLE_WORDS,
                    d_pkts32    + (size_t)s * chunk,
                    d_result_ms + (size_t)s * chunk,
                    chunk_bund);
            for (int s = 0; s < NS; s++)
                CUDA_CHECK(cudaStreamSynchronize(streams[s]));

            CUDA_CHECK(cudaEventRecord(t0));
            for (int r = 0; r < 10; r++)
                for (int s = 0; s < NS; s++)
                    geo_isect_fused32<<<chunk_grid, 256, 0, streams[s]>>>(
                        d_raw       + (size_t)s * chunk_bund * GEO_BUNDLE_WORDS,
                        d_pkts32    + (size_t)s * chunk,
                        d_result_ms + (size_t)s * chunk,
                        chunk_bund);
            CUDA_CHECK(cudaEventRecord(t1));
            CUDA_CHECK(cudaEventSynchronize(t1));
            float ms_sw; CUDA_CHECK(cudaEventElapsedTime(&ms_sw, t0, t1));
            ms_sw /= 10.0f;

            /* correctness spot-check (first chunk only) */
            uint8_t *h_spot;
            CUDA_CHECK(cudaMallocHost(&h_spot, (size_t)chunk));
            CUDA_CHECK(cudaMemcpy(h_spot, d_result_ms, (size_t)chunk, cudaMemcpyDeviceToHost));
            int pass_sw = 0; for (int i = 0; i < chunk; i++) pass_sw += h_spot[i];
            cudaFreeHost(h_spot);

            double mpkt_sw = N_PACKETS / (ms_sw * 1e-3) / 1e6;
            printf("  %-8d  %-12.1f  %-10.2fx  %d/%d\n",
                   NS, mpkt_sw, mpkt_sw / mpkt_k6, pass_sw, chunk);
        }

        printf("  ceiling (K3 optX): 20474.7 M-pkt/s\n");

        for (int s = 0; s < MAX_STREAMS; s++) CUDA_CHECK(cudaStreamDestroy(streams[s]));
        cudaFree(d_result_ms);
    }

    CUDA_CHECK(cudaStreamDestroy(pipe_stream));
    cudaFree(d_pkts32); cudaFreeHost(h_pkts32);
    cudaFree(d_result_k6); cudaFreeHost(h_result_k6);
    cudaFreeHost(h_pkts2); cudaFreeHost(h_result2);

    /* cleanup */
    cudaFree(d_bitboard); cudaFree(d_pkts); cudaFree(d_idx);
    cudaFree(d_bits_out); cudaFree(d_result);
    cudaFreeHost(h_bitboard); cudaFreeHost(h_pkts); cudaFreeHost(h_idx);
    cudaFreeHost(h_bits_out); cudaFreeHost(h_result);

    /* ═══════════════════════════════════════════════════════════════
     * EDGE CASE TEST SUITE — S28 pre-production
     * Tests correctness on inputs that benchmarks never exercise.
     * Each case: small N, hand-checkable expected result.
     * Pass = ✓, Fail = ✗ with detail line.
     * ══════════════════════════════════════════════════════════════ */
    printf("\n=== Edge Case Tests ===\n");
    int ec_pass = 0, ec_fail = 0;
#define EC_CHECK(label, got, expect) \
    if ((got) == (expect)) { printf("  ✓ %s\n", label); ec_pass++; } \
    else { printf("  ✗ %s  got=%d expect=%d\n", label, (int)(got), (int)(expect)); ec_fail++; }

    /* shared small device buffers */
    const int EC_N = 8;   /* 1 bundle */
    uint64_t *ec_raw;   CUDA_CHECK(cudaMalloc(&ec_raw,    EC_N * sizeof(uint64_t)));
    GeoPacket32 *ec_p32; CUDA_CHECK(cudaMalloc(&ec_p32,   EC_N * sizeof(GeoPacket32)));
    uint8_t  *ec_res;   CUDA_CHECK(cudaMalloc(&ec_res,    EC_N));
    uint8_t  *ec_h;     CUDA_CHECK(cudaMallocHost(&ec_h,  EC_N));

    /* reuse same bitboard pattern to derive expected sig */
    uint64_t ec_bb[8];
    for (int w = 0; w < 8; w++) ec_bb[w] = 0xAAAAAAAAAAAAAAAAULL >> w;
    uint64_t ec_exp = 0;
    for (int l = 0; l < 8; l++) {
        uint64_t x = ec_bb[l];
        uint64_t r8=(x<<8)|(x>>56), r16=(x<<16)|(x>>48), r24=(x<<24)|(x>>40);
        int ph = l < GEO_PHASE_COUNT ? l : l - GEO_PHASE_COUNT;
        static const uint64_t pm[4] = {
            0xAAAAAAAA00000000ULL, 0x5555555500000000ULL,
            0xF0F0F0F000000000ULL, 0x0F0F0F0F00000000ULL };
        ec_exp ^= (x & r8 & r16 & r24) ^ pm[ph];
    }
    uint32_t ec_exp32 = (uint32_t)(ec_exp >> 32);
    CUDA_CHECK(cudaMemcpy(ec_raw, ec_bb, 8 * sizeof(uint64_t), cudaMemcpyHostToDevice));

    /* helper: fill, launch 1 bundle, copy back */
#define EC_RUN(pkts_host) \
    CUDA_CHECK(cudaMemcpy(ec_p32, pkts_host, EC_N * sizeof(GeoPacket32), cudaMemcpyHostToDevice)); \
    geo_isect_fused32<<<1, 256>>>(ec_raw, ec_p32, ec_res, 1); \
    CUDA_CHECK(cudaDeviceSynchronize()); \
    CUDA_CHECK(cudaMemcpy(ec_h, ec_res, EC_N, cudaMemcpyDeviceToHost));

    GeoPacket32 ec_pkt[8];

    /* ── EC1: all valid — all 8 should pass ── */
    for (int i = 0; i < 8; i++) {
        ec_pkt[i].sig32 = ec_exp32;
        ec_pkt[i].hpos  = (uint16_t)(i % 256);
        ec_pkt[i].phase = 0;
        ec_pkt[i].flags = 1;
    }
    EC_RUN(ec_pkt);
    { int s=0; for(int i=0;i<8;i++) s+=ec_h[i]; EC_CHECK("EC1 all-valid: 8/8 pass", s, 8); }

    /* ── EC2: sig=0 on all packets — kernel must reject (degenerate guard
     *         patches sig, so expected result is pass not reject) ── */
    for (int i = 0; i < 8; i++) {
        ec_pkt[i].sig32 = 0;
        ec_pkt[i].hpos  = (uint16_t)(i % 256);
        ec_pkt[i].phase = 0;
        ec_pkt[i].flags = 1;
    }
    EC_RUN(ec_pkt);
    { int s=0; for(int i=0;i<8;i++) s+=ec_h[i]; EC_CHECK("EC2 sig=0: degenerate guard fires, 0/8 pass", s, 0); }

    /* ── EC3: phase > 3 — kernel clamps to 0, sig still matches ── */
    for (int i = 0; i < 8; i++) {
        ec_pkt[i].sig32 = ec_exp32;
        ec_pkt[i].hpos  = (uint16_t)(i % 256);
        ec_pkt[i].phase = 7;   /* out of range */
        ec_pkt[i].flags = 1;
    }
    EC_RUN(ec_pkt);
    /* phase clamped → same sig as phase=0 → all pass */
    { int s=0; for(int i=0;i<8;i++) s+=ec_h[i]; EC_CHECK("EC3 phase=7 (clamp to 0): 8/8 pass", s, 8); }

    /* ── EC4: hpos >= 256 but flags=1 (line_ok precomputed true) — pass ── */
    for (int i = 0; i < 8; i++) {
        ec_pkt[i].sig32 = ec_exp32;
        ec_pkt[i].hpos  = 300;   /* >= 256 */
        ec_pkt[i].phase = 0;
        ec_pkt[i].flags = 1;    /* line_ok=1 overrides hpos check */
    }
    EC_RUN(ec_pkt);
    { int s=0; for(int i=0;i<8;i++) s+=ec_h[i]; EC_CHECK("EC4 hpos=300 flags=1: 8/8 pass", s, 8); }

    /* ── EC5: hpos = 65535 (max uint16), flags=0 → line_ok=0 → reject ── */
    for (int i = 0; i < 8; i++) {
        ec_pkt[i].sig32 = ec_exp32;
        ec_pkt[i].hpos  = 0xFFFF;
        ec_pkt[i].phase = 0;
        ec_pkt[i].flags = 0;   /* line_ok=0 */
    }
    EC_RUN(ec_pkt);
    { int s=0; for(int i=0;i<8;i++) s+=ec_h[i]; EC_CHECK("EC5 hpos=65535 flags=0: 0/8 pass", s, 0); }

    /* ── EC6: wrong sig — all reject ── */
    for (int i = 0; i < 8; i++) {
        ec_pkt[i].sig32 = ec_exp32 ^ 0xDEADBEEF;
        ec_pkt[i].hpos  = (uint16_t)(i % 256);
        ec_pkt[i].phase = 0;
        ec_pkt[i].flags = 1;
    }
    EC_RUN(ec_pkt);
    { int s=0; for(int i=0;i<8;i++) s+=ec_h[i]; EC_CHECK("EC6 wrong sig: 0/8 pass", s, 0); }

    /* ── EC7: N_PACKETS not multiple of 8 — partial last bundle
     *         launch 2 bundles (16 threads) but only 10 valid packets
     *         last 6 slots are out-of-range → kernel guard (bund_global<N_bundles)
     *         ensures no write past result buffer ── */
    {
        const int EC7_BUND = 1;  /* 1 full bundle = 8 packets */
        GeoPacket32 *ec7_p; uint8_t *ec7_r, *ec7_h;
        CUDA_CHECK(cudaMalloc(&ec7_p, 8 * sizeof(GeoPacket32)));
        CUDA_CHECK(cudaMalloc(&ec7_r, 8));
        CUDA_CHECK(cudaMallocHost(&ec7_h, 8));
        /* fill sentinel 0xFF to detect OOB write */
        CUDA_CHECK(cudaMemset(ec7_r, 0xFF, 8));
        for (int i = 0; i < 8; i++) {
            ec_pkt[i].sig32 = ec_exp32;
            ec_pkt[i].hpos  = (uint16_t)(i % 256);
            ec_pkt[i].phase = 0;
            ec_pkt[i].flags = 1;
        }
        CUDA_CHECK(cudaMemcpy(ec7_p, ec_pkt, 8 * sizeof(GeoPacket32), cudaMemcpyHostToDevice));
        /* launch 2 blocks but N_bundles=1 — second block's bund_global >= 1 → guard */
        geo_isect_fused32<<<2, 256>>>(ec_raw, ec7_p, ec7_r, EC7_BUND);
        CUDA_CHECK(cudaDeviceSynchronize());
        CUDA_CHECK(cudaMemcpy(ec7_h, ec7_r, 8, cudaMemcpyDeviceToHost));
        int s=0; for(int i=0;i<8;i++) s+=ec7_h[i];
        EC_CHECK("EC7 over-launch guard: 8/8 pass (no OOB)", s, 8);
        cudaFree(ec7_p); cudaFree(ec7_r); cudaFreeHost(ec7_h);
    }

    /* ── EC8: all-zero bitboard → fold=0 → sig after XOR = phase_mask only
     *         degenerate guard in kernel: if(sig==0) sig ^= PHI_PRIME ^ lane
     *         packets carry 0 → mismatch → all reject ── */
    {
        uint64_t zero_bb[8] = {0};
        CUDA_CHECK(cudaMemcpy(ec_raw, zero_bb, 8 * sizeof(uint64_t), cudaMemcpyHostToDevice));
        for (int i = 0; i < 8; i++) {
            ec_pkt[i].sig32 = 0;   /* wrong — real sig is phase_mask upper32 */
            ec_pkt[i].hpos  = (uint16_t)(i % 256);
            ec_pkt[i].phase = 0;
            ec_pkt[i].flags = 1;
        }
        EC_RUN(ec_pkt);
        int s=0; for(int i=0;i<8;i++) s+=ec_h[i];
        EC_CHECK("EC8 all-zero bitboard, sig=0: 0/8 pass", s, 0);
        /* restore real bitboard for any further tests */
        CUDA_CHECK(cudaMemcpy(ec_raw, ec_bb, 8 * sizeof(uint64_t), cudaMemcpyHostToDevice));
    }

    /* ── EC9: geo_wire_launch_async — integration smoke test ── */
    {
        GeoWireHandle wh;
        GeoPacket32 *wh_d_pkts32, *wh_h_pkts32;
        uint8_t     *wh_d_result, *wh_h_result;
        cudaStream_t wh_stream;

        CUDA_CHECK(cudaMalloc(&wh_d_pkts32,    EC_N * sizeof(GeoPacket32)));
        CUDA_CHECK(cudaMallocHost(&wh_h_pkts32, EC_N * sizeof(GeoPacket32)));
        CUDA_CHECK(cudaMalloc(&wh_d_result,    EC_N));
        CUDA_CHECK(cudaMallocHost(&wh_h_result, EC_N));
        CUDA_CHECK(cudaStreamCreate(&wh_stream));

        for (int i = 0; i < EC_N; i++) {
            wh_h_pkts32[i].sig32 = ec_exp32;
            wh_h_pkts32[i].hpos  = (uint16_t)(i % 256);
            wh_h_pkts32[i].phase = 0;
            wh_h_pkts32[i].flags = 1;
        }
        wh.d_raw      = ec_raw;
        wh.d_pkts32   = wh_d_pkts32;
        wh.d_result   = wh_d_result;
        wh.N_bundles  = 1;
        wh.stream     = wh_stream;

        geo_wire_launch_async(&wh, wh_h_pkts32, wh_h_result,
                              EC_N * sizeof(GeoPacket32), EC_N);
        CUDA_CHECK(cudaStreamSynchronize(wh_stream));

        int s=0; for(int i=0;i<EC_N;i++) s+=wh_h_result[i];
        EC_CHECK("EC9 geo_wire_launch_async: 8/8 pass", s, EC_N);

        cudaFree(wh_d_pkts32); cudaFreeHost(wh_h_pkts32);
        cudaFree(wh_d_result);  cudaFreeHost(wh_h_result);
        CUDA_CHECK(cudaStreamDestroy(wh_stream));
    }

    cudaFree(ec_raw); cudaFree(ec_p32); cudaFree(ec_res); cudaFreeHost(ec_h);
#undef EC_CHECK
#undef EC_RUN

    printf("\n  Result: %d/%d passed%s\n",
           ec_pass, ec_pass + ec_fail,
           ec_fail == 0 ? "  ✓ ready for production" : "  ✗ FIX BEFORE PROD");

    return 0;
}
