/*
 * pogls_admin_snapshot.h — S21: Admin Observability Snapshot
 *
 * Single struct ครอบคลุมทุก layer:
 *   L0 Pipeline (PipelineWire)
 *   L1 SDK     (PoglsCtx / KVBridge / GpuKVCtx)
 *   L2 Hydra   (HydraCore / HydraHead × N)
 *   L3 Snapshot state machine (BranchHeader / SnapshotHeader)
 *   L4 Audit   (AuditContext)
 *
 * Usage:
 *   PoglsAdminSnapshot snap;
 *   pogls_admin_snapshot_collect(&snap, ctx, gpu, hydra, audit, branch, head, pw);
 *   pogls_admin_snapshot_print(&snap);          // human readable
 *   pogls_admin_snapshot_json(&snap, buf, len); // visualizer feed
 *
 * Build (no extra deps — header-only):
 *   Include after all layer headers.
 */

#ifndef POGLS_ADMIN_SNAPSHOT_H
#define POGLS_ADMIN_SNAPSHOT_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

/* forward-declare to avoid hard coupling — callers include real headers */
#ifndef POGLS_SDK_H
typedef void PoglsCtx;
#endif
/* GpuKVCtx stub — real def comes from kv_bridge_gpu.h */
#ifndef KV_BRIDGE_GPU_H
typedef struct { uint64_t total_sent; uint32_t total_overflow; uint32_t stage_n; } GpuKVCtx;
#endif
#ifndef POGLS_HYDRA_H
typedef void POGLS_HydraCore;
#endif
#ifndef POGLS_AUDIT_H
typedef void POGLS_AuditContext;
#endif

#define ADMIN_SNAP_MAGIC     0x504F4731UL   /* "POG1" */
#define ADMIN_MAX_HEADS      8
#define ADMIN_MAX_TILES      256

/* ═══════════════════════════════════════════════════════════════════
   SUB-SNAPSHOTS per layer
   ═══════════════════════════════════════════════════════════════════ */

/* ── L0: Pipeline Wire ───────────────────────────────────────────── */
typedef struct {
    uint32_t total_ops;
    uint32_t qrpn_fails;
    uint32_t audit_fails;    /* last batch */
    uint32_t sig_fails;      /* last batch */
    int32_t  score_paths;    /* of 18 passed sig */
} AdminPipelineSnap;

/* ── L1a: SDK top-level ──────────────────────────────────────────── */
typedef struct {
    uint64_t writes;
    uint64_t reads;
    uint64_t qrpns;
    /* L1 GeoCache */
    uint64_t l1_hits;
    uint64_t l1_misses;
    float    l1_hit_pct;     /* 0.0–100.0 */
} AdminSDKSnap;

/* ── L1b: KVBridge CPU ───────────────────────────────────────────── */
typedef struct {
    uint32_t kv_live;
    uint32_t kv_tombstones;
    uint32_t kv_max_probe;
    uint64_t kv_puts;
    uint64_t kv_gets;
    uint64_t kv_misses;
    float    kv_hit_pct;
    float    kv_load_pct;    /* live / 5734 × 100 */
    float    tomb_pct;       /* tomb / (live+tomb) × 100 */
    uint32_t flush_count;
    uint32_t ring_dropped;
    uint32_t ring_backlog[4];  /* per-lane: head-tail */
} AdminKVSnap;

/* ── L1c: GPU KV ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t total_sent;
    uint32_t total_overflow;
    uint32_t stage_pending;
    float    overflow_pct;   /* overflow / sent × 100 */
    uint8_t  available;      /* 0 = CPU-only build */
} AdminGPUSnap;

/* ── L2: Hydra head (one entry) ──────────────────────────────────── */
typedef struct {
    uint8_t  head_id;
    uint8_t  status;            /* head_status_t enum value */
    uint8_t  mode;              /* branch_mode_t */
    uint8_t  pad;
    uint32_t n_bits;
    uint64_t write_count;
    uint64_t snapshot_count;
    uint64_t compaction_count;
    uint32_t anomaly_count;
    uint32_t migration_count;
    uint32_t current_block_count;
    uint32_t peak_block_count;
    uint64_t last_active_ms;
    uint64_t last_certified_snap_id;
} AdminHeadSnap;

