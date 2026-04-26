/*
 * pogls_llm_hook.h — LLM Advisor Hook
 * ══════════════════════════════════════════════════════════════════════
 *
 * Design contract:
 *   LLM  = advisor   → picks action from finite set
 *   C    = authority → validates + executes (or ignores)
 *
 * Flow:
 *   pogls_llm_should_fire()       ← threshold check (O(1))
 *   pogls_llm_build_state_key()   ← deterministic state fingerprint
 *   pogls_llm_dispatch()          ← cache hit → return / miss → call LLM
 *   pogls_llm_policy_guard()      ← validate action vs current metrics
 *   pogls_llm_apply()             ← execute if guard passes
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_LLM_HOOK_H
#define POGLS_LLM_HOOK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Forward refs (consumers must include their own admin/sdk headers) ── */
/* Expects:  PoglsAdminSnapshot  from pogls_admin_snapshot.h             */


/* ══════════════════════════════════════════════════════════════════════
 * 1. ACTION ENUM  — the system contract
 *    Rules: deterministic · reversible · bounded cost
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    ACT_NOOP               = 0,

    /* KV layer */
    ACT_REHASH_NOW         = 1,   /* full KV rehash      — cost: HIGH, one-shot   */
    ACT_COMPACT_TOMBSTONE  = 2,   /* purge tomb entries  — cost: MED,  reversible */

    /* Cache layer */
    ACT_FLUSH_GPU_QUEUE    = 3,   /* drain GPU ring buf  — cost: LOW,  safe       */
    ACT_RESET_CACHE_WINDOW = 4,   /* A/B swap + reset    — cost: MED,  reversible */

    /* Routing */
    ACT_RESEED_LANE_SALT   = 5,   /* redistribute lanes  — cost: LOW,  safe       */

    /* Safety */
    ACT_DEGRADE_MODE       = 6,   /* reduce throughput   — cost: LOW,  reversible */
    ACT_RECOVER_MODE       = 7,   /* restore normal ops  — cost: LOW,  safe       */

    _ACT_COUNT             = 8    /* sentinel — keep last */
} PoglsAction;

/* Human-readable names (index == PoglsAction value) */
static const char * const POGLS_ACTION_NAMES[_ACT_COUNT] = {
    "NOOP",
    "REHASH_NOW",
    "COMPACT_TOMBSTONE",
    "FLUSH_GPU_QUEUE",
    "RESET_CACHE_WINDOW",
    "RESEED_LANE_SALT",
    "DEGRADE_MODE",
    "RECOVER_MODE",
};


/* ══════════════════════════════════════════════════════════════════════
 * 2. TRIGGER THRESHOLDS  — tune without touching logic
 * ══════════════════════════════════════════════════════════════════════ */

#define LLM_TRIG_KV_LOAD_PCT      75    /* kv.load_pct  > this  */
#define LLM_TRIG_GPU_OVERFLOW     0     /* gpu.overflow > this  */
#define LLM_TRIG_AUDIT_HEALTH     1     /* audit.health >= this */
#define LLM_TRIG_TOMB_PCT         30    /* kv.tomb_pct  > this  */

/* Policy guard thresholds */
#define LLM_GUARD_REHASH_MIN_LOAD    70  /* REHASH_NOW only if load > this */
#define LLM_GUARD_COMPACT_MIN_TOMB   20  /* COMPACT only if tomb  > this   */
#define LLM_GUARD_RECOVER_MAX_HEALTH  0  /* RECOVER only if health == OK   */
#define LLM_GUARD_MIN_CONFIDENCE    0.60f /* reject advice below this      */

/* Cooldown: minimum epochs between LLM calls (prevent thrash) */
#define LLM_COOLDOWN_EPOCHS          3


/* ══════════════════════════════════════════════════════════════════════
 * 3. ALLOWED ACTION SET per state  — policy hint fed to LLM prompt
 *    Bit mask: (1 << PoglsAction)
 * ══════════════════════════════════════════════════════════════════════ */

