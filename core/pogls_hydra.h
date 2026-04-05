#ifndef POGLS_HYDRA_H
#define POGLS_HYDRA_H

#include "pogls_v3.h"
#include "pogls_snapshot.h"

/* ═══════════════════════════════════════════════════════════════════════
   POGLS V3.1 — Hydra Core
   Branch lifecycle: Spawn → Active → (Safe/Migration) → Retract
   Local authority, Global visibility, Zero central override
   ═══════════════════════════════════════════════════════════════════════ */

#define HYDRA_MAGIC            "HYDR"
#define HYDRA_VERSION          0x31
#define HYDRA_MAX_HEADS        16
#define HYDRA_SPAWN_THRESHOLD  (512ULL << 20)
#define HYDRA_RETRACT_THRESHOLD (64ULL << 20)

/* ── Head status ────────────────────────────────────────────────────── */
typedef enum {
    HEAD_DORMANT    = 0,
    HEAD_SPAWNING   = 1,
    HEAD_ACTIVE     = 2,
    HEAD_SAFE       = 3,
    HEAD_MIGRATING  = 4,
    HEAD_RETRACTING = 5,
    HEAD_DEAD       = 6,
} head_status_t;

typedef enum {
    SPAWN_HIGH_DENSITY = 0x01,
    SPAWN_SPLIT        = 0x02,
    SPAWN_MERGE_IN     = 0x03,
    SPAWN_MANUAL       = 0x04,
} spawn_reason_t;

typedef enum {
    RETRACT_LEAN     = 0x01,
    RETRACT_COMPLETE = 0x02,
    RETRACT_FORCE    = 0x03,
} retract_reason_t;

/* ── Hydra Head (128B) ──────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    char     magic[4];
    uint8_t  head_id;
    uint8_t  status;               /* head_status_t  */
    uint8_t  spawn_reason;         /* spawn_reason_t */
    uint8_t  version;

    uint64_t zone_offset_start;
    uint64_t zone_offset_end;
    uint32_t n_bits_local;
    uint32_t topo_level_local;

    uint64_t branch_id;
    uint64_t parent_branch_id;
    uint64_t genesis_snapshot_id;
    uint64_t last_certified_snap_id;

    uint64_t spawned_at_ms;
    uint64_t last_active_ms;
    uint32_t effective_timeout_ms;
    uint32_t pad0;

    uint64_t write_count;
    uint64_t snapshot_count;
    uint64_t compaction_count;
    uint32_t anomaly_count;
    uint32_t migration_count;
    uint32_t current_block_count;
    uint32_t peak_block_count;
} POGLS_HydraHead;
/* 4+1+1+1+1+8+8+4+4+8+8+8+8+8+8+4+4+8+8+8+4+4+4+4 = 128B */

/* ── Forward declaration for self-ref callbacks ─────────────────────── */
typedef struct pogls_hydra_core_s POGLS_HydraCore;

/* ── Hydra Core ─────────────────────────────────────────────────────── */
struct pogls_hydra_core_s {
    char    magic[4];
    uint8_t version;
    uint8_t active_count;
    uint8_t pad[2];

    POGLS_HydraHead heads[HYDRA_MAX_HEADS];

    void    *core_mmap;
    uint64_t core_size;
    uint64_t core_append_offset;

    uint64_t radar_incident_count;
    uint64_t radar_spawn_count;
    uint64_t radar_retract_count;

    uint64_t spawn_threshold_bytes;
    uint64_t retract_threshold_bytes;

    /* Callbacks — NULL = skip */
    int (*on_spawn)(POGLS_HydraCore*, POGLS_HydraHead*);
    int (*on_retract)(POGLS_HydraCore*, POGLS_HydraHead*, retract_reason_t);
    int (*on_anomaly)(POGLS_HydraCore*, POGLS_HydraHead*, uint8_t);
};

/* ── Spawn Event (72B, append-only) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    char     magic[4];               /* "SPWN"        */
    uint8_t  head_id;
    uint8_t  reason;                 /* spawn_reason_t */
    uint16_t pad;
    uint64_t branch_id;
    uint64_t parent_branch_id;
    uint64_t zone_offset_start;
    uint64_t zone_offset_end;
    uint64_t genesis_snapshot_id;
    uint64_t spawned_at_ms;
    uint8_t  genesis_hash[16];
} POGLS_SpawnEvent;

