/*
 * pogls_17n_lattice.h — POGLS38  3-World 17n Lattice Foundation
 * ══════════════════════════════════════════════════════════════════════
 *
 * Core Discovery (Po Panthakhan, 2026-03):
 *
 *   8 + 9 : 17 : 8 + 9
 *   8 = 2³  World A (binary, CPU)
 *   9 = 3²  World B (ternary, GPU/adaptive)
 *   17 = prime bridge (no aliasing with 8 or 9)
 *
 *   World A:  8 groups × 17 = 136 lanes
 *   World B:  9 groups × 17 = 153 lanes
 *   Total:    289 = 17²  (full lattice)
 *
 *   dynamics  = 144 = Fib(12)   (active interaction space)
 *   buffer    = 145 = 289-144   (transition / ghost / overflow)
 *
 * gate_18 = common clock สำหรับทั้งสองโลก:
 *   144 ÷ 8 = 18   (World A → gate)
 *   162 ÷ 9 = 18   (World B → gate)
 *
 * Existing system — ไม่เปลี่ยน:
 *   gate_18  ✓    162 ✓    306=17×18 ✓    PHI_UP/DOWN ✓
 *   RUBIK_LANES=54 ยังใช้ได้ใน V4 mode (switch via LATTICE_MODE)
 *
 * CPU fallback: ทุก function ทำงานบน CPU ล้วน
 * GPU path:     เพิ่มทีหลัง (World B batch path, T4 Colab)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_17N_LATTICE_H
#define POGLS_17N_LATTICE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── PHI constants — single source (alias จาก pogls_fibo_addr.h) ─── */
#ifndef PHI_SCALE
#  define PHI_SCALE   (1u << 20)    /* 2²⁰ = 1,048,576               */
#  define PHI_UP      1696631u      /* floor(φ  × 2²⁰) World A        */
#  define PHI_DOWN    648055u       /* floor(φ⁻¹ × 2²⁰) World B       */
#endif

/* ══════════════════════════════════════════════════════════════════════
 * LATTICE CONSTANTS (FROZEN)
 * ══════════════════════════════════════════════════════════════════════ */

#define L17_BRIDGE          17u     /* prime — no aliasing with 8 or 9  */
#define L17_GROUPS_A        8u      /* World A groups (2³)              */
#define L17_GROUPS_B        9u      /* World B groups (3²)              */

#define L17_LANES_A         136u    /* 8  × 17 = World A                */
#define L17_LANES_B         153u    /* 9  × 17 = World B                */
#define L17_LANES_TOTAL     289u    /* 17²      = full lattice           */

#define L17_DYNAMICS        144u    /* Fib(12) = active interaction      */
#define L17_BUFFER          145u    /* 289-144 = transition/ghost        */
#define L17_GATE            18u     /* common clock: 144÷8 = 162÷9 = 18 */

/* Verify invariants at compile time */
typedef char _l17_check_a[(L17_GROUPS_A * L17_BRIDGE == L17_LANES_A)     ? 1 : -1];
typedef char _l17_check_b[(L17_GROUPS_B * L17_BRIDGE == L17_LANES_B)     ? 1 : -1];
typedef char _l17_check_t[(L17_LANES_A   + L17_LANES_B == L17_LANES_TOTAL)? 1 : -1];
typedef char _l17_check_d[(L17_DYNAMICS  + L17_BUFFER  == L17_LANES_TOTAL)? 1 : -1];
typedef char _l17_check_g[(L17_DYNAMICS  % L17_GROUPS_A == 0)             ? 1 : -1];

/* ── Lattice mode ── */
typedef enum {
    LATTICE_MODE_V4   = 0,   /* 54 lanes, compat กับ POGLS4 pipeline   */
    LATTICE_MODE_17N  = 1,   /* 289 cells, 3-World full lattice         */
} lattice_mode_t;

/* ══════════════════════════════════════════════════════════════════════
 * WORLD ENUM (compatible กับ V3.6 world_t)
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    L17_WORLD_A = 0,   /* binary  2ⁿ — CPU fast path  (136 lanes)      */
    L17_WORLD_B = 1,   /* ternary 3ⁿ — adaptive       (153 lanes)      */
} l17_world_t;

