/*
 * test_gpu_wire.c — CPU-only test for pogls38_gpu_wire.h
 *
 * Simulates GPU kernel output (SoA arrays) and feeds through wire.
 * No CUDA required — runs on G4400 or any C11 compiler.
 *
 * Compile:
 *   gcc -O2 -std=c11 test_gpu_wire.c -o t_gpuwire && ./t_gpuwire
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* ── Minimal stubs so the file compiles standalone ───────────────── */

#define PHI_UP    1696631u
#define PHI_DOWN  648055u
#define PHI_SCALE (1u<<20)
#define L17_LANES_TOTAL 289u
#define L17_GATE        18u
#define L38_BB_NODES    289u

/* Bitboard stub */
typedef struct { uint64_t w[5]; } L38Bitboard;
static inline void l38_bb_set(L38Bitboard *b, uint16_t n)
{ if(n<289) b->w[n>>6] |= 1ULL<<(n&63); }
static inline void l38_bb_clear_node(L38Bitboard *b, uint16_t n)
{ if(n<289) b->w[n>>6] &= ~(1ULL<<(n&63)); }
static inline int  l38_bb_test(const L38Bitboard *b, uint16_t n)
{ return n<289 && ((b->w[n>>6]>>(n&63))&1); }
static inline void l38_bb_or(L38Bitboard *a, const L38Bitboard *b)
{ for(int i=0;i<5;i++) a->w[i]|=b->w[i]; }
static inline uint16_t l38_bb_next(const L38Bitboard *b, uint16_t from)
{ for(uint16_t n=from;n<289;n++) if(l38_bb_test(b,n)) return n; return 289; }

/* BBGraph stub */
typedef struct { L38Bitboard adj[289]; } L38BBGraph;

/* L17Cell stub */
typedef struct { uint32_t cell_id; uint8_t world; } L17Cell;
typedef struct { uint32_t gate_count; } L17Lattice;

static inline L17Cell l17_cell_from_addr(uint64_t a, uint32_t g)
{
    (void)g;
    L17Cell c;
    c.cell_id = (uint32_t)((a * PHI_UP) % L17_LANES_TOTAL);
    c.world   = (uint8_t)((a >> 20) & 1);
    return c;
}
static inline void l17_write(L17Lattice *l, uint64_t a, uint64_t v)
{ (void)l; (void)a; (void)v; }

static inline int l38_in_circle(uint32_t a)
{
    uint64_t s2 = (uint64_t)PHI_SCALE * PHI_SCALE;
    return (2*(uint64_t)a*(uint64_t)a) < s2;
}

static inline uint64_t _l38_now_ns(void) { return 0; }

/* EjectFrame stub */
typedef struct {
    uint32_t frame_id;
    struct { uint8_t buf[64]; uint32_t count; } log;
} L38EjectFrame;
#define ENT_MOVE_HIBERNATE 9
#define ENT_MOVE_WAKE      10
static inline void l38_frame_init(L38EjectFrame *f, uint32_t id,
    uint32_t cid, uint64_t a, uint8_t w)
{ (void)cid;(void)a;(void)w; memset(f,0,sizeof(*f)); f->frame_id=id; }
static inline void l38_log_push(void *l, int m, uint8_t w, int r, int s, uint32_t c)
{ (void)l;(void)m;(void)w;(void)r;(void)s;(void)c; }

/* Repair/Tails stubs */
typedef struct { int dummy; } L38RepairCtx;
typedef struct { int dummy; } L38Tails;
typedef struct { uint8_t repaired; } L38TailsSummonResult;
static inline L38TailsSummonResult l38_tails_summon(
    L38Tails *t, L38RepairCtx *r, L17Lattice *l,
    uint32_t cid, uint64_t a, uint8_t w)
{ (void)t;(void)r;(void)l;(void)cid;(void)a;(void)w;
  L38TailsSummonResult s={1}; return s; }

