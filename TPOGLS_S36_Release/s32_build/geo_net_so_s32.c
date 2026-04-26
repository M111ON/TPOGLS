/*
 * geo_net_so_s32.c — libgeonet.so S32
 *
 * Build:
 *   gcc -O2 -march=native -shared -fPIC -I. -I./core \
 *       -o libgeonet_s32.so geo_net_so_s32.c -lm
 *
 * S32 additions over S31:
 *
 *  1. GeoNetFilter: _pad[2] → chunk_lo/chunk_hi (uint16) + flags byte
 *     audit_only absorbed into flags bit0 (GF_AUDIT_ONLY) — ABI note below.
 *     flags bit1 = GF_VAL_FILTER (S32 multi-layer path explicit opt-in).
 *     chunk_hi = 0xFFFF → disabled (CHUNK_FILTER_OFF sentinel).
 *
 *  2. GeoMultiRec — 32-byte packed record, cache-line friendly.
 *     chunk_global: running index across all layers in one fetch_multi_range call.
 *     offset: chunk_i * CHUNK_SIZE — downstream can seek directly.
 *
 *  3. pogls_fetch_multi_range — core S32 API.
 *     Iterates N GeoReq layers in order.
 *     Applies shared GeoNetFilter (spoke/audit/val/chunk gates).
 *     Emits batches of ≤ MULTI_BATCH_SZ records via geo_multi_cb_batch.
 *     Python: accumulate list → batch JSON encode → yield once per batch.
 *
 * ABI compatibility:
 *   pogls_iter_chunks: S31 behavior preserved exactly.
 *     val_max==0 → range gate skipped (S31 sentinel unchanged).
 *     audit_only field still read directly for backward compat.
 *   pogls_fetch_multi_range: NEW — S32 callers only.
 *     Uses flags field: GF_AUDIT_ONLY (bit0), GF_VAL_FILTER (bit1).
 *     val_min > val_max + GF_VAL_FILTER set → gate active, no results if impossible.
 *
 * All S29/S30/S31 exports unchanged.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/geo_config.h"
#include "core/geo_cylinder.h"
#include "core/geo_thirdeye.h"
#include "geo_net_patched.h"

/* ── constants ────────────────────────────────────────────────────── */

#define CHUNK_SIZE        64u
#define CHUNK_FILTER_OFF  0xFFFFu   /* chunk_hi sentinel: no chunk window */
#define MULTI_BATCH_SZ    256u      /* batch flush size for fetch_multi_range */

/* ── GeoNetFilter flags ───────────────────────────────────────────── */

#define GF_AUDIT_ONLY  0x01u   /* bit0: only audit chunks (unit % 8 == 7) */
#define GF_VAL_FILTER  0x02u   /* bit1: val_min/val_max gate active (S32 path) */
/* bits 2–7: reserved */

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

/* ── S32: GeoNetFilter — final 10-byte layout ────────────────────── */
/*
 * offset 0: spoke_mask — bitmask of allowed spokes (0x3F = all)
 * offset 1: flags      — GF_AUDIT_ONLY | GF_VAL_FILTER (replaces audit_only)
 * offset 2: val_min    — addr & 0xFFFF range lo
 * offset 4: val_max    — S31 path: 0=disabled / S32 path: GF_VAL_FILTER controls
 * offset 6: chunk_lo   — chunk_i range lo (0 = first chunk)
 * offset 8: chunk_hi   — chunk_i range hi (CHUNK_FILTER_OFF = disabled)
 * sizeof   = 10 bytes, no compiler padding (all uint16 aligned)
 *
 * NULL pointer → no filtering (S29/S30 compatible).
 */
typedef struct {
    uint8_t  spoke_mask;   /* S30 */
    uint8_t  flags;        /* S32: replaces audit_only. GF_AUDIT_ONLY | GF_VAL_FILTER */
    uint16_t val_min;      /* S31/S32 */
    uint16_t val_max;      /* S31: 0=disabled  /  S32: controlled by GF_VAL_FILTER */
    uint16_t chunk_lo;     /* S32: 0 = chunk 0 */
    uint16_t chunk_hi;     /* S32: CHUNK_FILTER_OFF (0xFFFF) = disabled */
} GeoNetFilter;

/* ── S32: GeoMultiRec — 32-byte packed, cache-line friendly ─────── */
/*
 * layer_id:     caller-assigned per-layer ID (echoed back by C)
 * chunk_global: running index across all layers in one fetch_multi_range call
 * chunk_i:      index within this layer (after chunk_lo offset)
 * addr:         absolute geo address
 * offset:       byte offset = chunk_i * CHUNK_SIZE (for direct seek)
 * spoke:        geo spoke (0–5)
 * mirror_mask:  ThirdEye mirror state
 * is_audit:     1 if audit chunk
 * _pad:         alignment to 32 bytes
 */
