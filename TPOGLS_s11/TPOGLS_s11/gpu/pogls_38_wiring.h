/*
 * pogls_38_wiring.h — POGLS38  System Wiring Layer
 * ══════════════════════════════════════════════════════════════════════
 *
 * รวม relationships ที่ถูกต้องระหว่าง subsystems:
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │                    ROLE MATRIX (FROZEN)                     │
 *  ├──────────────┬────────────────────────────────────────────  │
 *  │ Entangle     │ observer ของ DetachFrame — log ทุก step      │
 *  │              │ มี warp_open: ถ้า face hibernate → warp=1    │
 *  │              │ warp เชื่อมกับ Detach ตลอดเวลา               │
 *  ├──────────────┼────────────────────────────────────────────  │
 *  │ Tails        │ read-only ONLY — ดู log จด error SUMMON      │
 *  │              │ ห้าม write, ห้าม repair, ห้าม route          │
 *  ├──────────────┼────────────────────────────────────────────  │
 *  │ Tentacle     │ graph diffuse บน NodeState (SoA)             │
 *  │              │ MPMC task queue ส่งงานไป Hydra worker         │
 *  │              │ tentacle_detach_activate() bind DetachFrame   │
 *  ├──────────────┼────────────────────────────────────────────  │
 *  │ Hydra (H3)   │ V3.6 bridge: fibo_addr route + anomaly eject │
 *  │              │ Entangle hook ติดทุก head เมื่อ anomaly       │
 *  ├──────────────┼────────────────────────────────────────────  │
 *  │ QuadFibo     │ 4-axis audit: X+(-X)=0, Y+(-Y)=0            │
 *  │              │ Gear: PHI smooth transition                   │
 *  ├──────────────┼────────────────────────────────────────────  │
 *  │ Pressure     │ Mask + Backpressure + WorkSteal              │
 *  │              │ ป้องกัน queue overflow → slow producer       │
 *  ├──────────────┼────────────────────────────────────────────  │
 *  │ Temporal     │ FiftyFourBridge ring(256) + hash_index(1024) │
 *  │              │ World 4n/5n/6n slots: n, n+18, n+36          │
 *  └──────────────┴────────────────────────────────────────────  │
 *
 *  Flow:
 *    write(addr) → QuadFibo audit → Tentacle → Hydra H3
 *                → anomaly? → Entangle warp → DetachFrame
 *                → Tails log → (summon if needed)
 *                → Temporal ring → slot 4n/5n/6n
 *                → Pressure gate → ExecWindow flush
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_WIRING_H
#define POGLS_38_WIRING_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "pogls_17n_lattice.h"
#include "pogls_38_repair.h"
#include "pogls_38_observers.h"

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — ENTANGLE WARP ↔ DETACH
 *
 * V3.6 design (ถูกต้อง):
 *   Entangle ไม่ใช่แค่ repair — มันเชื่อมกับ DetachFrame ตลอดเวลา
 *   เมื่อ face hibernate → warp_open=1 → Entangle เห็น warp state
 *   warp = bridge ที่เปิดไว้ให้ Hydra read/write ผ่านได้
 *   แม้ face หลับ แต่ data ยังเข้าถึงได้ผ่าน warp
 *
 * ใน POGLS38: เราแทน EjectFrame ด้วย L38EjectFrame
 *   warp_open = 1 หมายความว่า cell ยัง "live" แม้จะ detached
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    L38EjectFrame   frame;          /* repair state + move log           */
    uint8_t         warp_open;      /* 1 = warp bridge open (hibernate)  */
    uint8_t         detach_active;  /* 1 = currently detached from core  */
    uint8_t         world;          /* current world 0=A 1=B             */
    uint8_t         _pad;
    uint32_t        cell_id;        /* L17 cell this warp covers         */
    uint64_t        angular_addr;   /* original angular address          */
    uint64_t        detach_at_ns;   /* timestamp of detach               */
} L38WarpDetach;                    /* Entangle + warp + detach in 1     */

