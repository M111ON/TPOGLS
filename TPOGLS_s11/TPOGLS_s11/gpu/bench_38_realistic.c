/*
 * bench_38_realistic.c — Realistic workload patterns
 * Pattern A: sequential (structured writes)
 * Pattern B: bursty (all same cell cluster)
 * Pattern C: chaos (random addrs)
 * Pattern D: mixed (50% seq + 30% burst + 20% chaos)
 * + compare: greedy steal vs Voronoi steal
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "pogls_38_steal.h"

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;
}

/* measure distribution: returns imbalance ratio (max/min) */
static double measure_imbalance(const L38Hydra *h) {
    uint32_t mx=0, mn=UINT32_MAX;
    for (int i=0;i<L38_HS_HEADS;i++) {
        uint32_t d=l38_hs_depth(&h->queues[i]);
        if(d>mx)mx=d; if(d<mn)mn=d;
    }
    return mn>0?(double)mx/mn:99.0;
}

static double measure_max_pct(const L38Hydra *h) {
    uint32_t tot=0,mx=0;
    for(int i=0;i<L38_HS_HEADS;i++){uint32_t d=l38_hs_depth(&h->queues[i]);tot+=d;if(d>mx)mx=d;}
    return tot>0?(double)mx/tot*100:0;
}

typedef struct { uint64_t ns; double imbalance; double max_pct; uint32_t total; } BenchResult;

/* Voronoi schedule bench */
static BenchResult run_voronoi(const uint64_t *addrs, const uint16_t *cells, int n) {
    L38VoronoiMap vm; l38_voronoi_init(&vm);
    L38Hydra h; l38_hydra_init(&h);
    L38SleepWire sw; l38_sleep_wire_init(&sw);
    L38FieldPressure fp; memset(&fp,0,sizeof(fp));
    L38RouteHysteresis hr; memset(&hr,0,sizeof(hr));

    uint64_t t0=now_ns();
    for(int i=0;i<n;i++){
        L38HydraTask t={.op=1,.cell_id=cells[i],.addr=addrs[i]};
        l38_voronoi_schedule(&h,&vm,&sw,&fp,&hr,&t);
        if(i%18==0) l38_pressure_update(&fp,&h,&vm);
    }
    uint64_t t1=now_ns();
    BenchResult r={t1-t0, measure_imbalance(&h), measure_max_pct(&h), 0};
    for(int i=0;i<L38_HS_HEADS;i++) r.total+=l38_hs_depth(&h.queues[i]);
    return r;
}

/* Greedy (old): route by addr & mask */
static BenchResult run_greedy(const uint64_t *addrs, const uint16_t *cells, int n) {
    L38Hydra h; l38_hydra_init(&h);
    uint64_t t0=now_ns();
    for(int i=0;i<n;i++){
        L38HydraTask t={.op=1,.cell_id=cells[i],.addr=addrs[i]};
        uint32_t hid=l38_hs_route_addr(addrs[i]);
        l38_hs_push(&h.queues[hid],&t);
    }
    uint64_t t1=now_ns();
    BenchResult r={t1-t0, measure_imbalance(&h), measure_max_pct(&h), 0};
    for(int i=0;i<L38_HS_HEADS;i++) r.total+=l38_hs_depth(&h.queues[i]);
    return r;
}

static void print_cmp(const char *name, BenchResult vor, BenchResult grd, int n) {
    double vor_mops = (double)n/(double)vor.ns*1000.0;
    double grd_mops = (double)n/(double)grd.ns*1000.0;
    printf("  %-10s | Voronoi: %5.2f M/s imbal=%.1fx max=%.1f%%"
           " | Greedy: %5.2f M/s imbal=%.1fx max=%.1f%%\n",
           name,
           vor_mops, vor.imbalance, vor.max_pct,
           grd_mops, grd.imbalance, grd.max_pct);
}

