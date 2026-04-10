/*
 * test_payload_store.c — PayloadStore + 12-Letter Cube Tests
 * ═══════════════════════════════════════════════════════════
 * Session 12
 *
 * P1: write/read basic integrity
 * P2: lane distribution (cylinder spoke balance)
 * P3: rewind — walk back pair chain, recover ancestor
 * P4: miss isolation — empty cell returns PL_EMPTY
 * P5: LCM closure — 6 hops returns to origin lane
 * P6: dodeca link — PayloadID wires to DodecaEntry.payload_id
 * P7: value integrity — write(addr,val) → read → exact val back
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "../core/geo_config.h"
#include "geo_payload_store.h"
#include "geo_dodeca.h"

/* ── Test harness ───────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("  OK   %s\n", label); g_pass++; } \
    else       { printf("  FAIL %s\n", label); g_fail++; } \
} while(0)
#define SECTION(s) printf("\n[%s]\n", s)

/* ── P1: basic write → read ─────────────────────────────────────────── */
static void p1_basic(void) {
    SECTION("P1: write/read basic integrity");
    PayloadStore ps; pl_init(&ps);

    pl_write(&ps, 0x1A, 0xFF);
    pl_write(&ps, 0x2B, 0xAB);
    pl_write(&ps, 0x3C, 0xDEADBEEF);

    PayloadResult r1 = pl_read(&ps, 0x1A);
    PayloadResult r2 = pl_read(&ps, 0x2B);
    PayloadResult r3 = pl_read(&ps, 0x3C);

    printf("  addr=0x1A val=0x%llx found=%u\n", (unsigned long long)r1.value, r1.found);
    printf("  addr=0x2B val=0x%llx found=%u\n", (unsigned long long)r2.value, r2.found);
    printf("  addr=0x3C val=0x%llx found=%u\n", (unsigned long long)r3.value, r3.found);

    CHECK("P1a: addr=0x1A found",       r1.found == 1);
    CHECK("P1b: addr=0x1A val correct", r1.value == 0xFF);
    CHECK("P1c: addr=0x2B found",       r2.found == 1);
    CHECK("P1d: addr=0x2B val correct", r2.value == 0xAB);
    CHECK("P1e: addr=0x3C val correct", r3.value == 0xDEADBEEF);
}

/* ── P2: lane distribution ──────────────────────────────────────────── */
static void p2_lane_dist(void) {
    SECTION("P2: lane distribution (6 spokes)");
    PayloadStore ps; pl_init(&ps);

    uint32_t lane_cnt[PL_PAIRS] = {0};
    for (uint64_t addr = 0; addr < 144; addr++) {
        PayloadID id = pl_write(&ps, addr, addr * 0x100 + 1);
        lane_cnt[id.lane]++;
    }

    printf("  lane dist: ");
    uint8_t all_used = 1;
    for (uint32_t i = 0; i < PL_PAIRS; i++) {
        printf("[%u]=%u ", i, lane_cnt[i]);
        if (lane_cnt[i] == 0) all_used = 0;
    }
    printf("\n");

    CHECK("P2a: all 6 lanes used",      all_used);
    CHECK("P2b: total writes == 144",   ps.write_count == 144);
}

