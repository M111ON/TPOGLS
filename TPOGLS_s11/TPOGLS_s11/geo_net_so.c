/*
 * geo_net_so.c — libgeonet.so: GeoNet routing exported for Python ctypes
 *
 * Build:
 *   gcc -O2 -march=native -shared -fPIC -I. -I./core \
 *       -o libgeonet.so geo_net_so.c -lm
 *
 * Exports (3 functions — hot path for S28):
 *
 *   GeoNetHandle  geonet_open(void)
 *   void          geonet_close(GeoNetHandle h)
 *
 *   int  pogls_fetch_chunk(GeoNetHandle h, uint32_t idx, uint8_t *out64)
 *        → routes idx through GeoNet, writes 64-byte routing result into out64
 *        → returns 0 OK, -1 NULL handle
 *
 *   int  pogls_fetch_range(GeoNetHandle h, uint64_t off, uint64_t len,
 *                          uint8_t *out)
 *        → routes [off..off+len) chunks in one C loop, writes packed results
 *        → out must be len * 8 bytes (one GeoNetAddr per chunk, packed)
 *        → returns chunks routed, -1 on error
 *
 *   int  pogls_verify_batch(GeoNetBatchRec *recs, int n)
 *        → checks n records: spoke in 0..5, inv_spoke == (spoke+3)%6
 *        → returns number of PASS records (n = all pass)
 *
 * GeoNetAddr packed layout (8 bytes per chunk, cache-line friendly):
 *   [0] spoke       uint8
 *   [1] inv_spoke   uint8
 *   [2] face        uint8
 *   [3] unit        uint8
 *   [4] group       uint8
 *   [5] mirror_mask uint8
 *   [6] is_center   uint8
 *   [7] is_audit    uint8   (unit % 8 == 7)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/geo_config.h"
#include "core/geo_cylinder.h"
#include "core/geo_thirdeye.h"
/* geo_net_patched.h = core/geo_net.h with te_tick 5→4 arg (S28 fix, matches geo_thirdeye.h in this zip) */
#include "geo_net_patched.h"

/* ── opaque handle ────────────────────────────────────────────────── */

typedef struct {
    GeoNet  gn;
    GeoSeed seed;
} GeoNetCtxInternal;

typedef void* GeoNetHandle;

/* ── batch record for verify ─────────────────────────────────────── */

typedef struct {
    uint8_t spoke;
    uint8_t inv_spoke;
    uint8_t face;
    uint8_t unit;
    uint8_t group;
    uint8_t mirror_mask;
    uint8_t is_center;
    uint8_t is_audit;
} GeoNetPackedAddr;   /* 8 bytes — matches out layout */

/* public alias */
typedef GeoNetPackedAddr GeoNetBatchRec;

/* ── lifecycle ────────────────────────────────────────────────────── */

GeoNetHandle geonet_open(void) {
    GeoNetCtxInternal *ctx = calloc(1, sizeof(GeoNetCtxInternal));
    if (!ctx) return NULL;
    /* canonical seed — same as geo_net_py.py default */
    ctx->seed.gen2 = 0xDEADBEEFCAFEBABEULL;
    ctx->seed.gen3 = 0x123456789ABCDEF0ULL;
    geo_net_init(&ctx->gn, ctx->seed);
    return (GeoNetHandle)ctx;
}

void geonet_close(GeoNetHandle h) {
    if (h) free(h);
}

/* ── pogls_fetch_chunk ────────────────────────────────────────────── */
/*
 * Route single chunk idx through GeoNet.
 * out64: caller-allocated 64-byte buffer.
 * We write packed GeoNetAddr (8 bytes) at offset 0; remaining 56 bytes zeroed.
 * This gives caller a full cache-line per chunk — zero false sharing.
 */
int pogls_fetch_chunk(GeoNetHandle h, uint32_t idx, uint8_t *out64) {
    if (!h || !out64) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;

    GeoNetAddr a = geo_net_route(&ctx->gn, (uint64_t)idx, 0, 0, ctx->seed);

    uint8_t is_audit = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;

    /* pack into first 8 bytes */
    out64[0] = a.spoke;
    out64[1] = a.inv_spoke;
    out64[2] = a.face;
    out64[3] = a.unit;
    out64[4] = a.group;
    out64[5] = a.mirror_mask;
    out64[6] = a.is_center;
    out64[7] = is_audit;

    /* zero remaining 56 bytes (future: store value/checksum here) */
    memset(out64 + 8, 0, 56);
    return 0;
}

/* ── pogls_fetch_range ────────────────────────────────────────────── */
/*
 * Route [off .. off+len) in one C loop — the hot path.
 * out: caller allocates len * 8 bytes.
 * Each 8-byte slot = packed GeoNetAddr for chunk (off + i).
 *
 * Key: ThirdEye state accumulates across the full range — identical to
 * calling pogls_fetch_chunk len times in order, but no Python overhead.
 */
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

/* ── pogls_verify_batch ───────────────────────────────────────────── */
/*
 * Verify n GeoNetBatchRec entries in one C loop:
 *   - spoke in [0..5]
 *   - inv_spoke == (spoke + 3) % 6
 *   - face in [0..8]
 * Returns count of PASS records. n == all_pass iff return == n.
 */
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

/* ── state query ──────────────────────────────────────────────────── */

uint8_t geonet_state(GeoNetHandle h) {
    if (!h) return 0;
    return geo_net_state(&((GeoNetCtxInternal *)h)->gn);
}

uint32_t geonet_op_count(GeoNetHandle h) {
    if (!h) return 0;
    return ((GeoNetCtxInternal *)h)->gn.op_count;
}

/* mirror_mask for a given spoke from current ThirdEye state */
uint8_t geonet_mirror_mask(GeoNetHandle h, uint8_t spoke) {
    if (!h) return 0;
    ThirdEye *te = &((GeoNetCtxInternal *)h)->gn.eye;
    return te_get_mask(te, spoke % 6);
}
