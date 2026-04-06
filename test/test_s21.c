/*
 * test_s21.c — S21: Admin Snapshot smoke test (CPU-only, no Hydra/Audit deps)
 *
 * Build:
 *   gcc -O2 -fsanitize=address,undefined -I. -I./core -o test_s21 test_s21.c -lm
 *
 * Tests:
 *   1. init: magic correct, timestamp non-zero
 *   2. collect SDK: writes/reads/kv fields populated
 *   3. kv derived metrics: load_pct, tomb_pct, hit_pct in range
 *   4. ring backlog: 0 after flush
 *   5. print: no crash
 *   6. JSON: valid prefix, no truncation, key fields present
 *   7. JSON: kv.live matches actual count
 *   8. JSON: gpu.avail=0 for CPU-only build
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

/* pull in real SDK types first so collect macros activate */
#include "../geo_kv.h"
#include "../kv_bridge.h"
#include "../pogls_sdk.h"
#include "pogls_admin_snapshot.h"

static int passed=0, failed=0;
#define RUN(name) do{printf("  " #name " ... ");test_##name();printf("PASS\n");passed++;}while(0)
#define FAIL(msg) do{printf("FAIL: %s\n",msg);failed++;return;}while(0)
#define ASSERT(c) do{if(!(c))FAIL(#c);}while(0)

static uint64_t mk(uint32_t n){return (uint64_t)n*0x9e3779b97f4a7c15ULL|1ULL;}
static void noop_gpu(uint64_t k,uint64_t v,void*c){(void)k;(void)v;(void)c;}

/* ── 1. init ─────────────────────────────────────────────────────── */
static void test_init(void){
    PoglsAdminSnapshot s;
    pogls_admin_snapshot_init(&s);
    ASSERT(s.magic == ADMIN_SNAP_MAGIC);
    ASSERT(s.collected_at_ms > 0);
}

/* ── 2. collect SDK fields ───────────────────────────────────────── */
static void test_collect_sdk(void){
    PoglsCtx *ctx = pogls_open();
    for(int i=0;i<100;i++) pogls_write(ctx, mk(i+1), (uint64_t)i);
    for(int i=0;i<50; i++) pogls_read (ctx, mk(i+1));

    PoglsAdminSnapshot s; pogls_admin_snapshot_init(&s);
    pogls_admin_collect_sdk(&s, ctx, NULL);

    ASSERT(s.sdk.writes == 100);
    ASSERT(s.sdk.reads  == 50);
    ASSERT(s.kv.kv_live == 100);
    ASSERT(s.gpu.available == 0);
    pogls_close(ctx);
}

/* ── 3. derived metrics in range ─────────────────────────────────── */
static void test_derived_metrics(void){
    PoglsCtx *ctx = pogls_open();
    for(int i=0;i<200;i++) pogls_write(ctx, mk(i+1), (uint64_t)i);
    /* delete 60 → tombstones */
    for(int i=0;i<60;i++) kvbridge_del(&ctx->kv, mk(i+1));

    PoglsAdminSnapshot s; pogls_admin_snapshot_init(&s);
    pogls_admin_collect_sdk(&s, ctx, NULL);

    ASSERT(s.kv.kv_load_pct >= 0.f && s.kv.kv_load_pct <= 100.f);
    ASSERT(s.kv.tomb_pct     >= 0.f && s.kv.tomb_pct    <= 100.f);
    ASSERT(s.kv.kv_hit_pct   >= 0.f && s.kv.kv_hit_pct  <= 100.f);
    /* 60 tombstones / (140+60) = 30% */
    ASSERT(s.kv.tomb_pct >= 25.f && s.kv.tomb_pct <= 35.f);
    pogls_close(ctx);
}

