/*
 * pogls_lane_rebalance.h — Lane Rebalance (Phase 1: Spoke Routing Salt)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Phase 1 scope (safe, deterministic, no QRPN):
 *   - Reseed spoke routing salt per-lane
 *   - Auto-fire on ring backlog > 72 OR skew_ratio > 2.0
 *   - LLM dispatches ACT_RESEED_LANE_SALT → calls pogls_reseed_lane()
 *   - C can also self-fire mechanically (no LLM needed)
 *
 * Phase 2 (future — NOT here):
 *   - QRPN seed reseed (affects verification space — higher risk)
 *
 * Salt formula (per GPT recommendation):
 *   salt[lane] ^= fold[lane] | 1     // guarantee non-zero
 *
 * fold[lane] = XOR of ring_backlog[lane] shifted by Fibonacci constant
 * Using 144 (Fibo constant from geo_fibo_clock.h) as the fold multiplier.
 *
 * Wire:
 *   #include "pogls_lane_rebalance.h"
 *
 *   PoglsLaneRebalancer rb;
 *   pogls_rebalancer_init(&rb, pipeline);   // pass GeoPipeline*
 *
 *   // call every epoch (after admin snapshot collected):
 *   pogls_rebalancer_tick(&rb, &snap, epoch, ctx);
 *
 * ACT_RESEED_LANE_SALT executor wiring (pogls_llm_executor.h):
 *   case ACT_RESEED_LANE_SALT:
 *       return pogls_reseed_all_skewed_lanes(&exec_ctx.rebalancer,
 *                                            &snap, ctx);
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_LANE_REBALANCE_H
#define POGLS_LANE_REBALANCE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* forward: real types included by caller */
#ifndef POGLS_V4_API_H
typedef struct pogls_ctx_s pogls_ctx_t;
extern int pogls_commit(pogls_ctx_t *ctx);
#endif

/* GeoSeed: {gen2, gen3} — from geo_thirdeye.h */
#ifndef GEO_THIRDEYE_H
typedef struct { uint64_t gen2; uint64_t gen3; } GeoSeed;
#endif

/* GeoPipeline stub if not included */
#ifndef GEO_PIPELINE_WIRE_H
typedef struct { GeoSeed seed; } GeoPipeline;
#endif


/* ══════════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ══════════════════════════════════════════════════════════════════════ */

#define LANE_COUNT_A         4       /* World A: lanes 0-3             */
#define LANE_COUNT_B         252     /* World B: lanes 4-255           */
#define LANE_COUNT_TOTAL     256     /* all lanes                      */
#define LANE_RING_COUNT      4       /* ring_backlog tracked by admin   */

#define FIBO_FOLD_CONST      144ULL  /* from geo_fibo_clock.h          */
#define RESEED_BACKLOG_TRIG  72      /* ring[lane] > this → skewed     */
#define RESEED_SKEW_RATIO    2.0f    /* max/mean > this → skewed       */
#define RESEED_COOLDOWN_EP   7       /* epochs between reseeds/lane    */


/* ══════════════════════════════════════════════════════════════════════
 * PER-LANE SALT STATE
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t salt;             /* current routing salt (never 0)     */
    uint64_t last_reseed_ep;   /* epoch of last reseed               */
    uint32_t reseed_count;     /* total reseeds this session         */
    uint32_t skew_events;      /* times this lane was detected skewed */
} PoglsLaneSalt;


/* ══════════════════════════════════════════════════════════════════════
 * REBALANCER STATE
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    PoglsLaneSalt  lanes[LANE_RING_COUNT];  /* one salt per ring lane  */
    GeoPipeline   *pipeline;                /* owned by caller         */
    uint64_t       total_reseeds;
    uint64_t       total_skew_events;
    uint32_t       auto_fires;              /* C self-fired (no LLM)   */
    uint32_t       llm_fires;              /* fired via ACT_RESEED     */
} PoglsLaneRebalancer;


/* ══════════════════════════════════════════════════════════════════════
 * INIT
 * ══════════════════════════════════════════════════════════════════════ */

static inline void pogls_rebalancer_init(PoglsLaneRebalancer *rb,
                                          GeoPipeline         *pipeline)
{
    memset(rb, 0, sizeof(*rb));
    rb->pipeline = pipeline;

    /* init salts from pipeline seed — non-zero guaranteed */
    for (int i = 0; i < LANE_RING_COUNT; i++) {
        uint64_t base = pipeline ? (pipeline->seed.gen2 ^ pipeline->seed.gen3) : 0xDEADC0DE1234ULL;
        rb->lanes[i].salt = (base ^ ((uint64_t)(i+1) * FIBO_FOLD_CONST)) | 1ULL;
    }
}


/* ══════════════════════════════════════════════════════════════════════
 * CORE: pogls_reseed_lane()
 *
 * Salt formula: salt[lane] ^= fold[lane] | 1
 * fold[lane]  = (ring_backlog[lane] * FIBO_FOLD_CONST) XOR upper_bits
 *
 * After reseed: pogls_commit() to sync routing state.
 * ══════════════════════════════════════════════════════════════════════ */

