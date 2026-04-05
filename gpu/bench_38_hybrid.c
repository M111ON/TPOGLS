/*
 * bench_38_hybrid.c — 2-Phase Hybrid Engine Benchmark
 * วัด: throughput + imbalance + overhead breakdown
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "pogls_38_steal.h"

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;
}

typedef struct { double mops; double imbal; double max_pct; } R;

static R run(const char *label,
             const uint64_t *addrs, const uint16_t *cells,
             int n, int use_hybrid)
{
    L38VoronoiMap vm; l38_voronoi_init(&vm);
    L38Hydra h; l38_hydra_init(&h);
    L38SleepWire sw; l38_sleep_wire_init(&sw);
    L38Scheduler s; l38_sched_init(&s);
    L38FieldPressure fp; memset(&fp,0,sizeof(fp));
    L38RouteHysteresis hr; memset(&hr,0,sizeof(hr));

    uint64_t t0 = now_ns();
    for (int i=0;i<n;i++) {
        L38HydraTask t2={.op=1,.cell_id=cells[i],.addr=addrs[i]};
        if (use_hybrid) {
            l38_sched_push(&s, &h, &vm, &sw, &t2);
        } else {
            l38_voronoi_schedule(&h, &vm, &sw, &fp, &hr, &t2);
        }
        if (i%200==0) { /* drain to avoid overflow */
            L38HydraTask o;
            for(int j=0;j<80;j++) l38_hs_pop(&h.queues[j%L38_HS_HEADS],&o);
        }
    }
    uint64_t t1 = now_ns();

    uint32_t tot=0,mx=0,mn=UINT32_MAX;
    for(int i=0;i<L38_HS_HEADS;i++){
        uint32_t d=l38_hs_depth(&h.queues[i]);
        tot+=d;if(d>mx)mx=d;if(d<mn)mn=d;
    }
    R r;
    r.mops    = (double)n / ((double)(t1-t0)/1e9) / 1e6;
    r.imbal   = mn>0?(double)mx/mn:99.0;
    r.max_pct = tot>0?(double)mx/tot*100:0;

    if (use_hybrid)
        printf("    ticks=%u slow_runs=%llu pressure_reroutes=%llu greedy=%llu\n",
               s.tick, (unsigned long long)s.slow_runs,
               (unsigned long long)s.pressure_reroutes,
               (unsigned long long)s.greedy_calls);
    return r;
}

int main(void) {
    srand(42);
    const int N = 200000;
    uint64_t *addrs = malloc(N*8);
    uint16_t *cells = malloc(N*2);

    printf("══════════════════════════════════════════════════════════\n");
    printf("  Hybrid 2-Phase Engine  vs  Old voronoi_schedule\n");
    printf("  N=%d per pattern\n", N);
    printf("══════════════════════════════════════════════════════════\n\n");

    const char *pnames[] = {"Sequential","Bursty","Chaos","Mixed"};
    for (int pat=0; pat<4; pat++) {
        for (int i=0;i<N;i++) {
            switch(pat) {
            case 0: /* sequential */
                addrs[i]=(uint64_t)i*17;
                cells[i]=(uint16_t)(i%L38_BB_NODES); break;
            case 1: /* bursty: all in head-0 region */
                cells[i]=(uint16_t)(i%18);
                addrs[i]=(uint64_t)cells[i]*1000; break;
            case 2: /* chaos */
                addrs[i]=(uint64_t)rand()*rand();
                cells[i]=(uint16_t)(rand()%L38_BB_NODES); break;
            case 3: /* mixed */
                if(i%10<5){addrs[i]=(uint64_t)i*17;cells[i]=(uint16_t)(i%L38_BB_NODES);}
                else if(i%10<8){cells[i]=(uint16_t)(i%25);addrs[i]=(uint64_t)cells[i]*500;}
                else{addrs[i]=(uint64_t)rand()*rand();cells[i]=(uint16_t)(rand()%L38_BB_NODES);}
                break;
            }
        }
        printf("  [%s]\n", pnames[pat]);
        R hyb = run("hybrid", addrs, cells, N, 1);
        R old = run("old",    addrs, cells, N, 0);

        printf("  Hybrid:  %6.1f M/s  imbal=%.1fx  max=%.1f%%\n",
               hyb.mops, hyb.imbal, hyb.max_pct);
        printf("  Old:     %6.1f M/s  imbal=%.1fx  max=%.1f%%\n",
               old.mops, old.imbal, old.max_pct);
        printf("  speedup: %.1fx  balance: %s\n\n",
               hyb.mops/old.mops,
               hyb.imbal < old.imbal+0.5 ? "SAME ✓" : "WORSE");
    }

    /* Overhead breakdown */
    printf("══════════════════════════════════════════════════════════\n");
    printf("  Overhead breakdown (N=1M, sequential)\n");
    printf("══════════════════════════════════════════════════════════\n");
    const int BIG = 1000000;
    uint64_t *ba = malloc(BIG*8); uint16_t *bc = malloc(BIG*2);
    for(int i=0;i<BIG;i++){ba[i]=(uint64_t)i*17;bc[i]=(uint16_t)(i%L38_BB_NODES);}

    L38VoronoiMap vm; l38_voronoi_init(&vm);
    L38Hydra h; l38_hydra_init(&h);
    L38SleepWire sw; l38_sleep_wire_init(&sw);
    L38Scheduler s; l38_sched_init(&s);

    uint64_t h0=now_ns();
    for(int i=0;i<BIG;i++){
        L38HydraTask t={.op=1,.cell_id=bc[i],.addr=ba[i]};
        l38_sched_push(&s,&h,&vm,&sw,&t);
        if(i%200==0){L38HydraTask o;for(int j=0;j<80;j++)l38_hs_pop(&h.queues[j%16],&o);}
    }
    uint64_t h1=now_ns();
    printf("  Total:       %.1f ns/op  (%.1f M/s)\n",
           (double)(h1-h0)/BIG, (double)BIG/((double)(h1-h0)/1e9)/1e6);
    printf("  slow_runs:   %llu  (every %u calls = %.2f%% overhead)\n",
           (unsigned long long)s.slow_runs, L38_CTRL_BATCH,
           100.0*s.slow_runs/BIG);
    printf("  pressure_reroutes: %llu  (%.2f%% of calls)\n",
           (unsigned long long)s.pressure_reroutes,
           100.0*s.pressure_reroutes/BIG);
    printf("  greedy_calls: %llu\n", (unsigned long long)s.greedy_calls);

    free(ba); free(bc); free(addrs); free(cells);
    return 0;
}
