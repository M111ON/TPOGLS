/*
 * pogls_detach_lane.h — POGLS V4  Detach Safety Layer
 * ══════════════════════════════════════════════════════════════════════
 *
 * Safety layer (Phase 1 of Process→Protect→Repair→Evolve).
 *
 * Role: shock absorber + anomaly dataset
 *   1. ring buffer รับ anomaly ทันที (ไม่ block pipeline)
 *   2. async flush thread เทลง isolated delta lane
 *   3. ข้อมูลใน lane ใช้ต่อยอด Rubik recovery + Shatter
 *
 * Trigger (DETACH_MODE = HYBRID — FROZEN):
 *   - ROUTE_SHADOW  = geo_invalid (unit circle fail)
 *   - ghost_streak  > POGLS_GHOST_STREAK_MAX (drift anomaly)
 *   - unit_circle   explicit fail flag from L3
 *
 * Architecture:
 *
 *   pipeline_wire_process()
 *          │  anomaly detected
 *          ▼
 *   detach_lane_push()     ← hot path (~3ns, no mutex)
 *          │  SPSC ring buffer [4096 slots]
 *          ▼
 *   _detach_flush_thread() ← async, 500µs interval
 *          │  batch 64 entries
 *          ▼
 *   delta_append()         ← isolated lane (DETACH_DELTA_LANE)
 *
 * On ring full:
 *   drop_oldest (overwrite) — never block main thread
 *   overflow counter increments — caller can observe pressure
 *
 * Constants (FROZEN):
 *   DETACH_RING_SIZE    = 4096   (power of 2)
 *   DETACH_FLUSH_BATCH  = 64     (aligned with delta batch)
 *   DETACH_DELTA_LANE   = 53     (last lane, isolated from MAIN/GHOST)
 *   DETACH_FLUSH_US     = 500    (drain interval)
 *
 * Integration (pogls_pipeline_wire.h):
 *   Add DetachLane to PipelineWire struct.
 *   After route_final, before storage write:
 *     if (l3_route == ROUTE_SHADOW || pw->detach_trigger)
 *         detach_lane_push(&pw->detach, value, angular_addr, reason)
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_DETACH_LANE_H
#define POGLS_DETACH_LANE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

#include "storage/pogls_delta.h"

/* forward declare — full type in pogls_mesh.h (included after) */
struct Mesh_s;
typedef void (*MeshIngestFn)(struct Mesh_s*, const void*, uint32_t);

/* ── constants (FROZEN) ──────────────────────────────────────────── */
#define DETACH_RING_SIZE      4096u
#define DETACH_RING_MASK      (DETACH_RING_SIZE - 1u)
#define DETACH_FLUSH_BATCH      64u
#define DETACH_DELTA_LANE       53u   /* isolated — never used by MAIN/GHOST */
#define DETACH_FLUSH_US        500u
#define DETACH_MAGIC      0x44544348u  /* "DTCH" */

/* ── anomaly reason bitmask ──────────────────────────────────────── */
#define DETACH_REASON_GEO_INVALID   0x01u  /* unit circle fail (SHADOW)   */
#define DETACH_REASON_GHOST_DRIFT   0x02u  /* ghost_streak > max          */
#define DETACH_REASON_UNIT_CIRCLE   0x04u  /* explicit L3 circle fail     */
#define DETACH_REASON_OVERFLOW      0x80u  /* ring was full, oldest evicted*/

