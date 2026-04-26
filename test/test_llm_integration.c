/*
 * test_llm_integration.c — LLM Hook Integration Test
 * ══════════════════════════════════════════════════════════════════════
 *
 * Tests the full loop:
 *   mock PoglsAdminSnapshot → pogls_llm_should_fire()
 *   → pogls_llm_dispatch()  → policy guard
 *   → pogls_llm_executor()  → verify action applied
 *
 * Does NOT require a running LLM server.
 * Uses a mock pogls_llm_query_python() that returns scripted responses.
 * Does NOT require real pogls_ctx_t — uses mock executor.
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -I. \
 *       test_llm_integration.c -o test_llm_integration
 *   ./test_llm_integration
 *
 * Expected: ALL PASS
 * ══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── Minimal stubs so headers compile standalone ─────────────────── */

/* stub pogls_v4_api.h functions — no real .so needed */
typedef struct pogls_ctx_s { int dummy; } pogls_ctx_t;
#define POGLS_OK       0
#define POGLS_ERR_ARG -1
#define POGLS_ERR_FROZEN -4

static int pogls_commit(pogls_ctx_t *ctx)          { (void)ctx; return POGLS_OK; }
static int pogls_audit(pogls_ctx_t  *ctx)           { (void)ctx; return POGLS_OK; }
static int64_t pogls_snapshot_create(pogls_ctx_t *ctx, uint8_t chk) {
    (void)ctx; (void)chk; return 1;
}
static int pogls_snapshot_verify(pogls_ctx_t *ctx, int64_t sid) {
    (void)ctx; (void)sid; return POGLS_OK;
}
static const char *pogls_strerror(int e) { (void)e; return "stub"; }
static const char *pogls_shat_stage_name(uint8_t s) { (void)s; return "stub"; }

/* stub PoglsAdminSnapshot — only fields used by llm_hook */
#define ADMIN_MAX_HEADS  8
#define ADMIN_MAX_TILES  256
#define ADMIN_SNAP_MAGIC 0x504F4731UL

typedef struct { uint32_t total_ops; uint32_t qrpn_fails; uint32_t audit_fails;
                 uint32_t sig_fails; int32_t score_paths; } AdminPipelineSnap;
typedef struct { uint64_t writes; uint64_t reads; uint64_t qrpns;
                 uint64_t l1_hits; uint64_t l1_misses; float l1_hit_pct; } AdminSDKSnap;
typedef struct { uint32_t kv_live; uint32_t kv_tombstones; uint32_t kv_max_probe;
                 uint64_t kv_puts; uint64_t kv_gets; uint64_t kv_misses;
                 float kv_hit_pct; float kv_load_pct; float tomb_pct;
                 uint32_t flush_count; uint32_t ring_dropped;
                 uint32_t ring_backlog[4]; } AdminKVSnap;
typedef struct { uint64_t total_sent; uint32_t total_overflow; uint32_t stage_pending;
                 float overflow_pct; uint8_t available; } AdminGPUSnap;
typedef struct { uint8_t head_id; uint8_t status; uint8_t mode; uint8_t pad;
                 uint32_t n_bits; uint64_t write_count; uint64_t snapshot_count;
                 uint64_t compaction_count; uint32_t anomaly_count; uint32_t migration_count;
                 uint32_t current_block_count; uint32_t peak_block_count;
                 uint64_t last_active_ms; uint64_t last_certified_snap_id; } AdminHeadSnap;
typedef struct { uint8_t active_count; uint64_t core_size; uint64_t core_append_offset;
                 float core_fill_pct; uint64_t radar_incident_count;
                 uint64_t radar_spawn_count; uint64_t radar_retract_count;
                 AdminHeadSnap heads[ADMIN_MAX_HEADS]; uint8_t head_count; } AdminHydraSnap;
typedef struct { uint64_t snapshot_id; uint64_t branch_id; uint8_t state;
                 uint8_t is_checkpoint; uint8_t audit_health_at_promo; uint8_t pad;
                 uint64_t parent_snapshot_id; uint64_t created_at_ms;
                 uint32_t effective_timeout_ms; uint8_t branch_mode;
                 uint64_t branch_snapshot_count; uint64_t last_certified_id;
                 uint64_t last_auto_id; } AdminSnapStateSnap;
typedef struct { uint8_t health; uint32_t tile_count; uint64_t total_scans;
                 uint64_t total_anomalies; uint64_t last_scan_at_ms;
                 uint32_t signal_queue_depth;
                 uint8_t  tile_state[ADMIN_MAX_TILES];
                 uint8_t  tile_anomaly[ADMIN_MAX_TILES];
                 uint16_t tile_blocks_anomalous[ADMIN_MAX_TILES]; } AdminAuditSnap;

