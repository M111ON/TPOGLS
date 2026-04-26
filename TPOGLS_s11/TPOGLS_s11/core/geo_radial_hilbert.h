/*
 * geo_radial_hilbert.h — Radial Hilbert Mapping + Group Audit
 * ════════════════════════════════════════════════════════════
 * P2 of Geomatrix v3.3 / Session 5
 *
 * Design:
 *   B) Pseudo-Hilbert  — bit_interleave O(1), no branch, vectorizable
 *   C) Group Audit     — unit%8==7 trigger, XOR pair check
 *
 * 1 face = 64 units = 8 groups × 8 lines
 * hidx   = (lane<<3) | bit_reverse3(group)   → zig-zag locality
 *
 * Stack: GeoNet → [THIS] → pogls_geomatrix → pogls_qrpn
 */

#ifndef GEO_RADIAL_HILBERT_H
#define GEO_RADIAL_HILBERT_H

#include <stdint.h>
#include "geo_config.h"
#include "geo_net.h"   /* GeoNetAddr, GN_GROUP_SIZE */

/* ── Structs ─────────────────────────────────────────────────────── */

typedef struct {
    uint16_t unit;   /* 0..63  (face-local)                          */
    uint8_t  spoke;  /* 0..5                                         */
    uint8_t  group;  /* unit >> 3  (0..7)                            */
    uint8_t  hidx;   /* pseudo-hilbert index (0..63)                 */
} RHLine;

typedef struct {
    uint8_t  ok;         /* 1 = pass, 0 = mismatch                   */
    uint8_t  fail_pos;   /* first mismatch lane (0..3), 0xFF=none    */
    uint8_t  repair_spoke; /* inv_spoke used for repair              */
} RHAuditResult;

/* ── Bit helpers ─────────────────────────────────────────────────── */

/* reverse 3-bit value: 0b000↔0b000, 0b001↔0b100, etc. */
static inline uint8_t _rh_rev3(uint8_t v) {
    return (uint8_t)(((v & 1u) << 2) | (v & 2u) | ((v >> 2) & 1u));
}

/* ── RHLine from GeoNetAddr ──────────────────────────────────────── */

/*
 * map: GeoNetAddr → RHLine
 * hidx = (lane<<3) | bit_reverse3(group)  — zig-zag locality
 */
static inline RHLine rh_map(const GeoNetAddr *a) {
    uint8_t group = (uint8_t)(a->unit >> 3);          /* 0..7  */
    uint8_t lane  = (uint8_t)(a->unit & 7u);          /* 0..7  */
    uint8_t hidx  = (uint8_t)((lane << 3) | _rh_rev3(group));
    return (RHLine){
        .unit  = a->unit,
        .spoke = a->spoke,
        .group = group,
        .hidx  = hidx,
    };
}

/* ── Audit (Group boundary: lane==7) ─────────────────────────────── */

/*
 * XOR pair check: buf[i] ^ buf[7-i]  for i in 0..3
 * expected_xor = constant per design (default 0 = symmetric pairs)
 * repair spoke = (spoke+3)%6 — inv_spoke is source of truth
 */
static inline RHAuditResult rh_audit_group(
    const uint8_t buf[GEO_GROUP_SIZE],   /* 8 forward values       */
    uint8_t spoke,
    uint8_t expected_xor                 /* 0 for symmetric check  */
) {
    RHAuditResult r = { .ok = 1u, .fail_pos = 0xFFu,
                        .repair_spoke = (uint8_t)((spoke + 3u) % GEO_SPOKES) };
    for (uint8_t i = 0; i < 4u; i++) {
        if ((buf[i] ^ buf[7u - i]) != expected_xor) {
            r.ok = 0u;
            r.fail_pos = i;
            break;
        }
    }
    return r;
}

/* ── Mirror: apply GeoNet mask → active spokes ───────────────────── */

/*
 * Returns bitmask of active spokes for this line
 * ThirdEye state drives mask:
 *   NORMAL   (mask 0x03): primary + 1 neighbor
 *   STRESSED (mask 0x07): primary + inv + 1 neighbor
 *   ANOMALY  (mask 0x3F): all 6 spokes
 */
static inline uint8_t rh_mirror_spokes(uint8_t spoke, uint8_t mirror_mask) {
    uint8_t inv = (uint8_t)((spoke + 3u) % GEO_SPOKES);
    uint8_t out = (uint8_t)(1u << spoke);
    if (mirror_mask & 0x02u) out |= (uint8_t)(1u << inv);
    if (mirror_mask & 0x04u) out |= (uint8_t)(1u << ((spoke + 1u) % GEO_SPOKES));
    if (mirror_mask == 0x3Fu) out = 0x3Fu;
    return out;
}

/* ── Pipeline entry: full rh step for one op ─────────────────────── */

/*
 * Caller flow:
 *   rh_step() every op
 *   if result.do_audit → fill buf[8], call rh_audit_group()
 *   if audit fails     → geo_net_signal_fail()
 */
typedef struct {
    RHLine   line;
    uint8_t  active_spokes; /* bitmask from mirror             */
    uint8_t  do_audit;      /* 1 = lane==7, trigger audit now  */
} RHStep;

static inline RHStep rh_step(const GeoNetAddr *a) {
    RHLine l = rh_map(a);
    return (RHStep){
        .line          = l,
        .active_spokes = rh_mirror_spokes(a->spoke, a->mirror_mask),
        .do_audit      = (uint8_t)((a->unit & 7u) == 7u ? 1u : 0u),
    };
}

#endif /* GEO_RADIAL_HILBERT_H */
