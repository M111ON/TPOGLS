/*
 * geo_thirdeye.h — Third Eye: 144-cycle observer
 *                + QRPN Radial Mirror (3-layer adaptive)
 *
 * Geometry awareness (v2):
 *   CYL_FULL_N = 3456 = 144 × 24
 *   1 full cycle = 144 ops = 1/24 of full space
 *   6 snaps × 144 = 864 = 3456/4
 *   24 cycles = full space traversal
 *
 * QRPN Radial Mirror — 3 layers:
 *   NORMAL  → mask ±1 spoke      (local, 2 neighbors)
 *   STRESSED→ mask ±1 + invert   (add 180°)
 *   ANOMALY → mask full 0x3F     (all 6 spokes)
 *
 * Threshold (number theory):
 *   slot_density threshold = 576/9 = 64   (1 face worth)
 *   imbalance threshold    = 576/6 = 96   (1/6 of spoke)
 *   → both from CYL geometry, zero magic numbers
 *
 * digit_sum chain:
 *   864 → 18 → 9 ✓
 *   144 → 9      ✓
 *   24  → 6      ✓
 */

#ifndef GEO_THIRDEYE_H
#define GEO_THIRDEYE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "geo_config.h"

/* ── ThirdEye constants (from geo_config.h) ── */
#define TE_CYCLE        GEO_TE_CYCLE
#define TE_MAX_SNAP     GEO_TE_SNAPS
#define TE_FULL_CYCLES  GEO_TE_FULL_CYCLES
#define TE_FULL_OPS     GEO_FULL_N

/* ── QRPN states ── */
#define QRPN_NORMAL     0       /* ±1 only                             */
#define QRPN_STRESSED   1       /* ±1 + invert(+3)                    */
#define QRPN_ANOMALY    2       /* full radial 0x3F                   */

/* threshold from geo_config.h — no magic numbers */
#define QRPN_HOT_THRESH     GEO_HOT_THRESH
#define QRPN_IMBAL_THRESH   GEO_IMBAL_THRESH
#define QRPN_ANOMALY_HOT    GEO_ANOMALY_HOT

typedef struct { uint64_t gen2; uint64_t gen3; } GeoSeed;

/* spoke distribution tracker (per snap) */
typedef struct {
    uint16_t spoke_count[6];    /* ops per spoke this cycle            */
    uint16_t hot_slots;         /* slots above density threshold       */
    uint8_t  qrpn_state;        /* NORMAL / STRESSED / ANOMALY        */
    uint8_t  _pad;
} GeoSnap;

typedef struct {
    GeoSeed  genesis;           /* immutable — write once              */
    GeoSeed  ring[TE_MAX_SNAP]; /* runtime snapshots                   */
    GeoSnap  snap[TE_MAX_SNAP]; /* distribution per snapshot           */
    GeoSnap  cur;               /* accumulator for current cycle       */
    uint32_t op_count;
    uint8_t  head;              /* ring write cursor 0..5              */
    uint8_t  count;             /* valid entries                       */
    uint8_t  qrpn_state;        /* current global QRPN state           */
    uint8_t  _pad;
} ThirdEye;

/* ── init ── */
static inline void te_init(ThirdEye *te, GeoSeed g) {
    memset(te, 0, sizeof(ThirdEye));
    te->genesis    = g;
    te->qrpn_state = QRPN_NORMAL;
}

/* ── QRPN: compute radial mirror mask for a spoke ── */
static inline uint8_t te_mirror_mask(uint8_t spoke, uint8_t state) {
    if (state == QRPN_ANOMALY) return 0x3F;  /* all 6 spokes */

    uint8_t mask = 0;
    /* ±1 neighbor (local — always) */
    mask |= (uint8_t)(1u << ((spoke + 1) % 6));
    mask |= (uint8_t)(1u << ((spoke + 5) % 6));

    /* +3 invert (stressed+) */
    if (state >= QRPN_STRESSED)
        mask |= (uint8_t)(1u << ((spoke + 3) % 6));

    return mask;
}