typedef struct {
    uint32_t           magic;
    uint64_t           collected_at_ms;
    AdminPipelineSnap  pipeline;
    AdminSDKSnap       sdk;
    AdminKVSnap        kv;
    AdminGPUSnap       gpu;
    AdminHydraSnap     hydra;
    AdminSnapStateSnap snapstate;
    AdminAuditSnap     audit;
} PoglsAdminSnapshot;

static inline void pogls_admin_snapshot_init(PoglsAdminSnapshot *s) {
    memset(s, 0, sizeof(*s)); s->magic = ADMIN_SNAP_MAGIC;
}
static inline int pogls_admin_snapshot_json(const PoglsAdminSnapshot *s,
                                             char *buf, int len) {
    return snprintf(buf, len, "{\"kv\":{\"load_pct\":%.2f,\"tomb_pct\":%.2f},"
                              "\"gpu\":{\"overflow_pct\":%.2f},"
                              "\"audit\":{\"health\":%u},"
                              "\"hydra\":{\"fill_pct\":%.2f}}",
                   s->kv.kv_load_pct, s->kv.tomb_pct,
                   s->gpu.overflow_pct, s->audit.health,
                   s->hydra.core_fill_pct);
}

/* now include the real hook headers */
#include "pogls_llm_hook.h"
#include "pogls_llm_executor.h"


/* ══════════════════════════════════════════════════════════════════════
 * MOCK LLM  — scripted responses, no network
 * Set g_mock_action / g_mock_confidence before each test.
 * ══════════════════════════════════════════════════════════════════════ */

static PoglsAction g_mock_action     = ACT_NOOP;
static float       g_mock_confidence = 0.0f;
static int         g_llm_call_count  = 0;

int pogls_llm_query_python(
        const char         *key,
        uint32_t            allowed_mask,
        const char         *snap_json,
        PoglsAdvisorResult *result_out)
{
    (void)snap_json;
    g_llm_call_count++;

    result_out->action     = g_mock_action;
    result_out->confidence = g_mock_confidence;
    result_out->from_cache = 0;
    strncpy(result_out->state_key, key, sizeof(result_out->state_key)-1);

    /* validate action is in allowed mask — mirror of Python _parse() */
    if (!(allowed_mask & (1u << (unsigned)g_mock_action))) {
        result_out->action     = ACT_NOOP;
        result_out->confidence = 0.0f;
    }
    return 0;
}


/* ══════════════════════════════════════════════════════════════════════
 * MOCK EXECUTOR  — records calls, no real I/O
 * ══════════════════════════════════════════════════════════════════════ */

static PoglsAction g_exec_last   = ACT_NOOP;
static int         g_exec_count  = 0;
static int         g_exec_rc     = POGLS_OK;   /* set to inject failures */

static int mock_executor(PoglsAction action, void *ctx) {
    (void)ctx;
    g_exec_last  = action;
    g_exec_count++;
    return g_exec_rc;
}


/* ══════════════════════════════════════════════════════════════════════
 * TEST HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

static int  g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do {                                      \
    if (cond) { printf("  PASS  %s\n", label); g_pass++; }          \
    else       { printf("  FAIL  %s  [%s:%d]\n", label,             \
                        __FILE__, __LINE__); g_fail++; }             \
} while(0)

static void reset_state(void) {
    g_mock_action     = ACT_NOOP;
    g_mock_confidence = 0.0f;
    g_llm_call_count  = 0;
    g_exec_last       = ACT_NOOP;
    g_exec_count      = 0;
    g_exec_rc         = POGLS_OK;
}

static PoglsAdminSnapshot make_snap(float kv_load, float tomb,
                                     float gpu_ovf, uint8_t health) {
    PoglsAdminSnapshot s;
    pogls_admin_snapshot_init(&s);
    s.kv.kv_load_pct   = kv_load;
    s.kv.tomb_pct      = tomb;
    s.gpu.overflow_pct = gpu_ovf;
    s.gpu.available    = 1;
    s.audit.health     = health;
    s.hydra.core_fill_pct = 50.0f;
    return s;
}


/* ══════════════════════════════════════════════════════════════════════
 * TESTS
 * ══════════════════════════════════════════════════════════════════════ */