/* ── L2: Hydra core ──────────────────────────────────────────────── */
typedef struct {
    uint8_t  active_count;
    uint64_t core_size;
    uint64_t core_append_offset;
    float    core_fill_pct;
    uint64_t radar_incident_count;
    uint64_t radar_spawn_count;
    uint64_t radar_retract_count;
    AdminHeadSnap heads[ADMIN_MAX_HEADS];
    uint8_t  head_count;
} AdminHydraSnap;

/* ── L3: Snapshot state ──────────────────────────────────────────── */
typedef struct {
    uint64_t snapshot_id;
    uint64_t branch_id;
    uint8_t  state;          /* snap_state_t */
    uint8_t  is_checkpoint;
    uint8_t  audit_health_at_promo;
    uint8_t  pad;
    uint64_t parent_snapshot_id;
    uint64_t created_at_ms;
    uint32_t effective_timeout_ms;
    /* branch summary */
    uint8_t  branch_mode;
    uint64_t branch_snapshot_count;
    uint64_t last_certified_id;
    uint64_t last_auto_id;
} AdminSnapStateSnap;

/* ── L4: Audit ───────────────────────────────────────────────────── */
typedef struct {
    uint8_t  health;           /* audit_health_t */
    uint32_t tile_count;
    uint64_t total_scans;
    uint64_t total_anomalies;
    uint64_t last_scan_at_ms;
    uint32_t signal_queue_depth;   /* head - tail */
    /* per-tile compact view */
    uint8_t  tile_state[ADMIN_MAX_TILES];        /* tile_state_t */
    uint8_t  tile_anomaly[ADMIN_MAX_TILES];      /* anomaly_flags bitmask */
    uint16_t tile_blocks_anomalous[ADMIN_MAX_TILES];
} AdminAuditSnap;

/* ═══════════════════════════════════════════════════════════════════
   MASTER SNAPSHOT
   ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t          magic;          /* ADMIN_SNAP_MAGIC */
    uint64_t          collected_at_ms;
    AdminPipelineSnap pipeline;
    AdminSDKSnap      sdk;
    AdminKVSnap       kv;
    AdminGPUSnap      gpu;
    AdminHydraSnap    hydra;
    AdminSnapStateSnap snapstate;
    AdminAuditSnap    audit;
} PoglsAdminSnapshot;

/* ═══════════════════════════════════════════════════════════════════
   COLLECT (typed version — include real headers before this file)
   ═══════════════════════════════════════════════════════════════════ */

#ifdef POGLS_SDK_H   /* only if real SDK types are available */
#include <stdatomic.h>

static inline uint64_t _admin_now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000ULL + (uint64_t)(t.tv_nsec / 1000000);
}

static inline void pogls_admin_collect_sdk(PoglsAdminSnapshot *out,
                                            PoglsCtx *ctx,
                                            GpuKVCtx *gpu)
{
    /* SDK top */
    out->sdk.writes  = ctx->writes;
    out->sdk.reads   = ctx->reads;
    out->sdk.qrpns   = ctx->qrpns;
    out->sdk.l1_hits   = ctx->l1.hits;
    out->sdk.l1_misses = ctx->l1.misses;
    uint64_t l1t = ctx->l1.hits + ctx->l1.misses;
    out->sdk.l1_hit_pct = l1t ? (float)(100.0 * ctx->l1.hits / l1t) : 0.f;

    /* KV Bridge */
    KVBridge *b = &ctx->kv;
    out->kv.kv_live        = b->cpu.count;
    out->kv.kv_tombstones  = b->cpu.tombstones;
    out->kv.kv_max_probe   = b->cpu.max_probe;
    out->kv.kv_puts        = b->cpu.puts;
    out->kv.kv_gets        = b->cpu.gets;
    out->kv.kv_misses      = b->cpu.misses;
    uint64_t kvhit = b->cpu.gets > b->cpu.misses ? b->cpu.gets - b->cpu.misses : 0;
    out->kv.kv_hit_pct  = b->cpu.gets ? (float)(100.0 * kvhit / b->cpu.gets) : 0.f;
    out->kv.kv_load_pct = (float)(100.0 * b->cpu.count / 5734.0);
    uint32_t tot = b->cpu.count + b->cpu.tombstones;
    out->kv.tomb_pct    = tot ? (float)(100.0 * b->cpu.tombstones / tot) : 0.f;
    out->kv.flush_count  = b->flush_count;
    out->kv.ring_dropped = b->dropped;
    for (int i = 0; i < 4; i++) {
        uint32_t h = atomic_load(&b->lanes[i].head);
        uint32_t t = atomic_load(&b->lanes[i].tail);
        out->kv.ring_backlog[i] = h - t;
    }

    /* GPU */
    out->gpu.available = (gpu != NULL) ? 1 : 0;
    if (gpu) {
        out->gpu.total_sent      = gpu->total_sent;
        out->gpu.total_overflow  = gpu->total_overflow;
        out->gpu.stage_pending   = gpu->stage_n;
        out->gpu.overflow_pct    = gpu->total_sent
            ? (float)(100.0 * gpu->total_overflow / gpu->total_sent) : 0.f;
    }
}
#endif /* POGLS_SDK_H */

