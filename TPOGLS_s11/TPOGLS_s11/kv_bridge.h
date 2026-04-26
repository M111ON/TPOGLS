/*
 * kv_bridge.h — CPU/GPU dual-path KV bridge (Session 18)
 *
 * Design contract:
 *   CPU  = correctness layer  (GeoKV, immediate, always truth)
 *   GPU  = throughput layer   (async batch only, never read-back)
 *   Ring = lock-free per-lane enqueue (reduce contention pre-batch)
 *
 * Frozen constants (do not change):
 *   KVB_GPU_CAP   = 65536   (power-of-2, GPU table size)
 *   KV_MAX_LOAD   = 5734    (70% of 8192, CPU rehash guard)
 *   MAX_PROBE     = 128
 *   KVB_LANES     = 4       (ring buffer lanes, power-of-2)
 *   KVB_LANE_SIZE = 256     (entries per lane, power-of-2)
 */

#ifndef KV_BRIDGE_H
#define KV_BRIDGE_H

#include "geo_kv_rehash.h"  /* GeoKV + kv_rehash + kvr_needs_rehash */
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* ── Ring buffer constants ──────────────────────────────────────── */
#define KVB_LANES      4
#define KVB_LANE_SIZE  256                        /* must be power-of-2 */
#define KVB_LANE_MASK  (KVB_LANE_SIZE - 1)

/* ── Load guard (mirrors frozen constant) ────────────────────────── */
#define KVB_MAX_LOAD   5734                       /* 70% of 8192 */

/* ── One entry in the ring ───────────────────────────────────────── */
typedef struct {
    uint64_t key;
    uint64_t val;
} KVBEntry;                                       /* 16B, matches TSBridgeEntry */

/* ── Lock-free SPSC ring lane ────────────────────────────────────── */
/*
 * Each lane is Single-Producer / Single-Consumer:
 *   producer = caller thread hashed to this lane
 *   consumer = flush thread (kvbridge_flush)
 *
 * head = next write slot (producer-owned)
 * tail = next read  slot (consumer-owned)
 * Slot full when (head - tail) == KVB_LANE_SIZE
 */
typedef struct {
    _Atomic uint32_t head;                        /* producer advances */
    _Atomic uint32_t tail;                        /* consumer advances */
    KVBEntry         buf[KVB_LANE_SIZE];
} KVBLane;

/* ── KVBridge: the bridge struct ─────────────────────────────────── */
typedef struct {
    GeoKV   cpu;                                  /* truth — always valid */
    KVBLane lanes[KVB_LANES];                     /* GPU work queues     */
    uint32_t flush_count;                         /* total GPU flushes   */
    uint32_t dropped;                             /* ring-full drops (monitor) */
} KVBridge;

/* ── Init ────────────────────────────────────────────────────────── */
static inline void kvbridge_init(KVBridge *b) {
    memset(b, 0, sizeof(*b));
    kv_init(&b->cpu);
}

/* ── Lane selector: hash key to lane index ───────────────────────── */
static inline uint32_t kvb_lane(uint64_t key) {
    return (uint32_t)((key ^ (key >> 32)) & (KVB_LANES - 1));
}

/* ── Enqueue to ring (non-blocking) ──────────────────────────────── */
/*
 * Returns 1 = enqueued, 0 = lane full (drop — GPU may miss, CPU is truth)
 */
static inline int kvb_enqueue(KVBridge *b, uint64_t key, uint64_t val) {
    KVBLane *lane = &b->lanes[kvb_lane(key)];
    uint32_t h = atomic_load_explicit(&lane->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&lane->tail, memory_order_acquire);
    if ((h - t) >= KVB_LANE_SIZE) return 0;      /* full — drop GPU copy */
    lane->buf[h & KVB_LANE_MASK] = (KVBEntry){key, val};
    atomic_store_explicit(&lane->head, h + 1, memory_order_release);
    return 1;
}

/* ── CPU put with load guard ─────────────────────────────────────── */
/*
 * CPU write = truth. Always succeeds (rehash if needed).
 * GPU enqueue is best-effort (drop OK — GPU is throughput only).
 */
static inline void kvbridge_put(KVBridge *b, uint64_t key, uint64_t val) {
    /* load guard: rehash before table saturates */
    if (b->cpu.count >= KVB_MAX_LOAD || kvr_needs_rehash(&b->cpu))
        kv_rehash(&b->cpu);
    kv_put(&b->cpu, key, val);
    if (!kvb_enqueue(b, key, val)) b->dropped++;
}

/* ── CPU read (never touches GPU) ────────────────────────────────── */
static inline uint64_t kvbridge_get(KVBridge *b, uint64_t key) {
    return kv_get(&b->cpu, key);
}

static inline int kvbridge_has(KVBridge *b, uint64_t key) {
    return kv_get(&b->cpu, key) != 0;            /* assumes 0 = not found */
}

/* ── CPU delete ──────────────────────────────────────────────────── */
static inline void kvbridge_del(KVBridge *b, uint64_t key) {
    kv_del_auto(&b->cpu, key);
    /* no GPU delete — GPU table is insert-only async shadow */
}

/* ── Drain one lane → caller supplies GPU insert fn ─────────────── */
/*
 * Callback signature: void gpu_insert(uint64_t key, uint64_t val, void *ctx)
 * Flush is single-threaded — call from one dedicated flush thread.
 */
typedef void (*kvb_gpu_insert_fn)(uint64_t key, uint64_t val, void *ctx);

static inline uint32_t kvb_flush_lane(KVBLane *lane,
                                       kvb_gpu_insert_fn fn, void *ctx) {
    uint32_t t = atomic_load_explicit(&lane->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&lane->head, memory_order_acquire);
    uint32_t n = 0;
    while (t != h) {
        KVBEntry *e = &lane->buf[t & KVB_LANE_MASK];
        fn(e->key, e->val, ctx);
        t++; n++;
    }
    atomic_store_explicit(&lane->tail, t, memory_order_release);
    return n;
}

/* ── Full flush: drain all lanes → GPU ──────────────────────────── */
static inline uint32_t kvbridge_flush(KVBridge *b,
                                       kvb_gpu_insert_fn fn, void *ctx) {
    uint32_t total = 0;
    for (int i = 0; i < KVB_LANES; i++)
        total += kvb_flush_lane(&b->lanes[i], fn, ctx);
    b->flush_count++;
    return total;                                 /* entries sent to GPU */
}

/* ── Stats ───────────────────────────────────────────────────────── */
static inline void kvbridge_stats(const KVBridge *b) {
    printf("[KVBridge] cpu.count=%u tomb=%u flushes=%u dropped=%u\n",
           b->cpu.count, b->cpu.tombstones, b->flush_count, b->dropped);
}

#endif /* KV_BRIDGE_H */