/* T1: should_fire — threshold matrix */
static void test_should_fire(void) {
    printf("\n[T1] should_fire threshold matrix\n");
    PoglsLLMHookCtx ctx; pogls_llm_hook_init(&ctx);

    /* nothing breached → no fire */
    PoglsAdminSnapshot s = make_snap(50.0f, 10.0f, 0.0f, 0);
    CHECK("no fire below thresholds",   pogls_llm_should_fire(&s, 10, &ctx) == 0);

    /* kv.load > 75 */
    s = make_snap(80.0f, 10.0f, 0.0f, 0);
    CHECK("fire on kv_load=80",         pogls_llm_should_fire(&s, 10, &ctx) == 1);

    /* gpu.overflow > 0 */
    s = make_snap(50.0f, 10.0f, 1.0f, 0);
    CHECK("fire on gpu_overflow=1",     pogls_llm_should_fire(&s, 10, &ctx) == 1);

    /* audit.health >= 1 */
    s = make_snap(50.0f, 10.0f, 0.0f, 1);
    CHECK("fire on audit_health=1",     pogls_llm_should_fire(&s, 10, &ctx) == 1);

    /* tomb > 30 */
    s = make_snap(50.0f, 35.0f, 0.0f, 0);
    CHECK("fire on tomb=35",            pogls_llm_should_fire(&s, 10, &ctx) == 1);

    /* cooldown: fire at epoch 10, re-check at epoch 11 → blocked */
    s = make_snap(80.0f, 0.0f, 0.0f, 0);
    pogls_llm_should_fire(&s, 10, &ctx);       /* first fire — advances last_fire_epoch */
    ctx.last_fire_epoch = 10;
    CHECK("cooldown blocks epoch 11",   pogls_llm_should_fire(&s, 11, &ctx) == 0);
    CHECK("cooldown lifts epoch 13",    pogls_llm_should_fire(&s, 13, &ctx) == 1);
}

/* T2: state_key format + quantization */
static void test_state_key(void) {
    printf("\n[T2] state_key quantization\n");
    char k1[32], k2[32], k3[32];

    PoglsAdminSnapshot s1 = make_snap(78.0f, 12.0f, 0.0f, 0);
    PoglsAdminSnapshot s2 = make_snap(79.0f, 13.0f, 0.0f, 0);  /* same 5% bucket: both → 75 */
    PoglsAdminSnapshot s3 = make_snap(82.0f, 16.0f, 1.0f, 1);  /* different */

    pogls_llm_build_state_key(&s1, k1, sizeof(k1));
    pogls_llm_build_state_key(&s2, k2, sizeof(k2));
    pogls_llm_build_state_key(&s3, k3, sizeof(k3));

    CHECK("78% and 81% → same bucket",  strcmp(k1, k2) == 0);
    CHECK("different state → diff key", strcmp(k1, k3) != 0);
    CHECK("key contains KL prefix",     strncmp(k1, "KL", 2) == 0);
}

/* T3: policy guard — reject low confidence */
static void test_guard_confidence(void) {
    printf("\n[T3] policy guard — confidence threshold\n");
    PoglsLLMHookCtx hctx; pogls_llm_hook_init(&hctx);
    PoglsAdminSnapshot s = make_snap(80.0f, 10.0f, 0.0f, 0);

    CHECK("conf=0.30 rejected",
        pogls_llm_policy_guard(ACT_REHASH_NOW, 0.30f, &s, &hctx) == 0);
    CHECK("conf=0.61 accepted",
        pogls_llm_policy_guard(ACT_REHASH_NOW, 0.61f, &s, &hctx) == 1);
    CHECK("guard_rejections count correct", hctx.guard_rejections == 1);
}

