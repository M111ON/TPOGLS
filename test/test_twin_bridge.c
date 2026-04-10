/*
 * test_twin_bridge.c — S10 Pattern Observation Test
 *
 * Compile:
 *   gcc -O2 -std=c11 -I./core -I./twin -o test_twin_bridge test_twin_bridge.c
 *
 * T1: FiboClock boundary fires (period 17/72/144)
 * T2: TwinBridge write→flush→lookup roundtrip
 * T3: raw mix determinism (same seed+input → same raw)
 * T4: isect==0 (dead packet) rate — decides GPU prefilter worth
 * T5: POGLS spoke/phase distribution over 144 ops
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

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
#define INFO(label) printf("  INFO %s\n", label)
#define SECTION(n) printf("\n[%s]\n", n)

static uint64_t _a(uint32_t i)  { return (uint64_t)i * 0x9E3779B185EBCA87ULL ^ 0x0001000100010001ULL; }
static uint64_t _v(uint32_t i)  { return (uint64_t)i * 0x6C62272E07BB0142ULL ^ 0xDEADBEEFCAFEBABEULL; }

static uint64_t g_bundle[9];
static void make_bundle(void) {
    for (int i = 0; i < 9; i++)
        g_bundle[i] = 0xAAAAAAAAAAAAAAAAULL ^ ((uint64_t)(i+1) << 32);
}

static GeoSeed make_seed(void) {
    GeoSeed s;
    s.gen2 = 0x9E3779B97F4A7C15ULL;
    s.gen3 = 0x6C62272E07BB0142ULL;
    return s;
}

/* ── T1: FiboClock boundaries ──────────────────────────────────── */
/*
 * DRIFT condition: delta = sum9 - sum8 ลดลงจาก cycle ก่อน
 * ต้องใช้ fx ที่ oscillate (สูง→ต่ำ→สูง) เพื่อให้ delta reverse จริง
 * pattern: 72 ops high → 72 ops low → repeat = การันตี DRIFT ใน cycle 2
 */
static void t1_fibo_clock(void)
{
    SECTION("T1: FiboClock boundary (2 cycles = 288 ops)");
    FiboCtx fibo;
    fibo_ctx_init(&fibo);
    fibo_ctx_set_seed(&fibo, make_seed());

    int first_flush = -1, first_drift = -1, first_sig = -1;
    int flush_count = 0;

    for (int i = 0; i < 288; i++) {
        /* oscillate: high (0xF0000000) for first 72, low (0x01) next 72, repeat
         * → sum8/sum9 accumulate large then small → delta reverses at op 144 */
        uint32_t fx = ((i / 72) % 2 == 0)
                    ? (uint32_t)(0xF0000000u ^ (i & 0xFFu))
                    : (uint32_t)(0x00000001u ^ (i & 0xFFu));

        FiboEvent ev = fibo_clock_tick(&fibo, fx);
        if ((ev & FIBO_EV_FLUSH)    && first_flush < 0) first_flush = i;
        if ((ev & FIBO_EV_DRIFT)    && first_drift < 0) first_drift = i;
        if ((ev & FIBO_EV_SIG_FAIL) && first_sig   < 0) first_sig   = i;
        if  (ev & FIBO_EV_FLUSH)    flush_count++;
    }

    printf("  first FLUSH=%d  first DRIFT=%d  first SIG_FAIL=%d\n",
           first_flush, first_drift, first_sig);
    printf("  flush_count over 288 ops = %d (expected 2)\n", flush_count);

    CHECK("T1a: FLUSH fires within 288 ops",   first_flush >= 0);
    CHECK("T1b: FLUSH at op 143 (period=144)", first_flush == 143);
    /* T1c: DRIFT fires when delta reverses (op 71 or 143 — after high→low transition) */
    CHECK("T1c: DRIFT fires within 288 ops",   first_drift >= 0);
    CHECK("T1d: flush_count==2 over 288 ops",  flush_count == 2);
}

/* ── T2: write → flush → dodeca probe ─────────────────────────── */
static void t2_roundtrip(void)
{
    SECTION("T2: write(144) → flush → dodeca state");
    TwinBridge b;
    twin_bridge_init(&b, make_seed(), g_bundle);

    FiboEvent ev_all = FIBO_EV_NONE;
    int flush_op = -1;

    for (int i = 0; i < 144; i++) {
        FiboEvent ev = twin_bridge_write(&b, _a(i), _v(i), 0, NULL);
        ev_all |= ev;
        if ((ev & FIBO_EV_FLUSH) && flush_op < 0) flush_op = i;
    }
    twin_bridge_flush(&b);  /* flush partial tail */

    TwinBridgeStats st = twin_bridge_stats(&b);
    printf("  total_ops=%u  twin_writes=%u  flush_count=%u  density=%.3f\n",
           st.total_ops, st.twin_writes, st.flush_count, st.write_density);
    printf("  FLUSH fired at op=%d  qrpn_fails=%u\n", flush_op, st.qrpn_fails);

    CHECK("T2a: total_ops==144",       st.total_ops == 144);
    CHECK("T2b: twin_writes >= 1",     st.twin_writes >= 1);
    CHECK("T2c: FLUSH event fired",    ev_all & FIBO_EV_FLUSH);
    CHECK("T2d: flush_count >= 1",     st.flush_count >= 1);
    CHECK("T2e: flush_op==143",        flush_op == 143);
}

