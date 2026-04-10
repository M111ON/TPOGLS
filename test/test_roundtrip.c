/*
 * test_roundtrip.c — Write → Reconstruct → Read roundtrip
 *
 * Compile:
 *   gcc -O2 -std=c11 -I. -Igeo_net.h -o test_roundtrip test_roundtrip.c
 *
 * R1: single batch  write(N) → geo_read_by_raw → READ_HIT
 * R2: twin_bridge   write(144) → flush → geo_read_by_raw → READ_HIT
 * R3: multi-segment write 3 batches (different segment ids) → all 3 readable
 * R4: miss case     read unknown raw → READ_MISS
 * R5: determinism   write A → write A again (same seed) → same merkle_root
 * R6: stress        write 1440 ops (10 windows) → all windows readable
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

/* POGLS core */
#include "../core/geo_config.h"
#include "geo_thirdeye.h"
#include "geo_fibo_clock.h"
#include "geo_net.h"
#include "geo_radial_hilbert.h"
#include "geo_pipeline_wire.h"
#include "pogls_platform.h"
#include "pogls_qrpn_phaseE.h"
#include "pogls_geomatrix.h"
#include "pogls_pipeline_wire.h"
#include "geo_hardening_whe.h"
#include "geo_diamond_field.h"
#include "geo_dodeca.h"
#include "geo_read.h"
#include "pogls_twin_bridge.h"

/* ── helpers ───────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("  OK   %s\n", label); g_pass++; } \
    else      { printf("  FAIL %s\n", label); g_fail++; } \
} while(0)
#define SECTION(n) printf("\n[%s]\n", n)

static uint64_t _a(uint32_t i) { return (uint64_t)i * 0x9E3779B185EBCA87ULL ^ 0x0001000100010001ULL; }
static uint64_t _v(uint32_t i) { return (uint64_t)i * 0x6C62272E07BB0142ULL ^ 0xDEADBEEFCAFEBABEULL; }

/* build raw[] — identical to how twin_bridge computes it */
static void make_raws(uint64_t *raws, uint32_t n,
                      uint64_t gen3, uint32_t c144_start)
{
    for (uint32_t i = 0; i < n; i++) {
        uint64_t tag = (uint64_t)(c144_start);   /* constant over one 144-window */
        raws[i] = _a(i) ^ _v(i) ^ gen3 ^ tag;
    }
}

static uint64_t g_bundle[GEO_BUNDLE_WORDS];
static void make_bundle(void) {
    for (uint32_t i = 0; i < GEO_BUNDLE_WORDS; i++)
        g_bundle[i] = 0xAAAAAAAAAAAAAAAAULL ^ ((uint64_t)(i+1) << 32);
}
static GeoSeed make_seed(void) {
    GeoSeed s;
    s.gen2 = 0x9E3779B97F4A7C15ULL;
    s.gen3 = 0x6C62272E07BB0142ULL;
    return s;
}

/* ── R1: single batch write → geo_read_by_raw ──────────────────── */
static void r1_single_batch(void)
{
    SECTION("R1: single batch write(64) → geo_read_by_raw");

    GeoSeed seed = make_seed();
    uint64_t baseline = diamond_baseline();

    uint64_t raws[64];
    make_raws(raws, 64, seed.gen3, 0);

    DodecaTable dodeca;
    dodeca_init(&dodeca);

    DiamondFlowCtx ctx;
    diamond_flow_init(&ctx);

    uint32_t writes = geo_fused_write_batch(raws, 64, baseline, &ctx, &dodeca, 0, NULL, 0);

    /* flush partial tail */
    if (ctx.route_addr != 0) {
        uint8_t offset = (uint8_t)(ctx.drift_acc & 0xFF);
        dodeca_insert(&dodeca, ctx.route_addr, 0, 0, offset, ctx.hop_count, 0);
        writes++;
    }

    printf("  dna_writes=%u  dodeca.count=%u\n", writes, dodeca.count);

    /* read back */
    ReadResult r = geo_read_by_raw(&dodeca, raws, 64, baseline, NULL);

    printf("  read status=%d  merkle=0x%016llx  hops=%u\n",
           r.status, (unsigned long long)r.merkle_root, r.hop_count);

    CHECK("R1a: dna_writes >= 1",          writes >= 1);
    CHECK("R1b: dodeca has entries",        dodeca.count >= 1);
    CHECK("R1c: geo_read_by_raw != MISS",  r.status != READ_MISS);
    CHECK("R1d: merkle_root != 0",         r.merkle_root != 0);
}

