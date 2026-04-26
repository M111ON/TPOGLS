/*
 * pogls_38_slice.h — POGLS38  EngineSlice
 * ══════════════════════════════════════════════════════════════════════
 *
 * Port จาก V4 pogls_engine_slice.h (frozen design)
 * เพิ่ม POGLS38-specific:
 *   1. HoneycombSlot tracking — Tails บันทึก slice state ใน honeycomb
 *   2. Spawn integration — slice ↔ L38SpawnCtrl (max 2 active)
 *   3. 17n mapping — slice → L17 cell range
 *
 * ══════════════════════════════════════════════════════════════════════
 * Design (FROZEN จาก V4):
 *
 *   54 lanes ÷ 3 = 18 lanes/slice
 *   162 nodes ÷ 3 = 54 nodes/slice
 *
 *   Slice 0: lanes  0-17, nodes   0-53   (Engine A)
 *   Slice 1: lanes 18-35, nodes  54-107  (Engine B)
 *   Slice 2: lanes 36-53, nodes 108-161  (Engine C)
 *
 * Ghost cross-slice: (lane + 27) % 54
 *   → K3 complete graph: ทุก slice เชื่อมกันหมดอัตโนมัติ
 *   → PHI math สร้าง inter-slice link ไม่ต้อง explicit sync
 *   → EngineSlice = descriptor เท่านั้น ไม่มี state
 *
 * HoneycombSlot (16B ใน DiamondBlock[48-63]):
 *   engine ไม่แตะ → Tails เขียนเองเพื่อ track slice state
 *   layout ใน POGLS38:
 *     [0-7]  merkle_root 8B  (existing)
 *     [8]    algo_id     1B  (existing)
 *     [9]    migration   1B  (existing)
 *     [10]   slice_id    1B  ← NEW: 0/1/2 หรือ 0xFF=unassigned
 *     [11]   slice_flags 1B  ← NEW: active/ghost/split bits
 *     [12-15] reserved   4B  (existing)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_SLICE_H
#define POGLS_38_SLICE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pogls_38_engine.h"   /* DiamondBlock, HoneycombSlot, L38SpawnCtrl */

/* ══════════════════════════════════════════════════════════════════════
 * CONSTANTS (FROZEN — port จาก V4)
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_SLICE_LANE_WIDTH    18u   /* lanes per slice                */
#define L38_SLICE_NODE_WIDTH    54u   /* nodes per slice                */
#define L38_SLICE_COUNT          3u   /* A / B / C                      */
#define L38_SLICE_GHOST_OFFSET  27u   /* ghost offset = 54/2            */
#define L38_SLICE_TOTAL_LANES   54u   /* RUBIK_LANES                    */
#define L38_SLICE_TOTAL_NODES  162u   /* NODE_MAX                       */
#define L38_SLICE_GHOST_MAX_HOPS 2u   /* ghost crosses at most 2 slices */
#define L38_SLICE_MAGIC   0x534C3338u /* "SL38"                         */

/* POGLS38 addition */
#define L38_SLICE_MAX_ACTIVE     2u   /* max concurrent active slices   */
#define L38_SLICE_UNASSIGNED  0xFFu   /* honeycomb slice_id sentinel    */

/* slice_flags bits in HoneycombSlot */
#define L38_SFLAG_ACTIVE    0x01u   /* slice currently running          */
#define L38_SFLAG_GHOST     0x02u   /* ghost hop in progress            */
#define L38_SFLAG_SPLIT     0x04u   /* slice spawned from split         */
#define L38_SFLAG_REJOIN    0x08u   /* slice rejoining base             */

/* ══════════════════════════════════════════════════════════════════════
 * ENGINE SLICE (descriptor, 24B, no state — port from V4)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t   engine_id;      /* 0=A, 1=B, 2=C                         */
    uint8_t   lane_start;     /* 0, 18, 36                              */
    uint8_t   lane_count;     /* 18 always                              */
    uint8_t   node_start;     /* 0, 54, 108 (soft boundary)             */
    uint8_t   node_count;     /* 54 always                              */
    uint8_t   active;         /* 1 = running                            */
    uint16_t  hop_count;      /* ghost hops from this slice             */
    uint32_t  magic;
    uint64_t  ops_processed;
} L38EngineSlice;             /* 24B ✓                                  */

typedef char _l38_slice_sz[(sizeof(L38EngineSlice)==24u)?1:-1];

typedef struct {
    L38EngineSlice slices[L38_SLICE_COUNT];
} L38SliceSet;

/* ── init ──────────────────────────────────────────────────────────── */
static inline void l38_slice_init(L38EngineSlice *s, uint8_t engine_id)
{
    if (!s || engine_id >= L38_SLICE_COUNT) return;
    memset(s, 0, sizeof(*s));
    s->engine_id  = engine_id;
    s->lane_start = (uint8_t)(engine_id * L38_SLICE_LANE_WIDTH);
    s->lane_count = L38_SLICE_LANE_WIDTH;
    s->node_start = (uint8_t)(engine_id * L38_SLICE_NODE_WIDTH);
    s->node_count = L38_SLICE_NODE_WIDTH;
    s->active     = 1;
    s->magic      = L38_SLICE_MAGIC;
}