/* ExecWindow stub */
#define L38_EW_OP_WRITE  1
#define L38_EW_OP_REPAIR 2
typedef struct { int dummy; } L38ExecWindow;
static inline void l38_ew_push(L38ExecWindow *e, int op, uint16_t cid,
    uint32_t fid, uint64_t a, uint64_t v)
{ (void)e;(void)op;(void)cid;(void)fid;(void)a;(void)v; }

/* QuadFibo stub */
typedef struct { uint32_t axis_x,axis_nx,axis_y,axis_ny; uint8_t ok,world; uint16_t _pad; } L38QuadFibo;
static inline L38QuadFibo l38_quad_fibo(uint32_t n, uint8_t w)
{
    L38QuadFibo q;
    q.axis_x  = (uint32_t)(((uint64_t)n * PHI_UP)   % PHI_SCALE);
    q.axis_nx = PHI_SCALE - q.axis_x;
    q.axis_y  = (uint32_t)(((uint64_t)n * PHI_DOWN)  % PHI_SCALE);
    q.axis_ny = PHI_SCALE - q.axis_y;
    q.world   = w;
    q.ok      = ((q.axis_x + q.axis_nx == PHI_SCALE) &&
                 (q.axis_y + q.axis_ny == PHI_SCALE)) ? 1u : 0u;
    return q;
}

/* Pressure stub */
#define L38_PRESSURE_HIGH 200
#define L38_PRESSURE_MAX  240
#define L38_PRESSURE_LOW   64
typedef enum { L38_PRESSURE_OK=0, L38_PRESSURE_SLOW=1, L38_PRESSURE_BLOCK=2 } l38_pressure_t;
typedef struct {
    _Atomic uint32_t queue_depth;
    L38Bitboard repair_mask;
    uint32_t steal_count, block_count;
} L38PressureBridge;
static inline void l38_pressure_init(L38PressureBridge *p)
{ memset(p,0,sizeof(*p)); }
static inline l38_pressure_t l38_pressure_check(const L38PressureBridge *p)
{
    uint32_t d = atomic_load_explicit(&p->queue_depth, memory_order_relaxed);
    if (d >= L38_PRESSURE_MAX) return L38_PRESSURE_BLOCK;
    if (d >= L38_PRESSURE_HIGH) return L38_PRESSURE_SLOW;
    return L38_PRESSURE_OK;
}
static inline void l38_pressure_mask(L38PressureBridge *p, uint16_t n)
{ l38_bb_set(&p->repair_mask, n); }
static inline void l38_pressure_unmask(L38PressureBridge *p, uint16_t n)
{ l38_bb_clear_node(&p->repair_mask, n); }
static inline int  l38_pressure_is_masked(const L38PressureBridge *p, uint16_t n)
{ return l38_bb_test(&p->repair_mask, n); }
static inline void l38_pressure_enqueue(L38PressureBridge *p)
{ atomic_fetch_add_explicit(&p->queue_depth, 1, memory_order_relaxed); }
static inline void l38_pressure_dequeue(L38PressureBridge *p)
{
    uint32_t d = atomic_load_explicit(&p->queue_depth, memory_order_relaxed);
    if (d > 0) atomic_fetch_sub_explicit(&p->queue_depth, 1, memory_order_relaxed);
}
static inline uint8_t l38_h3_route(uint32_t cid, uint8_t w)
{
    uint64_t base = w ? PHI_DOWN : PHI_UP;
    uint32_t a = (uint32_t)(((uint64_t)cid * base) % PHI_SCALE);
    if (!l38_in_circle(a)) return 0;
    return (uint8_t)(a % 16u);
}

/* Temporal stub */
#define L38_TEMPORAL_RING_SIZE 256
#define L38_TEMPORAL_HASH_SIZE 1024
#define L38_TEMPORAL_MAGIC 0x54454D50u
typedef struct { uint64_t angular_addr,value; uint32_t cell_id,seq; uint64_t ts; } L38TemporalEntry;
typedef struct {
    uint32_t magic;
    L38TemporalEntry ring[256];
    uint32_t hash_index[1024];
    _Atomic uint32_t head, tail;
    uint64_t total_written, total_wrapped;
    uint32_t slot_base, slot_next, slot_next2;
} L38TemporalBridge;
static inline void l38_temporal_init(L38TemporalBridge *t)
{ memset(t,0,sizeof(*t)); t->magic=L38_TEMPORAL_MAGIC;
  for(int i=0;i<1024;i++) t->hash_index[i]=~0u; }