#ifdef POGLS_HYDRA_H
static inline void pogls_admin_collect_hydra(PoglsAdminSnapshot *out,
                                              POGLS_HydraCore *hydra)
{
    out->hydra.active_count        = hydra->active_count;
    out->hydra.core_size           = hydra->core_size;
    out->hydra.core_append_offset  = hydra->core_append_offset;
    out->hydra.core_fill_pct       = hydra->core_size
        ? (float)(100.0 * hydra->core_append_offset / hydra->core_size) : 0.f;
    out->hydra.radar_incident_count = hydra->radar_incident_count;
    out->hydra.radar_spawn_count    = hydra->radar_spawn_count;
    out->hydra.radar_retract_count  = hydra->radar_retract_count;

    uint8_t n = hydra->active_count < ADMIN_MAX_HEADS
        ? hydra->active_count : ADMIN_MAX_HEADS;
    out->hydra.head_count = n;
    for (uint8_t i = 0; i < n; i++) {
        POGLS_HydraHead *h = &hydra->heads[i];
        AdminHeadSnap   *s = &out->hydra.heads[i];
        s->head_id               = h->head_id;
        s->status                = h->status;
        s->n_bits                = h->n_bits_local;
        s->write_count           = h->write_count;
        s->snapshot_count        = h->snapshot_count;
        s->compaction_count      = h->compaction_count;
        s->anomaly_count         = h->anomaly_count;
        s->migration_count       = h->migration_count;
        s->current_block_count   = h->current_block_count;
        s->peak_block_count      = h->peak_block_count;
        s->last_active_ms        = h->last_active_ms;
        s->last_certified_snap_id= h->last_certified_snap_id;
    }
}
#endif /* POGLS_HYDRA_H */

#ifdef POGLS_SNAPSHOT_H
static inline void pogls_admin_collect_snapstate(PoglsAdminSnapshot *out,
                                                  const POGLS_BranchHeader *branch,
                                                  const POGLS_SnapshotHeader *snap)
{
    out->snapstate.snapshot_id          = snap->snapshot_id;
    out->snapstate.branch_id            = snap->branch_id;
    out->snapstate.state                = snap->state;
    out->snapstate.is_checkpoint        = snap->is_checkpoint;
    out->snapstate.audit_health_at_promo= snap->audit_health_at_promo;
    out->snapstate.parent_snapshot_id   = snap->parent_snapshot_id;
    out->snapstate.created_at_ms        = snap->created_at_ms;
    out->snapstate.effective_timeout_ms = snap->effective_timeout_ms;
    if (branch) {
        out->snapstate.branch_mode          = branch->mode;
        out->snapstate.branch_snapshot_count= branch->snapshot_count;
        out->snapstate.last_certified_id    = branch->last_certified_id;
        out->snapstate.last_auto_id         = branch->last_auto_id;
    }
}
#endif /* POGLS_SNAPSHOT_H */

#ifdef POGLS_AUDIT_H
static inline void pogls_admin_collect_audit(PoglsAdminSnapshot *out,
                                              const POGLS_AuditContext *audit)
{
    out->audit.health             = (uint8_t)audit->health;
    out->audit.tile_count         = audit->tile_count;
    out->audit.total_scans        = audit->total_scans;
    out->audit.total_anomalies    = audit->total_anomalies;
    out->audit.last_scan_at_ms    = audit->last_scan_at_ms;
    uint32_t qd = (audit->signal_head - audit->signal_tail + AUDIT_MAX_SIGNAL_QUEUE)
                   % AUDIT_MAX_SIGNAL_QUEUE;
    out->audit.signal_queue_depth = qd;
    uint32_t n = audit->tile_count < ADMIN_MAX_TILES
        ? audit->tile_count : ADMIN_MAX_TILES;
    for (uint32_t i = 0; i < n; i++) {
        out->audit.tile_state[i]            = audit->tiles[i].state;
        out->audit.tile_anomaly[i]          = audit->tiles[i].anomaly_flags;
        out->audit.tile_blocks_anomalous[i] = audit->tiles[i].blocks_anomalous;
    }
}
#endif /* POGLS_AUDIT_H */