/* ── T3: raw mix determinism ───────────────────────────────────── */
static void t3_determinism(void)
{
    SECTION("T3: raw mix determinism");

    GeoSeed s = make_seed();
    uint64_t raw_a[16], raw_b[16];

    /* run A */
    {
        TwinBridge ba; twin_bridge_init(&ba, s, g_bundle);
        for (int i = 0; i < 16; i++) {
            uint64_t tag = (uint64_t)ba.fibo.clk.c144;
            raw_a[i] = _a(i) ^ _v(i) ^ ba.fibo.seed.gen3 ^ tag;
            twin_bridge_write(&ba, _a(i), _v(i), 0, NULL);
        }
    }
    /* run B — same seed/input */
    {
        TwinBridge bb; twin_bridge_init(&bb, s, g_bundle);
        for (int i = 0; i < 16; i++) {
            uint64_t tag = (uint64_t)bb.fibo.clk.c144;
            raw_b[i] = _a(i) ^ _v(i) ^ bb.fibo.seed.gen3 ^ tag;
            twin_bridge_write(&bb, _a(i), _v(i), 0, NULL);
        }
    }

    int all_match = 1;
    for (int i = 0; i < 16; i++)
        if (raw_a[i] != raw_b[i]) { all_match = 0; break; }

    printf("  raw[0] =0x%016llx\n", (unsigned long long)raw_a[0]);
    printf("  raw[15]=0x%016llx\n", (unsigned long long)raw_a[15]);

    /* different seed must differ */
    GeoSeed s2 = { .gen2 = 0x1111111111111111ULL, .gen3 = 0x2222222222222222ULL };
    TwinBridge bc; twin_bridge_init(&bc, s2, g_bundle);
    uint64_t tag_c = (uint64_t)bc.fibo.clk.c144;
    uint64_t raw_c0 = _a(0) ^ _v(0) ^ bc.fibo.seed.gen3 ^ tag_c;

    CHECK("T3a: same seed → same raw (deterministic)", all_match);
    CHECK("T3b: different seed → different raw",        raw_c0 != raw_a[0]);
}

/* ── T4: isect==0 rate (GPU prefilter decision) ────────────────── */
static void t4_dead_rate(void)
{
    SECTION("T4: isect==0 rate over 1440 ops (10 windows)");

    GeoSeed s = make_seed();
    FiboCtx fibo; fibo_ctx_init(&fibo); fibo_ctx_set_seed(&fibo, s);

    uint32_t dead = 0, alive = 0;

    for (int i = 0; i < 1440; i++) {
        uint64_t tag = (uint64_t)fibo.clk.c144;
        uint64_t raw = _a(i) ^ _v(i) ^ fibo.seed.gen3 ^ tag;
        uint64_t isect = geo_fast_intersect(raw);
        if (isect == 0) dead++; else alive++;

        uint32_t fx = (uint32_t)(raw & 0xFFFFFFFF);
        fibo_clock_tick(&fibo, fx);
    }

    float dead_pct = dead * 100.0f / 1440.0f;
    printf("  dead(isect==0)=%u  alive=%u  dead_rate=%.1f%%\n",
           dead, alive, dead_pct);
    printf("  GPU prefilter: %s\n",
           dead_pct >= 50.0f ? "WORTH IT (>50%)" : "NOT WORTH IT (<50%)");

    CHECK("T4a: alive > 0",          alive > 0);
    CHECK("T4b: dead+alive==1440",   dead + alive == 1440);
}

/* ── T5: POGLS spoke/phase pattern over 144 ops ─────────────────── */
static void t5_pogls_pattern(void)
{
    SECTION("T5: POGLS spoke/phase distribution (144 ops)");
    PipelineWire pw;
    pipeline_wire_init(&pw, make_seed(), g_bundle);

    uint32_t spoke_hist[6] = {0};
    uint32_t phase_hist[4] = {0};
    uint32_t sig_ok = 0, audit_fail = 0;

    for (int i = 0; i < 144; i++) {
        PipelineResult r;
        pipeline_wire_process(&pw, _a(i), _v(i), 0, &r);
        if (r.pkt.spoke < 6) spoke_hist[r.pkt.spoke]++;
        phase_hist[r.pkt.phase & 3]++;
        if (r.sig_ok)     sig_ok++;
        if (r.audit_fail) audit_fail++;
    }

    printf("  spoke dist: ");
    for (int i = 0; i < 6; i++) printf("[%d]=%u ", i, spoke_hist[i]);
    printf("\n  phase dist: ");
    for (int i = 0; i < 4; i++) printf("[%d]=%u ", i, phase_hist[i]);
    printf("\n  sig_ok=%u/144  audit_fail=%u  qrpn_fails=%u\n",
           sig_ok, audit_fail, pw.qrpn_fails);

    int spokes_used = 0;
    for (int i = 0; i < 6; i++) if (spoke_hist[i] > 0) spokes_used++;
    int phases_used = 0;
    for (int i = 0; i < 4; i++) if (phase_hist[i] > 0) phases_used++;

    CHECK("T5a: total_ops==144",         pw.total_ops == 144);
    CHECK("T5b: >= 2 spokes used",       spokes_used >= 2);
    CHECK("T5c: >= 2 phases used",       phases_used >= 2);
    CHECK("T5d: no OOB spoke (all<6)",
          spoke_hist[0]+spoke_hist[1]+spoke_hist[2]+
          spoke_hist[3]+spoke_hist[4]+spoke_hist[5] == 144);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== TwinBridge Pattern Test S10 ===\n");
    make_bundle();

    t1_fibo_clock();
    t2_roundtrip();
    t3_determinism();
    t4_dead_rate();
    t5_pogls_pattern();

    printf("\n════════════════════════════════════\n");
    printf("  Result: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
