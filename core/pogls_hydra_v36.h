/*
 * pogls_hydra_v36.h — POGLS V3.6 Hydra Bridge
 *
 * Connect Hydra (V3.1 router) กับ V3.6 layer:
 *   fibo_addr     → route by node index (ไม่ใช่ byte offset)
 *   DiamondBlock  → 64B unit ที่ Hydra append
 *   Entangle      → hook เมื่อ head anomaly → eject
 *   Unit Circle   → priority zone routing
 *
 * ไม่แก้ pogls_hydra.h — เป็น pure additive bridge
 * Namespace: hv36_* / HydraV36
 *
 * ห้าม include pogls_snapshot.h ใน header นี้
 */

#ifndef POGLS_HYDRA_V36_H
#define POGLS_HYDRA_V36_H

#include <stdint.h>
#include <string.h>
#include "pogls_fold.h"
#include "pogls_fibo_addr.h"
#include "pogls_entangle.h"
#include "pogls_hydra_scheduler.h"

/* ═══════════════════════════════════════════════════════════════════════
   ROUTING  — fibo_addr → head_id
   แทน pogls_hydra_route(byte_offset) ด้วย node index routing
   ═══════════════════════════════════════════════════════════════════════ */

/* route node_idx → head_id (0..HS_HEADS-1)
   ใช้ fibo_addr เป็น hash key — cache-locality: ใกล้กัน = head เดียวกัน */
static inline uint8_t hv36_route(uint32_t node_idx, uint8_t gear, uint8_t world)
{
    uint32_t addr = fibo_addr(node_idx, gear, world);
    return (uint8_t)(addr % HS_HEADS);
}

/* priority route — outside circle → head 0 (priority head)
   inside circle → normal round-robin */
static inline uint8_t hv36_route_priority(uint32_t node_idx,
                                           uint8_t  gear,
                                           uint8_t  world)
{
    uint32_t addr = fibo_addr(node_idx, gear, world);
    uint64_t a    = (uint64_t)addr;
    uint64_t s2   = (uint64_t)PHI_SCALE * PHI_SCALE;

    if (2 * a * a >= s2)
        return 0;   /* outside circle → priority head */

    return (uint8_t)(addr % HS_HEADS);
}

/* ═══════════════════════════════════════════════════════════════════════
   WRITE PATH  — DiamondBlock through Hydra
   ═══════════════════════════════════════════════════════════════════════ */

/* push DiamondBlock write task เข้า HydraQueue ของ head ที่ถูก route */
static inline int hv36_enqueue_block(HydraQueue   queues[HS_HEADS],
                                      uint32_t      node_idx,
                                      uint8_t       gear,
                                      uint8_t       world,
                                      const DiamondBlock *block)
{
    uint8_t  hid  = hv36_route_priority(node_idx, gear, world);
    HydraQueue *q  = &queues[hid];

    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t next = (tail + 1) % HS_QUEUE_SIZE;

    if (next == atomic_load_explicit(&q->head, memory_order_acquire))
        return -1;   /* queue full */

    HydraTask t = {
        .node_id  = (uint16_t)(node_idx & 0xFFFF),
        .op       = HS_OP_NODE_WRITE,
        .frame_id = 0,
        .addr     = fibo_addr(node_idx, gear, world),
        .value    = block->core.raw,   /* signature = core slot */
    };

    q->tasks[tail] = t;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    return (int)hid;   /* คืน head_id ที่ assign */
}

/* ═══════════════════════════════════════════════════════════════════════
   ANOMALY → ENTANGLE EJECT
   เมื่อ Hydra head detect anomaly → สร้าง EjectFrame + EntangleHook
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    EjectFrame   eject;
    EntangleHook hook;
    uint8_t      head_id;
    uint8_t      active;
} HydraEjectCtx;

/* สร้าง eject context เมื่อ head anomaly */
static inline void hv36_anomaly_eject(HydraEjectCtx *ctx,
                                       uint8_t         head_id,
                                       uint32_t        frame_id,
                                       uint64_t        gate_addr,
                                       const DiamondBlock *block,
                                       uint64_t        now_ms)
{
    uint8_t world = core_world(block->core) == WORLD_B ? 1 : 0;

    ej_frame_init(&ctx->eject, frame_id, EJ_MODE_EMERGENCY,
                  gate_addr, block, 0, now_ms);
    ent_hook_init(&ctx->hook, &ctx->eject, world);

    ctx->head_id = head_id;
    ctx->active  = 1;
}

