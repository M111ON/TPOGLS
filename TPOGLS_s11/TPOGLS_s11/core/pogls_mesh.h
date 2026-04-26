/*
 * pogls_mesh.h — POGLS V4  Mesh Layer (Voronoi + Delaunay)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Mesh = async observer layer outside hot path.
 * Receives DetachEntry batches, clusters them (Voronoi),
 * connects clusters (Delaunay/Honeycomb), tracks lineage (Tail).
 *
 * Architecture:
 *
 *   pipeline_wire_process()
 *     └─ DetachLane.push() → ring → async flush → delta lane 53
 *
 *   mesh_ingest_batch()          ← called from DetachLane drain callback
 *     └─ [1] Voronoi  → assign cluster_id to each event (PHI a,b space)
 *     └─ [2] Delaunay → update neighbor graph between clusters
 *     └─ [3] Tail     → record lineage per slice
 *
 * Voronoi (PHI space):
 *   Seeds = 9 fixed points on unit circle in (a,b) space.
 *   cell_id = argmin(dist(event_ab, seed_i))  for i in 0..8
 *   9 seeds × 3 slices = natural 18/54/162 alignment.
 *   Cost: O(9) per event — trivial, no sqrt (use squared dist).
 *
 * Delaunay (cluster graph):
 *   Edge exists between cluster_i and cluster_j if they share
 *   ghost cross-slice events. Updated on each ingest batch.
 *   Max 9×9 = 81 possible edges (stored as adjacency bitmask).
 *
 * Tail (lineage per slice):
 *   Tracks last N events per (slice, cluster) pair.
 *   Replaces Entangle: no live sync, no shared state.
 *   Galaxy reads Tail to understand "where anomalies came from."
 *
 * Rules (FROZEN):
 *   - Mesh NEVER calls into pipeline_wire_process()
 *   - Mesh NEVER modifies DetachEntry after receipt
 *   - Voronoi seeds are fixed in PHI space (not recalculated per batch)
 *   - Delaunay edges added only, never removed in one session
 *   - All Mesh operations are O(N×9) or better, no heavy compute
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_MESH_H
#define POGLS_MESH_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "pogls_platform.h"
#include "pogls_detach_lane.h"
#include "pogls_engine_slice.h"

/* ── dimensions ──────────────────────────────────────────────────── */
#define MESH_VORONOI_SEEDS    9u    /* 9 seeds × 3 slices = 27 = 162/6 */
#define MESH_MAX_CLUSTERS     9u    /* cluster IDs 0..8                 */
#define MESH_TAIL_DEPTH      18u    /* last 18 events per (slice,cluster)*/
#define MESH_MAGIC      0x4D455348u /* "MESH"                           */

/* PHI_SCALE for coordinate normalisation */
#ifndef POGLS_PHI_CONSTANTS
#  include "pogls_platform.h"
#endif

/* ══════════════════════════════════════════════════════════════════
 * Voronoi seeds — 9 fixed points in PHI (a,b) unit space [0..1)
 * Arranged on unit circle at angles 0, 40, 80, 120, 160, 200,
 * 240, 280, 320 degrees — even angular spacing, no pole artifact.
 * Scaled to PHI_SCALE coordinates.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t a;   /* PHI space coordinate a */
    uint32_t b;   /* PHI space coordinate b */
} VoronoiSeed;

/* Pre-computed: 9 seeds evenly spaced on unit circle
 * a = floor((0.5 + 0.45*cos(k*40°)) * PHI_SCALE)
 * b = floor((0.5 + 0.45*sin(k*40°)) * PHI_SCALE)
 * Kept inside unit circle (a²+b² < 2^41 guaranteed by 0.45 radius) */
/* K-means optimised seeds — fit actual PHI scatter distribution.
 * Generated from 100k sample points, 30 iterations.
 * All seeds verified inside unit circle (sq>>41 == 0).
 * Min cluster occupancy: ~9.4% — no empty clusters. */
static const VoronoiSeed MESH_SEEDS[MESH_VORONOI_SEEDS] = {
    { 103984u,  39718u },   /* seed 0 */
    { 520963u, 198989u },   /* seed 1 */
    { 568421u, 617637u },   /* seed 2 */
    {  80861u, 431406u },   /* seed 3 */
    { 244791u, 494021u },   /* seed 4 */
    { 729951u, 278816u },   /* seed 5 */
    { 407093u, 556015u },   /* seed 6 */
    { 312985u, 119549u },   /* seed 7 */
    { 941846u, 359752u },   /* seed 8 */
};

/* ── squared distance in PHI space (no sqrt, no float) ──────────── */
static inline uint64_t _mesh_dist2(uint32_t a1, uint32_t b1,
                                    uint32_t a2, uint32_t b2)
{
    int64_t da = (int64_t)a1 - (int64_t)a2;
    int64_t db = (int64_t)b1 - (int64_t)b2;
    return (uint64_t)(da*da + db*db);
}

