/*
 * pogls_llm_executor.h — LLM Action Executor
 * ══════════════════════════════════════════════════════════════════════
 *
 * Maps each PoglsAction → real pogls_v4_api.h calls.
 *
 * NOTE: pogls_v4_api.h exposes only primitives (write/commit/audit/
 * snapshot). Actions like REHASH or DEGRADE are synthesized from
 * combinations of these primitives.
 *
 * Mapping rationale:
 *
 *  REHASH_NOW        → commit() forces full Merkle rebuild + QRPN drain
 *  COMPACT_TOMBSTONE → commit() + audit() verifies tombstone sweep
 *  FLUSH_GPU_QUEUE   → commit() drains GPU QRPN queue (per API comment)
 *  RESET_CACHE_WINDOW→ snapshot_create(checkpoint) saves state, resets window
 *  RESEED_LANE_SALT  → NOT in v4 API → log warning, NOOP (add in v5)
 *  DEGRADE_MODE      → snapshot_create(checkpoint) + set degrade flag
 *  RECOVER_MODE      → audit() to verify health, clear degrade flag
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_LLM_EXECUTOR_H
#define POGLS_LLM_EXECUTOR_H

#include <stdint.h>
#include <stdio.h>
#include "pogls_v4_api.h"
#include "pogls_llm_hook.h"
#include "pogls_lane_rebalance.h"


/* ══════════════════════════════════════════════════════════════════════
 * EXECUTOR STATE  — tracks degrade flag, last action result
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  degrade_active;    /* 1 = throughput reduced             */
    uint32_t exec_count;        /* total actions executed             */
    uint32_t exec_errors;       /* actions that returned non-OK       */
    int      last_rc;           /* return code of last execution      */
    PoglsAction last_exec;      /* last action attempted              */
} PoglsExecutorState;

static inline void pogls_executor_init(PoglsExecutorState *s) {
    s->degrade_active = 0;
    s->exec_count     = 0;
    s->exec_errors    = 0;
    s->last_rc        = POGLS_OK;
    s->last_exec      = ACT_NOOP;
}


/* ══════════════════════════════════════════════════════════════════════
 * INDIVIDUAL ACTION IMPLEMENTATIONS
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * ACT_REHASH_NOW
 * Force full dual-Merkle rebuild.
 * commit() drains QRPN queue + runs Merkle on both worlds.
 */
static inline int _exec_rehash_now(pogls_ctx_t *ctx) {
    int rc = pogls_commit(ctx);
    if (rc != POGLS_OK && rc != POGLS_ERR_FROZEN) return rc;
    /* audit confirms rebuild was clean */
    return pogls_audit(ctx);
}

/*
 * ACT_COMPACT_TOMBSTONE
 * Tombstone sweep via commit + audit cycle.
 * commit() flushes pending writes; audit() triggers internal compaction.
 */
static inline int _exec_compact_tombstone(pogls_ctx_t *ctx) {
    int rc = pogls_commit(ctx);
    if (rc == POGLS_ERR_FROZEN) return POGLS_OK;  /* frozen = skip, not error */
    if (rc != POGLS_OK) return rc;
    return pogls_audit(ctx);
}

/*
 * ACT_FLUSH_GPU_QUEUE
 * Drain GPU QRPN queue.
 * Per API comment: "Drains GPU QRPN queue" is exactly what commit() does.
 * Safest and cheapest action — idempotent.
 */
static inline int _exec_flush_gpu_queue(pogls_ctx_t *ctx) {
    int rc = pogls_commit(ctx);
    return (rc == POGLS_ERR_FROZEN) ? POGLS_OK : rc;
}

/*
 * ACT_RESET_CACHE_WINDOW
 * Save checkpoint snapshot → forces A/B window reset on next write.
 */
static inline int _exec_reset_cache_window(pogls_ctx_t *ctx) {
    int64_t sid = pogls_snapshot_create(ctx, /*is_checkpoint=*/1);
    if (sid < 0) return (int)sid;
    return pogls_audit(ctx);
}

/*
 * ACT_RESEED_LANE_SALT
 * NOT available in v4 API — log and skip.
 * TODO: add pogls_reseed_lane(ctx) in v5.
 */
static inline int _exec_reseed_lane_salt(pogls_ctx_t *ctx) {
    (void)ctx;
    fprintf(stderr, "[llm_executor] RESEED_LANE_SALT: not in v4 API — NOOP\n");
    return POGLS_OK;   /* non-fatal — caller continues */
}

/*
 * ACT_DEGRADE_MODE
 * Checkpoint current state, then set degrade flag.
 * Caller must honour degrade_active by reducing write rate.
 */
static inline int _exec_degrade_mode(pogls_ctx_t *ctx, PoglsExecutorState *s) {
    /* save checkpoint before degrading */
    int64_t sid = pogls_snapshot_create(ctx, /*is_checkpoint=*/1);
    if (sid < 0) return (int)sid;
    s->degrade_active = 1;
    fprintf(stderr, "[llm_executor] DEGRADE_MODE active (checkpoint %lld)\n",
            (long long)sid);
    return POGLS_OK;
}

/*
 * ACT_RECOVER_MODE
 * Audit to confirm system health, then clear degrade flag.
 * Guard in pogls_llm_hook.h already ensures health == OK before we get here.
 */
