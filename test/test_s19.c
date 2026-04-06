/*
 * test_s19.c — Session 19: PoglsCtx wired with KVBridge
 *
 * Build (no CUDA needed — GPU path stubbed):
 *   gcc -O2 -fsanitize=address,undefined -I. -o test_s19 test_s19.c -lm
 *
 * 8 tests:
 *   1. open/close lifecycle
 *   2. write/read roundtrip (L1 hit path)
 *   3. read after rewind (L1 miss → KV fallback)
 *   4. has(): L1 hit, L1 miss+KV hit, total miss
 *   5. qrpn removes from CPU KV
 *   6. 1M writes: hit rate L1 vs KV fallback
 *   7. flush via no-op GPU sink (ring drains, CPU unaffected)
 *   8. stats smoke (no crash, sane output)
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <inttypes.h>
#include <time.h>
#include "pogls_sdk.h"

static int passed = 0, failed = 0;
#define RUN(name) do { printf("  " #name " ... "); \
                       test_##name(); \
                       printf("PASS\n"); passed++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); failed++; return; } while(0)
#define ASSERT(c)  do { if (!(c)) FAIL(#c); } while(0)

static uint64_t mk(uint32_t n) { return (uint64_t)n * 0x9e3779b97f4a7c15ULL | 1ULL; }

/* no-op GPU sink for CPU-only tests */
static void noop_gpu(uint64_t k, uint64_t v, void *ctx) { (void)k;(void)v;(void)ctx; }

/* ── 1. lifecycle ─────────────────────────────────────────────── */
static void test_lifecycle(void) {
    PoglsCtx *ctx = pogls_open();
    ASSERT(ctx != NULL);
    ASSERT(ctx->writes == 0);
    ASSERT(ctx->kv.cpu.count == 0);
    pogls_close(ctx);
}

/* ── 2. write/read L1 hot path ──────────────────────────────────── */
static void test_write_read_l1(void) {
    PoglsCtx *ctx = pogls_open();
    pogls_write(ctx, mk(1), 0xCAFEULL);
    /* L1 should hold it — reads++ but hits via cache */
    ASSERT(pogls_read(ctx, mk(1)) == 0xCAFEULL);
    ASSERT(ctx->l1.hits == 1);
    pogls_close(ctx);
}

/* ── 3. read after rewind → L1 miss, KV fallback ────────────────── */
static void test_read_after_rewind(void) {
    PoglsCtx *ctx = pogls_open();
    pogls_write(ctx, mk(2), 0xBEEFULL);
    pogls_rewind(ctx);
    /* L1 cleared — must fall back to KV */
    ASSERT(pogls_read(ctx, mk(2)) == 0xBEEFULL);
    ASSERT(ctx->l1.misses >= 1);
    pogls_close(ctx);
}

/* ── 4. has(): three cases ───────────────────────────────────────── */
static void test_has(void) {
    PoglsCtx *ctx = pogls_open();
    pogls_write(ctx, mk(3), 100ULL);
    ASSERT(pogls_has(ctx, mk(3)) == 1);      /* L1 hit */
    pogls_rewind(ctx);
    ASSERT(pogls_has(ctx, mk(3)) == 1);      /* KV fallback */
    ASSERT(pogls_has(ctx, mk(9999)) == 0);   /* total miss */
    pogls_close(ctx);
}

/* ── 5. qrpn removes from CPU KV ────────────────────────────────── */
static void test_qrpn_del(void) {
    PoglsCtx *ctx = pogls_open();
    pogls_write(ctx, mk(4), 0x42ULL);
    pogls_rewind(ctx);
    ASSERT(pogls_has(ctx, mk(4)) == 1);
    pogls_qrpn(ctx, mk(4), 1);
    ASSERT(pogls_has(ctx, mk(4)) == 0);
    ASSERT(ctx->qrpns == 1);
    pogls_close(ctx);
}

/* ── 6. 1M writes — measure L1 hit rate ─────────────────────────── */
static void test_1m_hit_rate(void) {
    PoglsCtx *ctx = pogls_open();
    /* write 1M, rewind every 512 (simulates real cycle) */
    uint64_t total_reads = 0;
    for (int cycle = 0; cycle < 2000; cycle++) {
        for (int i = 0; i < 512; i++)
            pogls_write(ctx, mk(cycle * 512 + i + 1), (uint64_t)i);
        /* read back 64 random keys from this cycle */
        for (int i = 0; i < 64; i++) {
            pogls_read(ctx, mk(cycle * 512 + i + 1));
            total_reads++;
        }
        if (cycle % 4 == 3) pogls_rewind(ctx);
    }
    uint64_t total_cache = ctx->l1.hits + ctx->l1.misses;
    double hit_pct = total_cache > 0
        ? 100.0 * ctx->l1.hits / total_cache : 0.0;
    printf("(L1=%.1f%% of %" PRIu64 " reads) ", hit_pct, total_reads);
    ASSERT(ctx->writes == 2000ULL * 512);
    pogls_close(ctx);
}

/* ── 7. flush via noop GPU sink — ring drains ───────────────────── */
static void test_flush_noop(void) {
    PoglsCtx *ctx = pogls_open();
    for (int i = 0; i < 200; i++)
        pogls_write(ctx, mk(5000 + i), (uint64_t)i);
    uint32_t sent = pogls_flush_gpu(ctx, noop_gpu, NULL);
    ASSERT(sent == 200);
    /* lanes fully drained */
    for (int i = 0; i < KVB_LANES; i++) {
        uint32_t h = atomic_load(&ctx->kv.lanes[i].head);
        uint32_t t = atomic_load(&ctx->kv.lanes[i].tail);
        ASSERT(h == t);
    }
    /* CPU truth intact */
    pogls_rewind(ctx);
    for (int i = 0; i < 200; i++)
        ASSERT(pogls_read(ctx, mk(5000 + i)) == (uint64_t)i);
    pogls_close(ctx);
}

/* ── 8. stats smoke ──────────────────────────────────────────────── */
static void test_stats_smoke(void) {
    PoglsCtx *ctx = pogls_open();
    pogls_write(ctx, mk(10), 1ULL);
    pogls_read(ctx, mk(10));
    pogls_print_stats(ctx);   /* must not crash */
    pogls_close(ctx);
}

int main(void) {
    printf("=== TPOGLS S19 ===\n");
    RUN(lifecycle);
    RUN(write_read_l1);
    RUN(read_after_rewind);
    RUN(has);
    RUN(qrpn_del);
    RUN(1m_hit_rate);
    RUN(flush_noop);
    RUN(stats_smoke);
    printf("─────────────────\n");
    printf("%d/%d PASS\n", passed, passed + failed);
    return failed ? 1 : 0;
}
