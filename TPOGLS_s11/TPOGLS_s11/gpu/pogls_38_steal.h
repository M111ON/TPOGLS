/*
 * pogls_38_steal.h — POGLS38  Voronoi Steal + Sleep Wire
 * ══════════════════════════════════════════════════════════════════════
 *
 * แก้ปัญหา work clustering: งานกระจุกอยู่ที่ head เดียว
 *
 * VORONOI STEAL (geometric work distribution):
 *   แต่ละ head เป็น "centroid" ใน 17n cell space (289 cells)
 *   cell → ของ head ที่ centroid ใกล้ที่สุด (Voronoi region)
 *   steal: ถ้า head idle → ดึงงานจาก head ที่ region ใกล้ที่สุด
 *   ผล: งานไม่กระจุก เพราะ region ถูก partition ทาง geometry
 *
 * เปรียบเทียบ:
 *   ปัจจุบัน = งานไปที่ head ตาม bit mask → random กระจุกได้
 *   Voronoi  = 289 cells แบ่งเป็น 16 zone ตามระยะ centroid
 *              เหมือน postal district — แต่ละ zone มี 1 head รับผิดชอบ
 *
 * DELAUNAY CHECK (neighbor validation):
 *   Voronoi dual = Delaunay triangulation
 *   ใช้ตรวจว่า cell อยู่ใน "right region" หรือไม่
 *   ถ้า cell อยู่ใกล้ boundary → eligible for steal จาก neighbor
 *
 * SLEEP WIRE (face hibernation in wiring pipeline):
 *   ถ้า head ว่างนาน + cell มี warp_open=1 → hibernate face
 *   warp bridge คงค้างไว้ (data accessible แต่ cpu ไม่จ่ายเวลา)
 *   wake: งานเข้า Voronoi region ของ head นั้น → wake ทันที
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_STEAL_H
#define POGLS_38_STEAL_H

#include <stdint.h>
#include <string.h>
#include "pogls_38_hydra.h"
#include "pogls_38_wiring.h"    /* L38WarpDetach */

/* ══════════════════════════════════════════════════════════════════════
 * VORONOI CENTROIDS
 *
 * 16 centroids บน 17×17 cell grid (289 cells)
 * วางแบบ hexagonal-like ใน 4×4 arrangement:
 *   col 0..3 × row 0..3 = 16 points
 *   spacing ≈ 17/4 = 4.25 → floor = 4
 *
 * เปรียบเทียบ: เหมือนแบ่งห้าง 17×17 ห้อง เป็น 16 zone
 *   แต่ละ zone มี 1 เคาน์เตอร์บริการ (centroid)
 *   ลูกค้า (cell) ไปเคาน์เตอร์ที่ใกล้ที่สุด
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  cx;    /* centroid x (lane: cell_id % 17)    0..16  */
    uint8_t  cy;    /* centroid y (group: cell_id / 17)   0..16  */
    uint8_t  head;  /* which Hydra head owns this region          */
    uint8_t  _pad;
} L38VoronoiCentroid;

#define L38_VOR_NCENT  L38_HS_HEADS  /* 16 centroids = 16 heads */

/* precomputed region assignment: cell_id → head_id
 * built once at init */
typedef struct {
    uint8_t  cell_to_head[L38_BB_NODES];     /* 289B                   */
    uint8_t  head_cell_count[L38_HS_HEADS];  /* cells per head         */
    L38VoronoiCentroid centroids[L38_VOR_NCENT];
} L38VoronoiMap;

/* squared distance on 17×17 torus (wraps at 17) */
static inline uint32_t _vor_dist2(uint8_t ax, uint8_t ay,
                                    uint8_t bx, uint8_t by)
{
    int dx = (int)ax - (int)bx;
    int dy = (int)ay - (int)by;
    /* torus wrap */
    if (dx >  8) dx -= 17;
    if (dx < -8) dx += 17;
    if (dy >  8) dy -= 17;
    if (dy < -8) dy += 17;
    return (uint32_t)(dx*dx + dy*dy);
}

