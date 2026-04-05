/*
 * pogls38_giant_shadow.h — POGLS38 Giant Shadow
 * ══════════════════════════════════════════════════════════════════════
 *
 * Giant Shadow = offline observer that sits BEHIND the Hydra detach
 * ring and converts quarantine data into actionable routing intelligence.
 *
 * Position in the stack:
 *
 *   GPU batch
 *     ↓  l38_hydra_batch_feed()
 *   L38HydraThin
 *     │  audit pass  → fed_write (FederationCtx, main flow)
 *     │  audit fail  → ring[4096] → flush → delta lane 53
 *     ↓  l38_gs_drain()          ← THIS FILE
 *   GiantShadow38
 *     │  [1] DetachEntry → MeshEntry via mesh_translate()
 *     │  [2] mesh_ingest()  — Voronoi cluster, Delaunay graph, Tail
 *     │  [3] reflex_update() — bias table update (lazy decay)
 *     ↓  l38_gs_feedback()       ← optional: feed bias back to Hydra
 *   L38HydraThin.heads[i].analytics (low-priority, non-blocking)
 *
 * Mental model:
 *
 *   Hydra     = real-time router  (hot path, µs)
 *   Detach    = shock absorber    (ring, async)
 *   GiantShadow = long memory     (offline, ms)
 *   ReflexBias  = feedback signal (bias table → Hydra routing hint)
 *
 *   Think of it as:
 *     Hydra   ≈ spinal cord reflex  (fast, no thinking)
 *     GiantShadow ≈ brain cortex   (slow, learns patterns)
 *
 * Data flow detail:
 *
 *   L38DetachEntry (32B, from ring)
 *        │ l38_detach_to_v4()
 *        ▼
 *   DetachEntry (V4 format, 32B) ← Mesh expects this
 *        │ mesh_translate()
 *        ▼
 *   MeshEntry (24B, rich context)
 *        │ mesh_ingest()
 *        ▼
 *   Voronoi cluster → Delaunay graph → Tail lineage
 *        │ reflex_update()
 *        ▼
 *   ReflexBias[256 buckets] — bias ≤ -4 → routing hint for next batch
 *
 * Work-stealing (L-System):
 *   3 slices × 18 lanes = 54 lanes total
 *   Each lane has its own GS sub-ring (18-deep FIFO)
 *   Neighbor steal: if lane N is idle and lane (N+1)%54 is hot,
 *     steal up to STEAL_MAX entries
 *   Backpressure: if GS total > 80% capacity → pause drain
 *
 * Rubik invariant (FROZEN):
 *   Ghost cross-slice offset = 27 (= 54/2)
 *   Lane pair (N, N+27) always in different slices
 *   GS processes them as symmetric entries (K3 complete graph)
 *
 * Constants (FROZEN):
 *   GS_LANES            = 54     (Rubik lanes)
 *   GS_LANE_DEPTH       = 18     (entries per lane sub-ring)
 *   GS_STEAL_MAX        = 4      (max entries to steal per cycle)
 *   GS_BACKPRESSURE_PCT = 80     (drain pause threshold)
 *   GS_MAGIC            = "GS38"
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS38_GIANT_SHADOW_H
#define POGLS38_GIANT_SHADOW_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>

#include "pogls_engine_slice.h"   /* SLICE_COUNT=3, SLICE_LANE_WIDTH=18 */
#include "pogls_mesh_entry.h"     /* MeshEntry, ReflexBias, mesh_translate */
#include "pogls_mesh.h"           /* Mesh, mesh_ingest, voronoi_classify   */

/* ── forward decl for L38HydraThin (avoid circular include) ──────── */
#ifndef POGLS38_HYDRA_THIN_H
typedef struct L38HydraThin_s L38HydraThin;
#endif

/* ══════════════════════════════════════════════════════════════════
 * Constants (FROZEN)
 * ══════════════════════════════════════════════════════════════════ */
