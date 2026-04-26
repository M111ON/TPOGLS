/*
 * bench_fused_x4.c — geo_fused_write_batch scalar vs x4 (AVX2)
 * S54 Twin Geo benchmark
 *
 * Build (AVX2):
 *   gcc -O3 -mavx2 -march=native -I./twin_core \
 *       bench_fused_x4.c -o bench_fused_x4
 *
 * Build (scalar fallback, no AVX2):
 *   gcc -O3 -I./twin_core bench_fused_x4.c -o bench_fused_x4_scalar
 *
 * Output:
 *   scalar  : X.XX ns/elem  Y,YYY Minputs/s
 *   x4 AVX2 : X.XX ns/elem  Y,YYY Minputs/s
 *   speedup : X.XXx
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── twin_core header chain ─────────────────────────────────────── */
#include "geo_config.h"
#include "geo_fibo_clock.h"
#include "geo_dodeca.h"
#include "geo_diamond_field.h"
#include "theta_map.h"
#include "geo_route.h"
#include "geo_whe.h"
#include "geo_read.h"
#include "geo_hardening_whe_s54.h"   /* includes scalar + x4 */

/* ── bench config ───────────────────────────────────────────────── */
#define BENCH_N       (1 << 20)   /* 1M inputs per run               */
#define BENCH_WARMUP  3           /* warmup runs (not timed)         */
#define BENCH_RUNS    7           /* timed runs — take median        */

/* ── helpers ────────────────────────────────────────────────────── */
static uint64_t _now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int _cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint64_t _median(uint64_t *arr, int n) {
    qsort(arr, n, sizeof(uint64_t), _cmp_u64);
    return arr[n / 2];
}

/* ── input generation ───────────────────────────────────────────── */
static void gen_inputs(uint64_t *buf, uint32_t n, uint64_t seed) {
    uint64_t x = seed;
    for (uint32_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;  /* xorshift64 */
        buf[i] = x;
    }
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void) {
    printf("bench_fused_x4 — S54 Twin Geo\n");
    printf("N = %d inputs per run, %d timed runs (median)\n\n",
           BENCH_N, BENCH_RUNS);

    /* allocate input buffer (aligned 32B for AVX2 loads) */
    uint64_t *raw = (uint64_t*)aligned_alloc(32, BENCH_N * sizeof(uint64_t));
    if (!raw) { fprintf(stderr, "alloc failed\n"); return 1; }
    gen_inputs(raw, BENCH_N, 0xDEADBEEFCAFEBABEULL);

    uint64_t baseline = diamond_baseline();

    /* ── SCALAR bench ─────────────────────────────────────────── */
    {
        uint64_t times[BENCH_RUNS];
        uint32_t dna_total = 0;

        for (int r = 0; r < BENCH_WARMUP + BENCH_RUNS; r++) {
            DodecaTable dodeca; dodeca_init(&dodeca);
            DiamondFlowCtx ctx; diamond_flow_init(&ctx);

            uint64_t t0 = _now_ns();
            uint32_t dna = geo_fused_write_batch(raw, BENCH_N, baseline,
                                                  &ctx, &dodeca, 0, NULL, 0);
            uint64_t t1 = _now_ns();

            if (r >= BENCH_WARMUP) {
                times[r - BENCH_WARMUP] = t1 - t0;
                dna_total += dna;
            }
        }

        uint64_t med_ns = _median(times, BENCH_RUNS);
        double ns_per   = (double)med_ns / BENCH_N;
        double minputs  = 1000.0 / ns_per;   /* 1e9 ns/s ÷ (ns/elem * 1e6) */

        printf("scalar   : %6.2f ns/elem   %8.1f Minputs/s   dna=%u\n",
               ns_per, minputs, dna_total / BENCH_RUNS);
    }

#ifdef __AVX2__
    /* ── AVX2 x4 bench ──────────────────────────────────────────── */
    {
        uint64_t times[BENCH_RUNS];
        uint32_t dna_total = 0;

        for (int r = 0; r < BENCH_WARMUP + BENCH_RUNS; r++) {
            DodecaTable dodeca; dodeca_init(&dodeca);
            DiamondFlowCtx4 ctx4; diamond_flow4_init(&ctx4);
            DiamondFlowCtx  ctx;  diamond_flow_init(&ctx);

            uint64_t t0 = _now_ns();
            uint32_t dna = geo_fused_write_batch_x4(raw, BENCH_N, baseline,
                                                     &ctx4, &ctx, &dodeca, 0);
            uint64_t t1 = _now_ns();

            if (r >= BENCH_WARMUP) {
                times[r - BENCH_WARMUP] = t1 - t0;
                dna_total += dna;
            }
        }

        uint64_t med_ns = _median(times, BENCH_RUNS);
        double ns_per   = (double)med_ns / BENCH_N;
        double minputs  = 1000.0 / ns_per;

        printf("x4 AVX2  : %6.2f ns/elem   %8.1f Minputs/s   dna=%u\n",
               ns_per, minputs, dna_total / BENCH_RUNS);
    }
#else
    printf("x4 AVX2  : (skipped — compiled without -mavx2)\n");
#endif

    /* ── correctness check: scalar vs x4 dna counts must match ─── */
#ifdef __AVX2__
    {
        DodecaTable d1; dodeca_init(&d1);
        DiamondFlowCtx c1; diamond_flow_init(&c1);
        uint32_t dna_s = geo_fused_write_batch(raw, 1024, baseline,
                                                &c1, &d1, 0, NULL, 0);

        DodecaTable d2; dodeca_init(&d2);
        DiamondFlowCtx4 c4; diamond_flow4_init(&c4);
        DiamondFlowCtx  c2; diamond_flow_init(&c2);
        uint32_t dna_x = geo_fused_write_batch_x4(raw, 1024, baseline,
                                                    &c4, &c2, &d2, 0);

        printf("\ncorrectness (n=1024): scalar dna=%u  x4 dna=%u  %s\n",
               dna_s, dna_x,
               (dna_s == dna_x) ? "MATCH OK" : "MISMATCH !");

        /* compare route_addr at end of bulk */
        uint64_t r_s = c1.route_addr;
        /* x4 ctx4 lane 0 route_addr after full run */
        uint64_t r_x = c4.route_addr[0];
        printf("route_addr lane0: scalar=0x%016llX  x4=0x%016llX  %s\n",
               (unsigned long long)r_s,
               (unsigned long long)r_x,
               (r_s == r_x) ? "MATCH OK" : "DIVERGE (expected — 4 lanes run independently)");
    }
#endif

    free(raw);
    return 0;
}
