#ifndef POGLS_SNAPSHOT_H
#define POGLS_SNAPSHOT_H

#include "pogls_v3.h"

/* ═══════════════════════════════════════════════════════════════════════
   POGLS V3.1 — Snapshot State Machine
   Branch-isolated | Append-only | Audit-certified
   ═══════════════════════════════════════════════════════════════════════ */

/* ───────────────────────────────────────────────────────────────────────
   SNAPSHOT STATE
   Lifecycle: PENDING → CERTIFIED (happy path)
              PENDING → AUTO      (audit timeout)
              AUTO    → VOID      (audit invalidate, one-shot)
              VOID    → MIGRATED  (hard migration, new lineage)
   ─────────────────────────────────────────────────────────────────────── */
typedef enum {
    SNAP_PENDING             = 0,   /* เพิ่งสร้าง รอ Audit ACK            */
    SNAP_CONFIRMED_CERTIFIED = 1,   /* Audit ตรวจผ่าน — Absolute Truth     */
    SNAP_CONFIRMED_AUTO      = 2,   /* Timeout promote — Provisional Truth */
    SNAP_VOID                = 3,   /* Force Rollback (one-shot) — Dead    */
    SNAP_MIGRATED            = 4,   /* New lineage หลัง Hard Migration     */
} snap_state_t;

/* Audit health ณ เวลา auto-promote — บันทึกไว้เพื่อ trace */
typedef enum {
    AUDIT_HEALTH_OK       = 0,
    AUDIT_HEALTH_DEGRADED = 1,
    AUDIT_HEALTH_OFFLINE  = 2,
} audit_health_t;

/* Branch mode — กำหนดสิทธิ์ read/write */
typedef enum {
    BRANCH_NORMAL        = 0,   /* ทำงานปกติ                             */
    BRANCH_SAFE_MODE     = 1,   /* Recovery: read OK, write → 503        */
    BRANCH_MIGRATION     = 2,   /* Hard migration: read OK, write → 503  */
} branch_mode_t;

/* ───────────────────────────────────────────────────────────────────────
   SNAPSHOT HEADER (64B = 2^6, aligned)
   เก็บใน ORBS file ของ branch นั้น — ไม่แตะ Deep Lane
   ─────────────────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) POGLS_SnapshotHeader {
    /* Identity */
    uint64_t  snapshot_id;           /* [0x00] monotonic, ห้าม reuse      */
    uint64_t  branch_id;             /* [0x08] เจ้าของ branch             */

    /* State */
    uint8_t   state;                 /* [0x10] snap_state_t               */
    uint8_t   is_checkpoint;         /* [0x11] 1 = immune to cleanup      */
    uint8_t   checkpoint_confirmed;  /* [0x12] 1 = Audit ACK checkpoint   */
    uint8_t   audit_health_at_promo; /* [0x13] audit_health_t ตอน promote */

    /* Lineage */
    uint64_t  parent_snapshot_id;    /* [0x14] สืบสายมาจาก               */
    uint64_t  void_snapshot_id;      /* [0x1C] ถ้า MIGRATED: VOID ที่หนีมา*/
    uint64_t  migrate_from_id;       /* [0x24] CERTIFIED ที่ใช้เป็น base  */

    /* Core link */
    uint64_t  deep_lane_offset;      /* [0x2C] ชี้ไป Deep Block ใน Core  */
    uint32_t  deep_lane_count;       /* [0x34] จำนวน Deep Block ใน zone  */

    /* Timing */
    uint64_t  created_at_ms;         /* [0x38] Unix ms                   */
    uint32_t  effective_timeout_ms;  /* [0x40] Adaptive timeout ของ branch*/

    /* Integrity */
    uint8_t   snapshot_hash[16];     /* [0x44] MD5 ของ zone payload       */
                                     /*        (เก็บใน parity[32] ของ Deep)*/
    uint8_t   reserved[4];           /* [0x54] alignment                  */
} POGLS_SnapshotHeader;
/* Size: 8+8+1+1+1+1+8+8+8+8+4+8+4+16+4 = 88B — จงใจ > 64B
   เพราะ content สำคัญกว่า alignment ที่ layer นี้               */