#define GS_LANES              54u   /* Rubik lanes — FROZEN               */
#define GS_LANE_DEPTH         18u   /* sub-ring depth per lane — FROZEN    */
#define GS_STEAL_MAX           4u   /* work-steal limit per cycle          */
#define GS_BACKPRESSURE_PCT   80u   /* % full → pause drain                */
#define GS_TOTAL_SLOTS       (GS_LANES * GS_LANE_DEPTH)   /* 54×18 = 972  */
#define GS_BACKPRESSURE_HWM  ((GS_TOTAL_SLOTS * GS_BACKPRESSURE_PCT) / 100u)
#define GS_MAGIC         0x47533338u   /* "GS38"                           */

/* ══════════════════════════════════════════════════════════════════
 * DetachEntry (V4 wire format) — subset used for Mesh translation
 * Mirrors pogls_detach_lane.h DetachEntry, adapted for L38 input.
 * Only the fields Mesh cares about are populated.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t  value;
    uint64_t  angular_addr;
    uint64_t  timestamp_ns;   /* 0 if not available                      */
    uint8_t   reason;         /* DETACH_REASON_* bitmask                 */
    uint8_t   route_was;      /* 0=main, 1=ghost                         */
    uint8_t   shell_n;        /* 0 for L38 (no shell concept)            */
    uint8_t   phase18;        /* lane % 18                               */
    uint16_t  phase288;       /* lane * (288/54)                         */
    uint16_t  phase306;       /* lane * (306/54)                         */
} GS38DetachEntry;            /* 32B — layout compatible with V4          */

typedef char _gs_det_sz[(sizeof(GS38DetachEntry) == 32u) ? 1 : -1];

/* ── convert L38DetachEntry → GS38DetachEntry for Mesh ───────────── */
/* (L38DetachEntry defined in pogls38_hydra_thin.h)                   */
static inline GS38DetachEntry gs38_make_detach(
        uint8_t   lane,
        uint8_t   audit,     /* audit code → reason mapping             */
        uint32_t  hil,
        uint64_t  value,
        uint64_t  batch_id)
{
    GS38DetachEntry e;
    memset(&e, 0, sizeof(e));
    e.value        = value;
    e.angular_addr = (uint64_t)hil;
    e.timestamp_ns = batch_id;   /* repurpose as batch sequence          */

    /* map audit code to detach reason */
    if (audit == 0u)        e.reason = 0u;
    else if (audit & 0x1u)  e.reason = DETACH_REASON_GEO_INVALID;
    else if (audit & 0x2u)  e.reason = DETACH_REASON_GHOST_DRIFT;
    else                    e.reason = DETACH_REASON_UNIT_CIRCLE;

    e.route_was = (uint8_t)(lane == ((lane + 27u) % 54u) ? 1u : 0u);
    e.shell_n   = 0u;
    e.phase18   = (uint8_t)(lane % 18u);
    e.phase288  = (uint16_t)(((uint32_t)lane * 288u) / 54u);
    e.phase306  = (uint16_t)(((uint32_t)lane * 306u) / 54u);

    return e;
}

/* ══════════════════════════════════════════════════════════════════
 * GS38LaneRing — per-lane 18-deep FIFO sub-ring
 * Stores GS38DetachEntry items pending drain to Mesh.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    GS38DetachEntry entries[GS_LANE_DEPTH];
    uint8_t  head;          /* next write (0..17)                         */
    uint8_t  tail;          /* next read  (0..17)                         */
    uint8_t  count;         /* current depth                              */
    uint8_t  lane_id;       /* which lane this ring belongs to            */
    uint64_t total_pushed;
    uint64_t total_drained;
    uint64_t total_stolen;  /* entries stolen by neighbor                 */
} GS38LaneRing;

static inline void gs38_lane_ring_init(GS38LaneRing *r, uint8_t lane_id)
{
    memset(r, 0, sizeof(*r));
    r->lane_id = lane_id;
}

static inline int gs38_lane_push(GS38LaneRing *r, const GS38DetachEntry *e)
{
    if (!r || !e || r->count >= GS_LANE_DEPTH) return 0;   /* full */
    r->entries[r->head] = *e;
    r->head = (uint8_t)((r->head + 1u) % GS_LANE_DEPTH);
    r->count++;
    r->total_pushed++;
    return 1;
}

