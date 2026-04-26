#ifndef POGLS_AUDIT_H
#define POGLS_AUDIT_H

#include "pogls_v3.h"
#include "pogls_snapshot.h"

#define AUDIT_MAGIC            "ADIT"
#define AUDIT_VERSION          0x31
#define AUDIT_MAX_TILES        256
#define AUDIT_OVERLAP_RATIO    0.0625
#define AUDIT_MAX_SIGNAL_QUEUE 64

typedef enum {
    TILE_IDLE=0, TILE_SCANNING=1, TILE_CLEAN=2,
    TILE_ANOMALY=3, TILE_CERTIFIED=4
} tile_state_t;

typedef enum {
    ANOMALY_NONE=0x00, ANOMALY_HASH_MISMATCH=0x01,
    ANOMALY_COORD_DRIFT=0x02, ANOMALY_OVERLAP_DELTA=0x04,
    ANOMALY_WARP_CORRUPT=0x08, ANOMALY_DEEP_UNREADABLE=0x10,
    ANOMALY_SEQUENCE_BREAK=0x20
} anomaly_type_t;

typedef struct {
    uint64_t addr_start, addr_end;
    uint64_t overlap_start, overlap_end;
    uint8_t  state;
    uint8_t  anomaly_flags;
    uint16_t blocks_scanned, blocks_anomalous, pad;
    uint8_t  tile_hash[16], overlap_hash[8];
    uint64_t scanned_at_ms;
    uint32_t scan_duration_ms;
    uint64_t branch_id;
} POGLS_AuditTile;

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint8_t  anomaly_flags, severity;
    uint16_t pad;
    uint64_t branch_id, snapshot_id, deep_lane_offset, detected_at_ms;
    uint8_t  tile_hash[16];
} POGLS_AuditIncident;

typedef struct {
    char           magic[4];
    uint8_t        version;
    audit_health_t health;
    const void    *core_mmap;
    uint64_t       core_size;
    POGLS_AuditTile tiles[AUDIT_MAX_TILES];
    uint32_t        tile_count, n_bits;
    POGLS_AuditSignal signal_queue[AUDIT_MAX_SIGNAL_QUEUE];
    uint32_t          signal_head, signal_tail;
    uint64_t       total_scans, total_anomalies, last_scan_at_ms;
    uint32_t       last_scan_ms;
    uint8_t        prev_overlap_hash[AUDIT_MAX_TILES][8];
} POGLS_AuditContext;

typedef struct {
    uint32_t tile_count;
    double   overlap_ratio;
    uint32_t scan_interval_ms;
    uint8_t  full_scan;
} POGLS_AuditConfig;

POGLS_AuditContext* pogls_audit_init(const void *core_mmap, uint64_t core_size,
                                     uint32_t n_bits, const POGLS_AuditConfig *cfg);
void pogls_audit_destroy(POGLS_AuditContext *ctx);
int  pogls_audit_build_tiles(POGLS_AuditContext *ctx, const POGLS_AuditConfig *cfg);
int  pogls_audit_scan_tile(POGLS_AuditContext *ctx, uint32_t idx,
                            const POGLS_BranchHeader *branch);
int  pogls_audit_scan_pass(POGLS_AuditContext *ctx, const POGLS_BranchHeader *branch);
int  pogls_audit_check_overlap(POGLS_AuditContext *ctx, uint32_t idx);
int  pogls_audit_emit_certify(POGLS_AuditContext *ctx, uint64_t bid,
                               uint64_t sid, const uint8_t th[16]);
int  pogls_audit_emit_invalidate(POGLS_AuditContext *ctx, uint64_t bid, uint64_t sid);
int  pogls_audit_emit_anomaly(POGLS_AuditContext *ctx, uint64_t bid,
                               uint64_t sid, uint8_t flags);
int  pogls_audit_emit_health(POGLS_AuditContext *ctx, audit_health_t health);
int  pogls_audit_signal_push(POGLS_AuditContext *ctx, const POGLS_AuditSignal *sig);
int  pogls_audit_signal_pop(POGLS_AuditContext *ctx, POGLS_AuditSignal *out);
int  pogls_audit_log_incident(int fd, const POGLS_AuditIncident *inc);
void pogls_audit_set_health(POGLS_AuditContext *ctx, audit_health_t h);
void pogls_audit_print_tile(const POGLS_AuditTile *t, uint32_t idx);
void pogls_audit_print_health(const POGLS_AuditContext *ctx);
void pogls_audit_print_summary(const POGLS_AuditContext *ctx);

static inline int audit_queue_empty(const POGLS_AuditContext *c)
{ return c->signal_head==c->signal_tail; }
static inline int audit_queue_full(const POGLS_AuditContext *c)
{ return ((c->signal_head+1)%AUDIT_MAX_SIGNAL_QUEUE)==c->signal_tail; }
static inline int audit_tile_has_anomaly(const POGLS_AuditTile *t)
{ return t->anomaly_flags!=(uint8_t)ANOMALY_NONE; }

#endif /* POGLS_AUDIT_H */