/* ───────────────────────────────────────────────────────────────────────
   BRANCH HEADER (32B = 2^5)
   อยู่ที่ต้น ORBS file ของแต่ละ branch
   ─────────────────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) POGLS_BranchHeader {
    char      magic[4];              /* [0x00] "BRNC"                     */
    uint8_t   version;               /* [0x04] 0x31 = V3.1                */
    uint8_t   mode;                  /* [0x05] branch_mode_t              */
    uint16_t  reserved_flags;        /* [0x06]                            */
    uint64_t  branch_id;             /* [0x08] unique branch identity     */
    uint64_t  parent_branch_id;      /* [0x10] 0 = root branch            */
    uint64_t  genesis_snapshot_id;   /* [0x18] Snapshot_0 ของ branch นี้  */
    uint8_t   genesis_hash[16];      /* [0x20] anchor point ของ branch    */
    uint32_t  n_bits;                /* [0x30] Angular resolution ของ zone */
    uint32_t  base_timeout_ms;       /* [0x34] base สำหรับ adaptive calc  */
    uint64_t  zone_offset_start;     /* [0x38] byte offset ใน Core        */
    uint64_t  zone_offset_end;       /* [0x40]                            */
    uint64_t  snapshot_count;        /* [0x48] จำนวน snapshot ทั้งหมด    */
    uint64_t  last_certified_id;     /* [0x50] snapshot CERTIFIED ล่าสุด  */
    uint64_t  last_auto_id;          /* [0x58] snapshot AUTO ล่าสุด       */
    uint8_t   reserved[8];           /* [0x60] padding                    */
} POGLS_BranchHeader;
/* Size: 4+1+1+2+8+8+8+16+4+4+8+8+8+8+8+8 = 104B               */

/* ───────────────────────────────────────────────────────────────────────
   MIGRATION EVENT (append-only log entry, 64B)
   ทุก hard migration ต้อง append event นี้ก่อนเสมอ
   ─────────────────────────────────────────────────────────────────────── */
#define MIGRATE_REASON_PHYSICAL_CORRUPTION  0x01
#define MIGRATE_REASON_OSCILLATION_DETECTED 0x02
#define MIGRATE_REASON_MANUAL               0x03

typedef struct __attribute__((packed)) POGLS_MigrationEvent {
    char      magic[4];              /* [0x00] "MEVT"                     */
    uint8_t   reason;                /* [0x04] MIGRATE_REASON_*           */
    uint8_t   reserved[3];          /* [0x05]                            */
    uint64_t  void_snapshot_id;      /* [0x08] snapshot ที่ถูก VOID       */
    uint64_t  migrate_from_id;       /* [0x10] CERTIFIED source (anchor)  */
    uint64_t  new_snapshot_id;       /* [0x18] snapshot ใหม่หลัง migrate  */
    uint64_t  new_deep_lane_offset;  /* [0x20] ที่อยู่ใหม่ใน Core         */
    uint64_t  bypassed_offset;       /* [0x28] offset ที่พังและ skip ไป   */
    uint64_t  event_at_ms;           /* [0x30] timestamp                  */
    uint8_t   reserved2[8];          /* [0x38]                            */
} POGLS_MigrationEvent;
/* Size: 4+1+3+8+8+8+8+8+8+8+8 = 72B                             */

/* ───────────────────────────────────────────────────────────────────────
   AUDIT SIGNAL (read-only, ส่งจาก Audit → Shadow)
   Audit ไม่ write ตรง — ส่ง signal ให้ Shadow จัดการ
   ยกเว้นกรณีเดียว: invalidate CONFIRMED_AUTO
   ─────────────────────────────────────────────────────────────────────── */
typedef enum {
    AUDIT_SIG_CERTIFY        = 0,   /* ACK snapshot → CONFIRMED_CERTIFIED */
    AUDIT_SIG_INVALIDATE_AUTO= 1,   /* ค้าน CONFIRMED_AUTO (one-shot)     */
    AUDIT_SIG_ANOMALY        = 2,   /* พบความผิดปกติ แจ้ง Shadow          */
    AUDIT_SIG_HEALTH_UPDATE  = 3,   /* รายงานสุขภาพ Audit ตัวเอง          */
} audit_signal_type_t;

typedef struct {
    audit_signal_type_t type;
    uint64_t  target_snapshot_id;   /* snapshot ที่ signal นี้เกี่ยวข้อง  */
    uint64_t  branch_id;
    audit_health_t audit_health;    /* สุขภาพ Audit ณ เวลาส่ง signal      */
    uint64_t  signal_at_ms;
    uint8_t   tile_hash[16];        /* hash ของ tile ที่สแกน              */
} POGLS_AuditSignal;