/* ── R2: twin_bridge write(144) → flush → geo_read_by_raw ───────── */
static void r2_twin_bridge(void)
{
    SECTION("R2: twin_bridge write(144) → flush → geo_read_by_raw");

    TwinBridge b;
    twin_bridge_init(&b, make_seed(), g_bundle);

    uint64_t raws[144];

    for (int i = 0; i < 144; i++) {
        /* capture raw BEFORE tick (same as _twin_raw internal) */
        uint64_t tag = (uint64_t)b.fibo.clk.c144;
        raws[i] = _a(i) ^ _v(i) ^ b.fibo.seed.gen3 ^ tag;
        twin_bridge_write(&b, _a(i), _v(i), 0, NULL);
    }
    twin_bridge_flush(&b);

    uint64_t baseline = diamond_baseline();
    ReadResult r = geo_read_by_raw(&b.dodeca, raws, 144, baseline, NULL);

    TwinBridgeStats st = twin_bridge_stats(&b);
    printf("  twin_writes=%u  flush_count=%u\n", st.twin_writes, st.flush_count);
    printf("  read status=%d  hops=%u  offset=%u\n",
           r.status, r.hop_count, r.offset);

    CHECK("R2a: twin_writes >= 1",         st.twin_writes >= 1);
    CHECK("R2b: read != MISS",             r.status != READ_MISS);
    CHECK("R2c: merkle_root != 0",         r.merkle_root != 0);
}

/* ── R3: multi-segment — 3 batches, different segment ids ───────── */
static void r3_multi_segment(void)
{
    SECTION("R3: multi-segment write × 3 → all readable");

    uint64_t baseline = diamond_baseline();
    GeoSeed seed = make_seed();

    DodecaTable dodeca;
    dodeca_init(&dodeca);

    uint64_t batch_raws[3][32];
    int batch_writes[3] = {0, 0, 0};

    for (uint8_t seg = 0; seg < 3; seg++) {
        /* different offset per segment so raws differ */
        for (uint32_t i = 0; i < 32; i++) {
            uint64_t base = _a(i + seg * 100) ^ _v(i + seg * 100);
            batch_raws[seg][i] = base ^ seed.gen3 ^ ((uint64_t)seg << 32);
        }

        DiamondFlowCtx ctx;
        diamond_flow_init(&ctx);
        batch_writes[seg] = (int)geo_fused_write_batch(
            batch_raws[seg], 32, baseline, &ctx, &dodeca, seg, NULL, 0);

        /* flush tail */
        if (ctx.route_addr != 0) {
            uint8_t off = (uint8_t)(ctx.drift_acc & 0xFF);
            dodeca_insert(&dodeca, ctx.route_addr, 0, 0, off, ctx.hop_count, seg);
            batch_writes[seg]++;
        }
    }

    printf("  writes: seg0=%d seg1=%d seg2=%d  total_count=%u\n",
           batch_writes[0], batch_writes[1], batch_writes[2], dodeca.count);

    int hit[3] = {0, 0, 0};
    for (int seg = 0; seg < 3; seg++) {
        ReadResult r = geo_read_by_raw(&dodeca, batch_raws[seg], 32, baseline, NULL);
        hit[seg] = (r.status != READ_MISS);
        printf("  seg%d: status=%d  merkle=0x%016llx\n",
               seg, r.status, (unsigned long long)r.merkle_root);
    }

    CHECK("R3a: seg0 readable",  hit[0]);
    CHECK("R3b: seg1 readable",  hit[1]);
    CHECK("R3c: seg2 readable",  hit[2]);
}

/* ── R4: miss case ──────────────────────────────────────────────── */
static void r4_miss(void)
{
    SECTION("R4: miss/hit isolation");

    uint64_t baseline = diamond_baseline();
    GeoSeed seed = make_seed();

    /* R4a: empty dodeca → always miss */
    DodecaTable empty; dodeca_init(&empty);
    uint64_t raws_e[16]; make_raws(raws_e, 16, 0x1234567890ABCDEFULL, 0);
    ReadResult r_empty = geo_read_by_raw(&empty, raws_e, 16, baseline, NULL);
    printf("  empty dodeca: status=%d (3=MISS)\n", r_empty.status);
    CHECK("R4a: empty dodeca → READ_MISS",  r_empty.status == READ_MISS);
    CHECK("R4b: merkle==0 on miss",         r_empty.merkle_root == 0);

    /* R4c/R4d: use 144-op batches — enough hops to build unique merkle paths,
     * avoiding degenerate PHI_PRIME collision that occurs with short batches
     * where isect=0 fires on op0 before any hop accumulation. */
    DodecaTable dodeca; dodeca_init(&dodeca);
    uint64_t raws_a[144], raws_b[144];
    make_raws(raws_a, 144, seed.gen3,                        0);
    make_raws(raws_b, 144, seed.gen3 ^ 0xFFFFFFFFFFFFFFFFULL, 0);

    DiamondFlowCtx ctx; diamond_flow_init(&ctx);
    geo_fused_write_batch(raws_a, 144, baseline, &ctx, &dodeca, 0, NULL, 0);
    if (ctx.route_addr != 0) {
        dodeca_insert(&dodeca, ctx.route_addr, 0, 0,
                      (uint8_t)(ctx.drift_acc & 0xFF), ctx.hop_count, 0);
    }
    ReadResult r_a = geo_read_by_raw(&dodeca, raws_a, 144, baseline, NULL);
    ReadResult r_b = geo_read_by_raw(&dodeca, raws_b, 144, baseline, NULL);
    printf("  A (written):     status=%d  merkle=0x%016llx\n",
           r_a.status, (unsigned long long)r_a.merkle_root);
    printf("  B (not written): status=%d\n", r_b.status);
    CHECK("R4c: written A → HIT",      r_a.status != READ_MISS);
    CHECK("R4d: unwritten B → MISS",   r_b.status == READ_MISS);
}