/* ── QRPN: evaluate state from snap ── */
static inline uint8_t te_eval_state(const GeoSnap *s) {
    /* check imbalance: A vs B pairs (0↔3, 1↔4, 2↔5) */
    uint8_t imbal = 0;
    for (int i = 0; i < 3; i++) {
        int diff = (int)s->spoke_count[i] - (int)s->spoke_count[i+3];
        if (diff < 0) diff = -diff;
        if (diff > QRPN_IMBAL_THRESH) imbal = 1;  /* > 72 */
    }
    /* check density */
    uint8_t hot      = (s->hot_slots > QRPN_HOT_THRESH);   /* > 64  stressed */
    uint8_t very_hot = (s->hot_slots > QRPN_ANOMALY_HOT);  /* > 96  anomaly  */

    /* ANOMALY: imbal OR very_hot  (OR logic) */
    if (imbal || very_hot) return QRPN_ANOMALY;
    /* STRESSED: hot only */
    if (hot) return QRPN_STRESSED;
    return QRPN_NORMAL;
}

/* ── tick: call every op, pass current spoke ── */
static inline void te_tick(ThirdEye *te, GeoSeed cur, uint8_t spoke, uint8_t slot_hot) {
    te->op_count++;
    te->cur.spoke_count[spoke & 0x7]++;
    if (slot_hot) te->cur.hot_slots++;

    if (te->op_count % TE_CYCLE == 0) {
        /* evaluate QRPN state for this cycle */
        te->cur.qrpn_state = te_eval_state(&te->cur);
        te->qrpn_state     = te->cur.qrpn_state;

        /* commit snapshot */
        te->head = (te->head + 1) % TE_MAX_SNAP;
        te->ring[te->head] = cur;
        te->snap[te->head] = te->cur;
        if (te->count < TE_MAX_SNAP) te->count++;

        /* reset accumulator */
        memset(&te->cur, 0, sizeof(GeoSnap));
    }
}

/* ── get mirror mask for current spoke ── */
static inline uint8_t te_get_mask(const ThirdEye *te, uint8_t spoke) {
    return te_mirror_mask(spoke, te->qrpn_state);
}

/* ── reset → genesis ── */
static inline GeoSeed te_reset(ThirdEye *te) {
    memset(te->ring, 0, sizeof(te->ring));
    memset(te->snap, 0, sizeof(te->snap));
    memset(&te->cur,  0, sizeof(GeoSnap));
    te->op_count   = 0;
    te->head       = 0;
    te->count      = 0;
    te->qrpn_state = QRPN_NORMAL;
    return te->genesis;
}

/* ── rewind n snapshots ── */
static inline GeoSeed te_rewind(ThirdEye *te, int n) {
    if (n <= 0 || te->count == 0) return te->genesis;
    if (n >= te->count)           return te->genesis;
    int slot = ((int)te->head - n + TE_MAX_SNAP) % TE_MAX_SNAP;
    return te->ring[slot];
}

/* ── status ── */
static inline const char* te_state_name(uint8_t s) {
    if (s == QRPN_ANOMALY)  return "ANOMALY";
    if (s == QRPN_STRESSED) return "STRESSED";
    return "NORMAL";
}

static inline void te_status(const ThirdEye *te) {
    printf("[ThirdEye] op=%u  next=%u  ring=%d/%d  head=%d\n",
           te->op_count,
           TE_CYCLE - (te->op_count % TE_CYCLE),
           te->count, TE_MAX_SNAP, te->head);
    printf("  QRPN state : %s\n", te_state_name(te->qrpn_state));
    printf("  cycle prog : %d/%d (full=%d)\n",
           te->op_count / TE_CYCLE, TE_FULL_CYCLES, TE_FULL_OPS);

    if (te->count > 0) {
        const GeoSnap *last = &te->snap[te->head];
        printf("  last snap  : spoke[");
        for (int i = 0; i < 6; i++)
            printf("%d%s", last->spoke_count[i], i<5?",":"]");
        printf("  hot=%d\n", last->hot_slots);

        /* shadow balance */
        printf("  A↔B balance: ");
        for (int i = 0; i < 3; i++) {
            int d = (int)last->spoke_count[i] - (int)last->spoke_count[i+3];
            if (d<0) d=-d;
            printf("%d↔%d|diff=%d%s  ", i, i+3, d,
                   d>QRPN_IMBAL_THRESH?"!":"✓");
        }
        printf("\n");
    }
}

#endif /* GEO_THIRDEYE_H */