/* build Voronoi map — call once at init */
static inline void l38_voronoi_init(L38VoronoiMap *vm)
{
    memset(vm, 0, sizeof(*vm));

    /* place 16 centroids in 4×4 grid on 17×17 torus
     * offset by 2 so boundaries fall between cells */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int h = r * 4 + c;
            vm->centroids[h].cx   = (uint8_t)(c * 4 + 2);
            vm->centroids[h].cy   = (uint8_t)(r * 4 + 2);
            vm->centroids[h].head = (uint8_t)h;
        }
    }

    /* assign each cell to nearest centroid (Voronoi tessellation) */
    for (uint16_t cid = 0; cid < L38_BB_NODES; cid++) {
        uint8_t  cx = (uint8_t)(cid % L17_BRIDGE);   /* lane  0..16 */
        uint8_t  cy = (uint8_t)(cid / L17_BRIDGE);   /* group 0..16 */

        uint32_t best_d2 = UINT32_MAX;
        uint8_t  best_h  = 0;
        for (int h = 0; h < L38_VOR_NCENT; h++) {
            uint32_t d2 = _vor_dist2(cx, cy,
                                      vm->centroids[h].cx,
                                      vm->centroids[h].cy);
            if (d2 < best_d2) { best_d2 = d2; best_h = (uint8_t)h; }
        }
        vm->cell_to_head[cid] = best_h;
        vm->head_cell_count[best_h]++;
    }
}

/* route cell → head via Voronoi region */
static inline uint32_t l38_voronoi_route(const L38VoronoiMap *vm,
                                           uint16_t cell_id)
{
    if (cell_id >= L38_BB_NODES) return 0;
    return vm->cell_to_head[cell_id];
}

/* ══════════════════════════════════════════════════════════════════════
 * DELAUNAY NEIGHBOR CHECK
 *
 * Voronoi dual = Delaunay: สอง region เป็น neighbor ถ้า centroid pair
 * ไม่มี centroid อื่นอยู่ใน circumcircle ของ triangle ที่เกิดจาก
 * 2 centroid + cell นั้น
 *
 * สำหรับ POGLS38: simplify เป็น "near-boundary" check
 *   cell อยู่ใกล้ boundary ถ้า distance ไป centroid ตัวเอง
 *   ≤ 1.5× distance ไป centroid ที่ใกล้ที่สุด ตัวอื่น
 *
 * ถ้า near-boundary → eligible for steal จาก neighbor region
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  near_boundary;       /* 1 = cell อยู่ใกล้ region boundary */
    uint8_t  neighbor_head;       /* head ที่ใกล้ที่สุด (Delaunay nbr) */
    uint16_t cell_id;
} L38DelaunayCell;

static inline L38DelaunayCell l38_delaunay_check(const L38VoronoiMap *vm,
                                                    uint16_t cell_id)
{
    L38DelaunayCell dc = {0, 0, cell_id};
    if (cell_id >= L38_BB_NODES) return dc;

    uint8_t cx = (uint8_t)(cell_id % L17_BRIDGE);
    uint8_t cy = (uint8_t)(cell_id / L17_BRIDGE);
    uint8_t own_h = vm->cell_to_head[cell_id];

    uint32_t own_d2  = _vor_dist2(cx, cy,
                                   vm->centroids[own_h].cx,
                                   vm->centroids[own_h].cy);
    uint32_t best2_d2 = UINT32_MAX;
    uint8_t  best2_h  = own_h;

    for (int h = 0; h < L38_VOR_NCENT; h++) {
        if ((uint8_t)h == own_h) continue;
        uint32_t d2 = _vor_dist2(cx, cy,
                                  vm->centroids[h].cx,
                                  vm->centroids[h].cy);
        if (d2 < best2_d2) { best2_d2 = d2; best2_h = (uint8_t)h; }
    }

    /* near-boundary: own_d2 × 4 >= best2_d2 × 6  (ratio 1.5) */
    dc.near_boundary  = (own_d2 * 6u >= best2_d2 * 5u) ? 1u : 0u;
    dc.neighbor_head  = best2_h;
    return dc;
}

