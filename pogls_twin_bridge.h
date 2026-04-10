/*
 * pogls_twin_bridge.h — POGLS ↔ Twin Geo Shared Bridge (S10)
 * ═══════════════════════════════════════════════════════════
 * FIX S10.1: bundle copied into struct → pw.bundle ชี้ internal field
 *   แทน external pointer ที่อาจ dangle หรือ misalign ผ่าน struct layout
 *
 * Debug: define TWIN_BRIDGE_DEBUG ก่อน include เพื่อเปิด assert
 * ═══════════════════════════════════════════════════════════
 */

#ifndef POGLS_TWIN_BRIDGE_H
#define POGLS_TWIN_BRIDGE_H

#include <stdint.h>
#include <string.h>

#ifdef TWIN_BRIDGE_DEBUG
#  include <stdio.h>
#  include <assert.h>
#  define TB_ASSERT_BUNDLE(b) do { \
       printf("[TB_DBG] pw.bundle=%p  &bundle[0]=%p  match=%d\n", \
              (void*)(b)->pw.bundle, (void*)(b)->bundle, \
              (b)->pw.bundle == (b)->bundle); \
       assert((b)->pw.bundle != NULL); \
       assert((b)->pw.bundle == (b)->bundle); \
   } while(0)
#else
#  define TB_ASSERT_BUNDLE(b) ((void)0)
#endif

/* POGLS core */
#include "core/geo_config.h"
#include "geo_fibo_clock.h"
#include "pogls_pipeline_wire.h"

/* Twin Geo */
#include "geo_hardening_whe.h"
#include "geo_diamond_field.h"
#include "geo_dodeca.h"

/* ── constants ──────────────────────────────────────────────────── */
#define TWIN_BRIDGE_VERSION   10u

/* ── bridge context ─────────────────────────────────────────────── */
/*
 * FIX: bundle[GEO_BUNDLE_WORDS] อยู่ใน struct
 *   → pw.bundle = &b->bundle[0]  (set ใน twin_bridge_init)
 *   → ไม่มี dangling pointer แม้ caller-side array หมด scope
 *
 * เปรียบ: เหมือน SQLite เก็บ page cache ใน sqlite3 struct เอง
 *   ไม่ให้ caller ถือ pointer แล้วส่งมาทีหลัง
 */
typedef struct {
    /* shared timebase */
    FiboCtx        fibo;

    /* POGLS side */
    PipelineWire   pw;
    uint64_t       bundle[GEO_BUNDLE_WORDS];  /* FIX: internal copy */

    /* Twin Geo side */
    DodecaTable    dodeca;
    DiamondFlowCtx flow;
    uint64_t       baseline;

    /* stats */
    uint32_t       twin_writes;
    uint32_t       flush_count;
    uint32_t       total_ops;
} TwinBridge;

/* S11: include after TwinBridge typedef so integration helper compiles */
#define S11_BRIDGE_INTEGRATION
#include "geo_s11.h"

/* ── init ───────────────────────────────────────────────────────── */
static inline void twin_bridge_init(TwinBridge         *b,
                                    GeoSeed             seed,
                                    const uint64_t     *bundle)  /* must not be NULL */
{
    memset(b, 0, sizeof(*b));

    /* FIX: copy bundle into struct BEFORE pipeline_wire_init */
    memcpy(b->bundle, bundle, GEO_BUNDLE_WORDS * sizeof(uint64_t));

    fibo_ctx_init(&b->fibo);
    fibo_ctx_set_seed(&b->fibo, seed);

    /* pw.bundle → internal field, survives struct lifetime */
    pipeline_wire_init(&b->pw, seed, b->bundle);

    dodeca_init(&b->dodeca);
    diamond_flow_init(&b->flow);
    b->baseline = diamond_baseline();

    TB_ASSERT_BUNDLE(b);
}

/* ── raw mix ────────────────────────────────────────────────────── */
static inline uint64_t _twin_raw(const TwinBridge *b,
                                  uint64_t addr, uint64_t value)
{
    uint64_t tag = (uint64_t)b->fibo.clk.c144;
    return addr ^ value ^ b->fibo.seed.gen3 ^ tag;
}