static inline int _exec_recover_mode(pogls_ctx_t *ctx, PoglsExecutorState *s) {
    int rc = pogls_audit(ctx);
    if (rc != POGLS_OK) return rc;   /* not clean yet — stay degraded */
    s->degrade_active = 0;
    fprintf(stderr, "[llm_executor] RECOVER_MODE — degrade cleared\n");
    return POGLS_OK;
}


/* ══════════════════════════════════════════════════════════════════════
 * MAIN EXECUTOR  — PoglsActionExecutor callback for pogls_llm_apply()
 *
 * user_ctx must be PoglsExecutorCtx* (defined below).
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    pogls_ctx_t           *ctx;         /* POGLS storage context          */
    PoglsExecutorState     state;        /* mutable executor state         */
    PoglsLaneRebalancer    rebalancer;   /* Phase 1: spoke routing salt    */
    uint32_t               ring_backlog[4]; /* latest ring state (caller updates) */
    uint64_t               epoch;        /* latest epoch (caller updates)  */
} PoglsExecutorCtx;

static inline void pogls_executor_ctx_init(PoglsExecutorCtx *ec,
                                             pogls_ctx_t       *ctx,
                                             GeoPipeline       *pipeline) {
    ec->ctx   = ctx;
    ec->epoch = 0;
    memset(ec->ring_backlog, 0, sizeof(ec->ring_backlog));
    pogls_executor_init(&ec->state);
    pogls_rebalancer_init(&ec->rebalancer, pipeline);
}

/*
 * pogls_llm_executor — pass this as `exec` to pogls_llm_dispatch()
 *
 * Signature matches PoglsActionExecutor typedef in pogls_llm_hook.h:
 *   int (*)(PoglsAction action, void *user_ctx)
 */
static inline int pogls_llm_executor(PoglsAction action, void *user_ctx) {
    PoglsExecutorCtx   *ec = (PoglsExecutorCtx *)user_ctx;
    pogls_ctx_t        *ctx = ec->ctx;
    PoglsExecutorState *s   = &ec->state;
    int rc = POGLS_OK;

    s->last_exec = action;

    switch (action) {
        case ACT_NOOP:
            break;

        case ACT_REHASH_NOW:
            rc = _exec_rehash_now(ctx);
            break;

        case ACT_COMPACT_TOMBSTONE:
            rc = _exec_compact_tombstone(ctx);
            break;

        case ACT_FLUSH_GPU_QUEUE:
            rc = _exec_flush_gpu_queue(ctx);
            break;

        case ACT_RESET_CACHE_WINDOW:
            rc = _exec_reset_cache_window(ctx);
            break;

        case ACT_RESEED_LANE_SALT:
            rc = pogls_reseed_all_skewed_lanes(
                     &ec->rebalancer,
                     ec->ring_backlog,
                     ec->epoch,
                     ctx);
            break;

        case ACT_DEGRADE_MODE:
            rc = _exec_degrade_mode(ctx, s);
            break;

        case ACT_RECOVER_MODE:
            rc = _exec_recover_mode(ctx, s);
            break;

        default:
            fprintf(stderr, "[llm_executor] unknown action %d — NOOP\n", action);
            rc = POGLS_OK;
            break;
    }

    s->last_rc = rc;
    s->exec_count++;
    if (rc != POGLS_OK) s->exec_errors++;

    return rc;
}


/* ══════════════════════════════════════════════════════════════════════
 * USAGE — complete wiring into epoch loop
 * ══════════════════════════════════════════════════════════════════════
 *
 *  #include "pogls_admin_snapshot.h"
 *  #include "pogls_llm_hook.h"
 *  #include "pogls_llm_executor.h"
 *
 *  // init once
 *  PoglsLLMHookCtx   llm_ctx;  pogls_llm_hook_init(&llm_ctx);
 *  PoglsExecutorCtx  exec_ctx; pogls_executor_ctx_init(&exec_ctx, ctx);
 *
 *  // every epoch
 *  PoglsAdminSnapshot snap;
 *  pogls_admin_snapshot_init(&snap);
 *  pogls_admin_collect_sdk(&snap, ctx, gpu);
 *  pogls_admin_collect_audit(&snap, audit);
 *
 *  if (pogls_llm_should_fire(&snap, epoch, &llm_ctx)) {
 *      char snap_json[8192];
 *      pogls_admin_snapshot_json(&snap, snap_json, sizeof(snap_json));
 *
 *      PoglsAction applied = pogls_llm_dispatch(
 *          &snap, snap_json, epoch,
 *          &llm_ctx,
 *          pogls_llm_executor,   // <— this file
 *          &exec_ctx             // <— carries ctx + state
 *      );
 *      // applied = what ran (ACT_NOOP if guard rejected)
 *  }
 *
 *  // honour degrade flag in your write path:
 *  if (exec_ctx.state.degrade_active) {
 *      usleep(500);  // or reduce batch size
 *  }
 * ══════════════════════════════════════════════════════════════════════
 */


/* ── Diagnostics ────────────────────────────────────────────────────── */

static inline void pogls_executor_stats(const PoglsExecutorState *s,
                                         char *buf, size_t sz) {
    snprintf(buf, sz,
        "{\"exec_count\":%u,\"exec_errors\":%u,"
        "\"degrade_active\":%d,\"last_action\":\"%s\",\"last_rc\":%d}",
        s->exec_count, s->exec_errors,
        (int)s->degrade_active,
        POGLS_ACTION_NAMES[s->last_exec],
        s->last_rc);
}


#endif /* POGLS_LLM_EXECUTOR_H */