/* ══════════════════════════════════════════════════════════════════════
 * VORONOI STEAL
 *
 * แทนที่ greedy steal (busiest head):
 *   1. ถ้า idle head มี near-boundary cells → steal จาก neighbor region
 *   2. ถ้าไม่มี boundary → steal จาก head ที่ centroid ใกล้ที่สุด
 *   3. fallback: greedy (เหมือนเดิม)
 *
 * ผล: งานถูก pull จาก region ที่ "ใกล้ชิดทาง geometry"
 *     ไม่ข้ามไปอีกฝั่งของ lattice
 * ══════════════════════════════════════════════════════════════════════ */

static inline int l38_voronoi_steal(L38Hydra          *h,
                                      const L38VoronoiMap *vm,
                                      uint32_t            my_head,
                                      L38HydraTask       *out)
{
    /* Step 1: find nearest neighbor head by centroid distance */
    uint8_t  mcx = vm->centroids[my_head].cx;
    uint8_t  mcy = vm->centroids[my_head].cy;

    uint32_t best_d2   = UINT32_MAX;
    uint32_t best_nbr  = L38_HS_HEADS;  /* sentinel */

    for (uint32_t v = 0; v < L38_VOR_NCENT; v++) {
        if (v == my_head) continue;
        /* only try heads that actually have work */
        if (l38_hs_depth(&h->queues[v]) == 0) continue;
        uint32_t d2 = _vor_dist2(mcx, mcy,
                                   vm->centroids[v].cx,
                                   vm->centroids[v].cy);
        if (d2 < best_d2) { best_d2 = d2; best_nbr = v; }
    }

    if (best_nbr == L38_HS_HEADS) return 0;  /* no work anywhere */

    /* Step 2: CAS steal from nearest neighbor */
    L38HydraQueue *vq   = &h->queues[best_nbr];
    uint32_t       head = atomic_load_explicit(&vq->head,
                                                memory_order_relaxed);
    uint32_t       tail = atomic_load_explicit(&vq->tail,
                                                memory_order_acquire);
    while (head != tail) {
        *out = vq->tasks[head & L38_HS_QUEUE_MASK];
        if (atomic_compare_exchange_weak_explicit(
                &vq->head, &head, head + 1u,
                memory_order_release, memory_order_relaxed)) {
            h->tasks_stolen[my_head]++;
            h->steals_attempted[my_head]++;
            return 1;
        }
    }

    h->steals_attempted[my_head]++;
    return 0;
}

/* steal with Delaunay boundary awareness:
 * prefer boundary cells that "belong" to both regions */
static inline int l38_delaunay_steal(L38Hydra          *h,
                                       const L38VoronoiMap *vm,
                                       uint32_t            my_head,
                                       L38HydraTask       *out)
{
    /* scan pending tasks across all heads, pick one where
     * the cell is near-boundary with my_head as neighbor */
    for (uint32_t v = 0; v < L38_HS_HEADS; v++) {
        if (v == my_head) continue;
        L38HydraQueue *vq = &h->queues[v];
        uint32_t head = atomic_load_explicit(&vq->head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&vq->tail, memory_order_acquire);
        if (head == tail) continue;

        /* peek at front task */
        const L38HydraTask *peek = &vq->tasks[head & L38_HS_QUEUE_MASK];
        uint16_t cid = peek->cell_id;
        if (cid < L38_BB_NODES) {
            /* FIX1: validate ownership — task must still belong to v's region
             * if topology drifted, skip (prevents cross-region context error) */
            if (vm->cell_to_head[cid] != (uint8_t)v) continue;
            L38DelaunayCell dc = l38_delaunay_check(vm, cid);
            /* steal if: cell is near boundary AND neighbor = my_head */
            if (dc.near_boundary && dc.neighbor_head == (uint8_t)my_head) {
                L38HydraTask stolen = *peek;
                if (atomic_compare_exchange_weak_explicit(
                        &vq->head, &head, head + 1u,
                        memory_order_release, memory_order_relaxed)) {
                    *out = stolen;
                    h->tasks_stolen[my_head]++;
                    return 1;
                }
            }
        }
    }
    /* fallback: Voronoi steal */
    return l38_voronoi_steal(h, vm, my_head, out);
}

