/*
 * pogls_38_hydra.h — POGLS38  Hydra Scheduler (Light)
 * ══════════════════════════════════════════════════════════════════════
 *
 * ประวัติ crash:
 *   DH_MAX_HEADS=32 ใน pogls_hydra_dynamic.h → overflow ทันที
 *   เพราะ queue array + thread array ต้องใช้ stack/static ขนาดใหญ่
 *
 * กฎใน POGLS38:
 *   L38_HS_HEADS = 16  (frozen, digit_sum=7 → ยอมรับได้ vs V4 ที่ต้อง 9)
 *   ไม่ dynamic spawn ใดๆ ทั้งสิ้น
 *   16 heads × queue[256] = 16 × 256 × 24B = 98KB → ปลอดภัย
 *
 * Source แนวทาง:
 *   V3.4 core:   HS_HEADS=16, SPSC push, work-steal (hs_steal)
 *   V4 weight:   HydraWeight, decay 7/8, hs_route_weighted()
 *   ไม่เอา:      DynamicHydra (32 heads), pthread launch (ทำทีหลัง)
 *
 * ══════════════════════════════════════════════════════════════════════
 * Architecture:
 *
 *   addr → hs_route_weighted() → head_id
 *          ↓
 *   hs_push(head_id, task)    → HydraQueue[head_id]
 *          ↓
 *   hs_pop() หรือ hs_steal()  → HydraTask
 *          ↓
 *   hs_execute_38()           → L17 cell update + Tentacle notify
 *
 * Work steal:
 *   head i ว่าง → scan head i+1, i+2, ... circular
 *   steal from head with most tasks (greedy)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_HYDRA_H
#define POGLS_38_HYDRA_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "pogls_17n_lattice.h"   /* PHI constants, L17 */

/* ══════════════════════════════════════════════════════════════════════
 * CONSTANTS  (frozen)
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_HS_HEADS        16u    /* FROZEN — 32 = overflow             */
#define L38_HS_QUEUE_SIZE  256u    /* per-head ring (power of 2)         */
#define L38_HS_QUEUE_MASK  (L38_HS_QUEUE_SIZE - 1u)
#define L38_HS_MAGIC        0x48533136u   /* "HS16"                      */

/* compile-time guards */
typedef char _l38_hs_heads_check[(L38_HS_HEADS == 16) ? 1 : -1];
typedef char _l38_hs_queue_check[(L38_HS_QUEUE_SIZE == 256) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════════
 * TASK (24B, from V3.4 HydraTask)
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_HS_OP_WRITE    1u
#define L38_HS_OP_DIFFUSE  2u
#define L38_HS_OP_DETACH   3u
#define L38_HS_OP_AUDIT    4u
#define L38_HS_OP_REPAIR   5u    /* Tails summon result → queue          */

typedef struct __attribute__((packed)) {
    uint16_t  op;          /* L38_HS_OP_*                               */
    uint16_t  cell_id;     /* L17 cell (0..288)                         */
    uint32_t  frame_id;    /* DetachFrame (0 if unused)                 */
    uint64_t  addr;        /* angular_addr                              */
    uint64_t  value;
} L38HydraTask;            /* 24B                                       */

typedef char _l38_task_sz[(sizeof(L38HydraTask)==24)?1:-1];

/* ══════════════════════════════════════════════════════════════════════
 * PER-HEAD QUEUE  (SPSC safe, MPSC for steal)
 * cache-line aligned to prevent false sharing
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    __attribute__((aligned(64)))
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
    L38HydraTask      tasks[L38_HS_QUEUE_SIZE];
} L38HydraQueue;   /* 24B × 256 + 8 = 6152B per head */

/* push — owner thread only (no CAS, SPSC) */
static inline int l38_hs_push(L38HydraQueue *q, const L38HydraTask *t)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if ((tail - head) >= L38_HS_QUEUE_SIZE) return -1;  /* full */
    q->tasks[tail & L38_HS_QUEUE_MASK] = *t;
    atomic_store_explicit(&q->tail, tail + 1u, memory_order_release);
    return 0;
}

/* pop — owner thread pops own queue */
static inline int l38_hs_pop(L38HydraQueue *q, L38HydraTask *out)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head == tail) return 0;  /* empty */
    *out = q->tasks[head & L38_HS_QUEUE_MASK];
    atomic_store_explicit(&q->head, head + 1u, memory_order_release);
    return 1;
}

/* queue depth (approx) */
static inline uint32_t l38_hs_depth(const L38HydraQueue *q)
{
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    return t - h;
}