/* ── DetachEntry — 32B ring slot ─────────────────────────────────── */
typedef struct {
    uint64_t  value;          /* data value that triggered anomaly      */
    uint64_t  angular_addr;   /* PHI-space addr (global — NOT shell-relative) */
    uint64_t  timestamp_ns;   /* nanosecond timestamp (monotonic)       */
    uint8_t   reason;         /* DETACH_REASON_* bitmask                */
    uint8_t   route_was;      /* RouteTarget before detach (0/1/2)      */
    uint8_t   shell_n;        /* ShellN param at time of event (0=unknown) */
    uint8_t   phase18;        /* op_count % 18   — gate heartbeat       */
    uint16_t  phase288;       /* op_count % 288  — World A checkpoint   */
    uint16_t  phase306;       /* op_count % 306  — World B checkpoint   */
    /* derived flag (packed into phase fields on push):
     *   twin_window = phase288 < 18 || phase306 < 18
     *   → anomaly during binary↔ternary crossing interval              */
    /* NOTE: 8+8+8 + 1+1+1+1+2+2 = 32B exactly */
} DetachEntry;                /* 32B — all phase fields frozen at capture */
/*
 * ISOLATION GUARANTEE (GPT warning, 2026-03):
 *   angular_addr = PHI space (global frame) — NEVER shell-relative index.
 *   shell_n      = snapshot of topology at moment of anomaly.
 *   These two fields together allow exact replay regardless of ShellN changes.
 *   Rule: Detach flush lifecycle is NEVER tied to topology events.
 */

typedef char _detach_entry_sz[(sizeof(DetachEntry) == 32u) ? 1 : -1];

/* twin_window: anomaly during the 18-op crossing interval between worlds */
static inline int detach_is_twin_window(const DetachEntry *e)
{
    return e && (e->phase288 < 18u || e->phase306 < 18u);
}

/* ── DetachStats ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t  pushed;         /* total entries pushed to ring           */
    uint64_t  flushed;        /* total entries written to delta         */
    uint64_t  overflows;      /* times ring was full → drop oldest      */
    uint64_t  flush_passes;   /* flush thread wake-ups                  */
    uint64_t  reason_geo;     /* DETACH_REASON_GEO_INVALID count        */
    uint64_t  reason_drift;   /* DETACH_REASON_GHOST_DRIFT count        */
    uint64_t  reason_circle;  /* DETACH_REASON_UNIT_CIRCLE count        */
} DetachStats;

/* ══════════════════════════════════════════════════════════════════
 * DetachLane — the complete detach subsystem
 *
 * Cache-line layout:
 *   head (producer) — cache line 0
 *   tail (consumer) — cache line 1
 *   ring data       — separate (avoids false sharing)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* producer — cache line 0 */
    volatile uint32_t  head __attribute__((aligned(64)));
    uint32_t           _hp[15];

    /* consumer — cache line 1 */
    volatile uint32_t  tail __attribute__((aligned(64)));
    uint32_t           _tp[15];

    /* ring data */
    DetachEntry        ring[DETACH_RING_SIZE];

    /* delta writer (not owned — pointer to PipelineWire's delta) */
    DeltaWriter       *delta;

    /* stats */
    DetachStats        stats;
    uint32_t           push_seq;   /* monotonic push counter             */

    /* async flush thread */
    volatile int       running;
    int                thread_started;
    pthread_t          flush_thread;

    uint32_t           magic;

    /* Tail summon: called from drain thread with each batch
     * Set via detach_lane_set_mesh() after mesh_init().
     * NULL = drain to delta only (no Tail update).            */
    MeshIngestFn       mesh_cb;   /* mesh_ingest_batch fn ptr  */
    struct Mesh_s     *mesh_ctx;  /* Mesh instance             */
} DetachLane;