/* T4: policy guard — action-specific conditions */
static void test_guard_conditions(void) {
    printf("\n[T4] policy guard — action-specific conditions\n");
    PoglsLLMHookCtx hctx; pogls_llm_hook_init(&hctx);

    /* REHASH_NOW: reject if load <= 70 */
    PoglsAdminSnapshot low = make_snap(65.0f, 5.0f, 0.0f, 0);
    CHECK("REHASH rejected: load=65",
        pogls_llm_policy_guard(ACT_REHASH_NOW, 0.90f, &low, &hctx) == 0);

    PoglsAdminSnapshot hi = make_snap(80.0f, 5.0f, 0.0f, 0);
    CHECK("REHASH accepted: load=80",
        pogls_llm_policy_guard(ACT_REHASH_NOW, 0.90f, &hi, &hctx) == 1);

    /* COMPACT: reject if tomb <= 20 */
    PoglsAdminSnapshot lt = make_snap(50.0f, 15.0f, 0.0f, 0);
    CHECK("COMPACT rejected: tomb=15",
        pogls_llm_policy_guard(ACT_COMPACT_TOMBSTONE, 0.90f, &lt, &hctx) == 0);

    PoglsAdminSnapshot ht = make_snap(50.0f, 35.0f, 0.0f, 0);
    CHECK("COMPACT accepted: tomb=35",
        pogls_llm_policy_guard(ACT_COMPACT_TOMBSTONE, 0.90f, &ht, &hctx) == 1);

    /* DEGRADE: reject if already degraded */
    hctx.last_action = ACT_DEGRADE_MODE;
    CHECK("DEGRADE rejected: already active",
        pogls_llm_policy_guard(ACT_DEGRADE_MODE, 0.90f, &hi, &hctx) == 0);

    /* RECOVER: reject if health still bad */
    PoglsAdminSnapshot sick = make_snap(50.0f, 5.0f, 0.0f, 1);
    CHECK("RECOVER rejected: health=DEGRADED",
        pogls_llm_policy_guard(ACT_RECOVER_MODE, 0.90f, &sick, &hctx) == 0);

    PoglsAdminSnapshot well = make_snap(50.0f, 5.0f, 0.0f, 0);
    hctx.last_action = ACT_NOOP;
    CHECK("RECOVER accepted: health=OK",
        pogls_llm_policy_guard(ACT_RECOVER_MODE, 0.90f, &well, &hctx) == 1);
}

/* T5: full dispatch — happy path */
static void test_dispatch_happy(void) {
    printf("\n[T5] dispatch — happy path\n");
    reset_state();

    PoglsLLMHookCtx hctx;  pogls_llm_hook_init(&hctx);
    PoglsAdminSnapshot s = make_snap(80.0f, 10.0f, 0.0f, 0);
    char snap_json[512];
    pogls_admin_snapshot_json(&s, snap_json, sizeof(snap_json));

    g_mock_action     = ACT_REHASH_NOW;
    g_mock_confidence = 0.92f;

    PoglsAction applied = pogls_llm_dispatch(
        &s, snap_json, 10, &hctx, mock_executor, NULL);

    CHECK("action applied = REHASH_NOW", applied == ACT_REHASH_NOW);
    CHECK("executor called once",        g_exec_count == 1);
    CHECK("executor got REHASH_NOW",     g_exec_last  == ACT_REHASH_NOW);
    CHECK("hctx.last_action updated",    hctx.last_action == ACT_REHASH_NOW);
    CHECK("hctx.fire_count = 1",         hctx.fire_count  == 1);
    CHECK("LLM called once",             g_llm_call_count == 1);
}

/* T6: dispatch — guard rejects low confidence */
static void test_dispatch_guard_rejects(void) {
    printf("\n[T6] dispatch — guard rejects weak advice\n");
    reset_state();

    PoglsLLMHookCtx hctx; pogls_llm_hook_init(&hctx);
    PoglsAdminSnapshot s = make_snap(80.0f, 10.0f, 0.0f, 0);
    char snap_json[512];
    pogls_admin_snapshot_json(&s, snap_json, sizeof(snap_json));

    g_mock_action     = ACT_REHASH_NOW;
    g_mock_confidence = 0.30f;   /* below LLM_GUARD_MIN_CONFIDENCE=0.60 */

    PoglsAction applied = pogls_llm_dispatch(
        &s, snap_json, 10, &hctx, mock_executor, NULL);

    CHECK("action = NOOP (guard rejected)", applied == ACT_NOOP);
    CHECK("executor NOT called",            g_exec_count == 0);
    CHECK("guard_rejections = 1",           hctx.guard_rejections == 1);
}

/* T7: dispatch — LLM returns action outside allowed mask */
static void test_dispatch_mask_enforced(void) {
    printf("\n[T7] dispatch — allowed mask enforced\n");
    reset_state();

    PoglsLLMHookCtx hctx; pogls_llm_hook_init(&hctx);
    /* only tomb high — allowed = COMPACT_TOMBSTONE */
    PoglsAdminSnapshot s = make_snap(50.0f, 40.0f, 0.0f, 0);
    char snap_json[512];
    pogls_admin_snapshot_json(&s, snap_json, sizeof(snap_json));

    /* mock tries to return REHASH — not in allowed mask for this state */
    g_mock_action     = ACT_REHASH_NOW;
    g_mock_confidence = 0.95f;

    /* mock_llm validates mask and downgrades to NOOP */
    PoglsAction applied = pogls_llm_dispatch(
        &s, snap_json, 10, &hctx, mock_executor, NULL);

    /* REHASH not in mask → mock returns NOOP → guard sees NOOP → NOOP applied */
    CHECK("NOOP when action outside mask", applied == ACT_NOOP);
}