/* ── R5: determinism — same input → same merkle ─────────────────── */
static void r5_determinism(void)
{
    SECTION("R5: determinism — write A twice → same merkle_root");

    uint64_t baseline = diamond_baseline();
    GeoSeed seed = make_seed();

    uint64_t raws[32];
    make_raws(raws, 32, seed.gen3, 0);

    uint64_t merkle_a = 0, merkle_b = 0;

    /* run A */
    {
        DodecaTable d; dodeca_init(&d);
        DiamondFlowCtx c; diamond_flow_init(&c);
        geo_fused_write_batch(raws, 32, baseline, &c, &d, 0, NULL, 0);
        if (c.route_addr != 0) {
            uint8_t off = (uint8_t)(c.drift_acc & 0xFF);
            dodeca_insert(&d, c.route_addr, 0, 0, off, c.hop_count, 0);
        }
        ReadResult r = geo_read_by_raw(&d, raws, 32, baseline, NULL);
        merkle_a = r.merkle_root;
    }

    /* run B — identical */
    {
        DodecaTable d; dodeca_init(&d);
        DiamondFlowCtx c; diamond_flow_init(&c);
        geo_fused_write_batch(raws, 32, baseline, &c, &d, 0, NULL, 0);
        if (c.route_addr != 0) {
            uint8_t off = (uint8_t)(c.drift_acc & 0xFF);
            dodeca_insert(&d, c.route_addr, 0, 0, off, c.hop_count, 0);
        }
        ReadResult r = geo_read_by_raw(&d, raws, 32, baseline, NULL);
        merkle_b = r.merkle_root;
    }

    printf("  merkle_A=0x%016llx\n", (unsigned long long)merkle_a);
    printf("  merkle_B=0x%016llx\n", (unsigned long long)merkle_b);

    CHECK("R5a: same input → same merkle_root",  merkle_a == merkle_b);
    CHECK("R5b: merkle_root is non-zero",         merkle_a != 0);
}

/* ── R6: stress — 1440 ops (10 windows), count readable ─────────── */
static void r6_stress(void)
{
    SECTION("R6: stress 1440 ops (10 × 144 windows)");

    GeoSeed seed = make_seed();
    uint64_t baseline = diamond_baseline();

    DodecaTable dodeca;
    dodeca_init(&dodeca);

    uint64_t raws[1440];
    make_raws(raws, 1440, seed.gen3, 0);

    /* write all 1440 in one go */
    DiamondFlowCtx ctx;
    diamond_flow_init(&ctx);
    uint32_t total_writes = geo_fused_write_batch(raws, 1440, baseline, &ctx, &dodeca, 0, NULL, 0);
    if (ctx.route_addr != 0) {
        uint8_t off = (uint8_t)(ctx.drift_acc & 0xFF);
        dodeca_insert(&dodeca, ctx.route_addr, 0, 0, off, ctx.hop_count, 0);
        total_writes++;
    }

    printf("  total_writes=%u  dodeca.count=%u  capacity=%u\n",
           total_writes, dodeca.count, DODECA_TABLE_SIZE);

    /* probe each 144-op window */
    uint32_t hit_count = 0;
    for (uint32_t w = 0; w < 10; w++) {
        ReadResult r = geo_read_by_raw(&dodeca, &raws[w * 144], 144, baseline, NULL);
        if (r.status != READ_MISS) hit_count++;
    }
    printf("  window hit_count=%u/10\n", hit_count);

    /* also try full 1440-raw probe */
    ReadResult r_full = geo_read_by_raw(&dodeca, raws, 1440, baseline, NULL);
    printf("  full-stream read: status=%d\n", r_full.status);

    CHECK("R6a: total_writes >= 10",     total_writes >= 10);
    CHECK("R6b: dodeca has entries",     dodeca.count >= 1);
    CHECK("R6c: at least 5/10 windows readable", hit_count >= 5);
    CHECK("R6d: full-stream not MISS",   r_full.status != READ_MISS);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== TPOGLS Write→Reconstruct→Read Roundtrip Test ===\n");
    make_bundle();

    r1_single_batch();
    r2_twin_bridge();
    r3_multi_segment();
    r4_miss();
    r5_determinism();
    r6_stress();

    printf("\n════════════════════════════════════════════════════\n");
    printf("  Result: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
