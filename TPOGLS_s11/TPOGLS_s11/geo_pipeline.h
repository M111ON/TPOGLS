/*
 * geo_pipeline.h — Full Wire: GeoCache + PayloadWired + UnionMask
 * ═══════════════════════════════════════════════════════════════════
 * Session 14 — final wire
 *
 * Stack:
 *   GeoCache      (L1 fast path: A/B window + 2-way + victim)
 *     ↓ miss
 *   PayloadWired  (inode+data: PayloadStore + UnionMask observer)
 *     ↓ miss
 *   KV_store      (truth — external, caller provides)
 *
 * Cross-layer feedback:
 *   UnionMask fold[lane] → geo_reseed(salt[lane])
 *   GeoCache collision   → geo_maybe_reseed (internal)
 *   geo_tick (144 writes) → pw_eval (144 writes) — same window boundary
 *
 * Single entry points:
 *   gp_write(ctx, key, val)
 *   gp_read (ctx, key, out)  → returns 0 if KV fallback needed
 *   gp_qrpn (ctx, key, failed)
 *   gp_tick (ctx)            → call after each write
 */

#ifndef GEO_PIPELINE_H
#define GEO_PIPELINE_H

#include "geo_cache.h"
#include "geo_payload_wired.h"

/* ── Pipeline Context ───────────────────────────────────────────── */

typedef struct {
    GeoCtx       cache;    /* L1: fast path          */
    PayloadWired store;    /* L2: inode + data + mask */
} GeoPipeline;

static inline void gp_init(GeoPipeline *gp) {
    geo_ctx_init(&gp->cache);
    pw_init(&gp->store);
}

/* ── Cross-layer reseed: UnionMask fold → GeoCache salt ─────────── */
/*
 * เมื่อ UnionMask detect DANGER lane → fold สูง
 * feed fold เข้า geo_reseed เพื่อ shift slot distribution
 * = self-heal: store layer บอก cache layer ให้ rehash
 */
static inline void gp_sync_reseed(GeoPipeline *gp) {
    for (uint8_t i = 0; i < PL_PAIRS; i++) {
        if (gp->store.mask.zone[i] == UM_ZONE_DANGER) {
            geo_reseed(&gp->cache, i,
                       (uint64_t)gp->store.mask.fold[i] * PHI64);
        }
    }
}

/* ── Tick: window boundary sync ─────────────────────────────────── */
/*
 * GEO_SLOTS == PW_WINDOW == 144
 * both layers share same boundary → one tick drives both
 */
static inline void gp_tick(GeoPipeline *gp) {
    geo_tick(&gp->cache);
    /* pw_eval fires internally via pw_tick_check in pw_write
     * but force sync at cache swap boundary */
    if (gp->cache.write_seq % GEO_SLOTS == 0) {
        pw_eval(&gp->store);
        gp_sync_reseed(gp);
    }
}

/* ── Write ───────────────────────────────────────────────────────── */
/*
 * 1. pw_write  → compress gate + PayloadStore + UnionMask count
 * 2. geo_write → GeoCache L1 (only if pw_write passed)
 * 3. geo_tick  → window advance
 */
static inline void gp_write(GeoPipeline *gp, uint64_t key, uint64_t val) {
    uint8_t stored = pw_write(&gp->store, key, val);
    if (stored) geo_write(&gp->cache, key, val);
    gp_tick(gp);
}

/* ── Read ────────────────────────────────────────────────────────── */
/*
 * returns: 1 = hit (out valid)
 *          0 = miss → caller must query KV_store
 *
 * path: GeoCache A → GeoCache B → PayloadWired → 0 (KV needed)
 */
static inline int gp_read(GeoPipeline *gp, uint64_t key, uint64_t *out) {
    /* L1: cache A → B */
    if (geo_read(&gp->cache, key, out)) return 1;

    /* L2: PayloadWired (inode lookup) */
    PayloadResult r = pw_read(&gp->store, key);
    if (r.found) {
        *out = r.value;
        /* promote back to cache */
        geo_write(&gp->cache, key, r.value);
        return 1;
    }

    return 0;   /* KV fallback — caller handles */
}

/* ── QRPN feedback ───────────────────────────────────────────────── */

static inline void gp_qrpn(GeoPipeline *gp, uint64_t key, uint8_t failed) {
    pw_qrpn(&gp->store, key, failed);
}

/* ── Stats ───────────────────────────────────────────────────────── */

static inline void gp_print_stats(const GeoPipeline *gp) {
    printf("── GeoCache ──\n");
    printf("  write_seq=%llu  reseeds: ", (unsigned long long)gp->cache.write_seq);
    for (int i = 0; i < GEO_LANES; i++)
        printf("[%d]=%u ", i, gp->cache.reseed_count[i]);
    printf("\n");
    printf("── PayloadWired ──\n");
    pw_print_stats(&gp->store);
}

/* ── Compile-time sanity ─────────────────────────────────────────── */
_Static_assert(GEO_SLOTS == PW_WINDOW,
    "GeoCache and PayloadWired must share same window size (144)");
_Static_assert(GEO_LANES == PL_PAIRS,
    "GeoCache lanes must match PayloadStore lanes (6)");

#endif /* GEO_PIPELINE_H */