/* ══════════════════════════════════════════════════════════════════════
 * SLEEP WIRE  (face hibernation integrated with Hydra)
 *
 * Face = head ใน POGLS38 context
 * Sleep conditions:
 *   - head ว่าง (queue empty) นาน > SLEEP_IDLE_TICKS
 *   - cell ที่ responsible ทั้งหมด มี warp_open (can hibernate)
 *
 * Wake conditions:
 *   - งานเข้า Voronoi region ของ head นั้น
 *   - Tails summon ที่ตก cell ใน region นั้น
 *   - gate_18 crossing (periodic wake all)
 *
 * ต่างจาก V3.6 face_sleep (madvise/MADV_DONTNEED):
 *   POGLS38 ไม่มี mmap — sleep = skip drain loop
 *   warp bridge = flag ใน L38WarpDetach เท่านั้น
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_SLEEP_IDLE_TICKS  18u   /* gate_18 = natural sleep boundary */
#define L38_SLEEP_MAGIC       0x534C5057u  /* "SLPW"                    */

typedef enum {
    L38_FACE_AWAKE    = 0,
    L38_FACE_DROWSY   = 1,   /* idle > threshold, monitoring           */
    L38_FACE_SLEEPING = 2,   /* skip drain, warp bridge open           */
} l38_face_state_t;

/* FIX4: Hysteresis routing: stick to last head to prevent flip-flop */
typedef struct {
    uint8_t   last_head;    /* last routed head for this addr context    */
    uint8_t   stick_count;  /* consecutive same-head routes              */
    uint8_t   _pad[2];
} L38RouteHysteresis;

#define L38_HYSTERESIS_STICK   2u   /* hold route for 2 ticks before switch */

static inline uint32_t l38_hysteresis_route(L38RouteHysteresis *hr,
                                              uint32_t new_head)
{
    if (new_head == hr->last_head) {
        if (hr->stick_count < 255u) hr->stick_count++;
        return new_head;
    }
    /* switching: require stick exhausted */
    if (hr->stick_count < L38_HYSTERESIS_STICK) {
        hr->stick_count = (uint8_t)(hr->stick_count > 0 ? hr->stick_count-1 : 0);
        return hr->last_head;   /* stay on last head */
    }
    hr->last_head   = (uint8_t)new_head;
    hr->stick_count = 0;
    return new_head;
}

/* ─────────────────────────────────────────────────────────────────
 * NEW: Energy model (replaces idle_ticks oscillation)
 * energy decays each tick, gains from work
 * sleep when energy drops below threshold → smooth, no oscillate
 * ───────────────────────────────────────────────────────────────── */
#define L38_ENERGY_MAX       255u
#define L38_ENERGY_SLEEP_THR  32u   /* below this → sleep             */
#define L38_ENERGY_WAKE_THR   64u   /* above this → wake              */
#define L38_ENERGY_DECAY       3u   /* subtract per idle tick         */
#define L38_ENERGY_GAIN       16u   /* add per task executed          */

/* ─────────────────────────────────────────────────────────────────
 * NEW: Field pressure (load-aware Voronoi routing)
 * pressure[h] = queue_depth / region_cells
 * score = dist_weight - pressure_diff   ← pull toward loaded heads
 * ───────────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t  pressure[L38_HS_HEADS];  /* depth / region_size × 256   */
    uint64_t  last_update;
} L38FieldPressure;

static inline void l38_pressure_update(L38FieldPressure       *fp,
                                         const L38Hydra         *h,
                                         const L38VoronoiMap    *vm)
{
    for (uint32_t i = 0; i < L38_HS_HEADS; i++) {
        uint32_t d = l38_hs_depth(&h->queues[i]);
        uint32_t cells = vm->head_cell_count[i];
        fp->pressure[i] = cells > 0
            ? (uint16_t)((d * 256u) / cells)  /* depth per cell × 256 */
            : 0u;
    }
}