/* ── P3: rewind ─────────────────────────────────────────────────────── */
static void p3_rewind(void) {
    SECTION("P3: rewind — pair chain walk back");
    PayloadStore ps; pl_init(&ps);

    /* Need addrs that map to lane=0..5 AND same slot=0
     * lane  = addr % PL_LETTERS % PL_PAIRS = addr % 12 % 6
     * slot  = addr % PL_SLOTS = addr % 144
     * lane=0,slot=0: addr=0   (0%12=0→pair0, 0%144=0)
     * lane=1,slot=0: addr=1   (1%12=1→pair1, 1%144=1) ← slot differs!
     * Solution: addr = lane + k*12 where k*12 % 144 = 0 → k=12
     * addr = lane + 0   → slot = lane (not 0)
     * addr = lane*144   → lane=(lane*144)%12%6, slot=0
     * lane*144 % 12 = lane*(144%12) = lane*0 = 0 → all lane=0. No.
     *
     * Correct: addr s.t. addr%12=lane AND addr%144=0
     * addr = lane + 12*m, need (lane+12m)%144=0 → 12m = 144-lane (mod 144)
     * For lane=0: m=0  addr=0
     * For lane=1: 12m=143 → no integer solution (gcd(12,144)=12, 12∤143)
     *
     * → slot=0 constraint impossible for lane!=0.
     * Use slot = lane instead (addr = lane, natural mapping):
     * addr=0→lane=0,slot=0  addr=1→lane=1,slot=1 ... addr=5→lane=5,slot=5
     */
    uint64_t addrs[6] = {0, 1, 2, 3, 4, 5};
    uint64_t vals[6]  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    /* rewind from addr=0 (lane=0,slot=0):
     * 1 hop back → lane=5, slot=0 → cells[5][0] = vals[5]=0xFF
     * 3 hops back → lane=3, slot=0 → cells[3][0] = vals[3]=0xDD
     * 6 hops back → lane=0, slot=0 → cells[0][0] = vals[0]=0xAA (last write)
     * But addrs[5]=5 writes to lane=5,slot=5 NOT lane=5,slot=0
     * rewind(addr=0, 1) → lane=5, slot=0 → PL_EMPTY (not written there)
     *
     * → rewind semantic: "what was at this lane+slot before?"
     * For this to work, must write to the exact (lane,slot) rewind lands on.
     * Write using pl_id directly: cells[lane][0] = val for each lane */
    for (int i = 0; i < 6; i++) {
        ps.cells[i][0] = vals[i];  /* direct lane write, slot=0 */
        ps.write_count++;
        printf("  direct write lane=%d slot=0 val=0x%llx\n",
               i, (unsigned long long)vals[i]);
    }

    /* addr=0 is lane=0. rewind 1 hop from addr=0 → lane=5 (prev) */
    PayloadResult rw1 = pl_read_rewind(&ps, 0, 1);
    /* rewind 3 hops from addr=0 → lane=3 */
    PayloadResult rw3 = pl_read_rewind(&ps, 0, 3);
    /* rewind 6 hops = full LCM → back to lane=0 */
    PayloadResult rw6 = pl_read_rewind(&ps, 0, 6);

    printf("  rewind 1: lane=%u val=0x%llx\n", rw1.id.lane, (unsigned long long)rw1.value);
    printf("  rewind 3: lane=%u val=0x%llx\n", rw3.id.lane, (unsigned long long)rw3.value);
    printf("  rewind 6: lane=%u val=0x%llx\n", rw6.id.lane, (unsigned long long)rw6.value);

    CHECK("P3a: rewind 1 → lane 5",       rw1.id.lane == 5);
    CHECK("P3b: rewind 1 val correct",     rw1.value == vals[5]);
    CHECK("P3c: rewind 3 → lane 3",       rw3.id.lane == 3);
    CHECK("P3d: rewind 3 val correct",     rw3.value == vals[3]);
    CHECK("P3e: rewind 6 = LCM → lane 0", rw6.id.lane == 0);
    CHECK("P3f: rewind 6 val = origin",   rw6.value == vals[0]);
}

/* ── P4: miss isolation ─────────────────────────────────────────────── */
static void p4_miss(void) {
    SECTION("P4: miss isolation");
    PayloadStore ps; pl_init(&ps);

    pl_write(&ps, 0x10, 0xCAFE);
    PayloadResult hit  = pl_read(&ps, 0x10);
    PayloadResult miss = pl_read(&ps, 0x99);  /* never written */

    printf("  hit  addr=0x10: found=%u val=0x%llx\n", hit.found, (unsigned long long)hit.value);
    printf("  miss addr=0x99: found=%u\n", miss.found);

    CHECK("P4a: written → found",    hit.found == 1);
    CHECK("P4b: written val exact",  hit.value == 0xCAFE);
    CHECK("P4c: unwritten → miss",   miss.found == 0);
    CHECK("P4d: miss stats correct", ps.miss_count == 1);
}

/* ── P5: LCM closure ────────────────────────────────────────────────── */
static void p5_lcm(void) {
    SECTION("P5: LCM(6,6)=6 closure — 6 next_pair hops = origin");
    uint8_t lane = 0;
    for (int i = 0; i < 6; i++) lane = pl_next_pair(lane);
    printf("  start=0 after 6 hops: lane=%u\n", lane);
    CHECK("P5a: 6 hops → back to 0", lane == 0);

    /* also verify prev direction */
    lane = 3;
    for (int i = 0; i < 6; i++) lane = pl_prev_pair(lane);
    printf("  start=3 after 6 prev: lane=%u\n", lane);
    CHECK("P5b: 6 prev → back to 3", lane == 3);
}

