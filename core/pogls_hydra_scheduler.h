/*
 * pogls_hydra_scheduler.h — POGLS V3.5 Hydra Work-Stealing Scheduler
 *
 * 16 heads, per-head queue, work-stealing fallback
 * cache-locality routing: addr → head (same region = same core = L1 hit)
 *
 * Namespace: hs_* / HydraTask / HydraQueue / HydraWorkerCtx
 *   ไม่ชน pogls_hydra_route() (core Hydra)
 *   ไม่ชน POGLS_HydraHead / POGLS_HydraCore (ของเดิม)
 *
 * ห้าม include pogls_hydra.h ใน header นี้
 * Scheduler เป็น pure compute layer — ไม่รู้จัก snapshot / audit
 */

#ifndef POGLS_HYDRA_SCHEDULER_H
#define POGLS_HYDRA_SCHEDULER_H

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE   /* pthread_setaffinity_np, cpu_set_t */
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>

#include "pogls_node_soa.h"        /* NODE_MAX, NodeMask, FrontierMask  */
#include "pogls_graph_topology.h"  /* topo_diffuse_fast, edge_masks      */

/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define HS_HEADS            16    /* worker threads = Hydra head count   */
#define HS_QUEUE_SIZE       256   /* per-head ring buffer (power of 2)   */
#define HS_NODE_MAX_DENSE   642   /* Icosphere L3 for adaptive density   */

/* ═══════════════════════════════════════════════════════════════════════
   TASK OPS  (ไม่ชน WAL op / detach op)
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    HS_OP_NODE_WRITE = 1,   /* write value to node                      */
    HS_OP_DIFFUSE    = 2,   /* trigger ntacle diffusion from node        */
    HS_OP_DETACH     = 3,   /* create detach zone at addr               */
    HS_OP_DENSITY    = 4,   /* run adaptive density update on node       */
} hs_op_t;

/* ═══════════════════════════════════════════════════════════════════════
   HYDRA TASK  (fits 1 cache line with queue slot metadata)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t  node_id;    /* target node (0..NODE_MAX-1)                 */
    uint16_t  op;         /* hs_op_t                                     */
    uint32_t  frame_id;   /* detach frame id (HS_OP_DETACH only)        */
    uint64_t  addr;       /* core address                                */
    uint64_t  value;      /* payload                                     */
} HydraTask;
/* 2+2+4+8+8 = 24B */

/* ═══════════════════════════════════════════════════════════════════════
   PER-HEAD QUEUE  (SPSC-safe for owner push, MPSC for steal)
   cacheline-padded to prevent false sharing
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    __attribute__((aligned(64)))
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
    HydraTask         tasks[HS_QUEUE_SIZE];
} HydraQueue;

/* ═══════════════════════════════════════════════════════════════════════
   ADAPTIVE NODE DENSITY
   ═══════════════════════════════════════════════════════════════════════ */

#define HS_RETOPO_SPLIT_THRESH   4096
#define HS_RETOPO_MERGE_THRESH   512

typedef enum {
    HS_NODE_LEAF     = 0,   /* active leaf node                         */
    HS_NODE_INTERNAL = 1,   /* split — children carry the data          */
} hs_node_kind_t;

typedef struct {
    uint32_t  density[HS_NODE_MAX_DENSE];
    uint16_t  parent [HS_NODE_MAX_DENSE];
    uint8_t   kind   [HS_NODE_MAX_DENSE];  /* hs_node_kind_t            */

    /* free-list for alloc/free node slots */
    uint16_t  free_stack[HS_NODE_MAX_DENSE];
    _Atomic uint32_t free_top;

    uint32_t  node_count;   /* total allocated (leaf + internal)        */
} HydraDensityMap;

/* ═══════════════════════════════════════════════════════════════════════
   WORKER CONTEXT  (one per thread)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    int               head_id;          /* 0..HS_HEADS-1                */

    /* shared state — pointer to global arrays */
    HydraQueue       *queues;           /* hydra_queue[HS_HEADS]        */
    NodeState        *node_state;       /* SoA node state               */
    FrontierMask     *frontier;         /* active frontier              */
    NodeMask         *edge_masks;       /* precomputed neighbor masks   */
    HydraDensityMap  *density;          /* adaptive node density        */

    volatile int     *stop;             /* shutdown flag                */

    uint64_t  now_ms;                   /* refreshed each loop iter     */

    /* per-thread stats (no atomic needed — written by owner only) */
    uint64_t  tasks_executed;
    uint64_t  tasks_stolen;

} HydraWorkerCtx;