#ifdef POGLS_PIPELINE_WIRE_H
static inline void pogls_admin_collect_pipeline(PoglsAdminSnapshot *out,
                                                 const PipelineWire *pw,
                                                 const BatchVerdict *bv)
{
    out->pipeline.total_ops  = pw->total_ops;
    out->pipeline.qrpn_fails = pw->qrpn_fails;
    if (bv) {
        out->pipeline.audit_fails = bv->audit_fails;
        out->pipeline.sig_fails   = bv->sig_fails;
        out->pipeline.score_paths = bv->score_paths;
    }
}
#endif /* POGLS_PIPELINE_WIRE_H */

/* ═══════════════════════════════════════════════════════════════════
   PRINT — human readable (always available, no deps)
   ═══════════════════════════════════════════════════════════════════ */

static const char *_snap_state_str(uint8_t s) {
    switch(s) {
        case 0: return "PENDING";
        case 1: return "CERTIFIED";
        case 2: return "AUTO";
        case 3: return "VOID";
        case 4: return "MIGRATED";
        default: return "?";
    }
}
static const char *_audit_health_str(uint8_t h) {
    switch(h) { case 0: return "OK"; case 1: return "DEGRADED"; case 2: return "OFFLINE"; default: return "?"; }
}
static const char *_branch_mode_str(uint8_t m) {
    switch(m) { case 0: return "NORMAL"; case 1: return "SAFE_MODE"; case 2: return "MIGRATION"; default: return "?"; }
}
static const char *_tile_state_str(uint8_t s) {
    switch(s) { case 0: return "IDLE"; case 1: return "SCAN"; case 2: return "CLEAN"; case 3: return "ANOMALY"; case 4: return "CERT"; default: return "?"; }
}