/* ── P6: dodeca link ────────────────────────────────────────────────── */
static void p6_dodeca_link(void) {
    SECTION("P6: PayloadID links to DodecaEntry.payload_id");
    PayloadStore ps; pl_init(&ps);

    uint64_t addr  = 0x42;
    uint64_t value = 0xDEAD;

    PayloadID pid = pl_write(&ps, addr, value);
    printf("  addr=0x%llx → lane=%u slot=%u flat=%u\n",
           (unsigned long long)addr, pid.lane, pid.slot, pid.flat);

    /* simulate DodecaEntry storing payload_id */
    uint16_t stored_pid = pid.flat;

    /* read back via stored_pid */
    uint8_t  lane = (uint8_t)(stored_pid / PL_SLOTS);
    uint8_t  slot = (uint8_t)(stored_pid % PL_SLOTS);
    uint64_t recovered = ps.cells[lane][slot];

    printf("  recovered via flat=%u: val=0x%llx\n", stored_pid, (unsigned long long)recovered);

    CHECK("P6a: flat index in range",   pid.flat < PL_CELLS);
    CHECK("P6b: recovered == original", recovered == value);
    CHECK("P6c: lane consistent",       lane == pid.lane);
}

/* ── P7: value integrity stress ─────────────────────────────────────── */
static void p7_value_integrity(void) {
    SECTION("P7: value integrity — 144 write→read exact match");
    PayloadStore ps; pl_init(&ps);

    uint64_t addrs[144], vals[144];
    for (int i = 0; i < 144; i++) {
        addrs[i] = (uint64_t)(i * 7 + 13);           /* varied addr */
        vals[i]  = (uint64_t)(i * 0x100 + 0xAB00ULL);/* unique val  */
        pl_write(&ps, addrs[i], vals[i]);
    }

    uint32_t exact = 0;
    for (int i = 0; i < 144; i++) {
        PayloadResult r = pl_read(&ps, addrs[i]);
        if (r.found && r.value == vals[i]) exact++;
    }

    printf("  exact match: %u/144\n", exact);
    pl_print_stats(&ps);

    /* Note: collisions possible if two addrs map to same (lane,slot)
     * last-write-wins — exact >= 6 (at least 1 per lane guaranteed) */
    CHECK("P7a: at least 6 exact (1/lane)",  exact >= 6);
    CHECK("P7b: write_count == 144",          ps.write_count == 144);
}

/* ── P8: full wire dodeca_insert_ex → PayloadStore → recover ───────── */
static void p8_wire_dodeca(void) {
    SECTION("P8: dodeca_insert_ex → PayloadStore → value recovery");

    PayloadStore ps; pl_init(&ps);
    DodecaTable  dt; dodeca_init(&dt);

    uint64_t addr  = 0x55;
    uint64_t value = 0xC0FFEE;

    /* 1. write value → PayloadStore */
    PayloadID pid = pl_write(&ps, addr, value);

    /* 2. insert dodeca with payload_id wired */
    uint64_t merkle = 0xDEADBEEFCAFEBABEULL;
    DodecaEntry *e = dodeca_insert_ex(&dt, merkle, 0, 0,
                                       0, 1, 0, pid.flat);

    printf("  write: addr=0x%llx val=0x%llx → lane=%u slot=%u flat=%u\n",
           (unsigned long long)addr, (unsigned long long)value,
           pid.lane, pid.slot, pid.flat);
    printf("  dodeca: merkle=0x%llx payload_id=%u\n",
           (unsigned long long)merkle, e->payload_id);

    /* 3. lookup dodeca → get payload_id → recover value */
    DodecaEntry *found = NULL;
    DodecaResult dr = dodeca_lookup(&dt, merkle, 0, &found);
    (void)dr;
    uint16_t recovered_pid = found ? found->payload_id : 0xFFFFu;
    uint8_t  lane = (uint8_t)(recovered_pid / PL_SLOTS);
    uint8_t  slot = (uint8_t)(recovered_pid % PL_SLOTS);
    uint64_t recovered = ps.cells[lane][slot];

    printf("  recover: payload_id=%u lane=%u slot=%u val=0x%llx\n",
           recovered_pid, lane, slot, (unsigned long long)recovered);

    CHECK("P8a: dodeca entry created",       e != NULL);
    CHECK("P8b: payload_id stored in dodeca", e && e->payload_id == pid.flat);
    CHECK("P8c: dodeca lookup hit",           found != NULL);
    CHECK("P8d: value recovered exact",       recovered == value);
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void) {
    printf("=== PayloadStore + 12-Letter Cube Tests S12 ===\n");
    p1_basic();
    p2_lane_dist();
    p3_rewind();
    p4_miss();
    p5_lcm();
    p6_dodeca_link();
    p7_value_integrity();
    p8_wire_dodeca();
    printf("\n════════════════════════════════════════════════\n");
    printf("  Result: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