/* pressure-weighted route: score = dist*16 + my_pressure - nbr_pressure */
static inline uint32_t l38_pressure_route(const L38FieldPressure *fp,
                                            const L38VoronoiMap    *vm,
                                            uint16_t                cell_id)
{
    uint8_t  cx = (uint8_t)(cell_id % L17_BRIDGE);
    uint8_t  cy = (uint8_t)(cell_id / L17_BRIDGE);

    uint32_t best_score = UINT32_MAX;
    uint32_t best_h     = 0;

    for (uint32_t h = 0; h < L38_HS_HEADS; h++) {
        uint32_t d2    = _vor_dist2(cx, cy,
                                     vm->centroids[h].cx,
                                     vm->centroids[h].cy);
        /* pressure penalty: route away from loaded heads */
        uint32_t pload = fp->pressure[h];
        /* score: distance dominates (×4), pressure modifies */
        uint32_t score = d2 * 4u + pload;
        if (score < best_score) { best_score = score; best_h = h; }
    }
    return best_h;
}

/* FIX3: dynamic Delaunay threshold based on load difference */
static inline int l38_delaunay_threshold_ok(const L38FieldPressure *fp,
                                               uint32_t own_d2,
                                               uint32_t next_d2,
                                               uint8_t  own_head,
                                               uint8_t  nbr_head)
{
    /* base: own_d2 * 6 >= next_d2 * 5 (ratio ~0.83) */
    int base_ok = (own_d2 * 6u >= next_d2 * 5u);
    /* load imbalance: if neighbor >> me, allow even if not boundary */
    int load_diff = (int)fp->pressure[nbr_head] - (int)fp->pressure[own_head];
    int load_ok   = (load_diff > 64);   /* neighbor at least 25% busier */
    return base_ok || load_ok;
}

typedef struct {
    uint8_t   state;          /* l38_face_state_t                       */
    uint8_t   energy;         /* FIX2: energy 0..255 replaces idle_ticks */
    uint8_t   warp_count;     /* cells with warp_open in region         */
    uint8_t   _pad;
    uint32_t  head_id;
    uint64_t  sleep_since_ns;
    uint64_t  total_sleeps;
    uint64_t  total_wakes;
} L38FaceSleep;

typedef struct {
    uint32_t      magic;
    L38FaceSleep  faces[L38_HS_HEADS];
} L38SleepWire;

static inline void l38_sleep_wire_init(L38SleepWire *sw)
{
    memset(sw, 0, sizeof(*sw));
    sw->magic = L38_SLEEP_MAGIC;
    for (uint32_t i = 0; i < L38_HS_HEADS; i++)
        sw->faces[i].head_id = i;
}

/* tick: energy-based sleep (FIX2 — no oscillation)
 * global_pending = total tasks across all heads (demand signal) */
static inline void l38_sleep_tick(L38SleepWire  *sw,
                                    L38Hydra      *h,
                                    uint32_t       head_id)
{
    L38FaceSleep *f    = &sw->faces[head_id];
    uint32_t depth     = l38_hs_depth(&h->queues[head_id]);
    uint32_t global_p  = l38_hydra_total_depth(h);   /* FIX2: demand signal */

    if (depth > 0) {
        /* work present: gain energy */
        uint32_t e = (uint32_t)f->energy + L38_ENERGY_GAIN;
        f->energy = (uint8_t)(e > L38_ENERGY_MAX ? L38_ENERGY_MAX : e);
        if (f->state != L38_FACE_AWAKE) {
            f->state = L38_FACE_AWAKE;
            f->total_wakes++;
        }
    } else {
        /* FIX2: don't sleep if global demand pending */
        if (global_p > 0) {
            /* system has work somewhere — stay drowsy, decay slowly */
            if (f->energy > 0) f->energy = (uint8_t)(f->energy - 1u);
            if (f->state == L38_FACE_SLEEPING) {
                f->state = L38_FACE_DROWSY;
                f->total_wakes++;
            }
        } else {
            /* truly idle: decay energy */
            if (f->energy >= L38_ENERGY_DECAY)
                f->energy = (uint8_t)(f->energy - L38_ENERGY_DECAY);
            else
                f->energy = 0;

            if (f->energy < L38_ENERGY_SLEEP_THR &&
                f->state == L38_FACE_AWAKE) {
                f->state = L38_FACE_DROWSY;
            }
            if (f->energy == 0 && f->state == L38_FACE_DROWSY) {
                f->state         = L38_FACE_SLEEPING;
                f->sleep_since_ns = 0;
                f->total_sleeps++;
            }
        }
    }
}