/* ── timestamp helper ────────────────────────────────────────────── */
static inline uint64_t _detach_now_ns(void)
{
#if defined(_WIN32) || defined(_WIN64)
    return 0; /* Windows: caller can fill in if needed */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ══════════════════════════════════════════════════════════════════
 * detach_lane_push — HOT PATH (producer, lock-free)
 *
 * On ring full: drops oldest entry (overwrite), increments overflow.
 * Never blocks. Never waits for disk.
 *
 * Returns 1 on normal push, 0 on overflow (oldest evicted).
 * ══════════════════════════════════════════════════════════════════ */
static inline int detach_lane_push(DetachLane *dl,
                                   uint64_t    value,
                                   uint64_t    angular_addr,
                                   uint8_t     reason,
                                   uint8_t     route_was,
                                   uint8_t     shell_n,
                                   uint64_t    op_count)
{
    if (!dl) return 0;

    uint32_t h = dl->head;
    uint32_t t = __atomic_load_n(&dl->tail, __ATOMIC_ACQUIRE);
    int overflow = 0;

    /* ring full: advance tail to drop oldest (never block) */
    if ((uint32_t)(h - t) >= DETACH_RING_SIZE) {
        __atomic_store_n(&dl->tail, t + 1u, __ATOMIC_RELEASE);
        __atomic_fetch_add(&dl->stats.overflows, 1, __ATOMIC_RELAXED);
        overflow = 1;
    }

    DetachEntry *e  = &dl->ring[h & DETACH_RING_MASK];
    e->value        = value;
    e->angular_addr = angular_addr;
    e->timestamp_ns = _detach_now_ns();
    e->reason       = reason | (overflow ? DETACH_REASON_OVERFLOW : 0);
    e->route_was    = route_was;
    e->shell_n      = shell_n;   /* frozen snapshot — never update after push */
    e->phase18      = (uint8_t)(op_count % 18u);
    e->phase288     = (uint16_t)(op_count % 288u);
    e->phase306     = (uint16_t)(op_count % 306u);
    /* seq encoded in phase306 high bits — no separate field needed */
    (void)(++dl->push_seq);  /* keep counter for stats */

    __atomic_store_n(&dl->head, h + 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&dl->stats.pushed, 1, __ATOMIC_RELAXED);

    /* reason counters */
    if (reason & DETACH_REASON_GEO_INVALID)
        __atomic_fetch_add(&dl->stats.reason_geo, 1, __ATOMIC_RELAXED);
    if (reason & DETACH_REASON_GHOST_DRIFT)
        __atomic_fetch_add(&dl->stats.reason_drift, 1, __ATOMIC_RELAXED);
    if (reason & DETACH_REASON_UNIT_CIRCLE)
        __atomic_fetch_add(&dl->stats.reason_circle, 1, __ATOMIC_RELAXED);

    return !overflow;
}

/* ══════════════════════════════════════════════════════════════════
 * detach_flush_pass — consumer (async thread or foreground)
 *
 * Pulls up to DETACH_FLUSH_BATCH entries and writes to delta lane 53.
 * Returns number of entries flushed.
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t detach_flush_pass(DetachLane *dl)
{
    if (!dl) return 0;

    uint32_t t = dl->tail;
    uint32_t h = __atomic_load_n(&dl->head, __ATOMIC_ACQUIRE);
    uint32_t avail = h - t;
    if (avail == 0) return 0;
    if (avail > DETACH_FLUSH_BATCH) avail = DETACH_FLUSH_BATCH;

    /* build DiamondBlock batch (for delta lane) + entry batch (for Mesh) */
    DiamondBlock blk[DETACH_FLUSH_BATCH];
    DetachEntry  entry_batch[DETACH_FLUSH_BATCH];
    for (uint32_t i = 0; i < avail; i++) {
        DetachEntry *e = &dl->ring[(t + i) & DETACH_RING_MASK];
        entry_batch[i] = *e;   /* copy for Mesh callback              */
        memset(&blk[i], 0, sizeof(blk[i]));
        blk[i].data[0] = e->value;
        blk[i].data[1] = e->angular_addr;
        blk[i].data[2] = e->timestamp_ns;
        blk[i].data[3] = ((uint64_t)e->reason)
                       | ((uint64_t)e->route_was << 8)
                       | ((uint64_t)e->shell_n   << 16)
                       | ((uint64_t)e->phase18   << 24);
        blk[i].data[4] = ((uint64_t)e->phase18)
                       | ((uint64_t)e->phase288 << 8)
                       | ((uint64_t)e->phase306 << 24)
                       | ((uint64_t)detach_is_twin_window(e) << 48);
        /* data[5..7] reserved for Rubik recovery / Shatter */
    }

    __atomic_store_n(&dl->tail, t + avail, __ATOMIC_RELEASE);
    dl->stats.flush_passes++;

    /* write to isolated delta lane */
    if (dl->delta) {
        delta_append_v4(dl->delta, DETACH_DELTA_LANE, blk, avail);
        dl->stats.flushed += avail;
    }

    /* Tail summon: forward batch to Mesh (Voronoi+Delaunay+Tail update) */
    if (dl->mesh_cb && dl->mesh_ctx)
        dl->mesh_cb(dl->mesh_ctx, entry_batch, avail);

    return avail;
}

