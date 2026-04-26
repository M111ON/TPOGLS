/*
 * geo_p4_bridge.h — P4 Dispatch Bridge (AVX2 vs CUDA)
 * ══════════════════════════════════════════════════════
 * Auto-dispatch: small batch → AVX2, large batch → CUDA.
 * Pinned memory pool for zero-copy H↔D transfers.
 *
 * Usage:
 *   GeoP4Bridge b;  geo_p4_bridge_init(&b, 1<<20);
 *   geo_p4_dispatch(&b, addrs, n, spoke_filter, addr_base, out_sig);
 *   geo_p4_bridge_free(&b);
 *
 * Requires: geo_simd_p4.h, geo_cuda_p4.cu compiled + linked
 */

#ifndef GEO_P4_BRIDGE_H
#define GEO_P4_BRIDGE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_simd_p4.h"

/* ── Breakeven threshold (measured on T4 Colab) ──────────────────── */
/* cudaMemcpy + kernel launch ~150µs → only worthwhile ≥ 10K packets */
#ifndef P4_GPU_THRESHOLD
#define P4_GPU_THRESHOLD  10000u   /* benchmark: T4 wins at ~10K pkts */
#endif

/* ── Forward decl from geo_cuda_p4.cu ────────────────────────────── */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void     *d_in;
    uint32_t *d_out_sig;
    uint32_t *d_out_valid;
    uint32_t *d_compact;
    uint32_t *d_count;
    uint32_t  capacity;
    void     *stream;   /* cudaStream_t opaque */
} GeoP4Ctx;

int  geo_p4_ctx_init(GeoP4Ctx *ctx, uint32_t capacity);
void geo_p4_ctx_free(GeoP4Ctx *ctx);
int  geo_p4_run(GeoP4Ctx *ctx, const void *h_wire16, uint32_t n,
                uint32_t spoke_filter, uint64_t addr_base,
                uint32_t *h_out_sig);

#ifdef __cplusplus
}
#endif

/* ── Bridge context ──────────────────────────────────────────────── */
typedef struct {
    GeoP4Ctx  gpu;                  /* CUDA context */
    GeoWire16 *pinned_wire;         /* pinned host buffer (GPU path) */
    GeoWire16 *heap_wire;           /* heap buffer (CPU path) */
    uint32_t  *pinned_out;          /* pinned output (GPU path) */
    uint32_t   capacity;
    int        gpu_ok;              /* 1 = CUDA available */
} GeoP4Bridge;

/* ── Init: allocate buffers, probe GPU ───────────────────────────── */
static inline int geo_p4_bridge_init(GeoP4Bridge *b, uint32_t capacity) {
    memset(b, 0, sizeof(*b));
    b->capacity = capacity;

    /* CPU heap buffer (always available) */
    b->heap_wire = (GeoWire16*)malloc(capacity * sizeof(GeoWire16));
    if (!b->heap_wire) return -1;

#ifdef __CUDACC__
    /* Try GPU init */
    int dev_count = 0;
    if (cudaGetDeviceCount(&dev_count) == cudaSuccess && dev_count > 0) {
        if (geo_p4_ctx_init(&b->gpu, capacity) == 0) {
            /* Allocate pinned memory for zero-copy */
            if (cudaMallocHost((void**)&b->pinned_wire,
                               capacity * sizeof(GeoWire16)) == cudaSuccess &&
                cudaMallocHost((void**)&b->pinned_out,
                               capacity * sizeof(uint32_t)) == cudaSuccess) {
                b->gpu_ok = 1;
            } else {
                geo_p4_ctx_free(&b->gpu);
            }
        }
    }
#endif
    return 0;
}

static inline void geo_p4_bridge_free(GeoP4Bridge *b) {
    free(b->heap_wire);
#ifdef __CUDACC__
    if (b->gpu_ok) {
        cudaFreeHost(b->pinned_wire);
        cudaFreeHost(b->pinned_out);
        geo_p4_ctx_free(&b->gpu);
    }
#endif
}

/* ── Dispatch: auto-select CPU vs GPU path ───────────────────────── */
/*
 * addrs[n]:      input addresses
 * n:             packet count
 * spoke_filter:  6-bit spoke mask (0 = all pass)
 * addr_base:     base for GPU recompute path
 * out_sig[n]:    output sig32 (caller-allocated)
 *
 * Returns: 0 = ok, -1 = error
 * out_sig[i] = 0xFFFFFFFF → filtered out (not valid)
 */
static inline int geo_p4_dispatch(
    GeoP4Bridge    *b,
    const uint64_t *addrs,
    uint32_t        n,
    uint32_t        spoke_filter,
    uint64_t        addr_base,
    uint32_t       *out_sig)
{
    if (n > b->capacity) return -1;
    if (n == 0) return 0;

#ifdef __CUDACC__
    if (b->gpu_ok && n >= P4_GPU_THRESHOLD) {
        /* ── GPU path ── */
        /* Pack addresses → GeoWire16 via AVX2 first */
        geo_simd_fill(addrs, n, 0, b->pinned_wire);

        int r = geo_p4_run(&b->gpu, b->pinned_wire, n,
                           spoke_filter, addr_base, b->pinned_out);
        if (r == 0) {
            memcpy(out_sig, b->pinned_out, n * sizeof(uint32_t));
            return 0;
        }
        /* fallthrough to CPU on GPU error */
    }
#endif

    /* ── CPU AVX2 path ── */
    geo_simd_fill(addrs, n, 0, b->heap_wire);

    for (uint32_t i = 0; i < n; i++) {
        uint8_t spoke = b->heap_wire[i].spoke;
        int     pass  = (spoke_filter == 0u) ||
                        ((spoke_filter >> spoke) & 1u);
        out_sig[i] = pass ? b->heap_wire[i].sig : 0xFFFFFFFFu;
    }
    return 0;
}

/* ── Query: which path will be used for n packets ────────────────── */
static inline const char* geo_p4_path_name(const GeoP4Bridge *b, uint32_t n) {
#ifdef __CUDACC__
    if (b->gpu_ok && n >= P4_GPU_THRESHOLD) return "GPU/T4-sm75";
#endif
    (void)b; (void)n;
    return "CPU/AVX2";
}

#endif /* GEO_P4_BRIDGE_H */