static inline void l38_temporal_write(L38TemporalBridge *tb, uint64_t a, uint64_t v, uint32_t c)
{
    uint32_t slot = atomic_fetch_add_explicit(&tb->head,1,memory_order_relaxed)%256;
    tb->ring[slot].angular_addr=a; tb->ring[slot].value=v; tb->ring[slot].cell_id=c;
    tb->ring[slot].seq=(uint32_t)tb->total_written++;
    uint32_t hk=(uint32_t)((a*PHI_UP)%1024); tb->hash_index[hk]=slot;
    tb->slot_base = c%289; tb->slot_next=(tb->slot_base+18)%289; tb->slot_next2=(tb->slot_base+36)%289;
}

/* WarpDetach stub */
typedef struct {
    L38EjectFrame frame; uint8_t warp_open,detach_active,world,_pad;
    uint32_t cell_id; uint64_t angular_addr, detach_at_ns;
} L38WarpDetach;

/* NtacleQueue stub */
typedef struct { uint32_t cell_id,op,frame_id,_pad; uint64_t addr; } L38NtacleTask;
#define L38_NTACLE_QUEUE_SIZE 512
#define L38_NTACLE_OP_WRITE 1
typedef struct { _Atomic uint32_t head,tail; L38NtacleTask tasks[512]; } L38NtacleQueue;
static inline int l38_ntacle_push(L38NtacleQueue *q, L38NtacleTask t)
{
    uint32_t tail=atomic_load_explicit(&q->tail,memory_order_relaxed);
    uint32_t next=(tail+1)%512;
    if(next==atomic_load_explicit(&q->head,memory_order_acquire)) return -1;
    q->tasks[tail]=t; atomic_store_explicit(&q->tail,next,memory_order_release); return 0;
}
static inline uint32_t l38_ntacle_write_path(L38NtacleQueue *q, uint64_t a, uint32_t fid)
{
    L17Cell c = l17_cell_from_addr(a, 0);
    L38NtacleTask t={c.cell_id,L38_NTACLE_OP_WRITE,fid,0,a};
    l38_ntacle_push(q, t); return c.cell_id;
}

/* ── WriteResult / WirePipeline / wire_write ────────────────────── */
typedef struct {
    uint32_t cell_id; uint8_t head_id,quad_ok,pressure_level,anomaly,warp_open,repair_ok;
    uint8_t _pad[2];
} L38WriteResult;

typedef struct {
    L17Lattice *lattice; L38NtacleQueue *ntacle; L38BBGraph *bbgraph;
    L38PressureBridge *pressure; L38TemporalBridge *temporal;
    L38ExecWindow *ew; L38Tails *tails; L38RepairCtx *repair;
    uint32_t next_frame_id;
} L38WirePipeline;