/* ══════════════════════════════════════════════════════════════════════
 * CELL ID  — core mapping function
 *
 * แนวคิด:
 *   interaction_id ∈ [0..143]  (144 dynamics)
 *   phase          ∈ [0..16]   (17 phases)
 *   cell_id        ∈ [0..288]  (289 = 17² cells)
 *
 *   cell_id = (interaction_id % 17) + (phase × 17)
 *
 * lane_id ∈ lattice:
 *   lane   = cell_id % 17                    → position ใน group
 *   group  = cell_id / 17                    → group index 0..16
 *   world  = group < 8 ? WORLD_A : WORLD_B   → world boundary
 *
 * เปรียบเทียบ: เหมือน ZIP code
 *   lane  = เลขถนน (position ใน group)
 *   group = รหัสเขต (0-7 = World A, 8-16 = World B)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t    cell_id;       /* 0..288  (17²)                          */
    uint8_t     lane;          /* 0..16   (position ใน group, % 17)      */
    uint8_t     group;         /* 0..16   (group index, / 17)            */
    l17_world_t world;         /* WORLD_A (group<8) / WORLD_B (group>=8) */
    uint8_t     phase;         /* 0..16   (input phase)                  */
    uint8_t     is_buffer;     /* 1 = buffer zone (dynamics 144+)        */
    uint8_t     _pad[2];
} L17Cell;

/* cell_id_of: interaction_id × phase → L17Cell
 *   interaction_id : content hash หรือ sequence id % 144
 *   phase          : gate counter % 17                                  */
static inline L17Cell l17_cell_of(uint32_t interaction_id, uint8_t phase)
{
    L17Cell c;
    uint32_t iid  = interaction_id % L17_DYNAMICS;   /* 0..143           */
    uint8_t  ph   = phase % L17_BRIDGE;              /* 0..16            */

    c.cell_id    = (uint16_t)((iid % L17_BRIDGE) + (uint32_t)(ph * L17_BRIDGE));
    c.lane       = (uint8_t)(c.cell_id % L17_BRIDGE);
    c.group      = (uint8_t)(c.cell_id / L17_BRIDGE);
    c.world      = (c.group < L17_GROUPS_A) ? L17_WORLD_A : L17_WORLD_B;
    c.phase      = ph;
    c.is_buffer  = (interaction_id >= L17_DYNAMICS) ? 1u : 0u;
    c._pad[0]    = 0; c._pad[1] = 0;
    return c;
}

/* ── direct lane mapping (addr path) ────────────────────────────────
 * ใช้กับ angular_addr แทน interaction_id
 * PHI scatter ก่อน แล้ว map เข้า lattice                             */
static inline L17Cell l17_cell_from_addr(uint64_t angular_addr, uint8_t phase)
{
    /* PHI scatter: World A (PHI_UP) กระจาย address ใน 289 cells */
    uint32_t scattered = (uint32_t)(((uint64_t)(angular_addr & 0xFFFFFu)
                                     * PHI_UP) % L17_LANES_TOTAL);
    return l17_cell_of(scattered, phase);
}

/* ══════════════════════════════════════════════════════════════════════
 * LANE CONTEXT — per-lane state ขนาดเล็กสำหรับ CPU path
 *
 * ออกแบบให้ fit ใน cache: L17LaneCtx array[289] = 289 × 16B = 4.6KB
 * เหมาะกับ L1 cache (32KB ทั่วไป)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t    seq;           /* write sequence (monotonic)             */
    uint16_t    hits;          /* access count (decay gate)              */
    uint8_t     heat;          /* activity heat 0..15                    */
    uint8_t     flags;         /* L17_FLAG_* bitmask                     */
    uint32_t    last_addr;     /* last angular_addr written              */
    uint32_t    _pad;
} L17LaneCtx;                  /* 16B = 1/4 cache line                   */

#define L17_FLAG_ACTIVE     0x01u   /* lane has been written              */
#define L17_FLAG_HOT        0x02u   /* heat >= 8 (threshold)              */
#define L17_FLAG_GHOST      0x04u   /* routing pushed to ghost            */
#define L17_FLAG_BUFFER     0x08u   /* buffer zone (group 8-16 overflow)  */

/* ══════════════════════════════════════════════════════════════════════
 * LATTICE CONTEXT — full 289-lane state (CPU fallback)
 * ══════════════════════════════════════════════════════════════════════ */

#define L17_LATTICE_MAGIC   0x4C543137u   /* "LT17" */

typedef struct {
    uint32_t      magic;
    lattice_mode_t mode;
    uint8_t       gate_count;    /* 0..17, resets at gate_18 crossing    */
    uint8_t       _pad[3];

    /* stats */
    uint64_t      total_writes;
    uint64_t      world_a_writes;
    uint64_t      world_b_writes;
    uint64_t      buffer_writes;
    uint64_t      gate_crossings;   /* gate_18 events (phase 17→0)       */

    /* lane states */
    L17LaneCtx    lanes[L17_LANES_TOTAL];   /* 289 × 16B = 4.6KB         */
} L17Lattice;                               /* ~4.7KB total               */

