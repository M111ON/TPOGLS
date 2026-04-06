/*
 * test_s16.c — Session 16: SDK boundary + KV bridge tests (CPU path)
 *
 * Build:
 *   gcc -O2 -fsanitize=address,undefined -I. -o test_s16 test_s16.c -lm
 *
 * 8 tests:
 *   1. SDK open/close lifecycle
 *   2. pogls_write + pogls_read round-trip
 *   3. pogls_read miss returns 0
 *   4. pogls_has disambiguates val=0 from miss
 *   5. pogls_rewind: cache cleared, KV survives
 *   6. pogls_qrpn: signal doesn't corrupt read
 *   7. bulk 1K writes → all readable
 *   8. dual-write contract: KV always has truth even if pipeline drops
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "pogls_sdk.h"

static int passed = 0, failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name)  do { printf("  " #name " ... "); \
                        test_##name(); \
                        printf("PASS\n"); passed++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); failed++; return; } while(0)
#define ASSERT(c)  do { if (!(c)) FAIL(#c); } while(0)

static uint64_t mk(uint32_t n) { return (uint64_t)n * 0x9e3779b97f4a7c15ULL | 1ULL; }

/* ── 1. lifecycle ─────────────────────────────────────────────── */
TEST(lifecycle) {
    PoglsCtx *c = pogls_open();
    ASSERT(c != NULL);
    ASSERT(c->writes == 0 && c->reads == 0);
    pogls_close(c);
}

/* ── 2. write + read round-trip ───────────────────────────────── */
TEST(write_read) {
    PoglsCtx *c = pogls_open();
    uint64_t k = mk(2), v = 0xDEADBEEFULL;
    pogls_write(c, k, v);
    ASSERT(pogls_read(c, k) == v);
    ASSERT(c->writes == 1 && c->reads == 1);
    pogls_close(c);
}

/* ── 3. read miss returns 0 ───────────────────────────────────── */
TEST(read_miss) {
    PoglsCtx *c = pogls_open();
    uint64_t missing = mk(999);
    ASSERT(pogls_read(c, missing) == 0);
    ASSERT(!pogls_has(c, missing));
    pogls_close(c);
}

/* ── 4. has() disambiguates val=0 ─────────────────────────────── */
TEST(has_zero_val) {
    PoglsCtx *c = pogls_open();
    uint64_t k = mk(4);
    pogls_write(c, k, 0);
    ASSERT(pogls_has(c, k) == 1);
    ASSERT(pogls_read(c, k) == 0);
    ASSERT(!pogls_has(c, mk(888)));
    pogls_close(c);
}

/* ── 5. rewind: cache cleared, KV truth survives ─────────────── */
TEST(rewind_kv_survives) {
    PoglsCtx *c = pogls_open();
    uint64_t k = mk(5), v = 0xCAFEULL;
    pogls_write(c, k, v);

    pogls_rewind(c);

    /* KV still has it */
    ASSERT(pogls_has(c, k));
    /* read: pipeline miss → KV fallback → returns correct val */
    ASSERT(pogls_read(c, k) == v);
    pogls_close(c);
}

/* ── 6. qrpn signal doesn't corrupt read ─────────────────────── */
TEST(qrpn_no_corrupt) {
    PoglsCtx *c = pogls_open();
    uint64_t k = mk(6), v = 0x1234ULL;
    pogls_write(c, k, v);
    pogls_qrpn(c, k, 1);  /* signal failure */
    ASSERT(pogls_read(c, k) == v);  /* val must survive */
    pogls_close(c);
}

/* ── 7. bulk 1K writes → all readable ────────────────────────── */
TEST(bulk_1k) {
    PoglsCtx *c = pogls_open();
    enum { N = 1024 };
    for (int i = 0; i < N; i++)
        pogls_write(c, mk(1000+i), (uint64_t)i * 7 + 1);
    int all_match = 1;
    for (int i = 0; i < N; i++) {
        uint64_t got = pogls_read(c, mk(1000+i));
        if (got != (uint64_t)i * 7 + 1) { all_match = 0; break; }
    }
    ASSERT(all_match);
    ASSERT(c->writes == N);
    pogls_close(c);
}

/* ── 8. dual-write: KV always has truth ───────────────────────── */
/*
 * Pipeline compress gate may drop writes in early window.
 * KV must always have the value regardless.
 */
TEST(dual_write_truth) {
    PoglsCtx *c = pogls_open();
    /* write in the very first window (compress gate may skip pipeline) */
    uint64_t k = mk(8), v = 0xF00DULL;
    pogls_write(c, k, v);

    /* KV has it regardless */
    ASSERT(kv_has(&c->kv, k));
    ASSERT(kv_get(&c->kv, k) == v);

    /* pogls_read falls back to KV if pipeline missed it */
    ASSERT(pogls_read(c, k) == v);
    pogls_close(c);
}

/* ── main ─────────────────────────────────────────────────────── */
int main(void) {
    printf("=== TPOGLS S16 SDK ===\n");
    RUN(lifecycle);
    RUN(write_read);
    RUN(read_miss);
    RUN(has_zero_val);
    RUN(rewind_kv_survives);
    RUN(qrpn_no_corrupt);
    RUN(bulk_1k);
    RUN(dual_write_truth);
    printf("─────────────────────\n");
    printf("%d/%d PASS\n", passed, passed + failed);
    return failed ? 1 : 0;
}