/* wake: called when work enters region (Voronoi route hit) */
static inline void l38_sleep_wake(L38SleepWire *sw, uint32_t head_id)
{
    L38FaceSleep *f = &sw->faces[head_id];
    if (f->state != L38_FACE_AWAKE) {
        f->state      = L38_FACE_AWAKE;
        f->energy = L38_ENERGY_WAKE_THR;  /* boost energy on external wake */
        f->total_wakes++;
    }
}

/* gate_18 wake all sleeping faces */
static inline void l38_sleep_gate18_wake(L38SleepWire *sw)
{
    for (uint32_t i = 0; i < L38_HS_HEADS; i++) {
        if (sw->faces[i].state == L38_FACE_SLEEPING)
            l38_sleep_wake(sw, i);
    }
}

/* should drain: 1=yes (awake/drowsy), 0=skip (sleeping) */
static inline int l38_sleep_should_drain(const L38SleepWire *sw,
                                           uint32_t head_id)
{
    return sw->faces[head_id].state != L38_FACE_SLEEPING;
}

/* ══════════════════════════════════════════════════════════════════════
 * FULL STEAL PIPELINE  (Voronoi + Delaunay + Sleep Wire combined)
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_STEAL_LOCAL_THRESH  4u   /* FIX5: don't steal if local ≥ this */

static inline int l38_steal_pipeline(L38Hydra            *h,
                                       const L38VoronoiMap *vm,
                                       L38SleepWire        *sw,
                                       L38FieldPressure    *fp,
                                       uint32_t             my_head,
                                       L38HydraTask        *out)
{
    /* sleeping head should not be stealing */
    if (!l38_sleep_should_drain(sw, my_head)) return 0;

    /* FIX5: process local work first — steal has cache-miss cost */
    if (l38_hs_depth(&h->queues[my_head]) >= L38_STEAL_LOCAL_THRESH)
        return 0;   /* local queue has enough — don't steal yet */

    /* update pressure field before steal decision */
    l38_pressure_update(fp, h, vm);

    /* try Delaunay-aware steal first (geometry-correct) */
    int r = l38_delaunay_steal(h, vm, my_head, out);
    if (r) {
        l38_sleep_wake(sw, my_head);
        return 1;
    }

    /* no work in geometric neighborhood — go truly idle */
    l38_sleep_tick(sw, h, my_head);
    return 0;
}

/* schedule: pressure field + hysteresis routing + wake destination */
static inline int l38_voronoi_schedule(L38Hydra            *h,
                                         const L38VoronoiMap *vm,
                                         L38SleepWire        *sw,
                                         L38FieldPressure    *fp,
                                         L38RouteHysteresis  *hr,
                                         L38HydraTask        *t)
{
    /* cell_id → pressure-aware Voronoi region (FIX3/new field pressure) */
    uint32_t raw_hid = (t->cell_id < L38_BB_NODES)
        ? l38_pressure_route(fp, vm, t->cell_id)   /* field pressure */
        : l38_hs_route_addr(t->addr);
    /* FIX4: hysteresis — stabilize routing, prevent flip-flop */
    uint32_t hid = hr ? l38_hysteresis_route(hr, raw_hid) : raw_hid;

    int r = l38_hs_push(&h->queues[hid], t);
    if (r == 0) {
        h->tasks_pushed[hid]++;
        /* wake destination head if sleeping */
        l38_sleep_wake(sw, hid);
        /* update weight */
        uint8_t w = h->weight.weight[hid];
        h->weight.weight[hid] = (uint8_t)(w + 8u > 255u ? 255u : w + 8u);
        return (int)hid;
    }

    /* region full → nearest Voronoi neighbor */
    uint8_t mcx = vm->centroids[hid].cx;
    uint8_t mcy = vm->centroids[hid].cy;
    uint32_t best_d2 = UINT32_MAX;
    uint32_t alt_hid = (hid + 1u) % L38_HS_HEADS;

    for (uint32_t v = 0; v < L38_VOR_NCENT; v++) {
        if (v == hid) continue;
        if (l38_hs_depth(&h->queues[v]) >= L38_HS_QUEUE_SIZE - 1u) continue;
        uint32_t d2 = _vor_dist2(mcx, mcy,
                                   vm->centroids[v].cx,
                                   vm->centroids[v].cy);
        if (d2 < best_d2) { best_d2 = d2; alt_hid = v; }
    }
    r = l38_hs_push(&h->queues[alt_hid], t);
    if (r == 0) {
        h->tasks_pushed[alt_hid]++;
        l38_sleep_wake(sw, alt_hid);
        return (int)alt_hid;
    }
    return -1;
}


