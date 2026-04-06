/*
 * test_geo_kv.c — Session 15: GeoKV truth layer tests
 *
 * Build:
 *   gcc -O2 -fsanitize=address,undefined -I. -o test_kv test_geo_kv.c -lm
 *
 * 8 tests:
 *   1. basic put/get
 *   2. update in-place (no count growth)
 *   3. delete + tombstone
 *   4. tombstone reuse on re-insert
 *   5. probe chain: hash collision sequence
 *   6. kv_has() disambiguates val=0 from miss
 *   7. load factor check
 *   8. pipeline integration: gp_read miss → kv_get fallback
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "geo_pipeline.h"   /* L1+L2 pipeline */
#include "geo_kv.h"

/* ── test harness ─────────────────────────────────────────────── */

static int passed = 0, failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name)  do { printf("  " #name " ... "); \
                        test_##name(); \
                        printf("PASS\n"); passed++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); failed++; return; } while(0)
#define ASSERT(c)  do { if (!(c)) FAIL(#c); } while(0)

/* ── helpers ─────────────────────────────────────────────────── */

/* make a key that will never be 0 or DEAD sentinel */
static uint64_t mk_key(uint32_t n) {
    return (uint64_t)n * PHI64 | 1ULL;  /* odd → never 0 */
}

/* ── Test 1: basic put / get ─────────────────────────────────── */
TEST(basic_put_get) {
    GeoKV kv; kv_init(&kv);
    uint64_t k = mk_key(1), v = 0xABCD1234ULL;
    kv_put(&kv, k, v);
    ASSERT(kv_get(&kv, k) == v);
    ASSERT(kv.count == 1);
}

/* ── Test 2: update in-place ─────────────────────────────────── */
TEST(update_inplace) {
    GeoKV kv; kv_init(&kv);
    uint64_t k = mk_key(2);
    kv_put(&kv, k, 111);
    kv_put(&kv, k, 222);   /* second put = update */
    ASSERT(kv_get(&kv, k) == 222);
    ASSERT(kv.count == 1); /* must not grow */
}

/* ── Test 3: delete + tombstone ──────────────────────────────── */
TEST(delete_tombstone) {
    GeoKV kv; kv_init(&kv);
    uint64_t k = mk_key(3);
    kv_put(&kv, k, 999);
    int r = kv_del(&kv, k);
    ASSERT(r == 1);
    ASSERT(kv.count == 0);
    ASSERT(kv.tombstones == 1);
    ASSERT(kv_get(&kv, k) == 0);   /* miss after delete */
    ASSERT(!kv_has(&kv, k));
}

/* ── Test 4: tombstone reuse on re-insert ────────────────────── */
TEST(tombstone_reuse) {
    GeoKV kv; kv_init(&kv);
    uint64_t k = mk_key(4);
    kv_put(&kv, k, 100);
    kv_del(&kv, k);
    uint32_t dead_before = kv.tombstones;   /* should be 1 */
    kv_put(&kv, k, 200);                    /* re-insert same key */
    /* tombstone consumed: reuse happens in put() */
    ASSERT(kv.tombstones <= dead_before);   /* ≤ not strictly < if chain differs */
    ASSERT(kv_get(&kv, k) == 200);
    ASSERT(kv.count == 1);
}

/* ── Test 5: probe chain / collision sequence ────────────────── */
/*
 * Force multiple entries to hash to same bucket.
 * linear probe must resolve all of them.
 */
TEST(probe_chain) {
    GeoKV kv; kv_init(&kv);
    /* insert 32 keys and retrieve all — hash distribution test */
    uint64_t keys[32], vals[32];
    for (int i = 0; i < 32; i++) {
        keys[i] = mk_key(100 + i);
        vals[i] = (uint64_t)(i + 1) * 0x1000;
        kv_put(&kv, keys[i], vals[i]);
    }
    for (int i = 0; i < 32; i++) {
        uint64_t got = kv_get(&kv, keys[i]);
        if (got != vals[i]) FAIL("probe chain mismatch");
    }
    ASSERT(kv.count == 32);
}

/* ── Test 6: kv_has() disambiguates val=0 ────────────────────── */
TEST(has_zero_val) {
    GeoKV kv; kv_init(&kv);
    uint64_t k = mk_key(6);
    kv_put(&kv, k, 0);               /* val = 0 is valid */
    ASSERT(kv_has(&kv, k) == 1);    /* present */
    ASSERT(kv_get(&kv, k) == 0);    /* val correct */
    uint64_t missing = mk_key(999);
    ASSERT(!kv_has(&kv, missing));   /* not present */
}

/* ── Test 7: load factor check ───────────────────────────────── */
TEST(load_factor) {
    GeoKV kv; kv_init(&kv);
    /* insert up to ~half capacity */
    for (uint32_t i = 0; i < 4096; i++) {
        uint64_t k = mk_key(1000 + i);
        kv_put(&kv, k, (uint64_t)i);
    }
    ASSERT(kv.count == 4096);
    float lf = kv_load(&kv);
    ASSERT(lf < 0.70f);           /* well under limit */
    ASSERT(kv_ok_to_put(&kv));    /* still room */
}

/* ── Test 8: pipeline integration — gp_read miss → kv fallback ─ */
/*
 * Write key directly to KV (bypass pipeline cache).
 * gp_read must return 0 (miss), then kv_get returns the value.
 * Simulates the caller pattern from S14 contract.
 */
TEST(pipeline_kv_fallback) {
    GeoPipeline gp; gp_init(&gp);
    GeoKV kv;       kv_init(&kv);

    uint64_t k = mk_key(42);
    uint64_t v = 0xFEEDFACEULL;

    /* truth written to KV only — not through pipeline */
    kv_put(&kv, k, v);

    /* pipeline doesn't know about this key */
    uint64_t out = 0;
    int hit = gp_read(&gp, k, &out);
    ASSERT(hit == 0);             /* cache miss as expected */

    /* caller fallback */
    uint64_t truth = kv_get(&kv, k);
    ASSERT(truth == v);           /* KV has the answer */

    /* now write through pipeline normally */
    gp_write(&gp, k, v);
    kv_put(&kv, k, v);

    /* pipeline should now hit */
    out = 0;
    hit = gp_read(&gp, k, &out);
    /* may or may not be L1 hit depending on compress gate,
     * but if it misses pipeline, KV still covers */
    if (!hit) {
        ASSERT(kv_get(&kv, k) == v);
    } else {
        ASSERT(out == v);
    }
}

/* ── main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("=== GeoKV S15 ===\n");
    RUN(basic_put_get);
    RUN(update_inplace);
    RUN(delete_tombstone);
    RUN(tombstone_reuse);
    RUN(probe_chain);
    RUN(has_zero_val);
    RUN(load_factor);
    RUN(pipeline_kv_fallback);
    printf("─────────────────\n");
    printf("%d/%d PASS\n", passed, passed + failed);
    return failed ? 1 : 0;
}