static inline int pogls_reseed_lane(PoglsLaneRebalancer *rb,
                                     int                  lane_idx,
                                     uint32_t             ring_backlog,
                                     uint64_t             epoch,
                                     pogls_ctx_t         *ctx)
{
    if (lane_idx < 0 || lane_idx >= LANE_RING_COUNT) return -1;

    PoglsLaneSalt *ls = &rb->lanes[lane_idx];

    /* cooldown guard */
    if (epoch - ls->last_reseed_ep < RESEED_COOLDOWN_EP) return 0;

    /* fold = backlog pressure × Fibonacci constant, mixed with current salt */
    uint64_t fold = ((uint64_t)ring_backlog * FIBO_FOLD_CONST)
                  ^ (ls->salt >> 17)          /* upper bits feedback      */
                  ^ ((uint64_t)lane_idx << 8);

    /* apply: XOR + guarantee non-zero */
    ls->salt ^= (fold | 1ULL);

    /* propagate to pipeline seed gen3 (spoke routing path) */
    if (rb->pipeline) {
        rb->pipeline->seed.gen3 ^= ls->salt ^ ((uint64_t)lane_idx * FIBO_FOLD_CONST);
        rb->pipeline->seed.gen3 |= 1ULL;   /* never zero */
    }

    ls->last_reseed_ep = epoch;
    ls->reseed_count++;
    rb->total_reseeds++;

    fprintf(stderr, "[lane_rebalance] lane=%d reseed #%u salt=0x%016llx epoch=%llu\n",
            lane_idx, ls->reseed_count,
            (unsigned long long)ls->salt,
            (unsigned long long)epoch);

    /* commit to sync routing state after salt change */
    return pogls_commit(ctx);
}


/* ══════════════════════════════════════════════════════════════════════
 * SKEW DETECTION
 *
 * Checks ring_backlog[0..3] from AdminKVSnap.
 * Returns bitmask of skewed lanes (bit i = lane i is skewed).
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  skewed_mask;     /* bitmask: bit i = lane i skewed     */
    uint8_t  skewed_count;    /* number of skewed lanes             */
    float    skew_ratio;      /* max_backlog / mean_backlog         */
    uint32_t max_backlog;
    uint32_t mean_backlog;
} PoglsSkewReport;

static inline PoglsSkewReport pogls_detect_skew(const uint32_t ring_backlog[4])
{
    PoglsSkewReport r = {0};

    uint64_t sum = 0;
    uint32_t max = 0;
    for (int i = 0; i < LANE_RING_COUNT; i++) {
        sum += ring_backlog[i];
        if (ring_backlog[i] > max) max = ring_backlog[i];
    }

    r.max_backlog  = max;
    r.mean_backlog = (uint32_t)(sum / LANE_RING_COUNT);
    r.skew_ratio   = r.mean_backlog
                   ? (float)max / (float)r.mean_backlog
                   : (max > 0 ? 99.0f : 0.0f);

    for (int i = 0; i < LANE_RING_COUNT; i++) {
        /* skewed if: backlog > threshold OR this lane >> mean */
        int over_abs  = ring_backlog[i] > RESEED_BACKLOG_TRIG;
        int over_ratio = r.mean_backlog > 0 &&
                         (float)ring_backlog[i] / (float)r.mean_backlog > RESEED_SKEW_RATIO;
        if (over_abs || over_ratio) {
            r.skewed_mask |= (uint8_t)(1u << i);
            r.skewed_count++;
        }
    }

    return r;
}


/* ══════════════════════════════════════════════════════════════════════
 * AUTO-FIRE: C self-fires without LLM (mechanical fix)
 *
 * Call every epoch after admin snapshot.
 * Returns number of lanes reseeded (0 = nothing to do).
 * ══════════════════════════════════════════════════════════════════════ */

static inline int pogls_rebalancer_tick(PoglsLaneRebalancer *rb,
                                         const uint32_t       ring_backlog[4],
                                         uint64_t             epoch,
                                         pogls_ctx_t         *ctx)
{
    PoglsSkewReport sr = pogls_detect_skew(ring_backlog);
    if (sr.skewed_count == 0) return 0;

    int reseeded = 0;
    for (int i = 0; i < LANE_RING_COUNT; i++) {
        if (!(sr.skewed_mask & (1u << i))) continue;

        rb->lanes[i].skew_events++;
        rb->total_skew_events++;

        int rc = pogls_reseed_lane(rb, i, ring_backlog[i], epoch, ctx);
        if (rc == 0) reseeded++;   /* 0 = OK or cooldown-skipped */
    }

    if (reseeded > 0) rb->auto_fires++;
    return reseeded;
}


/* ══════════════════════════════════════════════════════════════════════
 * LLM ENTRY: reseed all currently skewed lanes
 * Called by pogls_llm_executor() for ACT_RESEED_LANE_SALT
 * ══════════════════════════════════════════════════════════════════════ */

static inline int pogls_reseed_all_skewed_lanes(PoglsLaneRebalancer *rb,
                                                  const uint32_t       ring_backlog[4],
                                                  uint64_t             epoch,
                                                  pogls_ctx_t         *ctx)
{
    rb->llm_fires++;
    return pogls_rebalancer_tick(rb, ring_backlog, epoch, ctx);
}


/* ══════════════════════════════════════════════════════════════════════
 * DIAGNOSTICS
 * ══════════════════════════════════════════════════════════════════════ */

static inline void pogls_rebalancer_stats(const PoglsLaneRebalancer *rb,
                                           char *buf, size_t sz)
{
    int n = snprintf(buf, sz,
        "{\"total_reseeds\":%llu,\"total_skew_events\":%llu,"
        "\"auto_fires\":%u,\"llm_fires\":%u,\"lanes\":[",
        (unsigned long long)rb->total_reseeds,
        (unsigned long long)rb->total_skew_events,
        rb->auto_fires, rb->llm_fires);

    for (int i = 0; i < LANE_RING_COUNT && n < (int)sz - 64; i++) {
        const PoglsLaneSalt *ls = &rb->lanes[i];
        n += snprintf(buf + n, sz - n,
            "%s{\"lane\":%d,\"reseeds\":%u,\"skew_events\":%u}",
            i ? "," : "", i, ls->reseed_count, ls->skew_events);
    }
    snprintf(buf + n, sz - n, "]}");
}


#endif /* POGLS_LANE_REBALANCE_H */