static inline L38WriteResult l38_wire_write(L38WirePipeline *p,
                                              uint64_t angular_addr,
                                              uint64_t value)
{
    L38WriteResult r = {0};
    uint32_t n = (uint32_t)(angular_addr & (PHI_SCALE - 1u));
    L38QuadFibo qf = l38_quad_fibo(n, 0);
    r.quad_ok = qf.ok;
    l38_pressure_t pr = l38_pressure_check(p->pressure);
    r.pressure_level = (uint8_t)pr;
    if (pr == L38_PRESSURE_BLOCK) return r;
    L17Cell cell = l17_cell_from_addr(angular_addr, p->lattice->gate_count);
    r.cell_id = cell.cell_id;
    r.head_id = l38_h3_route(cell.cell_id, cell.world);
    uint32_t addr20 = (uint32_t)(angular_addr & (PHI_SCALE - 1u));
    r.anomaly = !l38_in_circle(addr20);
    if (r.anomaly && p->tails && !l38_pressure_is_masked(p->pressure,(uint16_t)cell.cell_id)) {
        l38_pressure_mask(p->pressure, (uint16_t)cell.cell_id);
        l38_pressure_enqueue(p->pressure);
        L38TailsSummonResult sr = l38_tails_summon(p->tails,p->repair,p->lattice,
                                                    cell.cell_id,angular_addr,(uint8_t)cell.world);
        r.repair_ok = sr.repaired;
        r.warp_open = !sr.repaired ? 1u : 0u;
        l38_pressure_dequeue(p->pressure);
        if (sr.repaired) l38_pressure_unmask(p->pressure, (uint16_t)cell.cell_id);
    }
    if (p->ntacle) { uint32_t fid=r.anomaly?p->next_frame_id++:0; l38_ntacle_write_path(p->ntacle,angular_addr,fid); }
    if (p->temporal) l38_temporal_write(p->temporal, angular_addr, value, cell.cell_id);
    l17_write(p->lattice, angular_addr, value);
    if (p->ew) l38_ew_push(p->ew,r.anomaly?L38_EW_OP_REPAIR:L38_EW_OP_WRITE,(uint16_t)cell.cell_id,0,angular_addr,value);
    return r;
}

/* ── NOW include the wire header under test ──────────────────────── */
#include "pogls38_gpu_wire.h"

/* ══════════════════════════════════════════════════════════════════
 * HELPERS
 * ══════════════════════════════════════════════════════════════════ */

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if(cond){ printf("  ✓ %s\n",msg); g_pass++; } \
    else    { printf("  ✗ FAIL: %s\n",msg); g_fail++; } \
} while(0)

static void make_pipeline(L38WirePipeline *p,
                           L17Lattice *lat, L38NtacleQueue *ntq,
                           L38PressureBridge *pb, L38TemporalBridge *tb,
                           L38Tails *tails, L38RepairCtx *repair)
{
    memset(p, 0, sizeof(*p));
    p->lattice   = lat;
    p->ntacle    = ntq;
    p->pressure  = pb;
    p->temporal  = tb;
    p->tails     = tails;
    p->repair    = repair;
}

/* ══════════════════════════════════════════════════════════════════
 * TESTS
 * ══════════════════════════════════════════════════════════════════ */

/* T01: all-pass batch (audit=0 for all) */
static void t01_all_pass(void)
{
    printf("\n-- T01: All-pass batch (N=289, audit=0) --\n");

    L17Lattice lat = {18};
    L38NtacleQueue ntq; memset(&ntq,0,sizeof(ntq));
    L38PressureBridge pb; l38_pressure_init(&pb);
    L38TemporalBridge tb; l38_temporal_init(&tb);
    L38Tails tails={0}; L38RepairCtx repair={0};
    L38WirePipeline pipe;
    make_pipeline(&pipe, &lat, &ntq, &pb, &tb, &tails, &repair);

    const uint32_t N = 289;
    uint32_t h_hil[289]; uint8_t h_lane[289], h_aud[289];
    for(uint32_t i=0;i<N;i++){
        uint64_t a = L38_GPU_BASE + (uint64_t)L38_GPU_SV * i;
        uint32_t t = (uint32_t)(((a & (PHI_SCALE-1)) * (uint64_t)PHI_UP) >> 20);
        h_hil[i]  = t;
        h_lane[i] = (uint8_t)(t % 54u);
        h_aud[i]  = 0; /* force all pass */
    }

    L38GpuFeedStats stats = {0};
    uint32_t ok = l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, N, &stats, NULL);

    CHECK(ok == N,           "all 289 cells passed");
    CHECK(stats.fed_pass == N,   "stats.fed_pass == 289");
    CHECK(stats.fed_ghost == 0,  "stats.fed_ghost == 0");
    CHECK(stats.fed_blocked == 0,"stats.fed_blocked == 0");
    CHECK(stats.batches == 1,    "batches == 1");
}

