#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include "pogls_sdk.h"

static uint64_t mk(uint32_t n){ return (uint64_t)n*0x9e3779b97f4a7c15ULL|1ULL; }
static void noop_gpu(uint64_t k,uint64_t v,void*c){(void)k;(void)v;(void)c;}
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec*1e-6;}

typedef struct{const char*name;uint64_t writes,reads;double wms,rms;
    uint64_t l1h,l1m,kvh,kvm;uint32_t drop,gpu;}Bench;

static void print_bench(const Bench*r){
    uint64_t l1t=r->l1h+r->l1m,kvt=r->kvh+r->kvm;
    printf("[%s]\n  write %7.2f Mops/s | read %7.2f Mops/s\n"
           "  L1 hit %5.1f%%  KV hit %5.1f%%  ring-drop %4.1f%%  GPU-staged=%u\n",
           r->name,r->writes/r->wms/1000.0,r->reads/r->rms/1000.0,
           l1t?100.0*r->l1h/l1t:0,kvt?100.0*r->kvh/kvt:0,
           r->writes?100.0*r->drop/r->writes:0,r->gpu);
}

static Bench run(const char*name,uint32_t uk,uint32_t cy,uint32_t rev,uint32_t flushev){
    Bench r;memset(&r,0,sizeof(r));r.name=name;
    PoglsCtx*ctx=pogls_open();
    uint32_t fa=0;
    double t0=now_ms();
    for(uint32_t c=0;c<cy;c++){
        for(uint32_t k=0;k<uk;k++){
            pogls_write(ctx,mk(k+1),(uint64_t)c*uk+k);
            if(++fa>=flushev){r.gpu+=pogls_flush_gpu(ctx,noop_gpu,NULL);fa=0;}
        }
        if(rev&&c%rev==rev-1)pogls_rewind(ctx);
    }
    r.gpu+=pogls_flush_gpu(ctx,noop_gpu,NULL);
    r.wms=now_ms()-t0; r.writes=(uint64_t)uk*cy; r.drop=ctx->kv.dropped;

    pogls_rewind(ctx);
    uint64_t bl1h=ctx->l1.hits,bl1m=ctx->l1.misses,bg=ctx->kv.cpu.gets,bm=ctx->kv.cpu.misses;
    uint32_t rn=uk<65536?uk:65536;
    double t1=now_ms();
    for(uint32_t k=0;k<rn;k++)pogls_read(ctx,mk(k+1));
    r.rms=now_ms()-t1; if(r.rms<0.001)r.rms=0.001; r.reads=rn;
    r.l1h=ctx->l1.hits-bl1h; r.l1m=ctx->l1.misses-bl1m;
    uint64_t ng=ctx->kv.cpu.gets-bg,nm=ctx->kv.cpu.misses-bm;
    r.kvh=ng-nm; r.kvm=nm;
    pogls_close(ctx);
    return r;
}

int main(void){
    printf("=== TPOGLS S20 Benchmark ===\n"
           "CPU truth / GPU shadow / flush every 512\n\n");
    Bench a=run("A: HOT  256 keys × 4000cy",  256, 4000, 8,  512);
    Bench b=run("B: WARM 2048 keys × 512cy", 2048,  512, 16, 512);
    Bench c=run("C: COLD 4096 keys × 256cy", 4096,  256, 4,  512);
    print_bench(&a); printf("\n");
    print_bench(&b); printf("\n");
    print_bench(&c); printf("\n");
    printf("Notes:\n"
           "  L1 hit%%   = GeoCache (direct-map, fastest)\n"
           "  KV hit%%   = CPU hash table (truth, after L1 miss)\n"
           "  ring-drop%% = GPU missed (CPU truth intact)\n");
    return 0;
}