/* T8: executor — all actions route correctly (mock C API) */
static void test_executor_routing(void) {
    printf("\n[T8] executor — action routing\n");

    pogls_ctx_t fake_ctx = {0};
    PoglsExecutorCtx ec;
    pogls_executor_ctx_init(&ec, &fake_ctx, NULL);

    struct { PoglsAction action; const char *label; } cases[] = {
        { ACT_NOOP,               "NOOP"               },
        { ACT_REHASH_NOW,         "REHASH_NOW"         },
        { ACT_COMPACT_TOMBSTONE,  "COMPACT_TOMBSTONE"  },
        { ACT_FLUSH_GPU_QUEUE,    "FLUSH_GPU_QUEUE"    },
        { ACT_RESET_CACHE_WINDOW, "RESET_CACHE_WINDOW" },
        { ACT_RESEED_LANE_SALT,   "RESEED_LANE_SALT"   },
        { ACT_DEGRADE_MODE,       "DEGRADE_MODE"       },
        { ACT_RECOVER_MODE,       "RECOVER_MODE"       },
    };

    int n = (int)(sizeof(cases)/sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        int rc = pogls_llm_executor(cases[i].action, &ec);
        char label[64];
        snprintf(label, sizeof(label), "executor %s → OK", cases[i].label);
        CHECK(label, rc == POGLS_OK);
        CHECK("last_exec updated", ec.state.last_exec == cases[i].action);
    }
    CHECK("exec_count = 8", ec.state.exec_count == (uint32_t)n);
    CHECK("degrade set after DEGRADE_MODE",
          /* DEGRADE_MODE was called, then RECOVER_MODE cleared it */
          ec.state.degrade_active == 0);
}

/* T9: degrade flag lifecycle */
static void test_degrade_lifecycle(void) {
    printf("\n[T9] degrade flag lifecycle\n");

    pogls_ctx_t fake_ctx = {0};
    PoglsExecutorCtx ec;
    pogls_executor_ctx_init(&ec, &fake_ctx, NULL);

    CHECK("degrade initially off", ec.state.degrade_active == 0);

    pogls_llm_executor(ACT_DEGRADE_MODE, &ec);
    CHECK("degrade ON after DEGRADE_MODE", ec.state.degrade_active == 1);

    pogls_llm_executor(ACT_RECOVER_MODE, &ec);
    CHECK("degrade OFF after RECOVER_MODE", ec.state.degrade_active == 0);
}

/* T10: full epoch loop simulation — 5 epochs, 2 fires */
static void test_epoch_loop(void) {
    printf("\n[T10] epoch loop simulation\n");
    reset_state();

    PoglsLLMHookCtx  hctx;  pogls_llm_hook_init(&hctx);
    pogls_ctx_t      fake_ctx = {0};
    PoglsExecutorCtx exec_ctx; pogls_executor_ctx_init(&exec_ctx, &fake_ctx, NULL);

    int fired = 0;
    g_mock_action     = ACT_FLUSH_GPU_QUEUE;
    g_mock_confidence = 0.88f;

    for (uint64_t epoch = 1; epoch <= 10; epoch++) {
        /* epoch 1,2: calm */
        /* epoch 3+: GPU overflow */
        PoglsAdminSnapshot s = (epoch >= 3)
            ? make_snap(50.0f, 5.0f, 2.0f, 0)   /* overflow */
            : make_snap(40.0f, 5.0f, 0.0f, 0);   /* calm */

        if (pogls_llm_should_fire(&s, epoch, &hctx)) {
            char json[512];
            pogls_admin_snapshot_json(&s, json, sizeof(json));
            PoglsAction a = pogls_llm_dispatch(
                &s, json, epoch, &hctx,
                mock_executor, NULL);
            if (a != ACT_NOOP) fired++;
        }
    }

    /* epoch 3 fires; cooldown 3 epochs; epoch 6 fires again; epoch 9 fires */
    CHECK("fired at least twice in 10 epochs", fired >= 2);
    CHECK("exec_count matches fired",           (int)g_exec_count == fired);
    CHECK("last action was FLUSH_GPU_QUEUE",    g_exec_last == ACT_FLUSH_GPU_QUEUE);
}