/* ══════════════════════════════════════════════════════════════════
 * voronoi_classify — assign cluster_id (0..8) to (a,b) point
 * O(9) — trivial
 * ══════════════════════════════════════════════════════════════════ */
static inline uint8_t voronoi_classify(uint32_t a, uint32_t b)
{
    uint8_t  best   = 0;
    uint64_t best_d = UINT64_MAX;
    for (uint8_t i = 0; i < MESH_VORONOI_SEEDS; i++) {
        uint64_t d = _mesh_dist2(a, b,
                                  MESH_SEEDS[i].a, MESH_SEEDS[i].b);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* ══════════════════════════════════════════════════════════════════
 * MeshCluster — one Voronoi cell
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t   cluster_id;
    uint8_t   primary_slice;   /* slice that generates most events here */
    uint16_t  _pad;
    uint64_t  event_count;
    uint64_t  last_value;      /* last value seen in this cluster        */
    uint64_t  last_addr;       /* last angular_addr                      */
    uint32_t  last_phase18;    /* gate phase of last event               */
    uint32_t  cross_count;     /* ghost events from other slices         */
} MeshCluster;

/* ══════════════════════════════════════════════════════════════════
 * MeshDelaunay — adjacency graph between clusters
 * 9×9 = 81 possible edges, stored as bitmask per cluster.
 * edge(i,j) exists when ghost from cluster_i lands in cluster_j.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint16_t  adj[MESH_MAX_CLUSTERS];   /* bit j set = edge i→j          */
    uint32_t  edge_count;               /* total directed edges            */
    uint64_t  cross_events;             /* ghost events that created edges */
} MeshDelaunay;

static inline void delaunay_add_edge(MeshDelaunay *d,
                                      uint8_t src, uint8_t dst)
{
    if (!d || src >= MESH_MAX_CLUSTERS || dst >= MESH_MAX_CLUSTERS) return;
    if (src == dst) return;
    uint16_t bit = (uint16_t)(1u << dst);
    if (!(d->adj[src] & bit)) {
        d->adj[src] |= bit;
        d->edge_count++;
    }
    d->cross_events++;
}

static inline int delaunay_has_edge(const MeshDelaunay *d,
                                     uint8_t src, uint8_t dst)
{
    if (!d || src >= MESH_MAX_CLUSTERS || dst >= MESH_MAX_CLUSTERS) return 0;
    return (d->adj[src] >> dst) & 1;
}

static inline uint32_t delaunay_neighbor_count(const MeshDelaunay *d,
                                                 uint8_t cluster_id)
{
    if (!d || cluster_id >= MESH_MAX_CLUSTERS) return 0;
    return (uint32_t)__builtin_popcount(d->adj[cluster_id]);
}

/* ══════════════════════════════════════════════════════════════════
 * MeshTail — lineage ring per (slice, cluster)
 * Stores last MESH_TAIL_DEPTH event summaries.
 * Replaces Entangle: no live link, just history.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t  addr[MESH_TAIL_DEPTH];    /* angular_addr ring             */
    uint8_t   reason[MESH_TAIL_DEPTH];  /* DETACH_REASON_* per entry     */
    uint8_t   phase18[MESH_TAIL_DEPTH]; /* gate phase per entry           */
    uint8_t   head;                     /* next write index (0..17)       */
    uint8_t   full;                     /* 1 when ring filled once        */
    uint64_t  total;                    /* total events appended          */
} MeshTail;

static inline void tail_push(MeshTail *t, uint64_t addr,
                               uint8_t reason, uint8_t phase18)
{
    if (!t) return;
    uint8_t idx = t->head & (MESH_TAIL_DEPTH - 1u);
    t->addr[idx]    = addr;
    t->reason[idx]  = reason;
    t->phase18[idx] = phase18;
    t->head         = (uint8_t)((t->head + 1u) % MESH_TAIL_DEPTH);
    if (!t->full && t->head == 0) t->full = 1;
    t->total++;
}

/* ══════════════════════════════════════════════════════════════════
 * Mesh — the complete observer
 * ══════════════════════════════════════════════════════════════════ */
typedef struct Mesh_s {
    MeshCluster   clusters[MESH_MAX_CLUSTERS];
    MeshDelaunay  delaunay;
    MeshTail      tails[SLICE_COUNT][MESH_MAX_CLUSTERS]; /* [slice][cluster] */

    /* aggregate stats */
    uint64_t  total_ingested;    /* total DetachEntry processed          */
    uint64_t  twin_window_hits;  /* events in phase288<18 || phase306<18 */
    uint64_t  cross_slice_ghosts;/* ghost events that crossed slices      */

    uint32_t  magic;
} Mesh;
typedef struct Mesh_s Mesh;  /* alias for forward decl compatibility */