static inline int gs38_lane_pop(GS38LaneRing *r, GS38DetachEntry *out)
{
    if (!r || !out || r->count == 0) return 0;  /* empty */
    *out   = r->entries[r->tail];
    r->tail = (uint8_t)((r->tail + 1u) % GS_LANE_DEPTH);
    r->count--;
    r->total_drained++;
    return 1;
}

/* ══════════════════════════════════════════════════════════════════
 * GiantShadow38 — the main observer struct
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  lanes[54]       — per-lane sub-rings (18-deep each)        │
 *  │  mesh            — Voronoi + Delaunay + Tail                │
 *  │  reflex          — bias table (256 buckets, lazy decay)     │
 *  │  entry_buf       — MeshEntry SPSC ring (→ POGLS38 consumer) │
 *  └─────────────────────────────────────────────────────────────┘
 *
 * Work-steal topology (L-System / K3):
 *   Lane N steals from (N + GS_STEAL_OFFSET) % 54
 *   GS_STEAL_OFFSET = 27 (same as ghost offset — intentional)
 *   → steal partner is always in a different slice
 * ══════════════════════════════════════════════════════════════════ */
#define GS_STEAL_OFFSET  27u   /* FROZEN — mirrors ghost cross-slice rule */

typedef struct {
    uint32_t      magic;
    GS38LaneRing  lanes[GS_LANES];   /* 54 per-lane rings                */
    Mesh          mesh;              /* observer (Voronoi/Delaunay/Tail)  */
    ReflexBias    reflex;            /* feedback bias table               */
    MeshEntryBuf  entry_buf;         /* output ring → POGLS38 consumer    */

    /* aggregate stats */
    atomic_uint_fast64_t total_ingested;   /* total items pushed to lanes */
    atomic_uint_fast64_t total_drained;    /* total items processed        */
    atomic_uint_fast64_t total_stolen;     /* work-steal events            */
    atomic_uint_fast64_t backpressure_hit; /* drain paused events          */

    uint64_t  drain_cycles;   /* how many drain passes run               */
    uint32_t  occupied_lanes; /* lanes with count > 0 (last drain scan)  */
} GiantShadow38;

/* ══════════════════════════════════════════════════════════════════
 * Init
 * ══════════════════════════════════════════════════════════════════ */
static inline void gs38_init(GiantShadow38 *gs)
{
    if (!gs) return;
    memset(gs, 0, sizeof(*gs));
    gs->magic = GS_MAGIC;

    for (uint8_t i = 0u; i < GS_LANES; i++)
        gs38_lane_ring_init(&gs->lanes[i], i);

    mesh_init(&gs->mesh);
    reflex_init(&gs->reflex);
    mesh_entry_buf_init(&gs->entry_buf);

    atomic_init(&gs->total_ingested,   0);
    atomic_init(&gs->total_drained,    0);
    atomic_init(&gs->total_stolen,     0);
    atomic_init(&gs->backpressure_hit, 0);
}

/* ══════════════════════════════════════════════════════════════════
 * Backpressure check
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t gs38_total_count(const GiantShadow38 *gs)
{
    uint32_t total = 0u;
    for (uint8_t i = 0u; i < GS_LANES; i++)
        total += gs->lanes[i].count;
    return total;
}

static inline int gs38_backpressure(const GiantShadow38 *gs)
{
    return gs38_total_count(gs) >= GS_BACKPRESSURE_HWM;
}

/* ══════════════════════════════════════════════════════════════════
 * Push — called from l38_hydra_batch_feed() after detach_push
 *
 * Converts L38 detach info into GS38DetachEntry and pushes
 * to the owning lane's sub-ring.
 *
 * Hot path: no mutex, no alloc. Returns 0 if lane ring full.
 * ══════════════════════════════════════════════════════════════════ */