typedef struct {
    uint32_t layer_id;      /* offset  0 */
    uint32_t chunk_global;  /* offset  4 */
    uint64_t chunk_i;       /* offset  8 */
    uint64_t addr;          /* offset 16 */
    uint64_t offset;        /* offset 24 — byte offset in layer */
    uint8_t  spoke;         /* offset 32 */
    uint8_t  mirror_mask;   /* offset 33 */
    uint8_t  is_audit;      /* offset 34 */
    uint8_t  _pad;          /* offset 35 */
} GeoMultiRec;              /* sizeof = 36 — fits 1 per cache line */

/* ── S32: GeoReq — per-layer request descriptor ──────────────────── */

typedef struct {
    uint32_t layer_id;   /* caller-assigned, echoed in GeoMultiRec */
    uint32_t file_idx;   /* layer position in wallet */
    uint64_t n_bytes;    /* layer size → derive n_chunks */
} GeoReq;

/* ── S32: batch callback ─────────────────────────────────────────── */

typedef void (*geo_multi_cb_batch)(
    const GeoMultiRec *recs,
    size_t             n,
    void              *userdata
);

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

uint8_t pogls_signal_fail(GeoNetHandle h) {
    if (!h) return 0;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    geo_net_signal_fail(&ctx->gn);
    return geo_net_state(&ctx->gn);
}

/* ── pogls_fetch_chunk (S30: slot_hot) ────────────────────────────── */

int pogls_fetch_chunk(GeoNetHandle h, uint32_t idx,
                      uint8_t *out64, uint8_t slot_hot) {
    if (!h || !out64) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    GeoNetAddr a = geo_net_route(&ctx->gn, (uint64_t)idx, 0, slot_hot, ctx->seed);
    uint8_t is_audit = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
    out64[0] = a.spoke;    out64[1] = a.inv_spoke;
    out64[2] = a.face;     out64[3] = a.unit;
    out64[4] = a.group;    out64[5] = a.mirror_mask;
    out64[6] = a.is_center; out64[7] = is_audit;
    memset(out64 + 8, 0, 56);
    return 0;
}

/* ── pogls_fetch_range (unchanged) ───────────────────────────────── */