/* ───────────────────────────────────────────────────────────────────────
   RETENTION CONFIG (ได้รับตอน Spawn — ไม่เปลี่ยนระหว่าง runtime)
   ─────────────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t  max_snapshots;         /* default 100                       */
    uint32_t  checkpoint_interval;   /* default 1000                      */
    uint32_t  base_timeout_ms;       /* base สำหรับ adaptive              */
    uint64_t  reference_zone_size;   /* reference สำหรับ timeout scaling  */
} POGLS_RetentionConfig;

/* ───────────────────────────────────────────────────────────────────────
   ADAPTIVE TIMEOUT
   effective = base × ceil(zone_size / reference_size)
   ห้าม auto-promote ถ้า audit_health = DEGRADED
   ─────────────────────────────────────────────────────────────────────── */
static inline uint32_t
pogls_calc_timeout(const POGLS_RetentionConfig* cfg, uint64_t zone_size)
{
    if (!cfg || cfg->reference_zone_size == 0)
        return 30000; /* fallback 30s */

    uint64_t ratio = (zone_size + cfg->reference_zone_size - 1)
                   / cfg->reference_zone_size; /* ceil division */
    if (ratio == 0) ratio = 1;

    uint64_t result = (uint64_t)cfg->base_timeout_ms * ratio;
    if (result > 300000) result = 300000; /* cap 5 นาที */
    return (uint32_t)result;
}

/* ───────────────────────────────────────────────────────────────────────
   FUNCTION DECLARATIONS
   ─────────────────────────────────────────────────────────────────────── */

/* Snapshot lifecycle */
POGLS_SnapshotHeader  pogls_snap_create(uint64_t branch_id,
                                        uint64_t parent_id,
                                        uint64_t deep_lane_offset,
                                        uint32_t deep_lane_count,
                                        uint32_t effective_timeout_ms);

int  pogls_snap_certify(POGLS_SnapshotHeader* snap,
                        const uint8_t tile_hash[16]);

int  pogls_snap_auto_promote(POGLS_SnapshotHeader* snap,
                             audit_health_t audit_health);

int  pogls_snap_invalidate(POGLS_SnapshotHeader* snap);   /* VOID, one-shot */

/* Checkpoint */
int  pogls_snap_mark_checkpoint(POGLS_SnapshotHeader* snap);
int  pogls_snap_is_immune(const POGLS_SnapshotHeader* snap);

/* Migration */
POGLS_MigrationEvent  pogls_migration_begin(uint64_t void_id,
                                            uint64_t certified_source_id,
                                            uint64_t new_snap_id,
                                            uint64_t new_deep_offset,
                                            uint64_t bypassed_offset,
                                            uint8_t  reason);

POGLS_SnapshotHeader  pogls_snap_migrated(uint64_t branch_id,
                                          const POGLS_MigrationEvent* ev,
                                          uint32_t effective_timeout_ms);

/* Branch */
POGLS_BranchHeader  pogls_branch_create(uint64_t branch_id,
                                        uint64_t parent_branch_id,
                                        uint64_t zone_start,
                                        uint64_t zone_end,
                                        uint32_t n_bits,
                                        const POGLS_RetentionConfig* cfg);

int  pogls_branch_set_mode(POGLS_BranchHeader* branch, branch_mode_t mode);

/* Audit signal handling (Shadow side) */
int  pogls_shadow_handle_signal(POGLS_BranchHeader*   branch,
                                POGLS_SnapshotHeader* snap,
                                const POGLS_AuditSignal* sig);

/* Retention / cleanup */
int  pogls_snap_eligible_for_cleanup(const POGLS_SnapshotHeader* snap,
                                     uint64_t last_certified_id,
                                     uint64_t window_start_id);

/* State query helpers */
static inline int snap_is_stable(const POGLS_SnapshotHeader* s) {
    return s->state == SNAP_CONFIRMED_CERTIFIED
        || s->state == SNAP_CONFIRMED_AUTO;
}

static inline int snap_is_dead(const POGLS_SnapshotHeader* s) {
    return s->state == SNAP_VOID;
}

static inline int snap_can_be_migration_source(const POGLS_SnapshotHeader* s) {
    /* ห้ามใช้ AUTO หรือ VOID เป็น source */
    return s->state == SNAP_CONFIRMED_CERTIFIED;
}

#endif /* POGLS_SNAPSHOT_H */