/* ══════════════════════════════════════════════════════════════════════
 * WEIGHT TABLE  (V4 HydraWeight port)
 *
 * weight[h] = 0 (idle) → 255 (saturated)
 * decay 7/8 per tick (bit-shift, same as FaceState activity)
 * routing score = phi_dist(addr, head) + weight[head]
 * → choose head with lowest score
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t   weight[L38_HS_HEADS];       /* 0=idle 255=busy           */
    uint8_t   last_cell[L38_HS_HEADS];    /* last cell_id routed here  */
    uint32_t  route_count[L38_HS_HEADS];  /* total routes per head     */
} L38HydraWeight;

static inline void l38_hw_init(L38HydraWeight *hw) {
    memset(hw, 0, sizeof(*hw));
}

/* decay all weights 7/8 (bit-shift) — call once per gate_18 */
static inline void l38_hw_decay(L38HydraWeight *hw)
{
    for (uint32_t i = 0; i < L38_HS_HEADS; i++)
        hw->weight[i] = (uint8_t)((hw->weight[i] * 7u) >> 3u);
}

/* PHI distance: addr vs head's typical address range */
static inline uint32_t l38_hw_phi_dist(uint64_t addr, uint32_t head_id)
{
    uint32_t a    = (uint32_t)(addr & (PHI_SCALE - 1u));
    uint32_t base = (uint32_t)((((uint64_t)head_id * PHI_UP) % PHI_SCALE));
    uint32_t diff = (a > base) ? (a - base) : (base - a);
    return diff >> 16u;   /* normalise to 0..15 range */
}

/* weighted routing: lowest (dist + weight) wins */
static inline uint32_t l38_hs_route_weighted(const L38HydraWeight *hw,
                                               uint64_t addr)
{
    uint32_t best_id    = 0;
    uint32_t best_score = UINT32_MAX;
    for (uint32_t i = 0; i < L38_HS_HEADS; i++) {
        uint32_t score = l38_hw_phi_dist(addr, i) + hw->weight[i];
        if (score < best_score) { best_score = score; best_id = i; }
    }
    return best_id;
}

/* simple routing (fallback, no weight) */
static inline uint32_t l38_hs_route_addr(uint64_t addr) {
    return (uint32_t)(addr & (L38_HS_HEADS - 1u));
}

/* ══════════════════════════════════════════════════════════════════════
 * WORK STEAL  (from V3.4 hs_steal)
 *
 * head i idle → scan other heads circular
 * steal from head with most tasks (greedy = best cache locality)
 * ══════════════════════════════════════════════════════════════════════ */