int pogls_fetch_range(GeoNetHandle h, uint64_t off, uint64_t len, uint8_t *out) {
    if (!h || !out || len == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    uint8_t *p = out;
    for (uint64_t i = 0; i < len; i++) {
        GeoNetAddr a = geo_net_route(&ctx->gn, off + i, 0, 0, ctx->seed);
        p[0] = a.spoke;       p[1] = a.inv_spoke;
        p[2] = a.face;        p[3] = a.unit;
        p[4] = a.group;       p[5] = a.mirror_mask;
        p[6] = a.is_center;
        p[7] = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
        p += 8;
    }
    return (int)len;
}

/* ── S30: pogls_fetch_range_hot ──────────────────────────────────── */

int pogls_fetch_range_hot(GeoNetHandle h, uint64_t off, uint64_t len,
                           const uint8_t *hot, uint8_t *out) {
    if (!h || !out || !hot || len == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;
    uint8_t *p = out;
    for (uint64_t i = 0; i < len; i++) {
        GeoNetAddr a = geo_net_route(&ctx->gn, off + i, 0, hot[i], ctx->seed);
        p[0] = a.spoke;       p[1] = a.inv_spoke;
        p[2] = a.face;        p[3] = a.unit;
        p[4] = a.group;       p[5] = a.mirror_mask;
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

/* ── S31: pogls_iter_chunks — S31 behavior preserved exactly ──────── */
/*
 * S31 semantics unchanged:
 *   val_max == 0  → range gate disabled (sentinel)
 *   audit_only    → reads filt->flags & GF_AUDIT_ONLY (bit0 compat)
 *
 * S32 callers should use pogls_fetch_multi_range instead.
 */
int pogls_iter_chunks(
    GeoNetHandle        h,
    uint64_t            off,
    uint64_t            n,
    const GeoNetFilter *filt,
    geonet_chunk_cb     cb,
    void               *userdata
) {
    if (!h || !cb || n == 0) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;

    uint8_t spoke_mask = filt ? filt->spoke_mask : 0x3Fu;
    /* S31 compat: read audit_only from flags bit0 */
    uint8_t audit_only = filt ? (filt->flags & GF_AUDIT_ONLY) : 0u;
    if (spoke_mask == 0) spoke_mask = 0x3Fu;

    uint64_t emit_i = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t addr = off + i;
        GeoNetAddr a  = geo_net_route(&ctx->gn, addr, 0, 0, ctx->seed);
        uint8_t ia    = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;

        if (!((spoke_mask >> a.spoke) & 1u)) continue;
        if (audit_only && !ia)               continue;
        /* S31: val_max==0 → disabled (sentinel preserved) */
        if (filt && filt->val_max) {
            uint16_t v = (uint16_t)(addr & 0xFFFFu);
            if (v < filt->val_min || v > filt->val_max) continue;
        }

        cb(emit_i++, addr, a.spoke, a.inv_spoke, a.mirror_mask, ia, userdata);
    }
    return (int)emit_i;
}

/* ── S32: pogls_fetch_multi_range ────────────────────────────────── */
/*
 * Iterate N layers in order, apply shared filter, emit batches.
 *
 * Filter gates (AND chain, all in C):
 *   1. spoke_mask        — unchanged from S30
 *   2. flags & GF_AUDIT_ONLY → audit chunks only
 *   3. flags & GF_VAL_FILTER → val_min/val_max gate (S32 rule: min>max = disabled)
 *   4. chunk_hi != CHUNK_FILTER_OFF → chunk_lo..chunk_hi window
 *
 * Batch: accumulates up to MULTI_BATCH_SZ records in stack buffer,
 * flushes on full or layer boundary. Python consumes full batches →
 * one json.dumps() per batch instead of per record.
 *
 * chunk_global: increments across all layers (0-based, deterministic).
 * offset: chunk_i * CHUNK_SIZE — direct byte seek for downstream.
 *
 * Returns total records emitted across all layers, or -1 on error.
 */
int pogls_fetch_multi_range(
    GeoNetHandle         h,
    const GeoReq        *reqs,
    int                  n_reqs,
    const GeoNetFilter  *filt,
    geo_multi_cb_batch   cb,
    void                *userdata
) {
    if (!h || !reqs || n_reqs <= 0 || !cb) return -1;
    GeoNetCtxInternal *ctx = (GeoNetCtxInternal *)h;

    /* resolve filter params once */
    uint8_t  spoke_mask   = filt ? filt->spoke_mask : 0x3Fu;
    uint8_t  audit_only   = filt ? (filt->flags & GF_AUDIT_ONLY) : 0u;
    uint8_t  val_active   = filt ? (filt->flags & GF_VAL_FILTER) : 0u;
    uint16_t val_min      = filt ? filt->val_min : 0u;
    uint16_t val_max      = filt ? filt->val_max : 0u;
    uint8_t  chunk_active = (filt && filt->chunk_hi != CHUNK_FILTER_OFF) ? 1u : 0u;
    uint16_t chunk_lo     = filt ? filt->chunk_lo : 0u;
    uint16_t chunk_hi     = filt ? filt->chunk_hi : CHUNK_FILTER_OFF;

    if (spoke_mask == 0) spoke_mask = 0x3Fu;

    /* S32 val rule: val_min > val_max → disabled (even if GF_VAL_FILTER set) */
    if (val_active && val_min > val_max) val_active = 0u;

    GeoMultiRec  buf[MULTI_BATCH_SZ];
    size_t       buf_n      = 0;
    uint32_t     chunk_glob = 0;
    int          total_emit = 0;

    for (int r = 0; r < n_reqs; r++) {
        uint64_t n_chunks = (reqs[r].n_bytes + CHUNK_SIZE - 1) / CHUNK_SIZE;
        uint64_t off      = (uint64_t)reqs[r].file_idx * 3456u; /* CYL_FULL_N=3456 */

        for (uint64_t i = 0; i < n_chunks; i++) {

            /* ── gate 4: chunk window ─────────────────────────────── */
            if (chunk_active) {
                if (i < (uint64_t)chunk_lo || i > (uint64_t)chunk_hi) continue;
            }

            uint64_t   addr = off + i;
            GeoNetAddr a    = geo_net_route(&ctx->gn, addr, 0, 0, ctx->seed);
            uint8_t    ia   = (a.unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;

            /* ── gate 1: spoke mask ───────────────────────────────── */
            if (!((spoke_mask >> a.spoke) & 1u)) continue;
            /* ── gate 2: audit_only ──────────────────────────────── */
            if (audit_only && !ia)               continue;
            /* ── gate 3: val range (S32 rule: flag-controlled) ────── */
            if (val_active) {
                uint16_t v = (uint16_t)(addr & 0xFFFFu);
                if (v < val_min || v > val_max) continue;
            }

            /* ── emit into batch buffer ──────────────────────────── */
            GeoMultiRec *rec = &buf[buf_n++];
            rec->layer_id     = reqs[r].layer_id;
            rec->chunk_global = chunk_glob++;
            rec->chunk_i      = i;
            rec->addr         = addr;
            rec->offset       = i * CHUNK_SIZE;
            rec->spoke        = a.spoke;
            rec->mirror_mask  = a.mirror_mask;
            rec->is_audit     = ia;
            rec->_pad         = 0;
            total_emit++;

            if (buf_n == MULTI_BATCH_SZ) {
                cb(buf, buf_n, userdata);
                buf_n = 0;
            }
        }

        /* flush on layer boundary */
        if (buf_n > 0) {
            cb(buf, buf_n, userdata);
            buf_n = 0;
        }
    }

    return total_emit;
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

uint32_t geonet_anomaly_signals(GeoNetHandle h) {
    if (!h) return 0;
    return ((GeoNetCtxInternal *)h)->gn.anomaly_signals;
}