/* ═══════════════════════════════════════════════════════════════════════
   GLOBAL QUEUES  (extern — defined in pogls_hydra_scheduler.c)
   ═══════════════════════════════════════════════════════════════════════ */

extern HydraQueue hydra_queue[HS_HEADS];

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: QUEUE OPS
   ═══════════════════════════════════════════════════════════════════════ */

/* push — owner thread only (no CAS needed) */
static inline void hs_push(int h, const HydraTask *t)
{
    HydraQueue *q   = &hydra_queue[h];
    uint32_t    pos = atomic_fetch_add_explicit(&q->tail, 1,
                                                 memory_order_relaxed);
    q->tasks[pos % HS_QUEUE_SIZE] = *t;
    atomic_thread_fence(memory_order_release);
}

/* pop — owner thread pops from its own queue */
static inline int hs_pop(int h, HydraTask *out)
{
    HydraQueue *q    = &hydra_queue[h];
    uint32_t    head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t    tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail) return 0;

    *out = q->tasks[head % HS_QUEUE_SIZE];
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: ROUTING
   ═══════════════════════════════════════════════════════════════════════ */

/* locality routing: lower address bits → head (same region = same core) */
static inline int hs_route_addr(uint64_t addr)
{
    return (int)(addr & (HS_HEADS - 1));
}

/* node → head: partition 162 nodes evenly across 16 heads */
static inline int hs_route_node(uint32_t node_id)
{
    return (int)((node_id * HS_HEADS) / NODE_MAX);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: SCHEDULE HELPERS
   ═══════════════════════════════════════════════════════════════════════ */

static inline void hs_schedule_write(uint64_t addr, uint64_t value)
{
    HydraTask t = {
        .op    = (uint16_t)HS_OP_NODE_WRITE,
        .addr  = addr,
        .value = value,
    };
    hs_push(hs_route_addr(addr), &t);
}

static inline void hs_schedule_diffuse(uint32_t node_id)
{
    HydraTask t = {
        .node_id = (uint16_t)node_id,
        .op      = (uint16_t)HS_OP_DIFFUSE,
    };
    hs_push(hs_route_node(node_id), &t);
}

static inline void hs_schedule_detach(uint64_t addr, uint32_t frame_id)
{
    HydraTask t = {
        .op       = (uint16_t)HS_OP_DETACH,
        .frame_id = frame_id,
        .addr     = addr,
    };
    hs_push(hs_route_addr(addr), &t);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: CPU RELAX  (spin-wait backoff)
   ═══════════════════════════════════════════════════════════════════════ */

static inline void hs_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
   API  (implemented in pogls_hydra_scheduler.c)
   ═══════════════════════════════════════════════════════════════════════ */

/* init global queues to zero */
void hs_init(void);

/* work-stealing pop — try to steal from another head's queue */
int hs_steal(int h, HydraTask *out);

/* execute one task — updates node_state + frontier */
void hs_execute(HydraWorkerCtx *ctx, const HydraTask *t);

/* main worker loop — runs until *ctx->stop != 0 */
void *hs_worker_loop(void *arg);   /* pthread-compatible signature */

/* bind calling thread to CPU core hid (requires CAP_SYS_NICE or root) */
int hs_bind_cpu(int hid);

/* launch all HS_HEADS worker threads */
int hs_launch(pthread_t threads[HS_HEADS],
              HydraWorkerCtx ctx_array[HS_HEADS]);

/* stop all workers + join */
void hs_shutdown(pthread_t threads[HS_HEADS],
                 HydraWorkerCtx ctx_array[HS_HEADS]);

/* ── Adaptive density ────────────────────────────────────────────────── */

/* init density map — set all leaf nodes 0..NODE_MAX-1 */
void hs_density_init(HydraDensityMap *dm);

/* alloc new node slot — returns node id or UINT16_MAX if full */
uint16_t hs_density_alloc(HydraDensityMap *dm);

/* free node slot */
void hs_density_free(HydraDensityMap *dm, uint16_t id);

/*
 * hs_density_update — check density[id] and split or merge
 * split: id → 2 children  (density > SPLIT_THRESH)
 * merge: id → parent      (density < MERGE_THRESH)
 */
void hs_density_update(HydraDensityMap *dm, uint16_t id);

/* full engine write path: map → schedule → diffuse → density */
void hs_engine_write(HydraWorkerCtx *ctx, uint64_t addr, uint64_t value);

#endif /* POGLS_HYDRA_SCHEDULER_H */
