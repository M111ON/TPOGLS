/*
 * test_s17.c — Session 17: rehash + SDK .so path
 *
 * Build:
 *   gcc -O2 -fsanitize=address,undefined -I. -o test_s17 test_s17.c -lm
 *
 * 8 tests:
 *   1. rehash not needed when no deletes
 *   2. rehash triggered at >30% tombstone
 *   3. rehash preserves all live keys
 *   4. rehash resets tombstone count
 *   5. kv_del_auto triggers rehash automatically
 *   6. .so path: open/write/read/close via so API
 *   7. .so rewind: cache reset, truth survives
 *   8. .so bulk 512 writes → all readable after rewind
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "geo_kv_rehash.h"
#include "pogls_sdk.h"
#include "pogls_sdk_so.h"

static int passed = 0, failed = 0;
#define TEST(name) static void test_##name(void)
#define RUN(name)  do { printf("  " #name " ... "); \
                        test_##name(); \
                        printf("PASS\n"); passed++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); failed++; return; } while(0)
#define ASSERT(c)  do { if (!(c)) FAIL(#c); } while(0)

static uint64_t mk(uint32_t n) { return (uint64_t)n * 0x9e3779b97f4a7c15ULL | 1ULL; }

/* ── 1. no rehash needed without deletes ─────────────────────── */
TEST(no_rehash_needed) {
    GeoKV kv; kv_init(&kv);
    for (int i = 0; i < 100; i++) kv_put(&kv, mk(i+1), i);
    ASSERT(!kvr_needs_rehash(&kv));
}

/* ── 2. rehash triggered at >30% tombstone ───────────────────── */
TEST(rehash_trigger) {
    GeoKV kv; kv_init(&kv);
    /* insert 100, delete 40 → tombstone=40, total=60+40=100 → 40% > 30% */
    for (int i = 0; i < 100; i++) kv_put(&kv, mk(i+1), i);
    for (int i = 0; i < 40;  i++) kv_del(&kv, mk(i+1));
    ASSERT(kvr_needs_rehash(&kv));
}

/* ── 3. rehash preserves all live keys ───────────────────────── */
TEST(rehash_preserves) {
    GeoKV kv; kv_init(&kv);
    for (int i = 0; i < 100; i++) kv_put(&kv, mk(i+1), (uint64_t)i*3+7);
    for (int i = 0; i < 40;  i++) kv_del(&kv, mk(i+1));
    kv_rehash(&kv);
    int miss = 0;
    for (int i = 40; i < 100; i++)
        if (kv_get(&kv, mk(i+1)) != (uint64_t)i*3+7) miss++;
    ASSERT(miss == 0);
    ASSERT(kv.count == 60);
}

/* ── 4. rehash resets tombstone count ────────────────────────── */
TEST(rehash_tombstone_reset) {
    GeoKV kv; kv_init(&kv);
    for (int i = 0; i < 100; i++) kv_put(&kv, mk(i+1), i);
    for (int i = 0; i < 40;  i++) kv_del(&kv, mk(i+1));
    kv_rehash(&kv);
    ASSERT(kv.tombstones == 0);
}

/* ── 5. kv_del_auto auto-triggers rehash ─────────────────────── */
TEST(del_auto_rehash) {
    GeoKV kv; kv_init(&kv);
    for (int i = 0; i < 100; i++) kv_put(&kv, mk(i+1), i);
    /* del 40: rehash fires at #31 (tomb=31/100=31%>30%) → resets tomb
     * remaining 9 dels add tomb again → final tomb=9, NOT 0
     * proof: if no rehash, tomb would be 40 — we see < 40 → fired */
    for (int i = 0; i < 40; i++) kv_del_auto(&kv, mk(i+1));
    ASSERT(kv.tombstones < 40);   /* rehash fired at least once */
    ASSERT(kv.count == 60);
}

/* ── 6. .so path: open/write/read/close ─────────────────────── */
TEST(so_roundtrip) {
    PoglsHandle h = pogls_so_open();
    ASSERT(h != NULL);
    pogls_so_write(h, mk(200), 0xABCDULL);
    ASSERT(pogls_so_read(h, mk(200)) == 0xABCDULL);
    ASSERT(pogls_so_has(h, mk(200)) == 1);
    ASSERT(pogls_so_has(h, mk(999)) == 0);
    pogls_so_close(h);
}

/* ── 7. .so rewind: cache reset, truth survives ─────────────── */
TEST(so_rewind) {
    PoglsHandle h = pogls_so_open();
    pogls_so_write(h, mk(201), 0xBEEFULL);
    pogls_so_rewind(h);
    ASSERT(pogls_so_has(h, mk(201)));
    ASSERT(pogls_so_read(h, mk(201)) == 0xBEEFULL);
    pogls_so_close(h);
}

/* ── 8. .so bulk 512 → all readable after rewind ─────────────── */
TEST(so_bulk_rewind) {
    PoglsHandle h = pogls_so_open();
    for (int i = 0; i < 512; i++)
        pogls_so_write(h, mk(300+i), (uint64_t)i*11+1);
    pogls_so_rewind(h);
    int miss = 0;
    for (int i = 0; i < 512; i++)
        if (pogls_so_read(h, mk(300+i)) != (uint64_t)i*11+1) miss++;
    ASSERT(miss == 0);
    pogls_so_close(h);
}

int main(void) {
    printf("=== TPOGLS S17 ===\n");
    RUN(no_rehash_needed);
    RUN(rehash_trigger);
    RUN(rehash_preserves);
    RUN(rehash_tombstone_reset);
    RUN(del_auto_rehash);
    RUN(so_roundtrip);
    RUN(so_rewind);
    RUN(so_bulk_rewind);
    printf("─────────────────\n");
    printf("%d/%d PASS\n", passed, passed + failed);
    return failed ? 1 : 0;
}
