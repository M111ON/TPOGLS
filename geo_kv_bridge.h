/*
 * geo_kv_bridge.h — CPU GeoKV ↔ GPU d_table sync bridge
 * ═══════════════════════════════════════════════════════
 * Session 16
 *
 * Strategy:
 *   - CPU path (geo_kv.h) is truth layer, always written first
 *   - GPU table mirrors CPU for bulk queries (≥64K batch)
 *   - Flush trigger: drop_rate >50% OR manual kvb_flush()
 *   - Flush = copy dirty CPU slots → d_input → kv_insert_kernel
 *
 * Layout:
 *   KVBridge.dirty[]  ring buffer of pending (key,val) pairs
 *   kvb_push()        append to dirty ring (O(1))
 *   kvb_flush()       bulk push dirty → GPU (async, stream 0)
 *   kvb_sync()        cudaStreamSynchronize + clear dirty
 */

#ifndef GEO_KV_BRIDGE_H
#define GEO_KV_BRIDGE_H

#include <cuda_runtime.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "geo_kv.h"

/* ── GPU KVPair (matches geo_kv_gpu.cu) ───────────────────────── */

typedef struct { uint64_t key; uint64_t val; } GpuKVPair;

/* forward decl from geo_kv_gpu.cu */
extern void kv_insert_gpu(GpuKVPair *d_table, uint64_t capacity,
                          GpuKVPair *d_input,  int n,
                          uint32_t  *d_overflow);

/* ── Constants ────────────────────────────────────────────────── */

#define KVB_GPU_CAP   65536U          /* GPU table capacity (power-of-2) */
#define KVB_DIRTY_MAX 65536U          /* dirty ring size = GPU cap */
#define KVB_FLUSH_THR 32768U          /* auto-flush at 50% dirty */

/* ── Bridge Context ───────────────────────────────────────────── */

typedef struct {
    /* CPU truth */
    GeoKV       cpu;

    /* GPU table (device) */
    GpuKVPair  *d_table;
    uint32_t   *d_overflow;

    /* dirty ring (pinned host memory for fast DMA) */
    GpuKVPair  *h_dirty;     /* cudaMallocHost pinned */
    uint32_t    dirty_head;  /* next write index */

    /* stats */
    uint32_t    flushes;
    uint32_t    total_overflow;
    int         gpu_ready;
} KVBridge;

/* ── Init / Destroy ───────────────────────────────────────────── */

static inline int kvb_init(KVBridge *b) {
    memset(b, 0, sizeof(*b));
    kv_init(&b->cpu);

    cudaError_t e;
    e = cudaMalloc(&b->d_table,    KVB_GPU_CAP * sizeof(GpuKVPair));
    if (e != cudaSuccess) { fprintf(stderr, "kvb: d_table alloc fail\n"); return 0; }
    e = cudaMalloc(&b->d_overflow, sizeof(uint32_t));
    if (e != cudaSuccess) { fprintf(stderr, "kvb: d_overflow alloc fail\n"); return 0; }
    e = cudaMallocHost(&b->h_dirty, KVB_DIRTY_MAX * sizeof(GpuKVPair));
    if (e != cudaSuccess) { fprintf(stderr, "kvb: pinned alloc fail\n"); return 0; }

    cudaMemset(b->d_table,    0, KVB_GPU_CAP * sizeof(GpuKVPair));
    cudaMemset(b->d_overflow, 0, sizeof(uint32_t));

    b->gpu_ready = 1;
    return 1;
}

static inline void kvb_destroy(KVBridge *b) {
    if (!b->gpu_ready) return;
    cudaFree(b->d_table);
    cudaFree(b->d_overflow);
    cudaFreeHost(b->h_dirty);
    b->gpu_ready = 0;
}

/* ── Push to dirty ring ───────────────────────────────────────── */

static inline void kvb_push(KVBridge *b, uint64_t key, uint64_t val) {
    assert(b->dirty_head < KVB_DIRTY_MAX);
    b->h_dirty[b->dirty_head].key = key;
    b->h_dirty[b->dirty_head].val = val;
    b->dirty_head++;
}

/* ── Flush dirty → GPU (async) ────────────────────────────────── */

static inline void kvb_flush(KVBridge *b) {
    if (!b->gpu_ready || b->dirty_head == 0) return;

    uint32_t n = b->dirty_head;

    /* reset overflow counter */
    cudaMemset(b->d_overflow, 0, sizeof(uint32_t));

    /* H2D: pinned → device (fast) */
    GpuKVPair *d_input;
    cudaMalloc(&d_input, n * sizeof(GpuKVPair));
    cudaMemcpy(d_input, b->h_dirty, n * sizeof(GpuKVPair),
               cudaMemcpyHostToDevice);

    kv_insert_gpu(b->d_table, KVB_GPU_CAP, d_input, (int)n, b->d_overflow);
    cudaDeviceSynchronize();

    /* check overflow */
    uint32_t ov = 0;
    cudaMemcpy(&ov, b->d_overflow, sizeof(uint32_t), cudaMemcpyDeviceToHost);
    b->total_overflow += ov;
    if (ov) fprintf(stderr, "kvb_flush: overflow=%u (table too full?)\n", ov);

    cudaFree(d_input);
    b->dirty_head = 0;
    b->flushes++;
}

/* ── Write (CPU truth + dirty ring, auto-flush at threshold) ──── */

static inline void kvb_put(KVBridge *b, uint64_t key, uint64_t val) {
    kv_put(&b->cpu, key, val);
    if (b->gpu_ready) {
        kvb_push(b, key, val);
        if (b->dirty_head >= KVB_FLUSH_THR)
            kvb_flush(b);
    }
}

/* ── Read (CPU only — GPU is write mirror, not read path) ─────── */

static inline uint64_t kvb_get(KVBridge *b, uint64_t key) {
    return kv_get(&b->cpu, key);
}

static inline int kvb_has(KVBridge *b, uint64_t key) {
    return kv_has(&b->cpu, key);
}

/* ── Stats ────────────────────────────────────────────────────── */

static inline void kvb_print_stats(const KVBridge *b) {
    printf("── KVBridge ──\n");
    printf("  flushes=%u  total_overflow=%u  dirty_pending=%u\n",
           b->flushes, b->total_overflow, b->dirty_head);
    kv_print_stats(&b->cpu);
}

/* ── Compile-time sanity ──────────────────────────────────────── */
_Static_assert((KVB_GPU_CAP & (KVB_GPU_CAP-1)) == 0, "KVB_GPU_CAP power-of-2");
_Static_assert(KVB_DIRTY_MAX <= KVB_GPU_CAP,          "dirty ring <= GPU cap");
_Static_assert(KVB_FLUSH_THR == KVB_DIRTY_MAX / 2,    "flush at 50%");

#endif /* GEO_KV_BRIDGE_H */