static inline void l38_slice_set_init(L38SliceSet *ss)
{
    if (!ss) return;
    for (uint8_t i = 0; i < L38_SLICE_COUNT; i++)
        l38_slice_init(&ss->slices[i], i);
}

/* ── ownership (port from V4) ──────────────────────────────────────── */
static inline uint8_t l38_slice_owner_of_lane(uint8_t lane)
    { return (uint8_t)(lane / L38_SLICE_LANE_WIDTH); }

static inline uint8_t l38_slice_owner_of_node(uint8_t node)
    { return (uint8_t)(node / L38_SLICE_NODE_WIDTH); }

static inline int l38_slice_owns_lane(const L38EngineSlice *s, uint8_t lane)
    { return s && lane >= s->lane_start &&
             lane < (uint8_t)(s->lane_start + s->lane_count); }

static inline int l38_slice_owns_node(const L38EngineSlice *s, uint8_t node)
    { return s && node >= s->node_start &&
             node < (uint8_t)(s->node_start + s->node_count); }

/* ── ghost cross-slice (port from V4, FROZEN) ──────────────────────── */
static inline uint8_t l38_slice_ghost_lane(uint8_t src_lane)
    { return (uint8_t)((src_lane + L38_SLICE_GHOST_OFFSET) % L38_SLICE_TOTAL_LANES); }

static inline uint8_t l38_slice_ghost_dst(uint8_t src_lane)
    { return l38_slice_owner_of_lane(l38_slice_ghost_lane(src_lane)); }

static inline int l38_slice_hop_ok(const L38EngineSlice *s, uint16_t hops)
    { return s && hops < L38_SLICE_GHOST_MAX_HOPS; }

static inline uint16_t l38_slice_ghost_tag(uint8_t origin, uint8_t hops)
    { return (uint16_t)(((uint16_t)origin << 8) | (hops & 0xFF)); }

static inline uint8_t l38_slice_tag_origin(uint16_t tag) { return (uint8_t)(tag >> 8); }
static inline uint8_t l38_slice_tag_hops(uint16_t tag)   { return (uint8_t)(tag & 0xFF); }

/* ══════════════════════════════════════════════════════════════════════
 * HONEYCOMB SLICE TRACKING (POGLS38 addition)
 *
 * Tails เขียน slice_id + slice_flags ลง HoneycombSlot ของ DiamondBlock
 * engine ไม่แตะ honeycomb → ไม่ชนกัน
 * ══════════════════════════════════════════════════════════════════════ */

/* extended HoneycombSlot layout (overlays bytes 10-11) */
typedef struct __attribute__((packed)) {
    uint64_t  merkle_root;   /* [0-7]   8B                              */
    uint8_t   algo_id;       /* [8]     1B                              */
    uint8_t   migration;     /* [9]     1B                              */
    uint8_t   slice_id;      /* [10]    1B  0/1/2 or 0xFF=unassigned   */
    uint8_t   slice_flags;   /* [11]    1B  L38_SFLAG_*                */
    uint8_t   reserved[4];   /* [12-15] 4B                              */
} L38HoneycombSliceSlot;     /* 16B ✓                                   */
typedef char _l38_hcss[(sizeof(L38HoneycombSliceSlot)==16)?1:-1];

/* Tails writes slice tracking into honeycomb */
static inline void l38_tails_mark_slice(L38DiamondBlock *block,
                                          uint8_t          slice_id,
                                          uint8_t          flags)
{
    L38HoneycombSliceSlot s;
    memcpy(&s, block->honeycomb, 16);
    s.slice_id    = slice_id;
    s.slice_flags = flags;
    memcpy(block->honeycomb, &s, 16);
}

static inline uint8_t l38_tails_read_slice_id(const L38DiamondBlock *block)
{
    L38HoneycombSliceSlot s;
    memcpy(&s, block->honeycomb, 16);
    return s.slice_id;
}

static inline uint8_t l38_tails_read_slice_flags(const L38DiamondBlock *block)
{
    L38HoneycombSliceSlot s;
    memcpy(&s, block->honeycomb, 16);
    return s.slice_flags;
}