static inline void pogls_admin_snapshot_print(const PoglsAdminSnapshot *s) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  POGLS ADMIN SNAPSHOT  t=%" PRIu64 " ms\n", s->collected_at_ms);
    printf("╠══ L0 PIPELINE ═══════════════════════════════╣\n");
    printf("  ops=%u  qrpn_fail=%u  audit_fail=%u  sig_fail=%u  score=%d/18\n",
           s->pipeline.total_ops, s->pipeline.qrpn_fails,
           s->pipeline.audit_fails, s->pipeline.sig_fails, s->pipeline.score_paths);
    printf("╠══ L1 SDK ════════════════════════════════════╣\n");
    printf("  writes=%" PRIu64 "  reads=%" PRIu64 "  qrpns=%" PRIu64 "\n",
           s->sdk.writes, s->sdk.reads, s->sdk.qrpns);
    printf("  L1 hit=%.1f%%  (%" PRIu64 "/%" PRIu64 ")\n",
           s->sdk.l1_hit_pct, s->sdk.l1_hits, s->sdk.l1_hits + s->sdk.l1_misses);
    printf("╠══ L1 KV BRIDGE ══════════════════════════════╣\n");
    printf("  live=%u  tomb=%u  max_probe=%u\n",
           s->kv.kv_live, s->kv.kv_tombstones, s->kv.kv_max_probe);
    printf("  load=%.1f%%  tomb=%.1f%%  kv_hit=%.1f%%\n",
           s->kv.kv_load_pct, s->kv.tomb_pct, s->kv.kv_hit_pct);
    printf("  flush=%u  dropped=%u  ring_backlog=[%u,%u,%u,%u]\n",
           s->kv.flush_count, s->kv.ring_dropped,
           s->kv.ring_backlog[0], s->kv.ring_backlog[1],
           s->kv.ring_backlog[2], s->kv.ring_backlog[3]);
    printf("╠══ L1 GPU KV ═════════════════════════════════╣\n");
    if (s->gpu.available)
        printf("  sent=%" PRIu64 "  overflow=%u(%.2f%%)  staged=%u\n",
               s->gpu.total_sent, s->gpu.total_overflow,
               s->gpu.overflow_pct, s->gpu.stage_pending);
    else
        printf("  [CPU-only build]\n");
    printf("╠══ L2 HYDRA ══════════════════════════════════╣\n");
    printf("  active=%u  fill=%.1f%%  incidents=%" PRIu64
           "  spawns=%" PRIu64 "  retracts=%" PRIu64 "\n",
           s->hydra.active_count, s->hydra.core_fill_pct,
           s->hydra.radar_incident_count,
           s->hydra.radar_spawn_count, s->hydra.radar_retract_count);
    for (uint8_t i = 0; i < s->hydra.head_count; i++) {
        const AdminHeadSnap *h = &s->hydra.heads[i];
        printf("  head[%u] status=%u n_bits=%u writes=%" PRIu64
               " snaps=%" PRIu64 " anom=%u migr=%u blocks=%u/%u\n",
               h->head_id, h->status, h->n_bits,
               h->write_count, h->snapshot_count,
               h->anomaly_count, h->migration_count,
               h->current_block_count, h->peak_block_count);
    }
    printf("╠══ L3 SNAPSHOT STATE ═════════════════════════╣\n");
    printf("  snap_id=%" PRIu64 "  branch=%" PRIu64 "  state=%s  ckpt=%u\n",
           s->snapstate.snapshot_id, s->snapstate.branch_id,
           _snap_state_str(s->snapstate.state), s->snapstate.is_checkpoint);
    printf("  branch_mode=%s  snaps=%" PRIu64
           "  last_cert=%" PRIu64 "  last_auto=%" PRIu64 "\n",
           _branch_mode_str(s->snapstate.branch_mode),
           s->snapstate.branch_snapshot_count,
           s->snapstate.last_certified_id, s->snapstate.last_auto_id);
    printf("  timeout=%u ms  parent=%" PRIu64 "\n",
           s->snapstate.effective_timeout_ms, s->snapstate.parent_snapshot_id);
    printf("╠══ L4 AUDIT ══════════════════════════════════╣\n");
    printf("  health=%s  tiles=%u  scans=%" PRIu64
           "  anomalies=%" PRIu64 "  sig_q=%u\n",
           _audit_health_str(s->audit.health), s->audit.tile_count,
           s->audit.total_scans, s->audit.total_anomalies,
           s->audit.signal_queue_depth);
    uint32_t anom_tiles = 0;
    for (uint32_t i = 0; i < s->audit.tile_count && i < ADMIN_MAX_TILES; i++)
        if (s->audit.tile_anomaly[i]) anom_tiles++;
    printf("  anomalous_tiles=%u/%u\n", anom_tiles, s->audit.tile_count);
    /* print only anomalous tiles */
    for (uint32_t i = 0; i < s->audit.tile_count && i < ADMIN_MAX_TILES; i++) {
        if (!s->audit.tile_anomaly[i]) continue;
        printf("  !! tile[%u] state=%s flags=0x%02x bad_blocks=%u\n",
               i, _tile_state_str(s->audit.tile_state[i]),
               s->audit.tile_anomaly[i], s->audit.tile_blocks_anomalous[i]);
    }
    printf("╚══════════════════════════════════════════════╝\n");
}

/* ═══════════════════════════════════════════════════════════════════
   JSON SERIALIZER — for visualizer / Prometheus bridge
   buf must be at least 4096 bytes
   Returns bytes written (< len) or -1 if truncated
   ═══════════════════════════════════════════════════════════════════ */
