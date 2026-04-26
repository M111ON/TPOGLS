/*
 * geo_p4_batcher.h — Batch Aggregator + Double-Buffer Stream Overlap
 * ═══════════════════════════════════════════════════════════════════
 * Accumulates small packets → flushes at BATCHER_FLUSH_N (10K).
 * Double-buffer: stream A computes while stream B transfers.
 *
 * Usage:
 *   GeoP4Batcher b;  geo_batcher_init(&b);
 *   geo_batcher_push(&b, addrs, n, spoke_filter);  // repeat
 *   geo_batcher_flush(&b);                          // drain remainder
 *   geo_batcher_free(&b);
 *
 * Requires: geo_p4_bridge.h (GeoP4Ctx, geo_p4_run)
 */

#ifndef GEO_P4_BATCHER_H
#define GEO_P4_BATCHER_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_simd_p4.h"

#ifndef BATCHER_FLUSH_N
#define BATCHER_FLUSH_N  10000u   /* flush threshold = GPU breakeven */
#endif

#define BATCHER_BUF_N    (BATCHER_FLUSH_N * 2u)   /* double-buffer size */

/* ── Flush callback: caller receives completed sig32[] ───────────── */
typedef void (*GeoP4FlushCb)(const uint32_t *sig, uint32_t n, void *user);

/* ── Double-buffer slot ──────────────────────────────────────────── */
typedef struct {
    GeoWire16 *wire;       /* pinned host input  */
    uint32_t  *sig;        /* pinned host output */
    uint32_t   count;
    uint32_t   spoke_filter;
    uint64_t   addr_base;
#ifdef __CUDACC__
    cudaStream_t stream;
    void        *d_wire;   /* device input  */
    uint32_t    *d_sig;    /* device output */
    uint32_t    *d_valid;  /* device ballot */
#endif
    int          in_flight;
} GeoP4Slot;

/* ── Batcher context ─────────────────────────────────────────────── */
typedef struct {
    GeoP4Slot  slot[2];       /* double-buffer A/B  */
    uint8_t    active;        /* 0 or 1             */
    GeoP4FlushCb cb;
    void        *cb_user;
    uint32_t    total_pushed;
    uint32_t    total_flushed;
    int         gpu_ok;
} GeoP4Batcher;

/* ── Forward decl ────────────────────────────────────────────────── */
#ifdef __CUDACC__
static int  _batcher_slot_init(GeoP4Slot *s, uint32_t cap);
static void _batcher_slot_free(GeoP4Slot *s);
static int  _batcher_launch(GeoP4Slot *s);
static int  _batcher_sync(GeoP4Slot *s, GeoP4FlushCb cb, void *user);
#endif

/* ── Init ────────────────────────────────────────────────────────── */
static inline int geo_batcher_init(GeoP4Batcher *b,
                                    GeoP4FlushCb cb, void *user)
{
    memset(b, 0, sizeof(*b));
    b->cb      = cb;
    b->cb_user = user;

#ifdef __CUDACC__
    int dev = 0;
    if (cudaGetDeviceCount(&dev) == cudaSuccess && dev > 0) {
        if (_batcher_slot_init(&b->slot[0], BATCHER_FLUSH_N) == 0 &&
            _batcher_slot_init(&b->slot[1], BATCHER_FLUSH_N) == 0) {
            b->gpu_ok = 1;
        }
    }
#endif
    if (!b->gpu_ok) {
        /* CPU fallback: plain heap buffers */
        b->slot[0].wire = (GeoWire16*)malloc(BATCHER_FLUSH_N * sizeof(GeoWire16));
        b->slot[0].sig  = (uint32_t*) malloc(BATCHER_FLUSH_N * sizeof(uint32_t));
        b->slot[1].wire = (GeoWire16*)malloc(BATCHER_FLUSH_N * sizeof(GeoWire16));
        b->slot[1].sig  = (uint32_t*) malloc(BATCHER_FLUSH_N * sizeof(uint32_t));
        if (!b->slot[0].wire || !b->slot[0].sig ||
            !b->slot[1].wire || !b->slot[1].sig) return -1;
    }
    return 0;
}

/* ── Free ────────────────────────────────────────────────────────── */
static inline void geo_batcher_free(GeoP4Batcher *b) {
#ifdef __CUDACC__
    if (b->gpu_ok) {
        _batcher_slot_free(&b->slot[0]);
        _batcher_slot_free(&b->slot[1]);
        return;
    }
#endif
    free(b->slot[0].wire); free(b->slot[0].sig);
    free(b->slot[1].wire); free(b->slot[1].sig);
}

/* ── Internal: CPU flush of one slot ────────────────────────────── */
static inline void _cpu_flush_slot(GeoP4Slot *s,
                                    GeoP4FlushCb cb, void *user)
{
    uint32_t n = s->count;
    /* AVX2 pack */
    for (uint32_t i = 0; i < n; i++)
        s->sig[i] = s->wire[i].sig;

    /* spoke filter */
    if (s->spoke_filter) {
        for (uint32_t i = 0; i < n; i++) {
            if (!((s->spoke_filter >> s->wire[i].spoke) & 1u))
                s->sig[i] = 0xFFFFFFFFu;
        }
    }
    if (cb) cb(s->sig, n, user);
    s->count = 0;
    s->in_flight = 0;
}

/* ── Push: add addrs to active slot, auto-flush when full ────────── */
/*
 * addrs[n]: input addresses
 * n:        count (can be any size — split across flush boundaries)
 */