#define ACT_MASK(a)  (1u << (unsigned)(a))

typedef struct {
    const char  *state_name;
    uint32_t     allowed_mask;   /* bitmask of PoglsAction */
} PoglsStatePolicy;

/* Table: condition name → allowed actions
 * C-side uses this to build the prompt constraint AND validate response. */
static const PoglsStatePolicy POGLS_STATE_POLICIES[] = {
    { "KV_LOAD_HIGH",    ACT_MASK(ACT_REHASH_NOW)        | ACT_MASK(ACT_COMPACT_TOMBSTONE) },
    { "GPU_OVERFLOW",    ACT_MASK(ACT_FLUSH_GPU_QUEUE)   | ACT_MASK(ACT_DEGRADE_MODE)      },
    { "TOMB_HIGH",       ACT_MASK(ACT_COMPACT_TOMBSTONE)                                    },
    { "LANE_SKEW",       ACT_MASK(ACT_RESEED_LANE_SALT)                                    },
    { "AUDIT_DEGRADED",  ACT_MASK(ACT_DEGRADE_MODE)      | ACT_MASK(ACT_RESET_CACHE_WINDOW)},
    { "RECOVERING",      ACT_MASK(ACT_RECOVER_MODE)                                        },
    { NULL, 0 }   /* sentinel */
};


/* ══════════════════════════════════════════════════════════════════════
 * 4. ADVISOR RESULT  — what C receives from LLM (via Python bridge)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    PoglsAction  action;          /* parsed from LLM JSON                  */
    float        confidence;      /* 0.0 – 1.0                             */
    uint8_t      from_cache;      /* 1 = cache hit, 0 = fresh LLM call     */
    char         state_key[32];   /* fingerprint used for cache lookup      */
} PoglsAdvisorResult;


/* ══════════════════════════════════════════════════════════════════════
 * 5. HOOK CONTEXT  — owns cooldown + last action state
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t     last_fire_epoch;   /* epoch of last LLM trigger            */
    PoglsAction  last_action;       /* last executed action                 */
    uint32_t     fire_count;        /* total LLM triggers this session      */
    uint32_t     cache_hits;        /* O(1) resolutions                     */
    uint32_t     guard_rejections;  /* actions blocked by policy guard      */
} PoglsLLMHookCtx;

static inline void pogls_llm_hook_init(PoglsLLMHookCtx *hctx) {
    memset(hctx, 0, sizeof(*hctx));
}


/* ══════════════════════════════════════════════════════════════════════
 * 6. TRIGGER CHECK  — O(1), call every epoch
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Returns 1 if any threshold breached AND cooldown elapsed.
 * snap  : current admin snapshot
 * epoch : current system epoch
 * hctx  : hook context (tracks last fire)
 */
static inline int pogls_llm_should_fire(
        const PoglsAdminSnapshot *snap,
        uint64_t                  epoch,
        PoglsLLMHookCtx          *hctx)
{
    /* cooldown guard — prevents thrash */
    if (epoch - hctx->last_fire_epoch < LLM_COOLDOWN_EPOCHS)
        return 0;

    if (snap->kv.kv_load_pct      >  LLM_TRIG_KV_LOAD_PCT)   return 1;
    if (snap->gpu.overflow_pct >  LLM_TRIG_GPU_OVERFLOW)   return 1;
    if (snap->audit.health     >= LLM_TRIG_AUDIT_HEALTH)   return 1;
    if (snap->kv.tomb_pct      >  LLM_TRIG_TOMB_PCT)       return 1;

    return 0;
}


/* ══════════════════════════════════════════════════════════════════════
 * 7. STATE KEY  — deterministic fingerprint for cache lookup
 *    Format: "KL{load}T{tomb}G{overflow}H{health}"
 *    Quantized to reduce cache misses from minor fluctuations
 * ══════════════════════════════════════════════════════════════════════ */