static inline void l38_warp_init(L38WarpDetach *wd,
                                   uint32_t       frame_id,
                                   uint32_t       cell_id,
                                   uint64_t       angular_addr,
                                   uint8_t        world)
{
    memset(wd, 0, sizeof(*wd));
    l38_frame_init(&wd->frame, frame_id, cell_id, angular_addr, world);
    wd->cell_id      = cell_id;
    wd->angular_addr = angular_addr;
    wd->world        = world;
    wd->detach_active = 1;
    wd->detach_at_ns  = _l38_now_ns();
}

/* hibernate: face → sleep, warp bridge stays open
 * data still accessible through warp even when face is sleeping */
static inline void l38_warp_hibernate(L38WarpDetach *wd)
{
    wd->warp_open = 1;
    l38_log_push(&wd->frame.log, ENT_MOVE_HIBERNATE,
                 wd->world, 0, 0, wd->cell_id);
}

/* wake: merge back to core, close warp bridge */
static inline void l38_warp_wake(L38WarpDetach *wd)
{
    wd->warp_open    = 0;
    wd->detach_active = 0;
    l38_log_push(&wd->frame.log, ENT_MOVE_WAKE,
                 wd->world, 0, 0, wd->cell_id);
}

/* query: can read through warp even when detached? */
static inline int l38_warp_readable(const L38WarpDetach *wd)
{
    /* readable if: warp open (hibernating) OR not detached at all */
    return wd->warp_open || !wd->detach_active;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — TENTACLE
 *
 * Port จาก V3.6 pogls_tentacle_graph.h
 * graph diffuse บน 289 L17 cells ผ่าน MPMC task queue
 *
 * Tentacle ≠ Entangle:
 *   Tentacle = compute diffusion (ขยาย active zone)
 *   Entangle = repair observer (ตาม DetachFrame)
 *
 * tentacle_detach_activate(): bind DetachFrame → frontier
 *   เมื่อ cell detach → cells ใน neighborhood ถูก mark ด้วย
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_NTACLE_QUEUE_SIZE  512   /* MPMC ring, power-of-2           */
#define L38_NTACLE_MAX_THREADS  16   /* matches Hydra heads             */

#define L38_NTACLE_OP_WRITE    1
#define L38_NTACLE_OP_READ     2
#define L38_NTACLE_OP_AUDIT    3
#define L38_NTACLE_OP_DETACH   4    /* bind DetachFrame to frontier     */
#define L38_NTACLE_OP_WARP     5    /* warp bridge read                 */

typedef struct {
    uint32_t  cell_id;    /* target L17 cell (0..288)                   */
    uint32_t  op;         /* L38_NTACLE_OP_*                            */
    uint64_t  addr;       /* angular_addr                               */
    uint32_t  frame_id;   /* DetachFrame (0 if none)                    */
    uint32_t  _pad;
} L38NtacleTask;          /* 24B                                        */

typedef struct {
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
    L38NtacleTask     tasks[L38_NTACLE_QUEUE_SIZE];
} L38NtacleQueue;

static inline int l38_ntacle_push(L38NtacleQueue *q, L38NtacleTask t)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t next = (tail + 1) % L38_NTACLE_QUEUE_SIZE;
    if (next == atomic_load_explicit(&q->head, memory_order_acquire))
        return -1;  /* full */
    q->tasks[tail] = t;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    return 0;
}

static inline int l38_ntacle_pop(L38NtacleQueue *q, L38NtacleTask *out)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (head == atomic_load_explicit(&q->tail, memory_order_acquire))
        return -1;  /* empty */
    *out = q->tasks[head];
    atomic_store_explicit(&q->head, (head+1) % L38_NTACLE_QUEUE_SIZE,
                          memory_order_release);
    return 0;
}

/* bind DetachFrame → frontier (marks neighborhood as "watch zone") */
static inline void l38_tentacle_detach_activate(L38Bitboard   *frontier,
                                                  uint32_t       cell_id,
                                                  const L38BBGraph *g)
{
    /* activate the detached cell + its immediate neighbors */
    l38_bb_set(frontier, (uint16_t)cell_id);
    l38_bb_or(frontier, &g->adj[cell_id]);
}

