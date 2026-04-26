/*
 * bench_38_steal.c — POGLS38 Voronoi Steal Benchmark
 * วัด:
 *   1. Throughput: tasks/sec (schedule + drain)
 *   2. Distribution quality: max-head % after N tasks
 *   3. Steal latency: ns per steal operation
 *   4. Energy model overhead vs idle_ticks baseline
 *   5. Pressure field update cost
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "pogls_38_steal.h"

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* drain callback — counts executed tasks */
static uint64_t _exec_count = 0;
static void count_cb(const L38HydraTask *t, void *ud)
{ (void)t; (void)ud; _exec_count++; }

/* ── helper: print bar ──────────────────────────────────────────── */
static void print_bar(double pct, int width) {
    int filled = (int)(pct / 100.0 * width);
    printf("[");
    for (int i=0;i<width;i++) printf(i<filled?"█":"░");
    printf("] %.1f%%", pct);
}

int main(void) {
    printf("══════════════════════════════════════════════════════════\n");
    printf("  POGLS38 Voronoi Steal Benchmark\n");
    printf("══════════════════════════════════════════════════════════\n\n");

    L38VoronoiMap vm;   l38_voronoi_init(&vm);
    L38Hydra      h;    l38_hydra_init(&h);
    L38SleepWire  sw;   l38_sleep_wire_init(&sw);
    L38FieldPressure fp; memset(&fp,0,sizeof(fp));
    L38RouteHysteresis hr; memset(&hr,0,sizeof(hr));

    /* ── BENCH 1: Schedule throughput ─────────────────────────── */
    printf("BENCH 1 — Schedule throughput (Voronoi route)\n");
    const int N = 100000;
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++) {
        L38HydraTask t = {
            .op      = L38_HS_OP_WRITE,
            .cell_id = (uint16_t)(i % L38_BB_NODES),
            .addr    = (uint64_t)i * 7919u
        };
        l38_voronoi_schedule(&h, &vm, &sw, &fp, &hr, &t);
    }
    uint64_t t1 = now_ns();
    double elapsed_s = (double)(t1 - t0) / 1e9;
    double tput = N / elapsed_s / 1e6;
    printf("  %d tasks in %.3f ms → %.2f M tasks/sec\n",
           N, elapsed_s*1000, tput);
    printf("  ns/task: %.1f\n\n", (double)(t1-t0)/N);

    /* ── BENCH 2: Distribution quality ────────────────────────── */
    printf("BENCH 2 — Distribution (per-head depth after %d tasks)\n", N);
    uint32_t depths[L38_HS_HEADS];
    uint32_t total_d = 0, max_d = 0, min_d = UINT32_MAX;
    for (int i=0;i<L38_HS_HEADS;i++) {
        depths[i] = l38_hs_depth(&h.queues[i]);
        total_d  += depths[i];
        if (depths[i]>max_d) max_d=depths[i];
        if (depths[i]<min_d) min_d=depths[i];
    }
    printf("  total=%u  min=%u  max=%u  imbalance=%.1fx\n",
           total_d, min_d, max_d,
           min_d>0?(double)max_d/min_d:99.9);
    for (int i=0;i<L38_HS_HEADS;i++) {
        double pct = total_d>0?(double)depths[i]/total_d*100:0;
        printf("  head%2d: %5u tasks  ",i,depths[i]);
        print_bar(pct,20);
        printf("\n");
    }
    printf("\n");

    /* ── BENCH 3: Drain throughput ─────────────────────────────── */
    printf("BENCH 3 — Drain throughput (all heads)\n");
    _exec_count = 0;
    uint64_t d0 = now_ns();
    l38_hydra_drain_all(&h, count_cb, NULL);
    uint64_t d1 = now_ns();
    double drain_s = (double)(d1-d0)/1e9;
    printf("  %llu tasks drained in %.3f ms → %.2f M/sec\n\n",
           (unsigned long long)_exec_count, drain_s*1000,
           (double)_exec_count/drain_s/1e6);

    /* ── BENCH 4: Voronoi steal latency ───────────────────────── */
    printf("BENCH 4 — Voronoi steal latency\n");
    /* refill head 8 with 1000 tasks */
    for (int i=0;i<1000;i++) {
        L38HydraTask t = {.op=1,.cell_id=8*4,.addr=(uint64_t)i};
        l38_hs_push(&h.queues[8], &t);
    }
    int steals = 0;
    uint64_t s0 = now_ns();
    for (int i=0;i<1000;i++) {
        L38HydraTask out;
        if (l38_voronoi_steal(&h, &vm, 0, &out)) steals++;
    }
    uint64_t s1 = now_ns();
    printf("  %d steals in %.3f ms → %.1f ns/steal\n\n",
           steals, (double)(s1-s0)/1e6, steals>0?(double)(s1-s0)/steals:0);

    /* ── BENCH 5: Delaunay check cost ─────────────────────────── */
    printf("BENCH 5 — Delaunay boundary check cost\n");
    volatile int bnd_count = 0;
    uint64_t b0 = now_ns();
    for (int rep=0;rep<1000;rep++)
        for (uint16_t c=0;c<L38_BB_NODES;c++) {
            L38DelaunayCell dc = l38_delaunay_check(&vm, c);
            if (dc.near_boundary) bnd_count++;
        }
    uint64_t b1 = now_ns();
    double checks = 1000.0 * L38_BB_NODES;
    printf("  %.0f checks in %.1f ms → %.1f ns/check\n",
           checks, (double)(b1-b0)/1e6, (double)(b1-b0)/checks);
    printf("  boundary cells found: %d / %d (%.1f%%)\n\n",
           bnd_count/1000, L38_BB_NODES,
           (double)bnd_count/1000/L38_BB_NODES*100);

    /* ── BENCH 6: Pressure field update cost ─────────────────── */
    printf("BENCH 6 — Pressure field update cost\n");
    /* refill queues */
    for (int i=0;i<L38_HS_HEADS;i++)
        for (int j=0;j<50;j++) {
            L38HydraTask t={.op=1,.addr=(uint64_t)(i*50+j)};
            l38_hs_push(&h.queues[i],&t);
        }
    uint64_t p0 = now_ns();
    for (int i=0;i<100000;i++)
        l38_pressure_update(&fp, &h, &vm);
    uint64_t p1 = now_ns();
    printf("  100K updates in %.3f ms → %.1f ns/update\n\n",
           (double)(p1-p0)/1e6, (double)(p1-p0)/100000.0);

    /* ── BENCH 7: Energy tick cost ────────────────────────────── */
    printf("BENCH 7 — Energy tick cost (vs old idle_ticks)\n");
    uint64_t e0 = now_ns();
    for (int i=0;i<1000000;i++)
        l38_sleep_tick(&sw, &h, i % L38_HS_HEADS);
    uint64_t e1 = now_ns();
    printf("  1M ticks in %.3f ms → %.1f ns/tick\n\n",
           (double)(e1-e0)/1e6, (double)(e1-e0)/1000000.0);

    /* ── BENCH 8: Full pipeline (schedule→steal→drain) ───────── */
    printf("BENCH 8 — Full pipeline cycle (10K tasks, mixed ops)\n");
    L38Hydra h2; l38_hydra_init(&h2);
    L38SleepWire sw2; l38_sleep_wire_init(&sw2);
    L38FieldPressure fp2; memset(&fp2,0,sizeof(fp2));
    L38RouteHysteresis hr2; memset(&hr2,0,sizeof(hr2));
    _exec_count = 0;

    uint64_t f0 = now_ns();
    for (int i=0;i<10000;i++) {
        /* schedule */
        L38HydraTask t = {
            .op      = (uint16_t)(1 + i%5),
            .cell_id = (uint16_t)(i % L38_BB_NODES),
            .addr    = (uint64_t)i * 997u
        };
        l38_voronoi_schedule(&h2, &vm, &sw2, &fp2, &hr2, &t);
        /* steal attempt from neighbor */
        if (i%4==0) {
            L38HydraTask stolen;
            l38_steal_pipeline(&h2, &vm, &sw2, &fp2,
                               (uint32_t)(i%L38_HS_HEADS), &stolen);
        }
        /* pressure update every 18 ops (gate_18) */
        if (i%18==0) {
            l38_pressure_update(&fp2, &h2, &vm);
            l38_hydra_gate18(&h2);
        }
    }
    /* final drain */
    l38_hydra_drain_all(&h2, count_cb, NULL);
    uint64_t f1 = now_ns();

    double pipe_s = (double)(f1-f0)/1e9;
    printf("  10K ops + steal + pressure in %.3f ms\n", pipe_s*1000);
    printf("  throughput: %.2f M ops/sec\n", 10000.0/pipe_s/1e6);

    /* final distribution */
    uint32_t total2=0, max2=0;
    for (int i=0;i<L38_HS_HEADS;i++) {
        uint32_t d=l38_hs_depth(&h2.queues[i]);
        total2+=d; if(d>max2)max2=d;
    }
    printf("  remaining=%u  max_head=%u (%.1f%%)\n\n",
           total2, max2, total2>0?(double)max2/total2*100:0);

    printf("══════════════════════════════════════════════════════════\n");
    printf("  Hardware: CPU-only (no GPU)\n");
    printf("══════════════════════════════════════════════════════════\n");
    return 0;
}