/* ── Retract Event (48B, append-only) ──────────────────────────────── */
typedef struct __attribute__((packed)) {
    char     magic[4];               /* "RETR"         */
    uint8_t  head_id;
    uint8_t  reason;                 /* retract_reason_t */
    uint16_t pad;
    uint64_t branch_id;
    uint64_t last_certified_snap_id;
    uint64_t final_append_offset;
    uint64_t retracted_at_ms;
    uint32_t total_writes;
    uint32_t total_snapshots;
} POGLS_RetractEvent;

/* ── Spawn Config ───────────────────────────────────────────────────── */
typedef struct {
    uint64_t  zone_start;
    uint64_t  zone_end;
    uint64_t  parent_branch_id;
    uint32_t  n_bits;
    uint32_t  topo_level;
    spawn_reason_t reason;
    const POGLS_RetentionConfig* retention;
} POGLS_SpawnConfig;

/* ── Function Declarations ──────────────────────────────────────────── */

/* Core lifecycle */
POGLS_HydraCore* pogls_hydra_init(void* core_mmap, uint64_t core_size,
                                   uint64_t spawn_thresh,
                                   uint64_t retract_thresh);
void pogls_hydra_destroy(POGLS_HydraCore* hydra);

/* Head lifecycle */
POGLS_HydraHead* pogls_hydra_spawn(POGLS_HydraCore*        hydra,
                                    const POGLS_SpawnConfig* cfg,
                                    int                      event_fd);

int pogls_hydra_certify_genesis(POGLS_HydraCore* hydra, uint8_t head_id,
                                 const uint8_t genesis_hash[16]);

int pogls_hydra_retract(POGLS_HydraCore* hydra, uint8_t head_id,
                         retract_reason_t reason, int event_fd);

/* Write routing */
POGLS_HydraHead* pogls_hydra_route(POGLS_HydraCore* hydra,
                                    uint64_t byte_offset);

uint64_t pogls_hydra_append(POGLS_HydraCore* hydra, POGLS_HydraHead* head,
                             const void* data, uint64_t size);

/* Density & scaling */
int pogls_hydra_check_density(POGLS_HydraCore* hydra, uint8_t head_id);
int pogls_hydra_scale_up(POGLS_HydraHead* head, uint32_t new_n_bits);
int pogls_hydra_scale_down(POGLS_HydraHead* head, uint32_t new_n_bits);

/* Mode transitions */
int pogls_hydra_enter_safe(POGLS_HydraCore* hydra, uint8_t head_id);
int pogls_hydra_enter_migration(POGLS_HydraCore* hydra, uint8_t head_id);
int pogls_hydra_exit_recovery(POGLS_HydraCore* hydra, uint8_t head_id);

/* Event log */
int pogls_hydra_log_spawn(int fd, const POGLS_SpawnEvent* ev);
int pogls_hydra_log_retract(int fd, const POGLS_RetractEvent* ev);

/* Anomaly & isolation */
int pogls_hydra_handle_anomaly(POGLS_HydraCore* hydra, uint8_t head_id,
                                uint8_t anomaly_flags, uint64_t snapshot_id);
int pogls_hydra_isolate(POGLS_HydraCore* hydra, uint8_t head_id);

/* Visualizer */
void pogls_hydra_print_head(const POGLS_HydraHead* head);
void pogls_hydra_print_all(const POGLS_HydraCore* hydra);

/* Inline helpers */
static inline int head_is_writable(const POGLS_HydraHead* h)
{ return h->status == (uint8_t)HEAD_ACTIVE; }

static inline int head_is_alive(const POGLS_HydraHead* h)
{ return h->status >= (uint8_t)HEAD_SPAWNING
      && h->status <= (uint8_t)HEAD_RETRACTING; }

static inline uint64_t head_zone_size(const POGLS_HydraHead* h)
{ return h->zone_offset_end - h->zone_offset_start; }

static inline int head_needs_spawn(const POGLS_HydraHead* h, uint64_t thresh)
{ return head_zone_size(h) > thresh; }

#endif /* POGLS_HYDRA_H */