static inline int geo_batcher_push(GeoP4Batcher *b,
                                    const uint64_t *addrs,
                                    uint32_t        n,
                                    uint32_t        spoke_filter)
{
    uint32_t pos = 0;
    while (pos < n) {
        GeoP4Slot *cur = &b->slot[b->active];
        uint32_t   room = BATCHER_FLUSH_N - cur->count;
        uint32_t   take = (n - pos < room) ? (n - pos) : room;

        /* pack addresses → GeoWire16 via AVX2 */
        geo_simd_fill(addrs + pos, take, 0, cur->wire + cur->count);
        cur->count        += take;
        cur->spoke_filter  = spoke_filter;
        cur->addr_base     = addrs[pos];
        pos               += take;
        b->total_pushed   += take;

        if (cur->count >= BATCHER_FLUSH_N) {
#ifdef __CUDACC__
            if (b->gpu_ok) {
                /* launch current slot async */
                _batcher_launch(cur);
                cur->in_flight = 1;

                /* sync the OTHER slot if it was in flight */
                GeoP4Slot *other = &b->slot[b->active ^ 1u];
                if (other->in_flight)
                    _batcher_sync(other, b->cb, b->cb_user);

                /* swap active slot */
                b->active ^= 1u;
                b->slot[b->active].count = 0;
                b->total_flushed += cur->count;
                continue;
            }
#endif
            /* CPU path */
            _cpu_flush_slot(cur, b->cb, b->cb_user);
            b->total_flushed += BATCHER_FLUSH_N;
        }
    }
    return 0;
}

/* ── Flush: drain remaining packets in active slot ───────────────── */
static inline int geo_batcher_flush(GeoP4Batcher *b) {
    /* sync any in-flight slot first */
#ifdef __CUDACC__
    if (b->gpu_ok) {
        GeoP4Slot *other = &b->slot[b->active ^ 1u];
        if (other->in_flight)
            _batcher_sync(other, b->cb, b->cb_user);
    }
#endif
    GeoP4Slot *cur = &b->slot[b->active];
    if (cur->count == 0) return 0;

#ifdef __CUDACC__
    if (b->gpu_ok && cur->count >= 1) {
        _batcher_launch(cur);
        _batcher_sync(cur, b->cb, b->cb_user);
        b->total_flushed += cur->count;
        cur->count = 0;
        return 0;
    }
#endif
    b->total_flushed += cur->count;
    _cpu_flush_slot(cur, b->cb, b->cb_user);
    return 0;
}

/* ── Stats ───────────────────────────────────────────────────────── */
static inline void geo_batcher_stats(const GeoP4Batcher *b,
                                      uint32_t *pushed,
                                      uint32_t *flushed,
                                      uint32_t *pending)
{
    if (pushed)  *pushed  = b->total_pushed;
    if (flushed) *flushed = b->total_flushed;
    if (pending) *pending = b->slot[b->active].count;
}

/* ══════════════════════════════════════════════════════════════════
 * CUDA slot helpers (compiled only with nvcc)
 * ══════════════════════════════════════════════════════════════════ */
#ifdef __CUDACC__

#define _BC(x) do { cudaError_t _e=(x); if(_e!=cudaSuccess) return -1; } while(0)

/* geo_cuda_step_kernel forward decl */
extern "C" __global__ void geo_cuda_step_kernel(
    const uint4*, uint32_t*, uint32_t*, uint32_t, uint32_t, uint64_t);

static int _batcher_slot_init(GeoP4Slot *s, uint32_t cap) {
    _BC(cudaMallocHost((void**)&s->wire,  cap * sizeof(GeoWire16)));
    _BC(cudaMallocHost((void**)&s->sig,   cap * sizeof(uint32_t)));
    _BC(cudaMalloc(&s->d_wire,  cap * sizeof(GeoWire16)));
    _BC(cudaMalloc((void**)&s->d_sig,   cap * sizeof(uint32_t)));
    _BC(cudaMalloc((void**)&s->d_valid, ((cap+31)/32) * sizeof(uint32_t)));
    _BC(cudaStreamCreate(&s->stream));
    s->count = 0; s->in_flight = 0;
    return 0;
}

static void _batcher_slot_free(GeoP4Slot *s) {
    cudaFreeHost(s->wire);  cudaFreeHost(s->sig);
    cudaFree(s->d_wire);    cudaFree(s->d_sig);    cudaFree(s->d_valid);
    cudaStreamDestroy(s->stream);
}

static int _batcher_launch(GeoP4Slot *s) {
    uint32_t n = s->count;
    uint32_t threads = 256, blocks = (n + threads - 1) / threads;
    /* async H→D */
    _BC(cudaMemcpyAsync(s->d_wire, s->wire, n * sizeof(GeoWire16),
                        cudaMemcpyHostToDevice, s->stream));
    /* kernel */
    geo_cuda_step_kernel<<<blocks, threads, 0, s->stream>>>(
        (const uint4*)s->d_wire, s->d_sig, s->d_valid,
        n, s->spoke_filter, s->addr_base);
    /* async D→H */
    _BC(cudaMemcpyAsync(s->sig, s->d_sig, n * sizeof(uint32_t),
                        cudaMemcpyDeviceToHost, s->stream));
    return 0;
}

static int _batcher_sync(GeoP4Slot *s, GeoP4FlushCb cb, void *user) {
    cudaStreamSynchronize(s->stream);
    if (cb) cb(s->sig, s->count, user);
    s->count = 0; s->in_flight = 0;
    return 0;
}

#undef _BC
#endif /* __CUDACC__ */

#endif /* GEO_P4_BATCHER_H */