static inline void pogls_llm_build_state_key(
        const PoglsAdminSnapshot *snap,
        char                     *key_out,   /* min 32 bytes */
        size_t                    key_sz)
{
    /* quantize to nearest 5% buckets → same state = same key */
    int load  = ((int)snap->kv.kv_load_pct  / 5) * 5;
    int tomb  = ((int)snap->kv.tomb_pct  / 5) * 5;
    int ovf   = snap->gpu.overflow_pct > 0 ? 1 : 0;
    int hlth  = (int)snap->audit.health;

    snprintf(key_out, key_sz, "KL%02dT%02dG%dH%d",
             load, tomb, ovf, hlth);
}


/* ══════════════════════════════════════════════════════════════════════
 * 8. POLICY GUARD  — C-side validation before execution
 *    LLM is advisor; C is authority.
 *    Returns 1 = approved, 0 = rejected
 * ══════════════════════════════════════════════════════════════════════ */

static inline int pogls_llm_policy_guard(
        PoglsAction               action,
        float                     confidence,
        const PoglsAdminSnapshot *snap,
        PoglsLLMHookCtx          *hctx)
{
    /* reject low-confidence advice */
    if (confidence < LLM_GUARD_MIN_CONFIDENCE) {
        hctx->guard_rejections++;
        return 0;
    }

    switch (action) {
        case ACT_NOOP:
            return 1;   /* always safe */

        case ACT_REHASH_NOW:
            /* only if KV genuinely under pressure */
            if (snap->kv.kv_load_pct <= LLM_GUARD_REHASH_MIN_LOAD) {
                hctx->guard_rejections++;
                return 0;
            }
            return 1;

        case ACT_COMPACT_TOMBSTONE:
            if (snap->kv.tomb_pct <= LLM_GUARD_COMPACT_MIN_TOMB) {
                hctx->guard_rejections++;
                return 0;
            }
            return 1;

        case ACT_FLUSH_GPU_QUEUE:
            return 1;   /* always safe — idempotent drain */

        case ACT_RESET_CACHE_WINDOW:
            return 1;   /* reversible A/B swap */

        case ACT_RESEED_LANE_SALT:
            return 1;   /* safe, bounded cost */

        case ACT_DEGRADE_MODE:
            /* don't degrade if already degraded */
            if (hctx->last_action == ACT_DEGRADE_MODE) {
                hctx->guard_rejections++;
                return 0;
            }
            return 1;

        case ACT_RECOVER_MODE:
            /* only recover if health is back to OK */
            if (snap->audit.health > LLM_GUARD_RECOVER_MAX_HEALTH) {
                hctx->guard_rejections++;
                return 0;
            }
            return 1;

        default:
            hctx->guard_rejections++;
            return 0;   /* unknown action → always reject */
    }
}


/* ══════════════════════════════════════════════════════════════════════
 * 9. APPLY + RECORD  — call after guard passes
 *    Actual implementation is system-specific; this records state only.
 *    Wire your pogls_ctx_t calls inside the switch.
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Signature for the executor callback — implement per-project.
 * Returns 0 on success.
 */
typedef int (*PoglsActionExecutor)(PoglsAction action, void *user_ctx);

static inline int pogls_llm_apply(
        PoglsAction        action,
        PoglsLLMHookCtx   *hctx,
        uint64_t           epoch,
        PoglsActionExecutor exec,
        void              *exec_ctx)
{
    int rc = 0;
    if (exec) rc = exec(action, exec_ctx);
    if (rc == 0) {
        hctx->last_action     = action;
        hctx->last_fire_epoch = epoch;
        hctx->fire_count++;
    }
    return rc;
}