/* ── async flush thread ──────────────────────────────────────────── */
static void *_detach_flush_thread(void *arg)
{
    DetachLane *dl = (DetachLane *)arg;
    struct timespec ts = { 0, DETACH_FLUSH_US * 1000L };

    while (__atomic_load_n(&dl->running, __ATOMIC_ACQUIRE)) {
        detach_flush_pass(dl);
        nanosleep(&ts, NULL);
    }
    /* drain remaining on shutdown */
    uint32_t n;
    do { n = detach_flush_pass(dl); } while (n == DETACH_FLUSH_BATCH);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════
 * init / start / stop
 * ══════════════════════════════════════════════════════════════════ */
static inline int detach_lane_init(DetachLane  *dl,
                                   DeltaWriter *delta)
{
    if (!dl) return -1;
    memset(dl, 0, sizeof(*dl));
    dl->delta         = delta;
    dl->running       = 0;
    dl->thread_started = 0;
    dl->magic         = DETACH_MAGIC;
    return 0;
}

static inline int detach_lane_start(DetachLane *dl)
{
    if (!dl || dl->thread_started) return -1;
    __atomic_store_n(&dl->running, 1, __ATOMIC_RELEASE);
    int rc = pthread_create(&dl->flush_thread, NULL,
                            _detach_flush_thread, dl);
    if (rc == 0) dl->thread_started = 1;
    return rc;
}

/* detach_lane_set_mesh — wire Mesh as Tail summon target
 * Call AFTER both detach_lane_init() and mesh_init().
 * fn = (MeshIngestFn)mesh_ingest_batch  */
static inline void detach_lane_set_mesh(DetachLane  *dl,
                                         MeshIngestFn fn,
                                         struct Mesh_s *mesh)
{
    if (!dl) return;
    dl->mesh_cb  = fn;
    dl->mesh_ctx = mesh;
}

static inline int detach_lane_stop(DetachLane *dl)
{
    if (!dl || !dl->thread_started) return -1;
    __atomic_store_n(&dl->running, 0, __ATOMIC_RELEASE);
    pthread_join(dl->flush_thread, NULL);
    dl->thread_started = 0;
    return 0;
}

/* foreground drain (no thread) */
static inline uint32_t detach_lane_drain(DetachLane *dl)
{
    if (!dl) return 0;
    uint32_t total = 0, n;
    do { n = detach_flush_pass(dl); total += n; }
    while (n == DETACH_FLUSH_BATCH);
    return total;
}

/* ── occupancy & pressure ────────────────────────────────────────── */
static inline uint32_t detach_lane_occupancy_pct(const DetachLane *dl)
{
    if (!dl) return 0;
    uint32_t used = dl->head - dl->tail;
    return (used * 100u) / DETACH_RING_SIZE;
}

static inline int detach_lane_under_pressure(const DetachLane *dl)
{
    return dl && detach_lane_occupancy_pct(dl) >= 75u;
}

/* ── stats ───────────────────────────────────────────────────────── */
static inline void detach_lane_stats(const DetachLane *dl)
{
    if (!dl) return;
    const DetachStats *s = &dl->stats;
    uint64_t tot = s->pushed ? s->pushed : 1;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  Detach Safety Layer Stats                      ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ Pushed:       %10llu                        ║\n",
           (unsigned long long)s->pushed);
    printf("║ Flushed:      %10llu                        ║\n",
           (unsigned long long)s->flushed);
    printf("║ Overflows:    %10llu (%3llu%% drop-oldest)    ║\n",
           (unsigned long long)s->overflows,
           (unsigned long long)(s->overflows * 100u / tot));
    printf("║ Geo invalid:  %10llu                        ║\n",
           (unsigned long long)s->reason_geo);
    printf("║ Ghost drift:  %10llu                        ║\n",
           (unsigned long long)s->reason_drift);
    printf("║ Circle fail:  %10llu                        ║\n",
           (unsigned long long)s->reason_circle);
    printf("║ Flush passes: %10llu                        ║\n",
           (unsigned long long)s->flush_passes);
    printf("║ Occupancy:    %9u%%                       ║\n",
           detach_lane_occupancy_pct(dl));
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_DETACH_LANE_H */
