/*
 * pogls_engine_slice.h — POGLS V4  EngineSlice
 * ══════════════════════════════════════════════════════════════════════
 *
 * Joint interface descriptor for the 18/54/162 modular system.
 *
 * A full PipelineWire (54 lanes, 162 nodes) can be decomposed into
 * 3 independent EngineSlice instances without remapping:
 *
 *   Slice 0: lanes  0-17, nodes   0-53   (Engine A)
 *   Slice 1: lanes 18-35, nodes  54-107  (Engine B)
 *   Slice 2: lanes 36-53, nodes 108-161  (Engine C)
 *
 * Ghost cross-slice (K3 complete graph, automatic):
 *   ghost_lane = (lane + 27) % 54
 *   → 100% of ghost lanes cross to a different slice
 *   → Each slice connects to both others (no isolation)
 *   → PHI math creates inter-engine links without explicit sync
 *
 * Rules (FROZEN):
 *   - Ghost formula (lane+27)%54 is NEVER modified
 *   - Lane ownership is a HARD boundary (routing, storage)
 *   - Node ownership is SOFT (routing hint only)
 *   - EngineSlice has NO state — it is a descriptor only
 *   - Slice can run independently and merge via Mesh
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_ENGINE_SLICE_H
#define POGLS_ENGINE_SLICE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── constants ───────────────────────────────────────────────────── */
#define SLICE_LANE_WIDTH    18u   /* lanes per slice (FROZEN)          */
#define SLICE_NODE_WIDTH    54u   /* nodes per slice (FROZEN)          */
#define SLICE_COUNT          3u   /* number of slices (FROZEN)         */
#define SLICE_GHOST_OFFSET  27u   /* ghost offset = 54/2 (FROZEN)      */
#define SLICE_TOTAL_LANES   54u   /* RUBIK_LANES                       */
#define SLICE_TOTAL_NODES  162u   /* NODE_MAX                          */
#define SLICE_MAGIC   0x534C4943u /* "SLIC"                            */

/* ── hop guard ───────────────────────────────────────────────────── */
#define SLICE_GHOST_MAX_HOPS  2u  /* ghost crosses at most 2 slices    */

/* ══════════════════════════════════════════════════════════════════
 * EngineSlice — joint descriptor (24B, no state)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t   engine_id;      /* 0, 1, 2                               */
    uint8_t   lane_start;     /* 0, 18, 36                             */
    uint8_t   lane_count;     /* 18 (always SLICE_LANE_WIDTH)          */
    uint8_t   node_start;     /* 0, 54, 108  (soft boundary)           */
    uint8_t   node_count;     /* 54 (always SLICE_NODE_WIDTH)          */
    uint8_t   active;         /* 1 = running, 0 = idle                 */
    uint16_t  hop_count;      /* ghost hops originated from this slice */
    uint32_t  magic;
    uint64_t  ops_processed;  /* total ops routed through this slice   */
} EngineSlice;                /* 24B                                   */

typedef char _slice_sz[(sizeof(EngineSlice) == 24u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════
 * EngineSliceSet — all 3 slices together
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    EngineSlice slices[SLICE_COUNT];
} EngineSliceSet;

/* ── init single slice ───────────────────────────────────────────── */
static inline void slice_init(EngineSlice *s, uint8_t engine_id)
{
    if (!s || engine_id >= SLICE_COUNT) return;
    memset(s, 0, sizeof(*s));
    s->engine_id  = engine_id;
    s->lane_start = (uint8_t)(engine_id * SLICE_LANE_WIDTH);
    s->lane_count = SLICE_LANE_WIDTH;
    s->node_start = (uint8_t)(engine_id * SLICE_NODE_WIDTH);
    s->node_count = SLICE_NODE_WIDTH;
    s->active     = 1;
    s->magic      = SLICE_MAGIC;
}

/* ── init all 3 slices ───────────────────────────────────────────── */
static inline void slice_set_init(EngineSliceSet *ss)
{
    if (!ss) return;
    for (uint8_t i = 0; i < SLICE_COUNT; i++)
        slice_init(&ss->slices[i], i);
}

/* ── which slice owns this lane? ─────────────────────────────────── */
static inline uint8_t slice_owner_of_lane(uint8_t lane)
{
    return (uint8_t)(lane / SLICE_LANE_WIDTH);
}

/* ── which slice owns this node? (soft) ──────────────────────────── */
static inline uint8_t slice_owner_of_node(uint8_t node)
{
    return (uint8_t)(node / SLICE_NODE_WIDTH);
}

/* ── ghost destination lane + slice ──────────────────────────────── */
static inline uint8_t slice_ghost_lane(uint8_t src_lane)
{
    return (uint8_t)((src_lane + SLICE_GHOST_OFFSET) % SLICE_TOTAL_LANES);
}

static inline uint8_t slice_ghost_dst(uint8_t src_lane)
{
    return slice_owner_of_lane(slice_ghost_lane(src_lane));
}

/* ── hop guard: is this ghost still valid? ───────────────────────── */
static inline int slice_hop_ok(const EngineSlice *s, uint16_t current_hops)
{
    if (!s) return 0;
    return current_hops < SLICE_GHOST_MAX_HOPS;
}

/* ── does lane belong to this slice? ─────────────────────────────── */
static inline int slice_owns_lane(const EngineSlice *s, uint8_t lane)
{
    return s && (lane >= s->lane_start) &&
           (lane < (uint8_t)(s->lane_start + s->lane_count));
}

/* ── does node belong to this slice? (soft) ─────────────────────── */
static inline int slice_owns_node(const EngineSlice *s, uint8_t node)
{
    return s && (node >= s->node_start) &&
           (node < (uint8_t)(s->node_start + s->node_count));
}

/* ── ghost tag (origin + hop count packed) ───────────────────────── */
static inline uint16_t slice_ghost_tag(uint8_t origin_engine, uint8_t hops)
{
    return (uint16_t)(((uint16_t)origin_engine << 8) | (hops & 0xFF));
}

static inline uint8_t slice_tag_origin(uint16_t tag) { return (uint8_t)(tag >> 8); }
static inline uint8_t slice_tag_hops(uint16_t tag)   { return (uint8_t)(tag & 0xFF); }

/* ── stats ───────────────────────────────────────────────────────── */
static inline void slice_stats(const EngineSliceSet *ss)
{
    if (!ss) return;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  EngineSlice Map (18/54/162 joint)              ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  ID  lanes    nodes    active  ops              ║\n");
    for (int i = 0; i < (int)SLICE_COUNT; i++) {
        const EngineSlice *s = &ss->slices[i];
        printf("║  %d   %2d-%2d    %3d-%3d  %s     %llu\n",
               s->engine_id,
               s->lane_start, s->lane_start + s->lane_count - 1,
               s->node_start, s->node_start + s->node_count - 1,
               s->active ? "yes" : "no ",
               (unsigned long long)s->ops_processed);
    }
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  Ghost K3 cross-slice map:                      ║\n");
    printf("║  A->B: lanes 0-8  ->ghost 27-35                 ║\n");
    printf("║  A->C: lanes 9-17 ->ghost 36-44                 ║\n");
    printf("║  B->C: lanes 18-26->ghost 45-53                 ║\n");
    printf("║  B->A: lanes 27-35->ghost 0-8                   ║\n");
    printf("║  C->A: lanes 36-44->ghost 9-17                  ║\n");
    printf("║  C->B: lanes 45-53->ghost 18-26                 ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_ENGINE_SLICE_H */
