/*
 * pogls_entangle_graph.h — POGLS V3.5 Entangle Diffusion Engine
 *
 * Entangle = compute layer ที่วิ่งบน NodeState (SoA)
 * ทำ graph diffusion ผ่าน frontier bitmask — ไม่มี malloc, ไม่มี branch
 *
 * latency target:
 *   entangle_diffuse()     < 200ns  (162 nodes, ~6 neighbors ต่อ node)
 *   entangle_bind()        < 5ns    (hash mod)
 *   entangle_task_push()   < 30ns   (atomic ring buffer)
 *
 * ห้าม include pogls_hydra.h ใน header นี้
 * Entangle อ่าน NodeState โดยตรง — ไม่ผ่าน VisualFeed
 */

#ifndef POGLS_NTANGLE_GRAPH_H
#define POGLS_NTANGLE_GRAPH_H

#include <stdint.h>
#include <stdatomic.h>
#include "pogls_node_soa.h"

/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define ENTANGLE_TASK_QUEUE  512    /* lock-free ring buffer size (power of 2) */
#define ENTANGLE_MAX_THREADS 16     /* ตรงกับ HYDRA_MAX_HEADS                   */

/* ═══════════════════════════════════════════════════════════════════════
   NTACLE TASK
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  node_id;     /* target node (0..NODE_MAX-1)              */
    uint32_t  op;          /* operation type (ขยายได้)                 */
} EntangleTask;

/* op codes */
#define ENTANGLE_OP_WRITE    1
#define ENTANGLE_OP_READ     2
#define ENTANGLE_OP_AUDIT    3
#define ENTANGLE_OP_DETACH   4   /* เชื่อม detach layer */

/* ═══════════════════════════════════════════════════════════════════════
   TASK QUEUE  (MPMC lock-free ring buffer)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
    EntangleTask        tasks[ENTANGLE_TASK_QUEUE];
} EntangleTaskQueue;

/* push — คืน 0=OK, -1=full */
static inline int entangle_task_push(EntangleTaskQueue *q, EntangleTask t)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t next = (tail + 1) % ENTANGLE_TASK_QUEUE;

    if (next == atomic_load_explicit(&q->head, memory_order_acquire))
        return -1; /* full */

    q->tasks[tail] = t;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    return 0;
}

/* pop — คืน 0=OK, -1=empty */
static inline int entangle_task_pop(EntangleTaskQueue *q, EntangleTask *out)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);

    if (head == atomic_load_explicit(&q->tail, memory_order_acquire))
        return -1; /* empty */

    *out = q->tasks[head];
    atomic_store_explicit(&q->head,
                          (head + 1) % ENTANGLE_TASK_QUEUE,
                          memory_order_release);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   NTACLE BIND
   กระจาย frame_id → thread ด้วย hash
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t entangle_bind(uint32_t frame_id,
                                   uint32_t depth,
                                   uint32_t threads)
{
    return (frame_id ^ (depth * 2654435761u)) % threads;
}

/* ═══════════════════════════════════════════════════════════════════════
   DIFFUSION ENGINE
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * entangle_diffuse — หนึ่ง diffusion pass
 *
 * อ่าน frontier (active nodes ตอนนี้)
 * OR ทุก neighbor mask เข้า next
 * ผล: next ครอบ nodes ที่ควร activate รอบถัดไป
 *
 * latency: ~150-200ns สำหรับ 162 nodes
 */
void entangle_diffuse(NodeState          *ns,
                    const FrontierMask *frontier,
                    FrontierMask       *next);

/*
 * entangle_worker — main loop สำหรับ 1 hydra worker thread
 *
 * pop task → node_update → activate frontier
 * วนไปเรื่อยๆ จนได้รับ stop signal (node_id == UINT32_MAX)
 */
void entangle_worker(NodeState       *ns,
                   EntangleTaskQueue *q,
                   FrontierMask    *frontier,
                   uint64_t         now_ms);

/*
 * entangle_frontier_iterate — เดิน frontier ด้วย TZCNT
 *
 * cb(node_id, userdata) เรียกทุก active node
 * ไม่มี branch overhead — ใช้ __builtin_ctzll
 */
void entangle_frontier_iterate(const FrontierMask *frontier,
                              void (*cb)(uint32_t node_id, void *ud),
                              void *userdata);

/*
 * entangle_detach_activate — bind DetachFrame.ntangle_mask กับ frontier
 * เรียกตอน detach_create() เพื่อ lock Ntacles ที่เกี่ยวข้อง
 *
 * ntangle_mask : bitmask ของ node ที่อยู่ใน detached zone
 * frontier    : set bits ที่ตรงกับ mask
 */
static inline void entangle_detach_activate(FrontierMask *frontier,
                                           uint64_t      ntangle_mask)
{
    /* ntangle_mask ใช้ 64 bit แรก (node 0-63) — ขยายได้ถ้าต้องการ */
    frontier->w[0] |= ntangle_mask;
}

/* ═══════════════════════════════════════════════════════════════════════
   WRITE PATH HELPER  (full pipeline ใน 1 call)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * entangle_write_path — เส้นทางเต็ม addr → node → queue → WAL prep
 *
 * คืน node_id ที่ address นี้ map ไป
 * ผลักงานเข้า queue สำหรับ hydra worker
 */
static inline uint32_t entangle_write_path(const NodeLUT   *lut,
                                          EntangleTaskQueue *q,
                                          uint32_t         addr)
{
    uint32_t  node = node_lut_lookup(lut, addr);
    EntangleTask t   = { node, ENTANGLE_OP_WRITE };
    entangle_task_push(q, t);   /* ถ้า full → drop (caller handle) */
    return node;
}

#endif /* POGLS_NTANGLE_GRAPH_H */