/* ── per-op write (hot path) ────────────────────────────────────── */
static inline FiboEvent twin_bridge_write(
    TwinBridge    *b,
    uint64_t       addr,
    uint64_t       value,
    uint8_t        slot_hot,
    PipelineResult *res_out)
{
    TB_ASSERT_BUNDLE(b);   /* [1] verify pointer ก่อนทุก write */

    uint64_t raw = _twin_raw(b, addr, value);
    uint32_t fx  = (uint32_t)(raw & 0xFFFFFFFFu);

    FiboEvent ev = fibo_clock_tick(&b->fibo, fx);

    PipelineResult res;
    pipeline_wire_process(&b->pw, addr, value, slot_hot, &res);
    if (res_out) *res_out = res;

    /* S11-C: apply QRPN ORB mask — expands spoke routing bitmap */
    qrpn_orb_apply(&b->fibo, &res.pkt);
    if (res_out) res_out->pkt = res.pkt;

    /* ── theta_map → core_raw → intersect (aligned with fused_write/geo_read) ──
     * raw passes through theta_mix64 to extract face/edge geometry,
     * then packed into core_raw before geo_fast_intersect.
     * geo_read_by_raw mirrors this exact path for correct roundtrip. */
    uint64_t h    = theta_mix64(raw);
    uint32_t h_hi = (uint32_t)(h >> 32);
    uint32_t h_lo = (uint32_t)(h & 0xFFFFFFFFu);
    uint8_t  face = (uint8_t)(((uint64_t)h_hi * 12u) >> 32);
    uint8_t  edge = (uint8_t)(((uint64_t)h_lo *  5u) >> 32);
    uint64_t core_raw = ((uint64_t)face << 59)
                      | ((uint64_t)edge << 52)
                      | (raw & UINT64_C(0x000FFFFFFFFFFFFF));

    uint64_t isect = geo_fast_intersect(core_raw);

    if ((core_raw & 7u) == 0u)
        b->flow.drift_acc +=
            (uint32_t)__builtin_popcountll(b->baseline & ~isect);

    uint64_t r_next = diamond_route_update(b->flow.route_addr, isect);
    r_next = geo_route_addr_guard(r_next, isect);

    int at_end = (ev & FIBO_EV_FLUSH)
              || (isect == 0)
              || (b->flow.drift_acc > 72u)
              || (b->flow.hop_count >= DIAMOND_HOP_MAX);

    if (at_end) {
        uint8_t offset = (uint8_t)(
            __builtin_popcountll(b->baseline & ~isect) & 0xFFu);
        dodeca_insert(&b->dodeca, r_next, 0, 0, offset,
                      b->flow.hop_count, 0);
        b->twin_writes++;
        diamond_flow_init(&b->flow);
        if (ev & FIBO_EV_FLUSH) b->flush_count++;
    } else {
        b->flow.route_addr = r_next;
        b->flow.hop_count++;
    }

    b->total_ops++;
    return ev;
}

/* ── batch write (S11: uses te_batch_accumulate, breaks serial te_tick chain) ── */
static inline FiboEvent twin_bridge_batch(
    TwinBridge     *b,
    const uint64_t *addrs,
    const uint64_t *values,
    uint32_t        n,
    uint8_t         slot_hot)
{
    if (!b || !addrs || n == 0) return FIBO_EV_NONE;

    /* pre-compute per-op spoke/hot/drift for batch ThirdEye flush */
    uint8_t  spokes[n], hots[n];
    uint32_t drifts[n];
    uint32_t prev_fx = b->fibo.prev_fx;

    FiboEvent ev_all = FIBO_EV_NONE;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t v   = values ? values[i] : 0;
        uint64_t raw = addrs[i] ^ v ^ b->fibo.seed.gen3
                       ^ (uint64_t)b->fibo.clk.c144;
        uint32_t fx  = (uint32_t)(raw & 0xFFFFFFFFu);
        uint32_t d   = (fx > prev_fx) ? fx - prev_fx : prev_fx - fx;
        prev_fx = fx;

        spokes[i] = (uint8_t)(b->fibo.seed.gen2 & 0x7u) % 6u;
        hots[i]   = (d > 4u) ? 1u : slot_hot;
        drifts[i] = d;

        /* clock L0/L1/L3 still per-op (SIG/DRIFT/SNAP timers) */
        FiboEvent ev = fibo_clock_tick(&b->fibo, fx);
        ev_all |= ev;

        twin_bridge_write(b, addrs[i], v, slot_hot, NULL);
    }

    /* S11-A: single batch flush into ThirdEye (replaces N serial te_tick) */
    te_batch_accumulate(&b->fibo.eye, b->fibo.seed, spokes, hots, drifts, n);

    return ev_all;
}

/* ── flush remaining flow ───────────────────────────────────────── */
static inline void twin_bridge_flush(TwinBridge *b)
{
    if (b->flow.route_addr == 0) return;
    uint8_t offset = (uint8_t)(b->flow.drift_acc & 0xFFu);
    dodeca_insert(&b->dodeca, b->flow.route_addr, 0, 0, offset,
                  b->flow.hop_count, 0);
    b->twin_writes++;
    diamond_flow_init(&b->flow);
}

/* ── stats ──────────────────────────────────────────────────────── */
typedef struct {
    uint32_t total_ops;
    uint32_t twin_writes;
    uint32_t flush_count;
    uint32_t qrpn_fails;
    float    write_density;
} TwinBridgeStats;

static inline TwinBridgeStats twin_bridge_stats(const TwinBridge *b)
{
    TwinBridgeStats s;
    s.total_ops     = b->total_ops;
    s.twin_writes   = b->twin_writes;
    s.flush_count   = b->flush_count;
    s.qrpn_fails    = b->pw.qrpn_fails;
    s.write_density = b->total_ops > 0
                    ? (float)b->twin_writes / (float)b->total_ops
                    : 0.0f;
    return s;
}

#endif /* POGLS_TWIN_BRIDGE_H */