/* run repair pipeline — คืน 1=fold ok, 0=recycled */
static inline int hv36_eject_repair(HydraEjectCtx *ctx, DiamondBlock *b)
{
    if (!ctx->active) return 0;
    int r = ent_repair_pipeline(&ctx->hook, &ctx->eject, b);
    if (!r) ctx->active = 0;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════
   HEAD DENSITY CHECK  — V3.6 unit circle aware
   นับ nodes ใน head zone แยก inside/outside circle
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total;     /* nodes ใน zone */
    uint32_t inside;    /* อยู่ใน unit circle */
    uint32_t outside;   /* อยู่นอก — priority audit zone */
    uint8_t  head_id;
    uint8_t  needs_spawn;   /* outside > threshold → spawn */
    uint8_t  needs_retract; /* total < threshold → retract */
} HydraV36Density;

#define HV36_OUTSIDE_SPAWN_THRESH   8   /* outside nodes เกิน 8 → spawn */
#define HV36_TOTAL_RETRACT_THRESH   4   /* total nodes ต่ำกว่า 4 → retract */

static inline HydraV36Density hv36_density(uint8_t  head_id,
                                             uint32_t node_start,
                                             uint32_t node_count,
                                             uint8_t  gear,
                                             uint8_t  world)
{
    HydraV36Density d = { .head_id = head_id };

    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t n    = node_start + i;
        uint32_t addr = fibo_addr(n, gear, world);
        uint64_t a    = (uint64_t)addr;
        uint64_t s2   = (uint64_t)PHI_SCALE * PHI_SCALE;

        d.total++;
        if (2 * a * a >= s2)
            d.outside++;
        else
            d.inside++;
    }

    d.needs_spawn   = d.outside > HV36_OUTSIDE_SPAWN_THRESH;
    d.needs_retract = d.total   < HV36_TOTAL_RETRACT_THRESH;
    return d;
}

/* ═══════════════════════════════════════════════════════════════════════
   WORK STEAL  — steal task จาก head ที่มี outside-circle node มาก
   ═══════════════════════════════════════════════════════════════════════ */

/* pop task จาก queue ที่มีงานค้าง
   ลอง head เรียงจาก outside ก่อน (priority) */
static inline int hv36_steal(HydraQueue   queues[HS_HEADS],
                              uint8_t      my_head,
                              HydraTask   *out)
{
    /* ลอง steal จาก head อื่นตามลำดับ circular */
    for (int i = 1; i < HS_HEADS; i++) {
        uint8_t victim = (uint8_t)((my_head + i) % HS_HEADS);
        HydraQueue *q  = &queues[victim];

        uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        if (head == atomic_load_explicit(&q->tail, memory_order_acquire))
            continue;   /* empty */

        *out = q->tasks[head];
        if (atomic_compare_exchange_weak_explicit(
                &q->head,
                &head,
                (head + 1) % HS_QUEUE_SIZE,
                memory_order_release,
                memory_order_relaxed))
            return (int)victim;   /* steal สำเร็จ จาก head victim */
    }
    return -1;   /* nothing to steal */
}

/* ═══════════════════════════════════════════════════════════════════════
   TAILS CHECKPOINT  — snapshot สำหรับ Hydra state
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint8_t   head_id;
    uint8_t   world;
    uint8_t   eject_active;
    uint8_t   _pad;
    uint32_t  last_node_idx;
    uint32_t  last_addr;
    uint32_t  outside_count;
    uint8_t   ent_checkpoint[sizeof(EntTailsCheckpoint)];
} HydraV36Checkpoint;

static inline void hv36_checkpoint(HydraV36Checkpoint    *cp,
                                    const HydraEjectCtx   *ctx,
                                    uint8_t                head_id,
                                    uint8_t                world,
                                    const HydraV36Density *density)
{
    memset(cp, 0, sizeof(*cp));
    cp->head_id      = head_id;
    cp->world        = world;
    cp->eject_active = ctx ? ctx->active : 0;
    cp->outside_count= density ? density->outside : 0;
    cp->last_addr    = density ? fibo_addr(0, 0, world) : 0;

    if (ctx && ctx->active) {
        EntTailsCheckpoint ent_cp;
        ent_tails_checkpoint(&ctx->hook, &ent_cp);
        memcpy(cp->ent_checkpoint, &ent_cp, sizeof(EntTailsCheckpoint));
    }
}

#endif /* POGLS_HYDRA_V36_H */
