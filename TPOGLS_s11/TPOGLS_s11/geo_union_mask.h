/*
 * geo_union_mask.h — Union Mask (Wolfram) : integer fold
 * ═══════════════════════════════════════════════════════
 * Session 14
 *
 * Model:
 *   input_A = write pressure  = writes / io_total      (per lane)
 *   input_B = qrpn fail rate  = fails  / qrpn_total    (per lane)
 *   fold    = (writes * fails * 144) / (io_total * qrpn_total)
 *
 * Zone borders (divisors of 144, 144%N==0):
 *   fold >= 72  → DANGER   polarity 3:-3
 *   fold >= 36  → NORMAL   polarity 4:-4
 *   fold <  36  → SAFE     compress 1/4 or 3/4
 *
 * Number anchor:
 *   72 = 144/2   border DANGER  (Pythagorean: 72 = 8×9 = 2³×3²)
 *   36 = 144/4   border NORMAL  (36 = 6²  digit_sum=9 ✓)
 *   144 = 12²    anchor         (15 divisors, highly composite)
 *
 * Cost:
 *   2 counters per lane (io_total, qrpn_fail)
 *   2 mul + 1 div per zone eval  (~15 cycles)
 *   O(1) per write  — no scan
 */

#ifndef GEO_UNION_MASK_H
#define GEO_UNION_MASK_H

#include <stdint.h>
#include "geo_payload_store.h"   /* PL_PAIRS, PL_SLOTS */

/* ── Constants ─────────────────────────────────────────────────── */

#define UM_SCALE        144u     /* projection space = 12² = PL_SLOTS */
#define UM_DANGER       72u      /* fold >= 72 → DANGER  (144/2)      */
#define UM_NORMAL       36u      /* fold >= 36 → NORMAL  (144/4)      */
#define UM_COMPRESS_HI  3u       /* SAFE: keep 1 in 4  (3/4 drop)     */
#define UM_COMPRESS_LO  1u       /* SAFE: keep 3 in 4  (1/4 drop)     */

/* polarity scores */
#define UM_POL_DANGER_HIT   (-3)
#define UM_POL_DANGER_MISS  ( 3)
#define UM_POL_NORMAL_HIT   ( 4)
#define UM_POL_NORMAL_MISS  (-4)

/* ── Zone Enum ─────────────────────────────────────────────────── */

typedef enum {
    UM_ZONE_SAFE   = 0,
    UM_ZONE_NORMAL = 1,
    UM_ZONE_DANGER = 2,
} UMZone;

/* ── Per-lane Counters ──────────────────────────────────────────── */

typedef struct {
    uint32_t writes;       /* write count this window     */
    uint32_t io_total;     /* writes + reads this window  */
    uint32_t qrpn_fail;    /* qrpn verify failures        */
    uint32_t qrpn_total;   /* qrpn verify attempts        */
} UMLaneCounter;

/* ── Mask State (6 lanes) ──────────────────────────────────────── */

typedef struct {
    UMLaneCounter lane[PL_PAIRS];   /* one counter set per lane */
    uint8_t       zone[PL_PAIRS];   /* last computed zone       */
    uint8_t       fold[PL_PAIRS];   /* last fold value (0-144)  */
} UnionMask;

/* ── Init ──────────────────────────────────────────────────────── */

static inline void um_init(UnionMask *m) {
    for (uint8_t i = 0; i < PL_PAIRS; i++) {
        m->lane[i] = (UMLaneCounter){0, 0, 0, 0};
        m->zone[i] = UM_ZONE_SAFE;
        m->fold[i] = 0;
    }
}

/* ── Counter Update (O(1) per op) ──────────────────────────────── */

static inline void um_on_write(UnionMask *m, uint8_t lane) {
    m->lane[lane].writes++;
    m->lane[lane].io_total++;
}

static inline void um_on_read(UnionMask *m, uint8_t lane) {
    m->lane[lane].io_total++;
}

static inline void um_on_qrpn(UnionMask *m, uint8_t lane, uint8_t failed) {
    m->lane[lane].qrpn_total++;
    if (failed) m->lane[lane].qrpn_fail++;
}

/* ── Fold + Zone Eval (call per batch or per N writes) ─────────── */
/*
 * fold = (writes * qrpn_fail * 144) / (io_total * qrpn_total)
 *
 * Guard: if denominator == 0  → fold = 0 → SAFE (no data yet)
 * Guard: clamp fold to 144    (numerator overflow protection)
 */

static inline uint8_t um_fold(const UMLaneCounter *c) {
    if (c->io_total == 0 || c->qrpn_total == 0) return 0u;
    uint64_t num = (uint64_t)c->writes * c->qrpn_fail * UM_SCALE;
    uint64_t den = (uint64_t)c->io_total * c->qrpn_total;
    uint64_t f   = num / den;
    return (uint8_t)(f > UM_SCALE ? UM_SCALE : f);
}

static inline UMZone um_zone_from_fold(uint8_t f) {
    if (f >= UM_DANGER) return UM_ZONE_DANGER;
    if (f >= UM_NORMAL) return UM_ZONE_NORMAL;
    return UM_ZONE_SAFE;
}

/* evaluate all 6 lanes — call at window boundary */
static inline void um_eval(UnionMask *m) {
    for (uint8_t i = 0; i < PL_PAIRS; i++) {
        m->fold[i] = um_fold(&m->lane[i]);
        m->zone[i] = (uint8_t)um_zone_from_fold(m->fold[i]);
    }
}

/* ── Polarity Score ────────────────────────────────────────────── */
/* returns score delta: positive = reward, negative = punish       */

static inline int8_t um_polarity(uint8_t zone, uint8_t hit) {
    if (zone == UM_ZONE_DANGER)
        return hit ? UM_POL_DANGER_HIT : UM_POL_DANGER_MISS;
    if (zone == UM_ZONE_NORMAL)
        return hit ? UM_POL_NORMAL_HIT : UM_POL_NORMAL_MISS;
    return 0;   /* SAFE — no polarity pressure */
}

/* ── Compress Decision (SAFE zone only) ────────────────────────── */
/* returns 1 = write, 0 = drop                                      */
/* fold < 18 (144/8) → aggressive 3/4 drop, else 1/4 drop          */

#define UM_COMPRESS_THRESH 18u   /* 144/8, divisor ✓ */

static inline uint8_t um_should_write(uint8_t zone, uint8_t fold,
                                       uint32_t write_seq) {
    if (zone != UM_ZONE_SAFE) return 1u;   /* DANGER/NORMAL: always write */
    if (fold < UM_COMPRESS_THRESH)
        return (write_seq % 4u) == 0u;     /* keep 1/4 */
    return (write_seq % 4u) != 0u;         /* keep 3/4 */
}

/* ── Window Reset (after eval, slide window) ───────────────────── */

static inline void um_reset_lane(UnionMask *m, uint8_t lane) {
    m->lane[lane] = (UMLaneCounter){0, 0, 0, 0};
}

static inline void um_reset_all(UnionMask *m) {
    for (uint8_t i = 0; i < PL_PAIRS; i++) um_reset_lane(m, i);
}

/* ── Divisor Safety Assert (compile-time) ──────────────────────── */

_Static_assert(UM_SCALE       % UM_DANGER         == 0, "UM_DANGER not divisor of 144");
_Static_assert(UM_SCALE       % UM_NORMAL         == 0, "UM_NORMAL not divisor of 144");
_Static_assert(UM_SCALE       % UM_COMPRESS_THRESH == 0, "UM_COMPRESS_THRESH not divisor of 144");

#endif /* GEO_UNION_MASK_H */