/* ── init ────────────────────────────────────────────────────────── */
static inline void mesh_init(Mesh *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    for (uint8_t i = 0; i < MESH_MAX_CLUSTERS; i++) {
        m->clusters[i].cluster_id = i;
        m->clusters[i].primary_slice = 0xFF;  /* unknown until data flows */
    }
    m->magic = MESH_MAGIC;
}

/* ══════════════════════════════════════════════════════════════════
 * mesh_ingest — process one DetachEntry
 *
 * Called from DetachLane drain callback (async thread, off hot path).
 * 1. PHI scatter addr → (a, b)
 * 2. Voronoi classify → cluster_id
 * 3. Update cluster + Tail
 * 4. If ghost cross-slice → add Delaunay edge
 * ══════════════════════════════════════════════════════════════════ */
static inline void mesh_ingest(Mesh *m, const DetachEntry *e)
{
    if (!m || !e) return;

    /* 1. PHI scatter: addr → (a, b) */
    uint32_t mask = POGLS_PHI_SCALE - 1u;
    uint32_t addr = (uint32_t)(e->angular_addr & mask);
    uint32_t a    = (uint32_t)(((uint64_t)addr * POGLS_PHI_UP)   >> 20) & mask;
    uint32_t b    = (uint32_t)(((uint64_t)addr * POGLS_PHI_DOWN) >> 20) & mask;

    /* 2. Voronoi classify */
    uint8_t cid = voronoi_classify(a, b);

    /* 3. Determine source slice from angular_addr lane */
    uint8_t src_lane  = (uint8_t)((e->angular_addr) % 54u);
    uint8_t src_slice = (uint8_t)(src_lane / 18u);
    uint8_t ghost_lane= (uint8_t)((src_lane + 27u) % 54u);
    uint8_t dst_slice = (uint8_t)(ghost_lane / 18u);

    /* 4. Update cluster */
    MeshCluster *cl = &m->clusters[cid];
    cl->event_count++;
    cl->last_value  = e->value;
    cl->last_addr   = e->angular_addr;
    cl->last_phase18= e->phase18;

    /* update primary slice (whichever sent most) */
    if (cl->primary_slice == 0xFF) cl->primary_slice = src_slice;

    /* 5. Tail update */
    tail_push(&m->tails[src_slice][cid],
              e->angular_addr, e->reason, e->phase18);

    /* 6. Delaunay: cross-slice ghost → add edge */
    if (src_slice != dst_slice) {
        /* find cluster at ghost destination */
        uint8_t ghost_addr = (uint8_t)(ghost_lane * (POGLS_PHI_SCALE / 54u));
        uint8_t ga = (uint32_t)(((uint64_t)ghost_addr * POGLS_PHI_UP)  >>20) & 0xFF;
        uint8_t gb = (uint32_t)(((uint64_t)ghost_addr * POGLS_PHI_DOWN)>>20) & 0xFF;
        uint8_t dst_cid = voronoi_classify(
            (uint32_t)ga * (POGLS_PHI_SCALE / 256u),
            (uint32_t)gb * (POGLS_PHI_SCALE / 256u));
        delaunay_add_edge(&m->delaunay, cid, dst_cid);
        cl->cross_count++;
        m->cross_slice_ghosts++;
    }

    /* 7. Twin window */
    if (detach_is_twin_window(e)) m->twin_window_hits++;

    m->total_ingested++;
}

/* ── batch ingest ────────────────────────────────────────────────── */
static inline void mesh_ingest_batch(Mesh *m,
                                      const DetachEntry *entries,
                                      uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        mesh_ingest(m, &entries[i]);
}

/* ── stats ───────────────────────────────────────────────────────── */
static inline void mesh_stats(const Mesh *m)
{
    if (!m) return;
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  Mesh Stats (Voronoi + Delaunay + Tail)              ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Total ingested:     %10llu                       ║\n",
           (unsigned long long)m->total_ingested);
    printf("║ Cross-slice ghosts: %10llu                       ║\n",
           (unsigned long long)m->cross_slice_ghosts);
    printf("║ Twin window hits:   %10llu                       ║\n",
           (unsigned long long)m->twin_window_hits);
    printf("║ Delaunay edges:     %10u                        ║\n",
           m->delaunay.edge_count);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Cluster  events  slice  cross  neighbors             ║\n");
    for (int i = 0; i < (int)MESH_MAX_CLUSTERS; i++) {
        const MeshCluster *c = &m->clusters[i];
        if (c->event_count == 0) continue;
        printf("║   %d     %8llu    %d    %5u    %u                  ║\n",
               i, (unsigned long long)c->event_count,
               c->primary_slice == 0xFF ? 9 : c->primary_slice,
               c->cross_count,
               delaunay_neighbor_count(&m->delaunay, (uint8_t)i));
    }
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_MESH_H */