static inline int pogls_admin_snapshot_json(const PoglsAdminSnapshot *s,
                                             char *buf, int len)
{
    int n = 0;
#define J(...) do { int r=snprintf(buf+n, len-n, __VA_ARGS__); \
                    if(r<0||r>=len-n) return -1; n+=r; } while(0)
    J("{");
    J("\"t\":%" PRIu64 ",", s->collected_at_ms);
    /* pipeline */
    J("\"pipeline\":{\"ops\":%u,\"qrpn_fail\":%u,"
      "\"audit_fail\":%u,\"sig_fail\":%u,\"score\":%d},",
      s->pipeline.total_ops, s->pipeline.qrpn_fails,
      s->pipeline.audit_fails, s->pipeline.sig_fails, s->pipeline.score_paths);
    /* sdk */
    J("\"sdk\":{\"writes\":%" PRIu64 ",\"reads\":%" PRIu64
      ",\"qrpns\":%" PRIu64 ",\"l1_hit_pct\":%.2f},",
      s->sdk.writes, s->sdk.reads, s->sdk.qrpns, s->sdk.l1_hit_pct);
    /* kv */
    J("\"kv\":{\"live\":%u,\"tomb\":%u,\"max_probe\":%u,"
      "\"load_pct\":%.2f,\"tomb_pct\":%.2f,\"hit_pct\":%.2f,"
      "\"flush\":%u,\"dropped\":%u,"
      "\"ring\":[%u,%u,%u,%u]},",
      s->kv.kv_live, s->kv.kv_tombstones, s->kv.kv_max_probe,
      s->kv.kv_load_pct, s->kv.tomb_pct, s->kv.kv_hit_pct,
      s->kv.flush_count, s->kv.ring_dropped,
      s->kv.ring_backlog[0], s->kv.ring_backlog[1],
      s->kv.ring_backlog[2], s->kv.ring_backlog[3]);
    /* gpu */
    J("\"gpu\":{\"avail\":%u,\"sent\":%" PRIu64
      ",\"overflow\":%u,\"overflow_pct\":%.2f,\"staged\":%u},",
      s->gpu.available, s->gpu.total_sent,
      s->gpu.total_overflow, s->gpu.overflow_pct, s->gpu.stage_pending);
    /* hydra */
    J("\"hydra\":{\"active\":%u,\"fill_pct\":%.2f,"
      "\"incidents\":%" PRIu64 ",\"spawns\":%" PRIu64
      ",\"retracts\":%" PRIu64 ",\"heads\":[",
      s->hydra.active_count, s->hydra.core_fill_pct,
      s->hydra.radar_incident_count,
      s->hydra.radar_spawn_count, s->hydra.radar_retract_count);
    for (uint8_t i = 0; i < s->hydra.head_count; i++) {
        const AdminHeadSnap *h = &s->hydra.heads[i];
        J("%s{\"id\":%u,\"status\":%u,\"n_bits\":%u,"
          "\"writes\":%" PRIu64 ",\"snaps\":%" PRIu64 ","
          "\"anom\":%u,\"migr\":%u,\"blocks\":%u,\"peak\":%u}",
          i?",":"", h->head_id, h->status, h->n_bits,
          h->write_count, h->snapshot_count,
          h->anomaly_count, h->migration_count,
          h->current_block_count, h->peak_block_count);
    }
    J("]},");
    /* snapstate */
    J("\"snap\":{\"id\":%" PRIu64 ",\"branch\":%" PRIu64
      ",\"state\":%u,\"ckpt\":%u,"
      "\"branch_mode\":%u,\"snap_count\":%" PRIu64
      ",\"last_cert\":%" PRIu64 ",\"last_auto\":%" PRIu64
      ",\"timeout_ms\":%u},",
      s->snapstate.snapshot_id, s->snapstate.branch_id,
      s->snapstate.state, s->snapstate.is_checkpoint,
      s->snapstate.branch_mode, s->snapstate.branch_snapshot_count,
      s->snapstate.last_certified_id, s->snapstate.last_auto_id,
      s->snapstate.effective_timeout_ms);
    /* audit */
    J("\"audit\":{\"health\":%u,\"tiles\":%u,"
      "\"scans\":%" PRIu64 ",\"anomalies\":%" PRIu64
      ",\"sig_q\":%u,\"tile_state\":[",
      s->audit.health, s->audit.tile_count,
      s->audit.total_scans, s->audit.total_anomalies,
      s->audit.signal_queue_depth);
    for (uint32_t i = 0; i < s->audit.tile_count && i < ADMIN_MAX_TILES; i++)
        J("%s%u", i?",":"", s->audit.tile_state[i]);
    J("],\"tile_anomaly\":[");
    for (uint32_t i = 0; i < s->audit.tile_count && i < ADMIN_MAX_TILES; i++)
        J("%s%u", i?",":"", s->audit.tile_anomaly[i]);
    J("]}");
    J("}");
#undef J
    return n;
}

/* ═══════════════════════════════════════════════════════════════════
   INIT helper — stamp magic + timestamp
   ═══════════════════════════════════════════════════════════════════ */
static inline void pogls_admin_snapshot_init(PoglsAdminSnapshot *s) {
    memset(s, 0, sizeof(*s));
    s->magic = ADMIN_SNAP_MAGIC;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    s->collected_at_ms = (uint64_t)ts.tv_sec * 1000ULL
                       + (uint64_t)(ts.tv_nsec / 1000000);
}

#endif /* POGLS_ADMIN_SNAPSHOT_H */