static inline void l17_lattice_init(L17Lattice *lat, lattice_mode_t mode)
{
    if (!lat) return;
    memset(lat, 0, sizeof(*lat));
    lat->magic       = L17_LATTICE_MAGIC;
    lat->mode        = mode;
    lat->gate_count  = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * WRITE PATH — CPU fallback (no GPU dependency)
 *
 * l17_write():
 *   1. scatter addr → cell (PHI)
 *   2. update lane context (heat, seq, flags)
 *   3. gate_18 crossing check
 *   4. return L17Cell (caller routes to delta lane / ghost)
 *
 * GPU path: World B lanes (153) → batch submit → เพิ่มทีหลัง
 *           ตอนนี้ทั้ง World A และ B ใช้ CPU path เหมือนกัน
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    L17Cell     cell;          /* resolved cell (world, lane, group)     */
    int         gate_event;    /* 1 = gate_18 crossing occurred          */
    uint8_t     heat;          /* lane heat after this write             */
} L17WriteResult;

static inline L17WriteResult l17_write(L17Lattice *lat,
                                        uint64_t    angular_addr,
                                        uint64_t    value)
{
    L17WriteResult r;
    (void)value;   /* value routing handled by caller (delta lane)      */

    /* ── cell mapping ───────────────────────────────────────────────── */
    r.cell = l17_cell_from_addr(angular_addr, lat->gate_count);

    /* ── lane update ────────────────────────────────────────────────── */
    L17LaneCtx *lc = &lat->lanes[r.cell.cell_id];
    lc->seq++;
    lc->last_addr = (uint32_t)(angular_addr & 0xFFFFFFFFu);
    lc->flags    |= L17_FLAG_ACTIVE;

    /* heat: increment capped at 15, 7/8 decay every 8 writes */
    if (lc->heat < 15u) lc->heat++;
    if ((lc->seq & 7u) == 0u) lc->heat = (uint8_t)(lc->heat * 7u / 8u);
    if (lc->heat >= 8u) lc->flags |= L17_FLAG_HOT;

    /* buffer zone flag */
    if (r.cell.is_buffer) lc->flags |= L17_FLAG_BUFFER;

    r.heat = lc->heat;

    /* ── stats ──────────────────────────────────────────────────────── */
    lat->total_writes++;
    if (r.cell.world == L17_WORLD_A)      lat->world_a_writes++;
    else                                   lat->world_b_writes++;
    if (r.cell.is_buffer)                  lat->buffer_writes++;

    /* ── gate_18 crossing ───────────────────────────────────────────── */
    lat->gate_count++;
    r.gate_event = 0;
    if (lat->gate_count >= L17_GATE) {
        lat->gate_count = 0;
        lat->gate_crossings++;
        r.gate_event = 1;
    }

    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * QUERY HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

/* lane count per world */
static inline uint32_t l17_lane_count(l17_world_t w) {
    return (w == L17_WORLD_A) ? L17_LANES_A : L17_LANES_B;
}

/* is cell in dynamics space (0..143) or buffer (144..288)? */
static inline int l17_is_dynamics(uint16_t cell_id) {
    return cell_id < L17_DYNAMICS;
}

/* lane heat lookup */
static inline uint8_t l17_heat(const L17Lattice *lat, uint16_t cell_id) {
    if (!lat || cell_id >= L17_LANES_TOTAL) return 0;
    return lat->lanes[cell_id].heat;
}

/* world distribution: % of writes in World A vs B */
static inline void l17_stats(const L17Lattice *lat,
                              double *pct_a, double *pct_b)
{
    if (!lat || lat->total_writes == 0) {
        if (pct_a) *pct_a = 0.0;
        if (pct_b) *pct_b = 0.0;
        return;
    }
    if (pct_a) *pct_a = (double)lat->world_a_writes / (double)lat->total_writes;
    if (pct_b) *pct_b = (double)lat->world_b_writes / (double)lat->total_writes;
}

/* ══════════════════════════════════════════════════════════════════════
 * V4 BRIDGE — map V4 lane (0..53) → L17Cell (backward compat)
 *
 * V4 uses 54 lanes (3×18). Map:
 *   V4 lane 0..35  → cell 0..35   (World A domain)
 *   V4 lane 36..53 → cell 36..53  (World B domain)
 *
 * ยังคง correctness: 54 ⊂ 289 (54 lanes เป็น subset ของ lattice)
 * ══════════════════════════════════════════════════════════════════════ */

static inline L17Cell l17_from_v4_lane(uint8_t v4_lane)
{
    L17Cell c;
    uint8_t cid     = v4_lane % L17_LANES_TOTAL;  /* 0..53 → direct map */
    c.cell_id       = cid;
    c.lane          = (uint8_t)(cid % L17_BRIDGE);
    c.group         = (uint8_t)(cid / L17_BRIDGE);
    c.world         = (c.group < L17_GROUPS_A) ? L17_WORLD_A : L17_WORLD_B;
    c.phase         = 0;
    c.is_buffer     = 0;
    c._pad[0] = c._pad[1] = 0;
    return c;
}

#endif /* POGLS_17N_LATTICE_H */
