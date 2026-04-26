/*
 * geo_net.h — GeoNet: Radial Routing Layer
 * ═══════════════════════════════════════════════════════════════════
 *
 * Role: ROUTE (not verify, not store)
 *   value + addr → spoke + slot + mirror_mask → pipeline
 *
 * Stack position:
 *   [L3 Quad]  →  [GeoNet]  →  [geo_cylinder]  →  [Delta/GPU]
 *                                                       ↓
 *                                               [pogls_qrpn verify]
 *                                                       ↓
 *                                          fail → ThirdEye.force_anomaly()
 *
 * Wires:
 *   geo_cylinder.h  — spatial mapping (spoke/slot/face)
 *   geo_thirdeye.h  — adaptive observer (state + mirror mask)
 *
 * Number theory:
 *   3456 = 6 × 576 = 144 × 24  (full space)
 *   576  = 24² = 9 × 64        (slots per spoke)
 *   GROUP_SIZE = 8              (lines per audit group)
 *   AUDIT_DEPTH = 8             (reverse traverse depth)
 *   8 × 8 = 64 = 1 face ✓
 *
 * digit_sum: 3456→9  576→9  144→9  64→1(origin)  8→8
 */

#ifndef GEO_NET_H
#define GEO_NET_H

#include <stdint.h>
#include <string.h>
#include "geo_cylinder.h"
#include "geo_thirdeye.h"

/* ── Constants ─────────────────────────────────────────────────── */
#define GN_GROUP_SIZE    8u    /* Hilbert lines per audit group      */
#define GN_AUDIT_DEPTH   8u    /* reverse traverse depth             */
#define GN_GROUPS_FACE   8u    /* groups per face (64/8)             */
#define GN_LINE_MAX      CYL_FULL_N   /* 3456 total lines            */

/* ── Route result ───────────────────────────────────────────────── */
typedef struct {
    uint8_t  spoke;        /* 0..5                                   */
    uint16_t slot;         /* 0..575                                 */
    uint8_t  face;         /* 0..8  (8 = center)                     */
    uint8_t  unit;         /* 0..63 within face                      */
    uint8_t  inv_spoke;    /* mirror: (spoke+3)%6  O(1)              */
    uint8_t  mirror_mask;  /* adaptive: from ThirdEye state          */
    uint8_t  group;        /* line group index (0..7 per face)       */
    uint8_t  is_center;    /* slot >= 512                            */
} GeoNetAddr;

/* ── GeoNet context ─────────────────────────────────────────────── */
typedef struct {
    ThirdEye  eye;          /* adaptive observer                     */
    uint32_t  line_cursor;  /* current Hilbert line position         */
    uint32_t  op_count;     /* total ops routed                      */
    uint32_t  anomaly_signals; /* from pogls_qrpn fail feedback      */
} GeoNet;

/* ── Init ───────────────────────────────────────────────────────── */
static inline void geo_net_init(GeoNet *gn, GeoSeed seed) {
    memset(gn, 0, sizeof(GeoNet));
    te_init(&gn->eye, seed);
}

/* ── Core: fast_mod6 (Barrett) ──────────────────────────────────── */
static inline uint8_t _gn_mod6(uint32_t n) {
    uint32_t q = (n * 10923U) >> 16;
    return (uint8_t)(n - q * 6U);
}

/* ── Route: value+addr → GeoNetAddr ────────────────────────────── */
/*
 * Hilbert-cylinder mapping:
 *   full_idx = addr % CYL_FULL_N   → position in 3456 space
 *   spoke    = full_idx % 6        → which spoke (fast_mod6)
 *   slot     = full_idx / 6        → depth on spoke (0..575)
 *
 * slot_hot: caller passes density signal (1 if slot busy)
 * GeoSeed cur: current engine seed for ThirdEye snapshot
 */
static inline GeoNetAddr geo_net_route(GeoNet    *gn,
                                        uint64_t   addr,
                                        uint64_t   value,
                                        uint8_t    slot_hot,
                                        GeoSeed    cur)
{
    (void)value;  /* reserved for future value-aware routing */

    /* map addr → cylinder position */
    uint16_t full_idx = (uint16_t)(addr % CYL_FULL_N);
    uint8_t  spoke    = _gn_mod6(full_idx);
    uint16_t slot     = full_idx / CYL_SPOKES;
    uint8_t  face     = geo_slot_face(slot);
    uint8_t  unit     = geo_slot_unit(slot);
    uint8_t  inv      = geo_spoke_invert(spoke);
    uint8_t  group    = unit / GN_GROUP_SIZE;  /* 0..7 within face */

    /* ThirdEye tick → adaptive state */
    te_tick(&gn->eye, cur, spoke, slot_hot);

    /* mirror mask from current state */
    uint8_t mask = te_get_mask(&gn->eye, spoke);

    gn->op_count++;

    return (GeoNetAddr){
        .spoke       = spoke,
        .slot        = slot,
        .face        = face,
        .unit        = unit,
        .inv_spoke   = inv,
        .mirror_mask = mask,
        .group       = group,
        .is_center   = geo_slot_is_center(slot),
    };
}

/* ── Audit group: reverse traverse (neutral wire) ───────────────── */
/*
 * Every GN_GROUP_SIZE lines → audit reverse (group boundary)
 * Caller checks: forward[i] XOR reverse[GN_GROUP_SIZE-1-i] == expected
 *
 * Returns 1 if addr is at group boundary (audit trigger)
 */
static inline uint8_t geo_net_is_audit_point(const GeoNetAddr *a) {
    return (a->unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
}

/* ── QRPN fail feedback → force ThirdEye ANOMALY ────────────────── */
/*
 * Called by caller after pogls_qrpn verify fails.
 * Injects corruption signal into ThirdEye observer.
 *
 * 1 fail = +1 hot_slot signal
 * hot > QRPN_ANOMALY_HOT (96) → ANOMALY triggers automatically
 */
static inline void geo_net_signal_fail(GeoNet *gn) {
    gn->anomaly_signals++;
    /* inject as hot slot — reuses existing threshold logic */
    gn->eye.cur.hot_slots++;
    /* force immediate state re-eval if critical */
    if (gn->eye.cur.hot_slots > QRPN_ANOMALY_HOT) {
        gn->eye.qrpn_state = QRPN_ANOMALY;
    }
}

/* ── Force rewind via ThirdEye ───────────────────────────────────── */
static inline GeoSeed geo_net_force_rewind(GeoNet *gn, int steps) {
    return te_rewind(&gn->eye, steps);
}

/* ── State query ────────────────────────────────────────────────── */
static inline uint8_t geo_net_state(const GeoNet *gn) {
    return gn->eye.qrpn_state;
}

static inline const char* geo_net_state_name(const GeoNet *gn) {
    return te_state_name(gn->eye.qrpn_state);
}

/* ── Status ─────────────────────────────────────────────────────── */
static inline void geo_net_status(const GeoNet *gn) {
    printf("[GeoNet] ops=%u  anomaly_signals=%u  state=%s\n",
           gn->op_count, gn->anomaly_signals,
           geo_net_state_name(gn));
    te_status(&gn->eye);
}

#endif /* GEO_NET_H */