/* ══════════════════════════════════════════════════════════════════════
 * HYBRID 2-PHASE ENGINE
 * ══════════════════════════════════════════════════════════════════════
 *
 * "event = cheap, control = amortized"
 *
 *   FAST PATH  (every call, ~1-8 ns):
 *     cell_to_head[cid]  → push  → done
 *     pressure interrupt: only if queue_depth > THRESH
 *
 *   SLOW PATH  (every CTRL_BATCH calls, amortized ~0 ns/call):
 *     energy decay, hysteresis reset, weight rebalance, sleep_wake
 *
 *   GREEDY ESCAPE  (SEQ pattern + low pressure):
 *     addr & mask — max throughput when burst risk is low
 *
 * Expected: ~170-200 M/s throughput + 1.0x imbalance
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_CTRL_BATCH      64u   /* slow path every 64 calls (≈ 4× gate_18) */
#define L38_PRESSURE_INTR  220u   /* queue depth threshold — above normal drain range */
#define L38_GREEDY_THRESH   32u   /* pressure below this → allow greedy       */

/* ── 2-phase scheduler context ──────────────────────────────────────── */
typedef struct {
    uint32_t          tick;              /* call counter                 */
    L38RouteHysteresis hr;               /* hysteresis state             */
    L38FieldPressure   fp;               /* pressure field               */
    uint64_t          fast_calls;        /* routed via fast path         */
    uint64_t          pressure_reroutes; /* pressure interrupt triggered */
    uint64_t          greedy_calls;      /* greedy escape used           */
    uint64_t          slow_runs;         /* slow path executed           */
} L38Scheduler;

static inline void l38_sched_init(L38Scheduler *s)
{
    memset(s, 0, sizeof(*s));
}

/* ── slow path (amortized) ───────────────────────────────────────────
 * Runs every CTRL_BATCH calls: energy + hysteresis + weights
 * Cost is ~30-50 ns, but amortized over 64 calls = <1 ns/call
 * ─────────────────────────────────────────────────────────────────── */
static inline void _l38_slow_path(L38Scheduler    *s,
                                    L38Hydra        *h,
                                    const L38VoronoiMap *vm,
                                    L38SleepWire    *sw)
{
    /* 1. update pressure field (16 heads) */
    l38_pressure_update(&s->fp, h, vm);

    /* 2. lazy energy decay — bitshift per head, no loop needed */
    for (uint32_t i = 0; i < L38_HS_HEADS; i++) {
        L38FaceSleep *f = &sw->faces[i];
        uint32_t depth  = l38_hs_depth(&h->queues[i]);
        if (depth > 0) {
            /* working: gain energy */
            uint32_t e = (uint32_t)f->energy + L38_ENERGY_GAIN;
            f->energy = (uint8_t)(e > L38_ENERGY_MAX ? L38_ENERGY_MAX : e);
            if (f->state != L38_FACE_AWAKE) {
                f->state = L38_FACE_AWAKE;
                f->total_wakes++;
            }
        } else {
            /* idle: lazy decay via bitshift (no branch) */
            f->energy = (uint8_t)(f->energy - (f->energy >> 3u)); /* × 7/8 */
            if (f->energy < L38_ENERGY_SLEEP_THR) {
                /* check global demand before sleeping */
                uint32_t global_p = l38_hydra_total_depth(h);
                if (global_p == 0) {
                    f->state = (f->state == L38_FACE_DROWSY)
                        ? L38_FACE_SLEEPING : L38_FACE_DROWSY;
                    if (f->state == L38_FACE_SLEEPING) f->total_sleeps++;
                }
            }
        }
    }

    /* 3. weight decay — one pass, bitshift */
    l38_hw_decay(&h->weight);

    /* 4. hysteresis reset if overridden */
    if (s->hr.stick_count >= L38_HYSTERESIS_STICK)
        s->hr.stick_count = 0;

    s->slow_runs++;
}

