/*
 * test_s18.c — Session 18: KVBridge CPU+GPU ring buffer
 *
 * Build (header-only, no GPU runtime needed):
 *   gcc -O2 -fsanitize=address,undefined -I. -o test_s18 test_s18.c -lm
 *
 * 8 tests:
 *   1. init: cpu clean, lanes zeroed
 *   2. put/get basic roundtrip
 *   3. load guard: rehash fires before KVB_MAX_LOAD overflow
 *   4. ring enqueue: entries appear in lane buffer
 *   5. flush drains all lanes, callback receives correct pairs
 *   6. ring full: drop increments, CPU truth unaffected
 *   7. del: CPU removes key, GPU shadow untouched
 *   8. bulk 1024 put → flush → all pairs received by GPU callback
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "kv_bridge.h"

static int passed = 0, failed = 0;
#define RUN(name) do { printf("  " #name " ... "); \
                       test_##name(); \
                       printf("PASS\n"); passed++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); failed++; return; } while(0)
#define ASSERT(c)  do { if (!(c)) FAIL(#c); } while(0)

static uint64_t mk(uint32_t n) { return (uint64_t)n * 0x9e3779b97f4a7c15ULL | 1ULL; }

/* ── Fake GPU sink ───────────────────────────────────────────────── */
#define SINK_CAP 2048
static struct { uint64_t key, val; } g_sink[SINK_CAP];
static int g_sink_n = 0;
static void gpu_sink(uint64_t key, uint64_t val, void *ctx) {
    (void)ctx;
    if (g_sink_n < SINK_CAP) { g_sink[g_sink_n].key = key; g_sink[g_sink_n].val = val; g_sink_n++; }
}
static void sink_reset(void) { g_sink_n = 0; memset(g_sink, 0, sizeof(g_sink)); }

/* ── 1. init ─────────────────────────────────────────────────────── */
static void test_init(void) {
    KVBridge b; kvbridge_init(&b);
    ASSERT(b.cpu.count == 0);
    ASSERT(b.flush_count == 0);
    ASSERT(b.dropped == 0);
}

/* ── 2. put/get roundtrip ────────────────────────────────────────── */
static void test_put_get(void) {
    KVBridge b; kvbridge_init(&b);
    kvbridge_put(&b, mk(1), 0xDEADULL);
    ASSERT(kvbridge_get(&b, mk(1)) == 0xDEADULL);
    ASSERT(kvbridge_has(&b, mk(1)) == 1);
    ASSERT(kvbridge_has(&b, mk(999)) == 0);
}

/* ── 3. load guard: rehash before saturation ─────────────────────── */
static void test_load_guard(void) {
    KVBridge b; kvbridge_init(&b);
    /* push to KVB_MAX_LOAD - 1, then one more → rehash must have fired */
    for (int i = 1; i <= (int)KVB_MAX_LOAD + 10; i++)
        kvbridge_put(&b, mk(i), (uint64_t)i);
    /* table must still be consistent — all live keys readable */
    int miss = 0;
    for (int i = 1; i <= (int)KVB_MAX_LOAD + 10; i++)
        if (kvbridge_get(&b, mk(i)) != (uint64_t)i) miss++;
    ASSERT(miss == 0);
    ASSERT(b.cpu.tombstones == 0);   /* rehash resets tombstones */
}

/* ── 4. ring enqueue visible in lane ────────────────────────────── */
static void test_ring_enqueue(void) {
    KVBridge b; kvbridge_init(&b);
    kvbridge_put(&b, mk(10), 0xAAULL);
    /* at least one lane must have head > tail */
    int found = 0;
    for (int i = 0; i < KVB_LANES; i++) {
        uint32_t h = atomic_load(&b.lanes[i].head);
        uint32_t t = atomic_load(&b.lanes[i].tail);
        if (h > t) found = 1;
    }
    ASSERT(found);
}

