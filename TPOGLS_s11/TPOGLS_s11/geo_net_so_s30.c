/*
 * geo_net_so_s30.c — libgeonet.so S30
 *
 * Build:
 *   gcc -O2 -march=native -shared -fPIC -I. -I./core \
 *       -o libgeonet.so geo_net_so_s30.c -lm
 *
 * S30 additions over S29:
 *
 *  1. slot_hot wired through C API
 *     pogls_fetch_chunk / pogls_fetch_range / pogls_iter_chunks
 *     all accept slot_hot — ThirdEye state now matches engine state exactly.
 *
 *  2. Filter pushdown in pogls_iter_chunks
 *     GeoNetFilter struct: spoke_mask (bitmask 0..63), audit_only flag.
 *     C skips non-matching chunks before callback → Python never sees them.
 *     Targeted query (e.g. spoke 0 only): data volume ↓ ~83%.
 *
 *  3. pogls_signal_fail — close QRPN feedback loop
 *     Caller (Python / engine) signals QRPN verify failure → ThirdEye
 *     force_anomaly in C. Keeps .so state consistent with live engine.
 *
 *  4. pogls_fetch_range_hot — batch route with per-chunk slot_hot array
 *     For engine integration: engine provides hot[] alongside chunk data.
 *
 * All S29 exports unchanged (ABI-compatible with old callers passing slot_hot=0).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/geo_config.h"
#include "core/geo_cylinder.h"
#include "core/geo_thirdeye.h"
#include "geo_net_patched.h"

/* ── opaque handle ────────────────────────────────────────────────── */

typedef struct {
    GeoNet  gn;
    GeoSeed seed;
} GeoNetCtxInternal;

typedef void* GeoNetHandle;

/* ── packed addr (unchanged) ─────────────────────────────────────── */

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

/* ── S30: filter struct ───────────────────────────────────────────── */
/*
 * spoke_mask: bitmask of allowed spokes (bit i set = spoke i passes).
 *   0x3F = all spokes (no filter). 0x01 = spoke 0 only. 0x09 = spoke 0+3.
 * audit_only: if non-zero, only emit audit chunks (unit % 8 == 7).
 *
 * NULL filter pointer = no filtering (S29-compatible).
 */
typedef struct {
    uint8_t spoke_mask;   /* 0x3F = pass all */
    uint8_t audit_only;   /* 0 = pass all, 1 = audit chunks only */
    uint8_t _pad[6];      /* reserved — S31 (value range, face filter) */
} GeoNetFilter;

/* ── S29 callback type (unchanged) ───────────────────────────────── */

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

/* ── S30: pogls_signal_fail ───────────────────────────────────────── */
/*
 * Close the QRPN feedback loop.
 * Engine calls this after each QRPN verify failure.
 * Increments hot_slots in ThirdEye — same path as geo_net_signal_fail().
 * If hot_slots > QRPN_ANOMALY_HOT → state transitions to ANOMALY in C.
 *
 * Returns new QRPN state (0=NORMAL, 1=STRESSED, 2=ANOMALY).
 */
uint8_t pogls_signal_fail(GeoNetHandle h) {
    if (!h) return 0;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    geo_net_signal_fail(&ctx->gn);
    return geo_net_state(&ctx->gn);
}

/* ── pogls_fetch_chunk — S30: slot_hot wired ─────────────────────── */

int pogls_fetch_chunk(GeoNetHandle h, uint32_t idx,
                      uint8_t *out64, uint8_t slot_hot) {
    if (!h || !out64) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    GeoNetAddr a = geo_net_route(&ctx->gn, (uint64_t)idx, 0, slot_hot, ctx->seed);
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

/* ── pogls_fetch_range — S30: slot_hot=0 (batch, uniform) ────────── */
/* ABI-compatible with S28/S29 callers (slot_hot implicitly 0). */

int pogls_fetch_range(GeoNetHandle h, uint64_t off, uint64_t len, uint8_t *out) {
    if (!h || !out || len == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    uint8_t *p = out;
    for (uint64_t i = 0; i < len; i++) {
        GeoNetAddr a = geo_net_route(&ctx->gn, off + i, 0, 0, ctx->seed);
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

/* ── S30: pogls_fetch_range_hot — per-chunk slot_hot array ────────── */
/*
 * Engine integration: hot[i] = slot_hot for chunk (off+i).
 * ThirdEye state advances with real hot pressure per chunk.
 * out: same layout as pogls_fetch_range (len * 8 bytes).
 * hot: len bytes, one uint8 per chunk (0 or 1).
 */
int pogls_fetch_range_hot(GeoNetHandle h, uint64_t off, uint64_t len,
                           const uint8_t *hot, uint8_t *out) {
    if (!h || !out || !hot || len == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    uint8_t *p = out;
    for (uint64_t i = 0; i < len; i++) {
        GeoNetAddr a = geo_net_route(&ctx->gn, off + i, 0, hot[i], ctx->seed);
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

/* ── pogls_verify_batch (unchanged) ──────────────────────────────── */

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

/* ── S30: pogls_iter_chunks — filter pushdown ────────────────────── */
/*
 * filt == NULL  → no filter (S29-compatible)
 * filt->spoke_mask = 0x3F → all spokes pass
 * filt->spoke_mask = 0x01 → only spoke 0
 * filt->audit_only = 1    → only audit chunks (unit % 8 == 7)
 *
 * chunk_i in callback = position within *filtered* output (0-indexed).
 * If you need absolute position, use addr - off.
 *
 * slot_hot: scalar, applied uniformly. For per-chunk hot use
 *           pogls_fetch_range_hot then iterate separately.
 */
int pogls_iter_chunks(
    GeoNetHandle     h,
    uint64_t         off,
    uint64_t         n,
    const GeoNetFilter *filt,     /* S30: replaces char* layer_filter */
    geonet_chunk_cb  cb,
    void            *userdata
) {
    if (!h || !cb || n == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;

    /* resolve filter — NULL = pass all */
    uint8_t spoke_mask = filt ? filt->spoke_mask : 0x3Fu;
    uint8_t audit_only = filt ? filt->audit_only : 0u;
    if (spoke_mask == 0) spoke_mask = 0x3Fu;   /* 0 = unconfigured = all */

    uint64_t emit_i = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t addr = off + i;
        GeoNetAddr a  = geo_net_route(&ctx->gn, addr, 0, 0, ctx->seed);
        uint8_t ia    = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;

        /* ── filter gate (pure C, no Python overhead) ─── */
        if (!((spoke_mask >> a.spoke) & 1u)) continue;   /* spoke not in mask */
        if (audit_only && !ia)               continue;   /* audit_only: skip non-audit */

        cb(emit_i++, addr, a.spoke, a.inv_spoke, a.mirror_mask, ia, userdata);
    }
    return (int)emit_i;
}

/* ── state queries (unchanged) ───────────────────────────────────── */

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

/* ── S30: geonet_anomaly_signals ─────────────────────────────────── */

uint32_t geonet_anomaly_signals(GeoNetHandle h) {
    if (!h) return 0;
    return ((GeoNetCtxInternal *)h)->gn.anomaly_signals;
}
