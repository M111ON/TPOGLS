/*
 * geo_net_so_s29.c — libgeonet.so S29: adds streaming iterator
 *
 * Build:
 *   gcc -O2 -march=native -shared -fPIC -I. -I./core \
 *       -o libgeonet.so geo_net_so_s29.c -lm
 *
 * S29 new export:
 *
 *   typedef void (*geonet_chunk_cb)(
 *       uint64_t chunk_i,   // position in layer
 *       uint64_t addr,      // absolute addr (file_idx * CYL_FULL_N + i)
 *       uint8_t  spoke,
 *       uint8_t  inv_spoke,
 *       uint8_t  mirror_mask,
 *       uint8_t  is_audit,
 *       void    *userdata
 *   );
 *
 *   int pogls_iter_chunks(
 *       GeoNetHandle h,
 *       uint64_t     off,         // file_idx * CYL_FULL_N
 *       uint64_t     n,           // chunk count
 *       const char  *layer_filter,// NULL = no filter; else layer name prefix match
 *       geonet_chunk_cb cb,
 *       void        *userdata
 *   );
 *   → calls cb() per chunk in C — no Python overhead between chunks
 *   → returns chunks emitted, -1 on error
 *
 * All S28 exports unchanged.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/geo_config.h"
#include "core/geo_cylinder.h"
#include "core/geo_thirdeye.h"
#include "geo_net_patched.h"

/* ── opaque handle (same as S28) ─────────────────────────────────── */

typedef struct {
    GeoNet  gn;
    GeoSeed seed;
} GeoNetCtxInternal;

typedef void* GeoNetHandle;

/* ── batch record (same as S28) ──────────────────────────────────── */

typedef struct {
    uint8_t spoke;
    uint8_t inv_spoke;
    uint8_t face;
    uint8_t unit;
    uint8_t group;
    uint8_t mirror_mask;
    uint8_t is_center;
    uint8_t is_audit;
} GeoNetPackedAddr;

typedef GeoNetPackedAddr GeoNetBatchRec;

/* ── S29: callback type ───────────────────────────────────────────── */

typedef void (*geonet_chunk_cb)(
    uint64_t chunk_i,
    uint64_t addr,
    uint8_t  spoke,
    uint8_t  inv_spoke,
    uint8_t  mirror_mask,
    uint8_t  is_audit,
    void    *userdata
);

/* ── lifecycle ────────────────────────────────────────────────────── */

GeoNetHandle geonet_open(void) {
    GeoNetCtxInternal *ctx = calloc(1, sizeof(GeoNetCtxInternal));
    if (!ctx) return NULL;
    ctx->seed.gen2 = 0xDEADBEEFCAFEBABEULL;
    ctx->seed.gen3 = 0x123456789ABCDEF0ULL;
    geo_net_init(&ctx->gn, ctx->seed);
    return (GeoNetHandle)ctx;
}

void geonet_close(GeoNetHandle h) {
    if (h) free(h);
}

/* ── pogls_fetch_chunk (S28 unchanged) ───────────────────────────── */

int pogls_fetch_chunk(GeoNetHandle h, uint32_t idx, uint8_t *out64) {
    if (!h || !out64) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    GeoNetAddr a = geo_net_route(&ctx->gn, (uint64_t)idx, 0, 0, ctx->seed);
    uint8_t is_audit = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
    out64[0] = a.spoke;
    out64[1] = a.inv_spoke;
    out64[2] = a.face;
    out64[3] = a.unit;
    out64[4] = a.group;
    out64[5] = a.mirror_mask;
    out64[6] = a.is_center;
    out64[7] = is_audit;
    memset(out64 + 8, 0, 56);
    return 0;
}

/* ── pogls_fetch_range (S28 unchanged) ───────────────────────────── */

int pogls_fetch_range(GeoNetHandle h, uint64_t off, uint64_t len, uint8_t *out) {
    if (!h || !out || len == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    uint8_t *p = out;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t idx = off + i;
        GeoNetAddr a = geo_net_route(&ctx->gn, idx, 0, 0, ctx->seed);
        p[0] = a.spoke;
        p[1] = a.inv_spoke;
        p[2] = a.face;
        p[3] = a.unit;
        p[4] = a.group;
        p[5] = a.mirror_mask;
        p[6] = a.is_center;
        p[7] = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
        p += 8;
    }
    return (int)len;
}

/* ── pogls_verify_batch (S28 unchanged) ──────────────────────────── */

int pogls_verify_batch(GeoNetBatchRec *recs, int n) {
    if (!recs || n <= 0) return 0;
    int pass = 0;
    for (int i = 0; i < n; i++) {
        uint8_t s  = recs[i].spoke;
        uint8_t is = recs[i].inv_spoke;
        uint8_t f  = recs[i].face;
        if (s < 6 && is == (uint8_t)((s + 3) % 6) && f < 9)
            pass++;
    }
    return pass;
}

/* ── S29: pogls_iter_chunks ──────────────────────────────────────── */
/*
 * Core insight: callback fires in C loop → Python never holds full array.
 * memory = O(1) regardless of layer size.
 *
 * layer_filter: currently unused at C level (filter pushdown in S30).
 * Pass NULL. Python-side filter happens inside cb via userdata if needed.
 */

int pogls_iter_chunks(
    GeoNetHandle   h,
    uint64_t       off,
    uint64_t       n,
    const char    *layer_filter,   /* reserved — pass NULL */
    geonet_chunk_cb cb,
    void          *userdata
) {
    if (!h || !cb || n == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;

    (void)layer_filter;   /* filter pushdown: S30 */

    for (uint64_t i = 0; i < n; i++) {
        uint64_t addr = off + i;
        GeoNetAddr a  = geo_net_route(&ctx->gn, addr, 0, 0, ctx->seed);
        uint8_t ia    = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
        cb(i, addr, a.spoke, a.inv_spoke, a.mirror_mask, ia, userdata);
    }
    return (int)n;
}

/* ── state queries (S28 unchanged) ───────────────────────────────── */

uint8_t geonet_state(GeoNetHandle h) {
    if (!h) return 0;
    return geo_net_state(&((GeoNetCtxInternal *)h)->gn);
}

uint32_t geonet_op_count(GeoNetHandle h) {
    if (!h) return 0;
    return ((GeoNetCtxInternal *)h)->gn.op_count;
}

uint8_t geonet_mirror_mask(GeoNetHandle h, uint8_t spoke) {
    if (!h) return 0;
    ThirdEye *te = &((GeoNetCtxInternal *)h)->gn.eye;
    return te_get_mask(te, spoke % 6);
}