static inline int gs38_push(GiantShadow38 *gs,
                             uint8_t   lane,
                             uint8_t   audit,
                             uint32_t  hil,
                             uint64_t  value,
                             uint64_t  batch_id)
{
    if (!gs) return 0;
    uint8_t lid = (uint8_t)(lane % GS_LANES);
    GS38DetachEntry e = gs38_make_detach(lane, audit, hil, value, batch_id);
    int ok = gs38_lane_push(&gs->lanes[lid], &e);
    if (ok)
        atomic_fetch_add_explicit(&gs->total_ingested, 1u, memory_order_relaxed);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════
 * Process one entry through Mesh + ReflexBias
 * ══════════════════════════════════════════════════════════════════ */
static inline void _gs38_process_one(GiantShadow38 *gs,
                                      const GS38DetachEntry *e)
{
    /* Mesh expects DetachEntry — GS38DetachEntry is layout-compatible */
    const DetachEntry *de = (const DetachEntry *)(const void *)e;

    /* 1. Translate → MeshEntry (rich context) */
    MeshEntry me = mesh_translate(de);

    /* 2. Ingest into Mesh (Voronoi + Delaunay + Tail) */
    mesh_ingest(&gs->mesh, de);

    /* 3. Update ReflexBias (lazy decay, feedback signal) */
    reflex_update(&gs->reflex, &me);

    /* 4. Push to output ring (→ POGLS38 consumer, optional) */
    mesh_entry_push(&gs->entry_buf, &me);

    atomic_fetch_add_explicit(&gs->total_drained, 1u, memory_order_relaxed);
}

/* ══════════════════════════════════════════════════════════════════
 * Work-steal
 *
 * If lane N is idle and neighbor (N + 27) % 54 has entries,
 * steal up to GS_STEAL_MAX entries and process them from lane N.
 *
 * Steal partner = lane + 27 (K3 complete graph — different slice)
 * This mirrors the ghost cross-slice topology exactly.
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t _gs38_work_steal(GiantShadow38 *gs, uint8_t lane)
{
    uint8_t steal_from = (uint8_t)((lane + GS_STEAL_OFFSET) % GS_LANES);
    GS38LaneRing *victim = &gs->lanes[steal_from];
    if (victim->count == 0u) return 0u;

    uint32_t stolen = 0u;
    GS38DetachEntry e;
    while (stolen < GS_STEAL_MAX && gs38_lane_pop(victim, &e)) {
        _gs38_process_one(gs, &e);
        victim->total_stolen++;
        stolen++;
    }

    if (stolen > 0u)
        atomic_fetch_add_explicit(&gs->total_stolen, stolen, memory_order_relaxed);
    return stolen;
}

/* ══════════════════════════════════════════════════════════════════
 * Drain — main drain loop (call from async thread or end-of-batch)
 *
 * Algorithm:
 *   1. Backpressure check — if >80% full, skip (return 0)
 *   2. For each of 54 lanes:
 *        a. drain own entries (up to max_per_lane)
 *        b. if idle, attempt work-steal from (lane+27)%54
 *   3. Update occupied_lanes count
 *
 * Returns total entries drained this cycle.
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t gs38_drain(GiantShadow38 *gs, uint32_t max_per_lane)
{
    if (!gs) return 0u;

    if (gs38_backpressure(gs)) {
        atomic_fetch_add_explicit(&gs->backpressure_hit, 1u, memory_order_relaxed);
        return 0u;
    }

    if (max_per_lane == 0u) max_per_lane = GS_LANE_DEPTH;

    uint32_t total_this_cycle = 0u;
    uint32_t occupied = 0u;

    for (uint8_t lane = 0u; lane < GS_LANES; lane++) {
        GS38LaneRing *r = &gs->lanes[lane];

        /* drain own entries */
        uint32_t drained_here = 0u;
        GS38DetachEntry e;
        while (drained_here < max_per_lane && gs38_lane_pop(r, &e)) {
            _gs38_process_one(gs, &e);
            drained_here++;
            total_this_cycle++;
        }

        /* work-steal if idle */
        if (drained_here == 0u) {
            total_this_cycle += _gs38_work_steal(gs, lane);
        }

        if (r->count > 0u) occupied++;
    }

    gs->occupied_lanes = occupied;
    gs->drain_cycles++;
    return total_this_cycle;
}