/* T02: all-ghost batch (audit=1 for all) */
static void t02_all_ghost(void)
{
    printf("\n-- T02: All-ghost batch (audit=1) --\n");

    L17Lattice lat={18}; L38NtacleQueue ntq; memset(&ntq,0,sizeof(ntq));
    L38PressureBridge pb; l38_pressure_init(&pb);
    L38TemporalBridge tb; l38_temporal_init(&tb);
    L38Tails tails={0}; L38RepairCtx repair={0};
    L38WirePipeline pipe;
    make_pipeline(&pipe, &lat, &ntq, &pb, &tb, &tails, &repair);

    const uint32_t N = 100;
    uint32_t h_hil[100]={0}; uint8_t h_lane[100]={0}, h_aud[100];
    for(uint32_t i=0;i<N;i++) h_aud[i]=1; /* all fail */

    L38GpuFeedStats stats = {0};
    uint32_t ok = l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, N, &stats, NULL);

    CHECK(ok == 0,           "0 cells passed (all ghosted)");
    CHECK(stats.fed_ghost == N,  "stats.fed_ghost == 100");
    CHECK(stats.fed_pass  == 0,  "stats.fed_pass == 0");
}

/* T03: mixed batch (half pass, half ghost) */
static void t03_mixed(void)
{
    printf("\n-- T03: Mixed batch (N=100, 50/50 pass/ghost) --\n");

    L17Lattice lat={18}; L38NtacleQueue ntq; memset(&ntq,0,sizeof(ntq));
    L38PressureBridge pb; l38_pressure_init(&pb);
    L38TemporalBridge tb; l38_temporal_init(&tb);
    L38Tails tails={0}; L38RepairCtx repair={0};
    L38WirePipeline pipe;
    make_pipeline(&pipe, &lat, &ntq, &pb, &tb, &tails, &repair);

    const uint32_t N = 100;
    uint32_t h_hil[100]; uint8_t h_lane[100], h_aud[100];
    for(uint32_t i=0;i<N;i++){
        uint64_t a = L38_GPU_BASE + (uint64_t)L38_GPU_SV * i;
        uint32_t t = (uint32_t)(((a & (PHI_SCALE-1)) * (uint64_t)PHI_UP) >> 20);
        h_hil[i]  = t;
        h_lane[i] = (uint8_t)(t % 54u);
        h_aud[i]  = (i % 2 == 0) ? 0u : 1u; /* even=pass, odd=ghost */
    }

    L38GpuFeedStats stats = {0};
    uint32_t ok = l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, N, &stats, NULL);

    CHECK(ok == 50,              "50 cells passed");
    CHECK(stats.fed_ghost == 50, "50 cells ghosted");
    CHECK(stats.cells_total == 100, "cells_total == 100");
}

/* T04: pressure block — fill queue past MAX */
static void t04_pressure_block(void)
{
    printf("\n-- T04: Pressure block (queue_depth >= MAX) --\n");

    L17Lattice lat={18}; L38NtacleQueue ntq; memset(&ntq,0,sizeof(ntq));
    L38PressureBridge pb; l38_pressure_init(&pb);
    L38TemporalBridge tb; l38_temporal_init(&tb);
    L38Tails tails={0}; L38RepairCtx repair={0};
    L38WirePipeline pipe;
    make_pipeline(&pipe, &lat, &ntq, &pb, &tb, &tails, &repair);

    /* force queue depth to MAX */
    atomic_store(&pb.queue_depth, L38_PRESSURE_MAX);

    const uint32_t N = 10;
    uint32_t h_hil[10]={1}; uint8_t h_lane[10]={0}, h_aud[10]={0};

    L38GpuFeedStats stats = {0};
    uint32_t ok = l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, N, &stats, NULL);

    CHECK(ok == 0,                "0 passed (all blocked)");
    CHECK(stats.fed_blocked == N, "all 10 cells blocked");
}