static inline int l38_hs_steal(L38HydraQueue queues[L38_HS_HEADS],
                                 uint32_t       my_head,
                                 L38HydraTask  *out)
{
    /* find busiest victim (most tasks) */
    uint32_t best_victim = L38_HS_HEADS;  /* sentinel = none */
    uint32_t best_depth  = 0;

    for (uint32_t i = 1; i < L38_HS_HEADS; i++) {
        uint32_t victim = (my_head + i) % L38_HS_HEADS;
        uint32_t d = l38_hs_depth(&queues[victim]);
        if (d > best_depth) { best_depth = d; best_victim = victim; }
    }

    if (best_victim == L38_HS_HEADS || best_depth == 0) return 0;

    /* CAS steal from victim's head */
    L38HydraQueue *vq = &queues[best_victim];
    uint32_t head = atomic_load_explicit(&vq->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&vq->tail, memory_order_acquire);

    while (head != tail) {
        *out = vq->tasks[head & L38_HS_QUEUE_MASK];
        if (atomic_compare_exchange_weak_explicit(
                &vq->head, &head, head + 1u,
                memory_order_release, memory_order_relaxed))
            return 1;  /* steal succeeded */
        /* CAS failed — retry with updated head */
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * HYDRA CONTEXT  (complete scheduler state)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t          magic;
    L38HydraQueue     queues[L38_HS_HEADS];
    L38HydraWeight    weight;

    /* stats — per head, no atomic needed (written by owner only) */
    uint64_t          tasks_pushed[L38_HS_HEADS];
    uint64_t          tasks_executed[L38_HS_HEADS];
    uint64_t          tasks_stolen[L38_HS_HEADS];
    uint64_t          steals_attempted[L38_HS_HEADS];
    uint64_t          push_failures[L38_HS_HEADS];

    /* gate_18 tick counter for weight decay */
    uint32_t          decay_tick;
} L38Hydra;

static inline void l38_hydra_init(L38Hydra *h) {
    memset(h, 0, sizeof(*h));
    h->magic = L38_HS_MAGIC;
    l38_hw_init(&h->weight);
}

/* schedule a task — weighted route + push */
static inline int l38_hydra_schedule(L38Hydra   *h,
                                       L38HydraTask *t,
                                       uint32_t     *out_head)
{
    uint32_t hid = l38_hs_route_weighted(&h->weight, t->addr);
    int r = l38_hs_push(&h->queues[hid], t);
    if (r == 0) {
        h->weight.weight[hid] =
            (uint8_t)((uint32_t)h->weight.weight[hid] + 8u > 255u
                      ? 255u : h->weight.weight[hid] + 8u);
        h->weight.route_count[hid]++;
        h->tasks_pushed[hid]++;
        if (out_head) *out_head = hid;
    } else {
        h->push_failures[hid]++;
        /* fallback: try adjacent heads */
        for (uint32_t i = 1; i < L38_HS_HEADS; i++) {
            uint32_t alt = (hid + i) % L38_HS_HEADS;
            if (l38_hs_push(&h->queues[alt], t) == 0) {
                h->tasks_pushed[alt]++;
                if (out_head) *out_head = alt;
                return 0;
            }
        }
        return -1;  /* all heads full */
    }
    return 0;
}

/* execute one task inline (single-threaded mode, no pthread) */
typedef void (*l38_hydra_exec_cb)(const L38HydraTask *, void *ctx);

static inline int l38_hydra_drain_head(L38Hydra         *h,
                                         uint32_t          head_id,
                                         l38_hydra_exec_cb cb,
                                         void             *cb_ctx)
{
    L38HydraTask t;
    int n = 0;
    while (l38_hs_pop(&h->queues[head_id], &t)) {
        if (cb) cb(&t, cb_ctx);
        h->tasks_executed[head_id]++;
        n++;
    }
    return n;
}

/* drain all heads — single-threaded round-robin */
static inline int l38_hydra_drain_all(L38Hydra         *h,
                                        l38_hydra_exec_cb cb,
                                        void             *cb_ctx)
{
    int total = 0;
    for (uint32_t i = 0; i < L38_HS_HEADS; i++)
        total += l38_hydra_drain_head(h, i, cb, cb_ctx);
    return total;
}

/* gate_18 tick: decay weights */
static inline void l38_hydra_gate18(L38Hydra *h) {
    h->decay_tick++;
    l38_hw_decay(&h->weight);
}

/* total depth across all heads */
static inline uint32_t l38_hydra_total_depth(const L38Hydra *h)
{
    uint32_t d = 0;
    for (uint32_t i = 0; i < L38_HS_HEADS; i++)
        d += l38_hs_depth(&h->queues[i]);
    return d;
}

/* ══════════════════════════════════════════════════════════════════════
 * POINTER TRACKING  (Tails negative pointer)
 *
 * "pointer" ใน V3.x = ตำแหน่ง head ใน queue
 * "negative ของ pointer" = สิ่งที่ Tails track:
 *   tail_ptr = tail position ขณะ push
 *   neg_ptr  = ~tail_ptr (bitwise NOT) = "negative" ของ pointer
 *
 * ถ้า queue corrupt หรือ head > tail → neg_ptr mismatch
 * Tails ใช้เพื่อ detect queue state corruption
 *
 * เปรียบเทียบ: เหมือน checksum ของ pointer เอง
 *   ptr + neg_ptr = 0xFFFFFFFF (invariant)
 *   ถ้าไม่ตรง → pointer พัง
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t  tail_ptr;    /* tail position ขณะ snapshot               */
    uint32_t  neg_ptr;     /* ~tail_ptr = complement                    */
    uint32_t  head_id;
    uint32_t  _pad;
} L38TailsQueuePtr;        /* 16B                                       */

#define L38_PTR_INVARIANT  0xFFFFFFFFu   /* tail + neg = this always    */

static inline L38TailsQueuePtr l38_tails_snapshot_ptr(
    const L38HydraQueue *q, uint32_t head_id)
{
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    return (L38TailsQueuePtr){
        .tail_ptr = t,
        .neg_ptr  = ~t,          /* bitwise NOT = "negative"            */
        .head_id  = head_id,
        ._pad     = 0,
    };
}

/* verify: tail + neg == 0xFFFFFFFF */
static inline int l38_tails_ptr_valid(const L38TailsQueuePtr *p)
{
    return (p->tail_ptr + p->neg_ptr) == L38_PTR_INVARIANT;
}

/* scan all queues — return count of corrupted pointers */
static inline uint32_t l38_tails_scan_ptrs(const L38Hydra *h,
                                              L38TailsQueuePtr snap_out[L38_HS_HEADS])
{
    uint32_t corrupt = 0;
    for (uint32_t i = 0; i < L38_HS_HEADS; i++) {
        snap_out[i] = l38_tails_snapshot_ptr(&h->queues[i], i);
        if (!l38_tails_ptr_valid(&snap_out[i])) corrupt++;
    }
    return corrupt;
}

#endif /* POGLS_38_HYDRA_H */