/* hash frame_id → thread_id (Tentacle bind) */
static inline uint32_t l38_ntacle_bind(uint32_t frame_id,
                                         uint32_t depth,
                                         uint32_t threads)
{
    return (frame_id ^ (depth * 2654435761u)) % threads;
}

/* write path: addr → cell → push task → return cell_id */
static inline uint32_t l38_ntacle_write_path(L38NtacleQueue *q,
                                               uint64_t        angular_addr,
                                               uint32_t        frame_id)
{
    L17Cell c = l17_cell_from_addr(angular_addr, 0);
    L38NtacleTask t = {
        .cell_id  = c.cell_id,
        .op       = L38_NTACLE_OP_WRITE,
        .addr     = angular_addr,
        .frame_id = frame_id,
    };
    l38_ntacle_push(q, t);
    return c.cell_id;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — HYDRA H3 BRIDGE (POGLS38 minimal port)
 *
 * "H3" = Hydra V3.6 bridge (hv36_*)
 * ใน POGLS38: ไม่ include DiamondBlock — ทำงานบน L17Cell แทน
 *
 * H3 bridge ทำ:
 *   fibo_addr → route cell → Hydra head_id (0..15)
 *   outside unit circle → head 0 (priority)
 *   anomaly → summon Entangle warp
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_H3_HEADS    16    /* matches HYDRA_MAX_HEADS                 */
#define L38_H3_QUEUE     64   /* per-head task ring                      */

#define L38_H3_OUTSIDE_SPAWN_THRESH   8
#define L38_H3_RETRACT_THRESH         4

/* route cell → head_id */
static inline uint8_t l38_h3_route(uint32_t cell_id, uint8_t world)
{
    /* PHI scatter on cell_id → head */
    uint64_t base = world ? PHI_DOWN : PHI_UP;
    uint32_t addr = (uint32_t)(((uint64_t)cell_id * base) % PHI_SCALE);
    /* outside circle → priority head 0 */
    if (!l38_in_circle(addr)) return 0;
    return (uint8_t)(addr % L38_H3_HEADS);
}

/* density info per head */
typedef struct {
    uint8_t  head_id;
    uint32_t total;
    uint32_t inside;
    uint32_t outside;
    uint8_t  needs_spawn;
    uint8_t  needs_retract;
} L38H3Density;

static inline L38H3Density l38_h3_density(uint8_t head_id,
                                            const L38Bitboard *head_cells)
{
    L38H3Density d = { .head_id = head_id };
    for (uint16_t n = l38_bb_next(head_cells, 0);
         n < L38_BB_NODES;
         n = l38_bb_next(head_cells, (uint16_t)(n+1)))
    {
        d.total++;
        uint32_t addr = (uint32_t)(((uint64_t)n * PHI_UP) % PHI_SCALE);
        if (l38_in_circle(addr)) d.inside++;
        else                     d.outside++;
    }
    d.needs_spawn   = d.outside > L38_H3_OUTSIDE_SPAWN_THRESH;
    d.needs_retract = d.total   < L38_H3_RETRACT_THRESH;
    return d;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — QUAD-FIBO AUDIT
 *
 * Port ตรรกะจาก pogls_quad_fibo_gearbox.py เป็น C
 * 4 axis: X=fibo_a(n)  -X=PHI_SCALE-fibo_a(n)
 *         Y=fibo_b(n)  -Y=PHI_SCALE-fibo_b(n)
 * Invariant: X+(-X) = PHI_SCALE  Y+(-Y) = PHI_SCALE (exact integer)
 *
 * เปรียบเทียบ: เหมือนตาชั่ง 4 ด้าน — ถ้า balance ดี = data clean
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t axis_x;     /* fibo_addr_a(n)                             */
    uint32_t axis_nx;    /* PHI_SCALE - axis_x  (complement)           */
    uint32_t axis_y;     /* fibo_addr_b(n)                             */
    uint32_t axis_ny;    /* PHI_SCALE - axis_y                         */
    uint8_t  ok;         /* 1 = audit pass                             */
    uint8_t  world;
    uint16_t _pad;
} L38QuadFibo;

static inline L38QuadFibo l38_quad_fibo(uint32_t n, uint8_t world)
{
    L38QuadFibo q;
    uint64_t base_a = PHI_UP, base_b = PHI_DOWN;
    if (world) { base_a = PHI_DOWN; base_b = PHI_UP; }

    q.axis_x  = (uint32_t)(((uint64_t)n * base_a) % PHI_SCALE);
    q.axis_nx = (uint32_t)(PHI_SCALE - q.axis_x);
    q.axis_y  = (uint32_t)(((uint64_t)n * base_b) % PHI_SCALE);
    q.axis_ny = (uint32_t)(PHI_SCALE - q.axis_y);
    q.world   = world;

    /* audit: X + (-X) == PHI_SCALE  &&  Y + (-Y) == PHI_SCALE */
    q.ok = ((q.axis_x + q.axis_nx == PHI_SCALE) &&
            (q.axis_y + q.axis_ny == PHI_SCALE)) ? 1u : 0u;
    return q;
}

/* batch audit: n cells starting from base_n
 * returns count of cells that FAIL audit */
static inline uint32_t l38_quad_fibo_audit(uint32_t base_n,
                                             uint32_t count,
                                             uint8_t  world)
{
    uint32_t fails = 0;
    for (uint32_t i = 0; i < count; i++) {
        L38QuadFibo q = l38_quad_fibo(base_n + i, world);
        if (!q.ok) fails++;
    }
    return fails;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — PRESSURE BRIDGE
 *
 * Port จาก V4 SKILL (pogls_pressure_bridge.h):
 * Mask + Backpressure + WorkSteal
 *
 * Backpressure: ถ้า queue ใกล้เต็ม → slow producer (ไม่ drop)
 * Mask: ซ่อน cells ที่กำลัง repair → ไม่ให้ write ซ้ำ
 * WorkSteal: thread ว่าง → ดึงงานจาก busy thread
 *
 * เปรียบเทียบ: เหมือน traffic light ที่ฉลาด
 *   เขียว = ไป  เหลือง = slow down  แดง = รอ (ไม่ใช่ drop)
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_PRESSURE_HIGH  200   /* ops ใน queue → ต้อง slow           */
#define L38_PRESSURE_MAX   240   /* ops ใน queue → block (ไม่ drop)    */
#define L38_PRESSURE_LOW    64   /* ops ใน queue → ปกติ                */

typedef enum {
    L38_PRESSURE_OK     = 0,   /* ไหลปกติ                              */
    L38_PRESSURE_SLOW   = 1,   /* slow producer                        */
    L38_PRESSURE_BLOCK  = 2,   /* block จนกว่า queue จะ drain          */
} l38_pressure_t;

typedef struct {
    _Atomic uint32_t  queue_depth;   /* current ops in flight            */
    L38Bitboard       repair_mask;   /* cells currently under repair     */
    uint32_t          steal_count;   /* successful steals                */
    uint32_t          block_count;   /* times blocked                    */
} L38PressureBridge;

static inline void l38_pressure_init(L38PressureBridge *pb)
{
    memset(pb, 0, sizeof(*pb));
}

static inline l38_pressure_t l38_pressure_check(const L38PressureBridge *pb)
{
    uint32_t d = atomic_load_explicit(&pb->queue_depth, memory_order_relaxed);
    if (d >= L38_PRESSURE_MAX)  return L38_PRESSURE_BLOCK;
    if (d >= L38_PRESSURE_HIGH) return L38_PRESSURE_SLOW;
    return L38_PRESSURE_OK;
}

/* mask a cell: mark as "under repair → no new writes" */
static inline void l38_pressure_mask(L38PressureBridge *pb, uint16_t cell_id)
{
    l38_bb_set(&pb->repair_mask, cell_id);
}

static inline void l38_pressure_unmask(L38PressureBridge *pb, uint16_t cell_id)
{
    l38_bb_clear_node(&pb->repair_mask, cell_id);
}

static inline int l38_pressure_is_masked(const L38PressureBridge *pb,
                                           uint16_t cell_id)
{
    return l38_bb_test(&pb->repair_mask, cell_id);
}

static inline void l38_pressure_enqueue(L38PressureBridge *pb) {
    atomic_fetch_add_explicit(&pb->queue_depth, 1, memory_order_relaxed);
}

static inline void l38_pressure_dequeue(L38PressureBridge *pb) {
    uint32_t d = atomic_load_explicit(&pb->queue_depth, memory_order_relaxed);
    if (d > 0) atomic_fetch_sub_explicit(&pb->queue_depth, 1,
                                          memory_order_relaxed);
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 6 — TEMPORAL BRIDGE (FiftyFourBridge stub)
 *
 * Port concept จาก V4 SKILL + V3.6 แผนภาพ:
 *   ring[256] (RAM) + hash_index[1024]
 *   World slots: n (current), n+18 (prefetch), n+36 (prefetch2)
 *   หลักการ: เขียน → ring → dispatch → 4n/5n/6n world slots
 *
 * ใน POGLS38: ใช้ L17 cell แทน 54 lanes
 *   slot_base   = cell_id % 17 (ตาม 17n lattice)
 *   slot_next   = slot_base + 18 (prefetch, mod 289)
 *   slot_next2  = slot_base + 36 (prefetch2, mod 289)
 *
 * เปรียบเทียบ: เหมือน triple-buffering ใน video game
 *   buffer A = กำลัง write, buffer B = กำลัง read, buffer C = next
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_TEMPORAL_RING_SIZE   256
#define L38_TEMPORAL_HASH_SIZE  1024
#define L38_TEMPORAL_MAGIC       0x54454D50u  /* "TEMP" */

typedef struct __attribute__((packed)) {
    uint64_t  angular_addr;
    uint64_t  value;
    uint32_t  cell_id;
    uint32_t  seq;
    uint64_t  timestamp_ns;
} L38TemporalEntry;   /* 32B */

typedef struct {
    uint32_t          magic;
    L38TemporalEntry  ring[L38_TEMPORAL_RING_SIZE];
    uint32_t          hash_index[L38_TEMPORAL_HASH_SIZE];  /* addr → ring slot */
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
    uint64_t          total_written;
    uint64_t          total_wrapped;

    /* world slot tracking (gate_18 based) */
    uint32_t          slot_base;    /* current slot (mod 289)            */
    uint32_t          slot_next;    /* prefetch1 = base + 18             */
    uint32_t          slot_next2;   /* prefetch2 = base + 36             */
} L38TemporalBridge;

static inline void l38_temporal_init(L38TemporalBridge *tb)
{
    memset(tb, 0, sizeof(*tb));
    tb->magic = L38_TEMPORAL_MAGIC;
    for (int i = 0; i < L38_TEMPORAL_HASH_SIZE; i++)
        tb->hash_index[i] = UINT32_MAX;  /* empty sentinel */
}

static inline void l38_temporal_write(L38TemporalBridge *tb,
                                        uint64_t           angular_addr,
                                        uint64_t           value,
                                        uint32_t           cell_id)
{
    uint32_t slot = atomic_fetch_add_explicit(&tb->head, 1,
                        memory_order_relaxed) % L38_TEMPORAL_RING_SIZE;
    L38TemporalEntry *e = &tb->ring[slot];
    e->angular_addr = angular_addr;
    e->value        = value;
    e->cell_id      = cell_id;
    e->seq          = (uint32_t)tb->total_written;
    e->timestamp_ns = _l38_now_ns();
    tb->total_written++;

    /* update hash index */
    uint32_t hk = (uint32_t)((angular_addr * PHI_UP) % L38_TEMPORAL_HASH_SIZE);
    tb->hash_index[hk] = slot;

    /* advance world slots by gate_18 */
    tb->slot_base  = (uint32_t)(cell_id % L17_LANES_TOTAL);
    tb->slot_next  = (tb->slot_base + L17_GATE) % L17_LANES_TOTAL;
    tb->slot_next2 = (tb->slot_base + L17_GATE * 2u) % L17_LANES_TOTAL;
}

/* lookup by addr → ring slot (O(1) via hash) */
static inline const L38TemporalEntry *l38_temporal_lookup(
    const L38TemporalBridge *tb, uint64_t angular_addr)
{
    uint32_t hk = (uint32_t)((angular_addr * PHI_UP) % L38_TEMPORAL_HASH_SIZE);
    uint32_t slot = tb->hash_index[hk];
    if (slot == UINT32_MAX || slot >= L38_TEMPORAL_RING_SIZE) return NULL;
    const L38TemporalEntry *e = &tb->ring[slot];
    return (e->angular_addr == angular_addr) ? e : NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 7 — FULL WRITE PIPELINE (wired together)
 *
 * addr → QuadFibo audit → Pressure check → Tentacle queue
 *      → H3 route → anomaly? → WarpDetach + Tails summon
 *      → Temporal ring → ExecWindow
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    L17Lattice        *lattice;
    L38NtacleQueue    *ntacle;
    L38BBGraph        *bbgraph;
    L38PressureBridge *pressure;
    L38TemporalBridge *temporal;
    L38ExecWindow     *ew;
    L38Tails          *tails;
    L38RepairCtx      *repair;
    uint32_t           next_frame_id;
} L38WirePipeline;

typedef struct {
    uint32_t  cell_id;
    uint8_t   head_id;
    uint8_t   quad_ok;
    uint8_t   pressure_level;  /* l38_pressure_t */
    uint8_t   anomaly;
    uint8_t   warp_open;
    uint8_t   repair_ok;
    uint8_t   _pad[2];
} L38WriteResult;

static inline L38WriteResult l38_wire_write(L38WirePipeline *p,
                                              uint64_t          angular_addr,
                                              uint64_t          value)
{
    L38WriteResult r = {0};

    /* 1. QuadFibo audit */
    uint32_t n = (uint32_t)(angular_addr & (PHI_SCALE - 1u));
    L38QuadFibo qf = l38_quad_fibo(n, 0);
    r.quad_ok = qf.ok;

    /* 2. Pressure check */
    l38_pressure_t pr = l38_pressure_check(p->pressure);
    r.pressure_level = (uint8_t)pr;
    if (pr == L38_PRESSURE_BLOCK) return r;  /* blocked — no write */

    /* 3. Map to L17 cell */
    L17Cell cell = l17_cell_from_addr(angular_addr, p->lattice->gate_count);
    r.cell_id = cell.cell_id;

    /* 4. H3 route */
    r.head_id = l38_h3_route(cell.cell_id, cell.world);

    /* 5. Anomaly check: addr outside unit circle? */
    uint32_t addr20 = (uint32_t)(angular_addr & (PHI_SCALE - 1u));
    r.anomaly = !l38_in_circle(addr20);

    if (r.anomaly && p->tails && !l38_pressure_is_masked(&(*p->pressure),
                                                           (uint16_t)cell.cell_id)) {
        /* mask cell during repair */
        l38_pressure_mask(p->pressure, (uint16_t)cell.cell_id);
        l38_pressure_enqueue(p->pressure);

        /* Tails summons Entangle */
        L38TailsSummonResult sr = l38_tails_summon(
            p->tails, p->repair, p->lattice,
            cell.cell_id, angular_addr, (uint8_t)cell.world);
        r.repair_ok  = sr.repaired;
        r.warp_open  = !sr.repaired ? 1u : 0u;  /* stay warp if failed */

        l38_pressure_dequeue(p->pressure);
        if (sr.repaired)
            l38_pressure_unmask(p->pressure, (uint16_t)cell.cell_id);
    }

    /* 6. Tentacle queue */
    if (p->ntacle) {
        uint32_t fid = r.anomaly ? p->next_frame_id++ : 0;
        l38_ntacle_write_path(p->ntacle, angular_addr, fid);
    }

    /* 7. Temporal ring */
    if (p->temporal)
        l38_temporal_write(p->temporal, angular_addr, value, cell.cell_id);

    /* 8. L17 lattice write */
    l17_write(p->lattice, angular_addr, value);

    /* 9. ExecWindow */
    if (p->ew)
        l38_ew_push(p->ew, r.anomaly ? L38_EW_OP_REPAIR : L38_EW_OP_WRITE,
                    (uint16_t)cell.cell_id, 0, angular_addr, value);
    return r;
}

#endif /* POGLS_38_WIRING_H */
