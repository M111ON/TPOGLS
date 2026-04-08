/*
 * pogls_sdk.h — PoglsCtx internal SDK (Session 19)
 *
 * S19 change: GeoKV field → KVBridge (CPU truth + GPU async ring)
 * All prior .so symbols unchanged — opaque PoglsHandle hides the swap.
 *
 * Architecture:
 *   pogls_so_*  (.so boundary)
 *       ↓
 *   PoglsCtx: GeoPipeline(L1 GeoCache) + KVBridge
 *       ↓
 *   KVBridge: GeoKV (CPU truth) + 4×ring → GPU kv_insert_gpu
 *       ↓
 *   geo_kv_rehash: tomb>30% → compact
 */

#ifndef POGLS_SDK_H
#define POGLS_SDK_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "kv_bridge.h"
#include <inttypes.h>

/* ── L1 GeoCache (unchanged from S16) ──────────────────────────── */
#define GEO_BUNDLE_WORDS  8          /* frozen */
#define GEO_CACHE_SLOTS   256

typedef struct {
    uint64_t key;
    uint64_t words[GEO_BUNDLE_WORDS];
    int      valid;
} GeoCacheSlot;

typedef struct {
    GeoCacheSlot slots[GEO_CACHE_SLOTS];
    uint64_t     hits, misses;
} GeoCache;

static inline void geocache_init(GeoCache *c) { memset(c, 0, sizeof(*c)); }

static inline GeoCacheSlot *geocache_get(GeoCache *c, uint64_t key) {
    uint32_t idx = (uint32_t)(key & (GEO_CACHE_SLOTS - 1));
    GeoCacheSlot *s = &c->slots[idx];
    if (s->valid && s->key == key) { c->hits++; return s; }
    c->misses++;
    return NULL;
}

static inline void geocache_put(GeoCache *c, uint64_t key, uint64_t val) {
    uint32_t idx = (uint32_t)(key & (GEO_CACHE_SLOTS - 1));
    GeoCacheSlot *s = &c->slots[idx];
    s->key      = key;
    s->words[0] = val;   /* primary word; rest zeroed on init */
    s->valid    = 1;
}

static inline void geocache_invalidate(GeoCache *c) {
    memset(c, 0, sizeof(*c));
}

/* ── PoglsCtx ───────────────────────────────────────────────────── */
typedef struct {
    GeoCache  l1;        /* L1: hot path, direct-map, evict-on-conflict */
    KVBridge  kv;        /* S19: was GeoKV — now CPU+GPU bridge         */
    uint64_t  writes;
    uint64_t  reads;
    uint64_t  qrpns;
} PoglsCtx;

/* ── Lifecycle ──────────────────────────────────────────────────── */
static inline PoglsCtx *pogls_open(void) {
    PoglsCtx *ctx = (PoglsCtx *)calloc(1, sizeof(PoglsCtx));
    if (!ctx) return NULL;
    geocache_init(&ctx->l1);
    kvbridge_init(&ctx->kv);
    return ctx;
}

static inline void pogls_close(PoglsCtx *ctx) {
    if (ctx) free(ctx);
}

/* ── Write: L1 + KVBridge (CPU truth + GPU enqueue) ────────────── */
static inline void pogls_write(PoglsCtx *ctx, uint64_t key, uint64_t val) {
    geocache_put(&ctx->l1, key, val);
    kvbridge_put(&ctx->kv, key, val);
    ctx->writes++;
}

/* ── Read: L1 → CPU KV (never GPU) ─────────────────────────────── */
static inline uint64_t pogls_read(PoglsCtx *ctx, uint64_t key) {
    ctx->reads++;
    GeoCacheSlot *s = geocache_get(&ctx->l1, key);
    if (s) return s->words[0];
    return kvbridge_get(&ctx->kv, key);
}

static inline int pogls_has(PoglsCtx *ctx, uint64_t key) {
    GeoCacheSlot *s = geocache_get(&ctx->l1, key);
    if (s) return 1;
    return kvbridge_has(&ctx->kv, key);
}

/* ── QRPN: mark failed probe ────────────────────────────────────── */
static inline void pogls_qrpn(PoglsCtx *ctx, uint64_t key, uint8_t failed) {
    (void)failed;
    kvbridge_del(&ctx->kv, key);
    ctx->qrpns++;
}

/* ── Rewind: flush L1, truth in KV survives ─────────────────────── */
static inline void pogls_rewind(PoglsCtx *ctx) {
    geocache_invalidate(&ctx->l1);
}

/* ── GPU flush: drain ring → kv_insert_gpu ──────────────────────── */
/*
 * Call from a dedicated flush thread or explicit checkpoint.
 * fn = pointer to GPU insert adapter (see kv_bridge_gpu.h).
 * In CPU-only builds, pass a no-op sink.
 */
static inline uint32_t pogls_flush_gpu(PoglsCtx *ctx,
                                        kvb_gpu_insert_fn fn, void *gpu_ctx) {
    return kvbridge_flush(&ctx->kv, fn, gpu_ctx);
}

/* ── Stats ──────────────────────────────────────────────────────── */
static inline void pogls_print_stats(PoglsCtx *ctx) {
    printf("[Pogls] writes=%" PRIu64 " reads=%" PRIu64 " qrpns=%" PRIu64 "\n",
           ctx->writes, ctx->reads, ctx->qrpns);
    printf("[L1]    hits=%" PRIu64 " misses=%" PRIu64 " hit%%=%.1f\n",
           ctx->l1.hits, ctx->l1.misses,
           (ctx->l1.hits + ctx->l1.misses) > 0
               ? 100.0 * ctx->l1.hits / (ctx->l1.hits + ctx->l1.misses) : 0.0);
    kvbridge_stats(&ctx->kv);
}

#endif /* POGLS_SDK_H */