/* ══════════════════════════════════════════════════════════════════
 * Feedback — read bias hint for a lane
 *
 * Returns reflex bias for the angular address space of this lane.
 * Hydra can call this before routing to decide if a lane zone
 * is "hot anomaly" and should get lower priority.
 *
 * bias ≤ REFLEX_DEMOTE_THRESHOLD (-4) → suggest routing to ghost
 * ══════════════════════════════════════════════════════════════════ */
static inline int8_t gs38_lane_bias(const GiantShadow38 *gs, uint8_t lane)
{
    if (!gs) return 0;
    /* derive representative address for this lane: lane × PHI_SCALE/54 */
    uint64_t addr = (uint64_t)lane * (POGLS_PHI_SCALE / GS_LANES);
    return reflex_lookup(&gs->reflex, addr);
}

static inline int gs38_lane_should_demote(const GiantShadow38 *gs, uint8_t lane)
{
    if (!gs) return 0;
    uint64_t addr = (uint64_t)lane * (POGLS_PHI_SCALE / GS_LANES);
    return reflex_should_demote(&gs->reflex, addr);
}

/* ══════════════════════════════════════════════════════════════════
 * Stats
 * ══════════════════════════════════════════════════════════════════ */
static inline void gs38_stats(const GiantShadow38 *gs)
{
    if (!gs) return;

    uint64_t ingested   = atomic_load(&((GiantShadow38*)gs)->total_ingested);
    uint64_t drained    = atomic_load(&((GiantShadow38*)gs)->total_drained);
    uint64_t stolen     = atomic_load(&((GiantShadow38*)gs)->total_stolen);
    uint64_t bp_hit     = atomic_load(&((GiantShadow38*)gs)->backpressure_hit);

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  GiantShadow38  magic=%08X                          ║\n", gs->magic);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  ingested:        %10" PRIu64 "                        ║\n", ingested);
    printf("║  drained:         %10" PRIu64 "                        ║\n", drained);
    printf("║  work-stolen:     %10" PRIu64 "                        ║\n", stolen);
    printf("║  backpressure:    %10" PRIu64 "                        ║\n", bp_hit);
    printf("║  drain_cycles:    %10" PRIu64 "                        ║\n", gs->drain_cycles);
    printf("║  occupied_lanes:  %10u                        ║\n",  gs->occupied_lanes);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Lane fill (non-empty):                              ║\n");
    uint32_t nonempty = 0u;
    for (uint8_t i = 0u; i < GS_LANES; i++)
        if (gs->lanes[i].count > 0u) nonempty++;
    printf("║    %u / %u lanes have pending entries                 ║\n",
           nonempty, GS_LANES);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Mesh: ingested=%" PRIu64 "  edges=%u  ghosts=%" PRIu64 "      ║\n",
           gs->mesh.total_ingested,
           gs->mesh.delaunay.edge_count,
           gs->mesh.cross_slice_ghosts);
    printf("║  Reflex: rewards=%" PRIu64 "  reinforcements=%" PRIu64 "      ║\n",
           gs->reflex.rewards, gs->reflex.reinforcements);
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

/* ══════════════════════════════════════════════════════════════════
 *  CONCEPT MAP (FROZEN)
 *
 *  Component        │  Role              │  Timing
 *  ─────────────────┼────────────────────┼────────────────────────
 *  L38HydraThin     │  real-time router  │  hot path (µs)
 *  DetachRing       │  shock absorber    │  SPSC, drop_oldest
 *  GiantShadow38    │  long memory       │  async drain (ms)
 *  Mesh             │  pattern observer  │  Voronoi/Delaunay/Tail
 *  ReflexBias       │  feedback signal   │  bias[256], lazy decay
 *  MeshEntryBuf     │  output to L38     │  SPSC ring 1024 × 24B
 *
 *  Work-steal (L-System / K3):
 *    idle lane N → steals from (N+27)%54
 *    steal partner always in different slice (K3 topology)
 *    mirrors ghost cross-slice formula exactly
 *
 *  Backpressure:
 *    total_slots = 54 × 18 = 972
 *    HWM = 80% = 777 entries
 *    above HWM → drain pauses → detach ring absorbs pressure
 *
 * ══════════════════════════════════════════════════════════════════ */

#endif /* POGLS38_GIANT_SHADOW_H */