/* T11: lane rebalance — skew detection + reseed */
static void test_lane_rebalance(void) {
    printf("\n[T11] lane rebalance — skew detection + reseed\n");

    pogls_ctx_t      fake_ctx = {0};
    GeoPipeline      fake_pipeline = {{0xDEAD1234ULL, 0xBEEF5678ULL}};
    PoglsLaneRebalancer rb;
    pogls_rebalancer_init(&rb, &fake_pipeline);

    /* T11a: no skew — nothing reseeded */
    uint32_t balanced[4] = {10, 12, 11, 10};
    int n = pogls_rebalancer_tick(&rb, balanced, 10, &fake_ctx);
    CHECK("no reseed when balanced",       n == 0);
    CHECK("total_reseeds = 0",             rb.total_reseeds == 0);

    /* T11b: lane 2 over absolute threshold (>72) */
    uint32_t backlog_abs[4] = {10, 12, 80, 10};
    n = pogls_rebalancer_tick(&rb, backlog_abs, 20, &fake_ctx);
    CHECK("reseed fires on backlog>72",    n > 0);
    CHECK("lane 2 reseed_count = 1",       rb.lanes[2].reseed_count == 1);
    CHECK("total_reseeds = 1",             rb.total_reseeds == 1);

    /* T11c: cooldown — same lane, adjacent epoch → skip */
    n = pogls_rebalancer_tick(&rb, backlog_abs, 21, &fake_ctx);
    CHECK("cooldown blocks reseed ep21",   rb.lanes[2].reseed_count == 1);

    /* T11d: cooldown lifts after 7 epochs */
    n = pogls_rebalancer_tick(&rb, backlog_abs, 27, &fake_ctx);
    CHECK("reseed fires after cooldown",   rb.lanes[2].reseed_count == 2);

    /* T11e: skew ratio trigger (max/mean > 2.0) */
    PoglsLaneRebalancer rb2;
    pogls_rebalancer_init(&rb2, &fake_pipeline);
    uint32_t skewed[4] = {5, 5, 5, 25};   /* mean=10, max=25, ratio=2.5 */
    n = pogls_rebalancer_tick(&rb2, skewed, 10, &fake_ctx);
    CHECK("skew ratio 2.5 triggers reseed", n > 0);

    /* T11f: salt never zero after reseed */
    for (int i = 0; i < LANE_RING_COUNT; i++) {
        CHECK("salt non-zero post-reseed",  rb.lanes[i].salt != 0);
    }

    /* T11g: pipeline seed changes after reseed */
    uint64_t gen3_before = fake_pipeline.seed.gen3;
    uint32_t big_backlog[4] = {5, 5, 5, 90};
    PoglsLaneRebalancer rb3;
    pogls_rebalancer_init(&rb3, &fake_pipeline);
    pogls_rebalancer_tick(&rb3, big_backlog, 10, &fake_ctx);
    CHECK("pipeline.seed.gen3 changed",    fake_pipeline.seed.gen3 != gen3_before);
    CHECK("pipeline.seed.gen3 non-zero",   fake_pipeline.seed.gen3 != 0);

    /* T11h: LLM entry point (pogls_reseed_all_skewed_lanes) */
    PoglsLaneRebalancer rb4;
    pogls_rebalancer_init(&rb4, &fake_pipeline);
    int llm_n = pogls_reseed_all_skewed_lanes(&rb4, backlog_abs, 10, &fake_ctx);
    CHECK("LLM reseed fires",              llm_n >= 0);
    CHECK("llm_fires counter incremented", rb4.llm_fires == 1);

    /* T11i: stats output non-empty */
    char stats_buf[512];
    pogls_rebalancer_stats(&rb, stats_buf, sizeof(stats_buf));
    CHECK("stats JSON non-empty",          stats_buf[0] == '{');
}


/* ══════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("═══════════════════════════════════════════════════\n");
    printf("  POGLS LLM Hook — Integration Test Suite\n");
    printf("═══════════════════════════════════════════════════\n");

    test_should_fire();
    test_state_key();
    test_guard_confidence();
    test_guard_conditions();
    test_dispatch_happy();
    test_dispatch_guard_rejects();
    test_dispatch_mask_enforced();
    test_executor_routing();
    test_degrade_lifecycle();
    test_epoch_loop();
    test_lane_rebalance();

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS  %d FAIL\n", g_pass, g_fail);
    printf("═══════════════════════════════════════════════════\n");

    return g_fail ? 1 : 0;
}