int main(void) {
    srand(42);
    const int N = 50000;
    uint64_t *addrs = malloc(N*sizeof(uint64_t));
    uint16_t *cells = malloc(N*sizeof(uint16_t));

    printf("══════════════════════════════════════════════════════════\n");
    printf("  POGLS38 Realistic Workload Benchmark  (N=%d)\n", N);
    printf("  Voronoi vs Greedy — imbalance + distribution\n");
    printf("══════════════════════════════════════════════════════════\n\n");

    /* Pattern A: sequential */
    for(int i=0;i<N;i++){addrs[i]=(uint64_t)i*17;cells[i]=(uint16_t)(i%L38_BB_NODES);}
    BenchResult va=run_voronoi(addrs,cells,N), ga=run_greedy(addrs,cells,N);

    /* Pattern B: bursty (all cells 0..17 = head 0 region) */
    for(int i=0;i<N;i++){cells[i]=(uint16_t)(i%18);addrs[i]=(uint64_t)cells[i]*1000;}
    BenchResult vb=run_voronoi(addrs,cells,N), gb=run_greedy(addrs,cells,N);

    /* Pattern C: chaos (random) */
    for(int i=0;i<N;i++){addrs[i]=(uint64_t)rand()*rand();cells[i]=(uint16_t)(rand()%L38_BB_NODES);}
    BenchResult vc=run_voronoi(addrs,cells,N), gc=run_greedy(addrs,cells,N);

    /* Pattern D: mixed */
    for(int i=0;i<N;i++){
        if(i%10<5){addrs[i]=(uint64_t)i*17;cells[i]=(uint16_t)(i%L38_BB_NODES);}      /* 50% seq */
        else if(i%10<8){cells[i]=(uint16_t)(i%25);addrs[i]=(uint64_t)cells[i]*500;}   /* 30% burst */
        else{addrs[i]=(uint64_t)rand()*rand();cells[i]=(uint16_t)(rand()%L38_BB_NODES);}/* 20% chaos */
    }
    BenchResult vd=run_voronoi(addrs,cells,N), gd=run_greedy(addrs,cells,N);

    printf("  Pattern    | Voronoi (new)                          | Greedy (old)\n");
    printf("  -----------+----------------------------------------+---------------------------\n");
    print_cmp("Sequential", va, ga, N);
    print_cmp("Bursty",     vb, gb, N);
    print_cmp("Chaos",      vc, gc, N);
    print_cmp("Mixed",      vd, gd, N);

    printf("\n  Imbalance ratio: 1.0x = perfect even  |  higher = clustering\n");
    printf("  Max%%: fraction of all tasks in busiest head\n\n");

    /* Steal comparison */
    printf("STEAL COMPARISON: Voronoi vs Greedy steal\n");
    printf("  (steal 1000 tasks from artificially loaded system)\n");

    L38VoronoiMap vm2; l38_voronoi_init(&vm2);
    L38Hydra hv, hg; l38_hydra_init(&hv); l38_hydra_init(&hg);
    /* load head 8 heavily */
    for(int i=0;i<800;i++){
        L38HydraTask t={.op=1,.cell_id=136,.addr=(uint64_t)i};
        l38_hs_push(&hv.queues[8],&t);
        l38_hs_push(&hg.queues[8],&t);
    }

    int sv=0,sg=0;
    uint64_t sv0=now_ns();
    for(int i=0;i<800;i++){L38HydraTask o; if(l38_voronoi_steal(&hv,&vm2,0,&o))sv++;}
    uint64_t sv1=now_ns();
    for(int i=0;i<800;i++){L38HydraTask o; if(l38_hs_steal(&hg.queues[0],0,&o))sg++;}
    uint64_t sg1=now_ns();

    printf("  Voronoi steal: %d/%d stolen, %.1f ns/steal\n",
           sv,800, sv>0?(double)(sv1-sv0)/sv:0);
    printf("  Greedy  steal: %d/%d stolen, %.1f ns/steal\n",
           sg,800, sg>0?(double)(sg1-sv1)/sg:0);

    free(addrs); free(cells);
    printf("\n══════════════════════════════════════════════════════════\n");
    return 0;
}
