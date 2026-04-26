#pragma once
#include <stdint.h>
#include <string.h>

#define GEO_LANES  6
#define GEO_CACHE_SLOTS  144
#define GEO_WAYS   2
#define GEO_VICTIM 8
#define PHI64      0x9e3779b97f4a7c15ULL

/* ── collision counter per lane (reseed trigger) ── */
#define GEO_COLLIDE_THRESH 72u   /* 144/2, UM_ZONE_DANGER border */

typedef struct {
    uint64_t key;
    uint64_t val;
    uint8_t  hit;
    uint8_t  age;
    uint8_t  valid;
} GeoEntry;

typedef struct {
    GeoEntry ways[GEO_CACHE_SLOTS][GEO_WAYS];
    GeoEntry victim[GEO_VICTIM];
    uint8_t  vcount;
    uint32_t collisions;   /* per-window collision count */
} GeoLane;

typedef struct { GeoLane lanes[GEO_LANES]; } GeoCache;

typedef struct {
    GeoCache A, B;
    uint64_t write_seq;
    uint64_t salt[GEO_LANES];
    uint32_t reseed_count[GEO_LANES];   /* diagnostic */
} GeoCtx;

/* ── hash ── */
static inline uint32_t geo_lane(uint64_t m) {
    return ((uint32_t)(m >> 32) ^ (uint32_t)(m >> 48)) % GEO_LANES;
}
static inline uint32_t geo_slot(uint64_t m, uint64_t salt) {
    uint64_t h = m * (PHI64 ^ salt);
    return (uint32_t)(h >> 32) % GEO_CACHE_SLOTS; /* spread=8 vs >>56 spread=67 */
}

/* ── clear ── */
static inline void geo_cache_clear(GeoCache *c) { memset(c, 0, sizeof(*c)); }
static inline void geo_ctx_init(GeoCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* ── victim evict: hit=0 first, then oldest ── */
static inline void geo_victim_push(GeoLane *l, GeoEntry e) {
    if (l->vcount < GEO_VICTIM) { l->victim[l->vcount++] = e; return; }
    int idx = 0;
    for (int i = 1; i < GEO_VICTIM; i++) {
        GeoEntry *a = &l->victim[idx], *b = &l->victim[i];
        if (b->hit < a->hit || (b->hit == a->hit && b->age < a->age))
            idx = i;
    }
    l->victim[idx] = e;
}

/* ── self-heal: reseed if collision > threshold ── */
static inline void geo_maybe_reseed(GeoCtx *ctx, uint32_t lane) {
    if (ctx->A.lanes[lane].collisions >= GEO_COLLIDE_THRESH) {
        /* fold collision count into salt → self-healing distribution */
        ctx->salt[lane] ^= (uint64_t)ctx->A.lanes[lane].collisions * PHI64;
        ctx->A.lanes[lane].collisions = 0;
        ctx->reseed_count[lane]++;
    }
}

/* ── write ── */
static inline void geo_write(GeoCtx *ctx, uint64_t key, uint64_t val) {
    uint32_t lane = geo_lane(key);
    uint32_t slot = geo_slot(key, ctx->salt[lane]);
    GeoLane *l    = &ctx->A.lanes[lane];
    uint8_t  age  = (uint8_t)(ctx->write_seq & 0x7Fu);

    /* update existing */
    for (int w = 0; w < GEO_WAYS; w++) {
        GeoEntry *e = &l->ways[slot][w];
        if (e->valid && e->key == key) {
            e->val = val; e->hit = 1; e->age = age;
            return;
        }
    }
    /* insert free way */
    for (int w = 0; w < GEO_WAYS; w++) {
        GeoEntry *e = &l->ways[slot][w];
        if (!e->valid) { *e = (GeoEntry){key, val, 0, age, 1}; return; }
    }
    /* collision — push victim, count */
    l->collisions++;
    geo_victim_push(l, (GeoEntry){key, val, 0, age, 1});
    geo_maybe_reseed(ctx, lane);
}

/* ── read single lane (salt-aware) ── */
static inline int geo_read_lane(GeoLane *l, uint64_t key,
                                 uint64_t salt, uint64_t *out) {
    uint32_t slot = geo_slot(key, salt);   /* FIX: pass salt */
    for (int w = 0; w < GEO_WAYS; w++) {
        GeoEntry *e = &l->ways[slot][w];
        if (e->valid && e->key == key) { e->hit = 1; *out = e->val; return 1; }
    }
    for (int i = 0; i < l->vcount; i++) {
        GeoEntry *e = &l->victim[i];
        if (e->valid && e->key == key) { e->hit = 1; *out = e->val; return 1; }
    }
    return 0;
}

/* ── read: A → B → (KV outside) ── */
static inline int geo_read(GeoCtx *ctx, uint64_t key, uint64_t *out) {
    uint32_t lane = geo_lane(key);
    if (geo_read_lane(&ctx->A.lanes[lane], key, ctx->salt[lane], out)) return 1;
    if (geo_read_lane(&ctx->B.lanes[lane], key, ctx->salt[lane], out)) return 1;
    return 0;
}

/* ── window swap every GEO_CACHE_SLOTS writes ── */
static inline void geo_tick(GeoCtx *ctx) {
    ctx->write_seq++;
    if (ctx->write_seq % GEO_CACHE_SLOTS == 0) {
        GeoCache tmp = ctx->B;
        ctx->B       = ctx->A;
        ctx->A       = tmp;
        geo_cache_clear(&ctx->A);
    }
}

/* ── manual reseed (Union Mask hook) ── */
static inline void geo_reseed(GeoCtx *ctx, uint32_t lane, uint64_t fold) {
    ctx->salt[lane] ^= fold;
}