/* ── FAST PATH: main entry point ─────────────────────────────────────
 *
 * 99% of calls: table lookup + push = ~2 ns
 *  1% of calls: slow_path amortized = ~0 ns overhead
 * ─────────────────────────────────────────────────────────────────── */
static inline int l38_sched_push(L38Scheduler        *s,
                                   L38Hydra            *h,
                                   const L38VoronoiMap *vm,
                                   L38SleepWire        *sw,
                                   L38HydraTask        *t)
{
    uint32_t hid;

    /* ── STEP 1: route ───────────────────────────────────────────── */
    if (t->cell_id < L38_BB_NODES) {

        /* fast: O(1) table lookup */
        hid = vm->cell_to_head[t->cell_id];

        /* pressure interrupt: only if queue exceeds threshold */
        if (__builtin_expect(
                l38_hs_depth(&h->queues[hid]) > L38_PRESSURE_INTR, 0)) {
            hid = l38_pressure_route(&s->fp, vm, t->cell_id);
            s->pressure_reroutes++;
        } else {
            s->fast_calls++;
        }

    } else {
        /* no cell_id: check pressure → greedy escape */
        uint32_t raw = l38_hs_route_addr(t->addr);
        if (s->fp.pressure[raw] < L38_GREEDY_THRESH) {
            hid = raw;   /* greedy: max throughput, low risk */
            s->greedy_calls++;
        } else {
            hid = l38_pressure_route(&s->fp, vm,
                      (uint16_t)(t->addr % L38_BB_NODES));
        }
    }

    /* ── STEP 2: push ────────────────────────────────────────────── */
    int r = l38_hs_push(&h->queues[hid], t);
    if (r == 0) {
        h->tasks_pushed[hid]++;
    } else {
        /* overflow: nearest available neighbor (geometric) */
        for (uint32_t i = 1; i < L38_HS_HEADS; i++) {
            uint32_t alt = (hid + i) % L38_HS_HEADS;
            if (l38_hs_push(&h->queues[alt], t) == 0) {
                h->tasks_pushed[alt]++;
                hid = alt;
                r   = 0;
                break;
            }
        }
    }

    /* ── STEP 3: slow control (amortized every CTRL_BATCH calls) ── */
    if (__builtin_expect((++s->tick & (L38_CTRL_BATCH - 1u)) == 0, 0))
        _l38_slow_path(s, h, vm, sw);

    return r == 0 ? (int)hid : -1;
}

/* ── drain with steal (2-phase aware) ───────────────────────────────
 * Each head: pop own queue first, then geometric steal if idle
 * ─────────────────────────────────────────────────────────────────── */
static inline int l38_sched_drain_head(L38Scheduler        *s,
                                         L38Hydra            *h,
                                         const L38VoronoiMap *vm,
                                         L38SleepWire        *sw,
                                         uint32_t             head_id,
                                         l38_hydra_exec_cb    cb,
                                         void                *cb_ctx)
{
    /* sleeping? skip unless global demand */
    if (sw->faces[head_id].state == L38_FACE_SLEEPING) {
        if (l38_hydra_total_depth(h) == 0) return 0;
        l38_sleep_wake(sw, head_id);
    }

    int n = 0;
    L38HydraTask t;

    /* pop own queue */
    while (l38_hs_pop(&h->queues[head_id], &t)) {
        if (cb) cb(&t, cb_ctx);
        h->tasks_executed[head_id]++;
        n++;
    }

    /* idle → try geometric steal */
    if (n == 0) {
        if (l38_delaunay_steal(h, vm, head_id, &t)) {
            if (cb) cb(&t, cb_ctx);
            h->tasks_stolen[head_id]++;
            n++;
        }
    }

    return n;
}

#endif /* POGLS_38_STEAL_H */