/* T05: cell results output */
static void t05_results_array(void)
{
    printf("\n-- T05: Per-cell results array --\n");

    L17Lattice lat={18}; L38NtacleQueue ntq; memset(&ntq,0,sizeof(ntq));
    L38PressureBridge pb; l38_pressure_init(&pb);
    L38TemporalBridge tb; l38_temporal_init(&tb);
    L38Tails tails={0}; L38RepairCtx repair={0};
    L38WirePipeline pipe;
    make_pipeline(&pipe, &lat, &ntq, &pb, &tb, &tails, &repair);

    const uint32_t N = 4;
    uint32_t h_hil[4] = {100,200,300,400};
    uint8_t  h_lane[4]= {1,2,3,4};
    uint8_t  h_aud[4] = {0,1,0,1}; /* pass, ghost, pass, ghost */

    L38GpuFeedStats stats = {0};
    L38GpuCellResult results[4];
    l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, N, &stats, results);

    CHECK(results[0].result == 1, "cell[0] result=pass");
    CHECK(results[1].result == 0, "cell[1] result=ghost");
    CHECK(results[2].result == 1, "cell[2] result=pass");
    CHECK(results[3].result == 0, "cell[3] result=ghost");
}

/* T06: temporal ring populated */
static void t06_temporal_ring(void)
{
    printf("\n-- T06: Temporal ring populated on pass --\n");

    L17Lattice lat={18}; L38NtacleQueue ntq; memset(&ntq,0,sizeof(ntq));
    L38PressureBridge pb; l38_pressure_init(&pb);
    L38TemporalBridge tb; l38_temporal_init(&tb);
    L38Tails tails={0}; L38RepairCtx repair={0};
    L38WirePipeline pipe;
    make_pipeline(&pipe, &lat, &ntq, &pb, &tb, &tails, &repair);

    const uint32_t N = 5;
    uint32_t h_hil[5]; uint8_t h_lane[5], h_aud[5];
    for(uint32_t i=0;i<N;i++){
        uint64_t a = L38_GPU_BASE + (uint64_t)L38_GPU_SV * i;
        uint32_t t = (uint32_t)(((a & (PHI_SCALE-1)) * (uint64_t)PHI_UP) >> 20);
        h_hil[i]=t; h_lane[i]=(uint8_t)(t%54); h_aud[i]=0;
    }

    L38GpuFeedStats stats = {0};
    l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, N, &stats, NULL);

    CHECK(tb.total_written == N, "temporal ring has 5 entries");
    CHECK(tb.total_written > 0,  "temporal.total_written > 0");
}

/* T07: stats accumulate across multiple batches */
static void t07_multi_batch(void)
{
    printf("\n-- T07: Stats accumulate across 3 batches --\n");

    L17Lattice lat={18}; L38NtacleQueue ntq; memset(&ntq,0,sizeof(ntq));
    L38PressureBridge pb; l38_pressure_init(&pb);
    L38TemporalBridge tb; l38_temporal_init(&tb);
    L38Tails tails={0}; L38RepairCtx repair={0};
    L38WirePipeline pipe;
    make_pipeline(&pipe, &lat, &ntq, &pb, &tb, &tails, &repair);

    uint32_t h_hil[10]; uint8_t h_lane[10], h_aud[10];
    for(int i=0;i<10;i++){
        h_hil[i]=(uint32_t)(i*100); h_lane[i]=(uint8_t)(i%54); h_aud[i]=0;
    }

    L38GpuFeedStats stats = {0};
    l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, 10, &stats, NULL);
    l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, 10, &stats, NULL);
    l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud, 10, &stats, NULL);

    CHECK(stats.batches == 3,         "batches == 3");
    CHECK(stats.cells_total == 30,    "cells_total == 30");
    CHECK(stats.fed_pass == 30,       "fed_pass == 30");
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("══════════════════════════════════════════════════\n");
    printf("  pogls38_gpu_wire.h — Test Suite\n");
    printf("══════════════════════════════════════════════════\n");

    t01_all_pass();
    t02_all_ghost();
    t03_mixed();
    t04_pressure_block();
    t05_results_array();
    t06_temporal_ring();
    t07_multi_batch();

    printf("\n══════════════════════════════════════════════════\n");
    if (g_fail == 0)
        printf("  %d / %d PASS  ✓ ALL PASS\n", g_pass, g_pass);
    else
        printf("  %d / %d PASS  ✗ %d FAILED\n", g_pass, g_pass+g_fail, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