/* ── 4. ring backlog zero after flush ────────────────────────────── */
static void test_ring_backlog(void){
    PoglsCtx *ctx = pogls_open();
    for(int i=0;i<64;i++) pogls_write(ctx, mk(i+1), (uint64_t)i);

    PoglsAdminSnapshot s; pogls_admin_snapshot_init(&s);
    pogls_admin_collect_sdk(&s, ctx, NULL);
    /* before flush: some lanes have backlog */
    uint32_t total_before = s.kv.ring_backlog[0]+s.kv.ring_backlog[1]
                           +s.kv.ring_backlog[2]+s.kv.ring_backlog[3];
    ASSERT(total_before == 64);

    pogls_flush_gpu(ctx, noop_gpu, NULL);
    pogls_admin_snapshot_init(&s);
    pogls_admin_collect_sdk(&s, ctx, NULL);
    uint32_t total_after = s.kv.ring_backlog[0]+s.kv.ring_backlog[1]
                          +s.kv.ring_backlog[2]+s.kv.ring_backlog[3];
    ASSERT(total_after == 0);
    pogls_close(ctx);
}

/* ── 5. print no crash ───────────────────────────────────────────── */
static void test_print(void){
    PoglsCtx *ctx = pogls_open();
    pogls_write(ctx, mk(1), 0xABCDULL);
    PoglsAdminSnapshot s; pogls_admin_snapshot_init(&s);
    pogls_admin_collect_sdk(&s, ctx, NULL);
    pogls_admin_snapshot_print(&s);  /* must not crash */
    pogls_close(ctx);
}

/* ── 6. JSON no truncation, key fields present ────────────────────── */
static void test_json_basic(void){
    PoglsCtx *ctx = pogls_open();
    for(int i=0;i<10;i++) pogls_write(ctx, mk(i+1),(uint64_t)i);
    PoglsAdminSnapshot s; pogls_admin_snapshot_init(&s);
    pogls_admin_collect_sdk(&s, ctx, NULL);

    char buf[8192];
    int n = pogls_admin_snapshot_json(&s, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(buf[0] == '{');
    ASSERT(buf[n-1] == '}');
    ASSERT(strstr(buf, "\"pipeline\"") != NULL);
    ASSERT(strstr(buf, "\"sdk\"")      != NULL);
    ASSERT(strstr(buf, "\"kv\"")       != NULL);
    ASSERT(strstr(buf, "\"gpu\"")      != NULL);
    ASSERT(strstr(buf, "\"hydra\"")    != NULL);
    ASSERT(strstr(buf, "\"snap\"")     != NULL);
    ASSERT(strstr(buf, "\"audit\"")    != NULL);
    pogls_close(ctx);
}

/* ── 7. JSON kv.live matches actual ──────────────────────────────── */
static void test_json_kv_live(void){
    PoglsCtx *ctx = pogls_open();
    for(int i=0;i<77;i++) pogls_write(ctx, mk(i+1),(uint64_t)i);
    PoglsAdminSnapshot s; pogls_admin_snapshot_init(&s);
    pogls_admin_collect_sdk(&s, ctx, NULL);
    char buf[8192];
    pogls_admin_snapshot_json(&s, buf, sizeof(buf));
    ASSERT(strstr(buf, "\"live\":77") != NULL);
    pogls_close(ctx);
}

/* ── 8. JSON gpu.avail=0 for CPU build ───────────────────────────── */
static void test_json_gpu_avail(void){
    PoglsAdminSnapshot s; pogls_admin_snapshot_init(&s);
    /* no collect — gpu.available stays 0 */
    char buf[8192];
    pogls_admin_snapshot_json(&s, buf, sizeof(buf));
    ASSERT(strstr(buf, "\"avail\":0") != NULL);
}

int main(void){
    printf("=== TPOGLS S21 — Admin Snapshot ===\n");
    RUN(init);
    RUN(collect_sdk);
    RUN(derived_metrics);
    RUN(ring_backlog);
    RUN(print);
    RUN(json_basic);
    RUN(json_kv_live);
    RUN(json_gpu_avail);
    printf("──────────────────────────────────\n");
    printf("%d/%d PASS\n", passed, passed+failed);
    return failed?1:0;
}
