/*
 * pogls_stats_out.h — Stats output struct + .so boundary
 * ══════════════════════════════════════════════════════════════════════
 * S19 — fixed to match actual geo_kv.h + kv_bridge.h fields
 *
 * GeoKV real fields:  count, tombstones, puts, gets, misses, max_probe
 * KVBridge real fields: cpu(GeoKV), lanes[4](KVBLane), flush_count, dropped
 * KV_CAP = 8192 (frozen constant from geo_kv.h)
 * KVB_LANES = 4  (frozen constant from kv_bridge.h)
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_STATS_OUT_H
#define POGLS_STATS_OUT_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "pogls_sdk.h"   /* PoglsCtx → kv(KVBridge) → cpu(GeoKV) */

/* ── Output struct ───────────────────────────────────────────────── */
/* All uint64_t — ctypes c_uint64, no padding, ABI-stable.           */
/* Append-only: never reorder existing fields.                        */
typedef struct {
    uint64_t version;            /* sentinel = POGLS_STATS_VERSION    */

    /* PoglsCtx */
    uint64_t ctx_writes;         /* pogls_write() calls               */
    uint64_t ctx_reads;          /* pogls_read() calls                */
    uint64_t ctx_qrpns;          /* failed QRPN probes (→ kv_del)     */

    /* L1 GeoCache */
    uint64_t l1_hits;
    uint64_t l1_misses;
    uint64_t l1_hit_pct_x100;    /* hit% × 100  e.g. 9550 = 95.50%   */

    /* KV — GeoKV truth layer (cpu side of KVBridge) */
    uint64_t kv_capacity;        /* KV_CAP = 8192 (frozen)            */
    uint64_t kv_live;            /* GeoKV.count       ← gap closed ✓  */
    uint64_t kv_tomb;            /* GeoKV.tombstones  ← gap closed ✓  */
    uint64_t kv_load_pct_x100;   /* count/8192 × 10000                */
    uint64_t kv_tomb_pct_x100;   /* tombstones/8192 × 10000           */
    uint64_t kv_puts;            /* GeoKV.puts (raw insert calls)      */
    uint64_t kv_gets;            /* GeoKV.gets                        */
    uint64_t kv_misses;          /* GeoKV.misses                      */
    uint64_t kv_max_probe;       /* GeoKV.max_probe (collision depth) */

    /* GPU ring — KVBridge async side */
    uint64_t gpu_ring_pending;   /* sum(head-tail) across 4 lanes     */
    uint64_t gpu_ring_flushed;   /* KVBridge.flush_count              */
    uint64_t gpu_ring_dropped;   /* KVBridge.dropped (ring-full drops)*/

} PoglsStatsOut;

#define POGLS_STATS_VERSION    0x504F5302ULL   /* "POS\x02" — S19      */
#define POGLS_STATS_OUT_FIELDS 20              /* field count           */


/* ── Filler: PoglsCtx → PoglsStatsOut ───────────────────────────── */
static inline void pogls_stats_fill(const PoglsCtx *ctx, PoglsStatsOut *out)
{
    memset(out, 0, sizeof(*out));
    out->version = POGLS_STATS_VERSION;

    /* PoglsCtx counters */
    out->ctx_writes = ctx->writes;
    out->ctx_reads  = ctx->reads;
    out->ctx_qrpns  = ctx->qrpns;

    /* L1 GeoCache */
    uint64_t total_l1      = ctx->l1.hits + ctx->l1.misses;
    out->l1_hits           = ctx->l1.hits;
    out->l1_misses         = ctx->l1.misses;
    out->l1_hit_pct_x100   = total_l1
                             ? (ctx->l1.hits * 10000ULL / total_l1) : 0;

    /* KV — direct GeoKV fields (geo_kv.h confirmed) */
    const GeoKV *kv        = &ctx->kv.cpu;
    out->kv_capacity       = KV_CAP;                        /* 8192   */
    out->kv_live           = (uint64_t)kv->count;
    out->kv_tomb           = (uint64_t)kv->tombstones;
    out->kv_load_pct_x100  = kv->count      * 10000ULL / KV_CAP;
    out->kv_tomb_pct_x100  = kv->tombstones * 10000ULL / KV_CAP;
    out->kv_puts           = (uint64_t)kv->puts;
    out->kv_gets           = (uint64_t)kv->gets;
    out->kv_misses         = (uint64_t)kv->misses;
    out->kv_max_probe      = (uint64_t)kv->max_probe;

    /* GPU ring — sum (head-tail) across KVB_LANES=4 lanes */
    uint32_t pending = 0;
    for (int i = 0; i < KVB_LANES; i++) {
        uint32_t h = atomic_load_explicit(
                         &ctx->kv.lanes[i].head, memory_order_relaxed);
        uint32_t t = atomic_load_explicit(
                         &ctx->kv.lanes[i].tail, memory_order_relaxed);
        pending += (h - t);
    }
    out->gpu_ring_pending  = (uint64_t)pending;
    out->gpu_ring_flushed  = (uint64_t)ctx->kv.flush_count;
    out->gpu_ring_dropped  = (uint64_t)ctx->kv.dropped;
}


/* ── Convenience: pct as float ───────────────────────────────────── */
static inline float pogls_stats_load_pct   (const PoglsStatsOut *o)
    { return (float)o->kv_load_pct_x100 / 100.0f; }
static inline float pogls_stats_tomb_pct   (const PoglsStatsOut *o)
    { return (float)o->kv_tomb_pct_x100 / 100.0f; }
static inline float pogls_stats_l1_hit_pct (const PoglsStatsOut *o)
    { return (float)o->l1_hit_pct_x100  / 100.0f; }


/*
 * ── Python ctypes mirror (20 × c_uint64) ──────────────────────────
 *
 *   class PoglsStatsOut(ctypes.Structure):
 *       _fields_ = [(f, ctypes.c_uint64) for f in [
 *           "version",
 *           "ctx_writes", "ctx_reads", "ctx_qrpns",
 *           "l1_hits", "l1_misses", "l1_hit_pct_x100",
 *           "kv_capacity", "kv_live", "kv_tomb",
 *           "kv_load_pct_x100", "kv_tomb_pct_x100",
 *           "kv_puts", "kv_gets", "kv_misses", "kv_max_probe",
 *           "gpu_ring_pending", "gpu_ring_flushed", "gpu_ring_dropped",
 *       ]]
 *
 * ── .so wiring (pogls_sdk_so.c already updated) ───────────────────
 *
 *   void pogls_so_stats_out(PoglsHandle h, PoglsStatsOut *out) {
 *       pogls_stats_fill((PoglsCtx *)h, out);
 *   }
 */

#endif /* POGLS_STATS_OUT_H */