/* ══════════════════════════════════════════════════════════════════════
 * 17n LATTICE MAPPING (POGLS38 addition)
 *
 * 289 cells ÷ 3 slices:
 *   Slice A: cells   0-95   (96 cells, ≈289/3)
 *   Slice B: cells  96-191  (96 cells)
 *   Slice C: cells 192-288  (97 cells)
 *
 * ไม่ใช่ exact 1/3 เพราะ 289 ไม่หาร 3 ลงตัว
 * → Slice C ได้เพิ่ม 1 cell (cell 288)
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_SLICE_CELL_A_START   0u
#define L38_SLICE_CELL_A_END    95u
#define L38_SLICE_CELL_B_START  96u
#define L38_SLICE_CELL_B_END   191u
#define L38_SLICE_CELL_C_START 192u
#define L38_SLICE_CELL_C_END   288u

static inline uint8_t l38_slice_of_cell(uint16_t cell_id)
{
    if (cell_id <= L38_SLICE_CELL_A_END) return 0;
    if (cell_id <= L38_SLICE_CELL_B_END) return 1;
    return 2;
}

static inline int l38_slice_owns_cell(const L38EngineSlice *s, uint16_t cell_id)
{
    if (!s) return 0;
    return l38_slice_of_cell(cell_id) == s->engine_id;
}

/* ══════════════════════════════════════════════════════════════════════
 * SPAWN INTEGRATION (POGLS38 addition)
 *
 * max 2 slices active at once (L38_SLICE_MAX_ACTIVE=2)
 * slice → spawn head: slice_id maps to hydra head_id
 *   Slice A → head 0 (base, always active)
 *   Slice B → head 1 (spawned)
 *   Slice C → head 2 (future, needs GPU)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    L38SliceSet    slices;
    uint8_t        active_mask;    /* bit N = slice N active            */
    uint8_t        active_count;   /* how many slices running           */
    uint32_t       split_tick;     /* gate_18 ticks since last split    */
    uint64_t       total_splits;
    uint64_t       total_rejoins;
} L38SliceCtrl;

static inline void l38_slice_ctrl_init(L38SliceCtrl *sc)
{
    memset(sc, 0, sizeof(*sc));
    l38_slice_set_init(&sc->slices);
    /* only slice A active at start */
    sc->active_mask  = 0x01u;
    sc->active_count = 1;
    /* slices B and C start idle */
    sc->slices.slices[1].active = 0;
    sc->slices.slices[2].active = 0;
}

static inline int l38_slice_ctrl_active(const L38SliceCtrl *sc, uint8_t sid)
    { return sc && (sc->active_mask & (1u << sid)); }

/* split: activate slice B (max 2) */
static inline int l38_slice_split(L38SliceCtrl *sc, L38SpawnCtrl *spawn)
{
    if (!sc || !spawn) return -1;
    if (sc->active_count >= L38_SLICE_MAX_ACTIVE) return -2;   /* at max */
    if (spawn->cooldown_ticks > 0) return -3;                   /* cooldown */

    /* activate slice B */
    sc->slices.slices[1].active = 1;
    sc->active_mask  |= 0x02u;
    sc->active_count++;
    sc->total_splits++;

    /* tell spawn controller */
    l38_spawn_head(spawn);
    spawn->cooldown_ticks = L38_SPAWN_COOLDOWN_TICKS;

    return 1;   /* slice B now active */
}

/* rejoin: deactivate slice B, merge back to A */
static inline int l38_slice_rejoin(L38SliceCtrl *sc, L38SpawnCtrl *spawn)
{
    if (!sc || !spawn) return -1;
    if (sc->active_count <= 1) return -2;   /* nothing to rejoin */

    sc->slices.slices[1].active = 0;
    sc->active_mask  &= ~0x02u;
    sc->active_count--;
    sc->total_rejoins++;

    /* mark head as cooling in spawn controller */
    l38_kill_head(spawn, 1);
    l38_kill_confirm(spawn, 1);  /* immediate — slice handles drain separately */

    return 1;
}

/* gate_18 tick for slice */
static inline void l38_slice_gate18(L38SliceCtrl *sc)
{
    if (!sc) return;
    sc->split_tick++;
    for (uint8_t i = 0; i < L38_SLICE_COUNT; i++)
        sc->slices.slices[i].ops_processed++;   /* tick ops counter */
}

/* ── stats ──────────────────────────────────────────────────────────── */
static inline void l38_slice_stats(const L38SliceCtrl *sc)
{
    if (!sc) return;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  POGLS38 EngineSlice Map (18/54/162 joint)      ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  ID  lanes    nodes    cells    active  ops      ║\n");
    for (int i = 0; i < (int)L38_SLICE_COUNT; i++) {
        const L38EngineSlice *s = &sc->slices.slices[i];
        uint16_t cs = (uint16_t)(i * 96);
        uint16_t ce = (uint16_t)(i == 2 ? 288 : cs + 95);
        printf("║  %c   %2d-%2d    %3d-%3d  %3d-%3d  %s     %llu\n",
               'A' + i,
               s->lane_start, s->lane_start + s->lane_count - 1,
               s->node_start, s->node_start + s->node_count - 1,
               cs, ce,
               s->active ? "yes" : "no ",
               (unsigned long long)s->ops_processed);
    }
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  active=%u/%u  splits=%llu  rejoins=%llu\n",
           sc->active_count, L38_SLICE_MAX_ACTIVE,
           (unsigned long long)sc->total_splits,
           (unsigned long long)sc->total_rejoins);
    printf("║  Ghost K3: (lane+27)%%54  max_hops=2            ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_38_SLICE_H */