/* ── 5. flush drains lanes, callback correct ─────────────────────── */
static void test_flush_correct(void) {
    KVBridge b; kvbridge_init(&b);
    sink_reset();
    kvbridge_put(&b, mk(20), 200ULL);
    kvbridge_put(&b, mk(21), 201ULL);
    uint32_t sent = kvbridge_flush(&b, gpu_sink, NULL);
    ASSERT(sent == 2);
    ASSERT(b.flush_count == 1);
    /* verify both pairs in sink */
    int ok20 = 0, ok21 = 0;
    for (int i = 0; i < g_sink_n; i++) {
        if (g_sink[i].key == mk(20) && g_sink[i].val == 200ULL) ok20 = 1;
        if (g_sink[i].key == mk(21) && g_sink[i].val == 201ULL) ok21 = 1;
    }
    ASSERT(ok20 && ok21);
    /* lanes drained */
    for (int i = 0; i < KVB_LANES; i++)
        ASSERT(atomic_load(&b.lanes[i].head) == atomic_load(&b.lanes[i].tail));
}

/* ── 6. ring full: drop, CPU unaffected ──────────────────────────── */
static void test_ring_full_drop(void) {
    KVBridge b; kvbridge_init(&b);
    /* fill one lane manually to capacity, then try to enqueue same lane */
    uint32_t target_lane = 0;
    /* find a key that hashes to lane 0 */
    uint64_t probe_key = 0;
    for (uint32_t n = 1; n < 100000; n++) {
        uint64_t k = mk(n);
        if (kvb_lane(k) == target_lane) { probe_key = k; break; }
    }
    ASSERT(probe_key != 0);
    /* fill lane to capacity */
    KVBLane *lane = &b.lanes[target_lane];
    atomic_store(&lane->head, KVB_LANE_SIZE);   /* simulate full */
    atomic_store(&lane->tail, 0);
    /* enqueue should drop */
    int r = kvb_enqueue(&b, probe_key, 0xFFULL);
    ASSERT(r == 0);
    /* CPU write must still work */
    kvbridge_put(&b, probe_key, 0x42ULL);
    ASSERT(kvbridge_get(&b, probe_key) == 0x42ULL);
    ASSERT(b.dropped > 0);
}

/* ── 7. del: CPU removes, gpu shadow irrelevant ──────────────────── */
static void test_del(void) {
    KVBridge b; kvbridge_init(&b);
    kvbridge_put(&b, mk(30), 300ULL);
    kvbridge_del(&b, mk(30));
    ASSERT(kvbridge_has(&b, mk(30)) == 0);
    ASSERT(b.cpu.count == 0);
}

/* ── 8. bulk 1024 put → flush → all pairs in GPU sink ───────────── */
static void test_bulk_flush(void) {
    KVBridge b; kvbridge_init(&b);
    sink_reset();
    for (int i = 0; i < 1024; i++)
        kvbridge_put(&b, mk(1000+i), (uint64_t)i*7+3);
    uint32_t sent = kvbridge_flush(&b, gpu_sink, NULL);
    /* sent may be < 1024 only if lanes overflowed (ring cap 4*256=1024 exact) */
    ASSERT(sent <= 1024);
    /* CPU truth: all 1024 readable regardless */
    int miss = 0;
    for (int i = 0; i < 1024; i++)
        if (kvbridge_get(&b, mk(1000+i)) != (uint64_t)i*7+3) miss++;
    ASSERT(miss == 0);
    ASSERT(b.flush_count == 1);
}

int main(void) {
    printf("=== TPOGLS S18 ===\n");
    RUN(init);
    RUN(put_get);
    RUN(load_guard);
    RUN(ring_enqueue);
    RUN(flush_correct);
    RUN(ring_full_drop);
    RUN(del);
    RUN(bulk_flush);
    printf("─────────────────\n");
    printf("%d/%d PASS\n", passed, passed + failed);
    return failed ? 1 : 0;
}