/* ══════════════════════════════════════════════════════════════════════
 * 10. FULL DISPATCH HELPER
 *     Combines: should_fire → build_key → [cache or LLM] → guard → apply
 *     Python bridge fills result via pogls_llm_query_python() (extern).
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Extern: implemented in llm_advisor.py via ctypes/cffi Python bridge.
 * Signature: query(state_key, allowed_mask_json, snap_json) → AdvisorResult
 *
 * key        : state fingerprint (from pogls_llm_build_state_key)
 * allowed    : bitmask of allowed actions for this state
 * snap_json  : full admin snapshot JSON (from pogls_admin_snapshot_json)
 * result_out : filled on success
 * Returns 0 on success, -1 on failure (result = ACT_NOOP, conf=0)
 */
extern int pogls_llm_query_python(
        const char         *key,
        uint32_t            allowed_mask,
        const char         *snap_json,
        PoglsAdvisorResult *result_out);


/*
 * Compute allowed mask for current snapshot state.
 * ORs all matching policies.
 */
static inline uint32_t pogls_llm_allowed_mask(const PoglsAdminSnapshot *snap) {
    uint32_t mask = ACT_MASK(ACT_NOOP);

    if (snap->kv.kv_load_pct  > LLM_TRIG_KV_LOAD_PCT)  mask |= POGLS_STATE_POLICIES[0].allowed_mask;
    if (snap->gpu.overflow_pct > LLM_TRIG_GPU_OVERFLOW) mask |= POGLS_STATE_POLICIES[1].allowed_mask;
    if (snap->kv.tomb_pct  > LLM_TRIG_TOMB_PCT)      mask |= POGLS_STATE_POLICIES[2].allowed_mask;
    if (snap->audit.health >= LLM_TRIG_AUDIT_HEALTH)  mask |= POGLS_STATE_POLICIES[4].allowed_mask;

    return mask;
}

/*
 * Main dispatch — call once per epoch when should_fire() == 1.
 *
 * snap      : current system snapshot
 * snap_json : pre-serialized JSON (from pogls_admin_snapshot_json)
 * epoch     : current epoch
 * hctx      : hook context
 * exec      : your action executor callback
 * exec_ctx  : passed through to exec
 *
 * Returns PoglsAction that was applied (ACT_NOOP if nothing done).
 */
static inline PoglsAction pogls_llm_dispatch(
        const PoglsAdminSnapshot *snap,
        const char               *snap_json,
        uint64_t                  epoch,
        PoglsLLMHookCtx          *hctx,
        PoglsActionExecutor       exec,
        void                     *exec_ctx)
{
    char               key[32];
    PoglsAdvisorResult result;
    uint32_t           allowed;

    pogls_llm_build_state_key(snap, key, sizeof(key));
    allowed = pogls_llm_allowed_mask(snap);

    /* query LLM (or cache — cache logic lives in Python bridge) */
    result.action     = ACT_NOOP;
    result.confidence = 0.0f;
    result.from_cache = 0;
    memcpy(result.state_key, key, sizeof(key));

    if (pogls_llm_query_python(key, allowed, snap_json, &result) != 0)
        return ACT_NOOP;   /* LLM unavailable → safe fallback */

    if (result.from_cache)
        hctx->cache_hits++;

    /* C is authority — validate before execute */
    if (!pogls_llm_policy_guard(result.action, result.confidence, snap, hctx))
        return ACT_NOOP;

    pogls_llm_apply(result.action, hctx, epoch, exec, exec_ctx);
    return result.action;
}


/* ══════════════════════════════════════════════════════════════════════
 * 11. DIAGNOSTICS
 * ══════════════════════════════════════════════════════════════════════ */

static inline void pogls_llm_hook_stats(
        const PoglsLLMHookCtx *hctx,
        char                  *buf,
        size_t                 sz)
{
    snprintf(buf, sz,
        "{\"fire_count\":%u,\"cache_hits\":%u,\"guard_rejections\":%u,"
        "\"last_action\":\"%s\",\"last_epoch\":%llu}",
        hctx->fire_count,
        hctx->cache_hits,
        hctx->guard_rejections,
        POGLS_ACTION_NAMES[hctx->last_action],
        (unsigned long long)hctx->last_fire_epoch);
}


#endif /* POGLS_LLM_HOOK_H */
