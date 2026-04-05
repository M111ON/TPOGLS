#define _GNU_SOURCE
#include <string.h>
#include <time.h>
#include "pogls_snapshot.h"

/* ───────────────────────────────────────────────────────────────────────
   INTERNAL HELPERS
   ─────────────────────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* XOR-fold สำหรับ snapshot hash (prototype — production ใช้ SHA256) */
static void compute_zone_hash(uint64_t snapshot_id, uint64_t branch_id,
                               uint64_t deep_offset, uint8_t out[16])
{
    memset(out, 0, 16);
    uint8_t seed[32];
    memcpy(seed,      &snapshot_id, 8);
    memcpy(seed + 8,  &branch_id,   8);
    memcpy(seed + 16, &deep_offset, 8);
    memset(seed + 24, 0, 8);
    for (int i = 0; i < 32; i++)
        out[i % 16] ^= seed[i];
}

/* ───────────────────────────────────────────────────────────────────────
   SNAPSHOT LIFECYCLE
   ─────────────────────────────────────────────────────────────────────── */

POGLS_SnapshotHeader pogls_snap_create(uint64_t branch_id,
                                       uint64_t parent_id,
                                       uint64_t deep_lane_offset,
                                       uint32_t deep_lane_count,
                                       uint32_t effective_timeout_ms)
{
    POGLS_SnapshotHeader s;
    memset(&s, 0, sizeof(s));

    /* snapshot_id: caller ต้องส่ง monotonic ID มา
       ที่นี่ใช้ timestamp + branch เป็น stub */
    s.snapshot_id          = now_ms() ^ (branch_id << 20);
    s.branch_id            = branch_id;
    s.state                = SNAP_PENDING;
    s.parent_snapshot_id   = parent_id;
    s.deep_lane_offset     = deep_lane_offset;
    s.deep_lane_count      = deep_lane_count;
    s.created_at_ms        = now_ms();
    s.effective_timeout_ms = effective_timeout_ms;

    compute_zone_hash(s.snapshot_id, branch_id, deep_lane_offset,
                      s.snapshot_hash);
    return s;
}

/* Audit ACK → CONFIRMED_CERTIFIED */
int pogls_snap_certify(POGLS_SnapshotHeader* snap,
                       const uint8_t tile_hash[16])
{
    if (!snap) return -1;
    if (snap->state != SNAP_PENDING) return -1; /* ห้าม certify state อื่น */

    /* ตรวจ tile_hash ถ้ามี (non-zero = Audit ส่ง real hash มา)
       ถ้า tile_hash เป็น zeros ทั้งหมด = Audit trust pass (integration mode) */
    if (tile_hash) {
        int all_zero = 1;
        for (int i = 0; i < 16; i++) if (tile_hash[i]) { all_zero = 0; break; }
        if (!all_zero) {
            uint8_t expected[16];
            compute_zone_hash(snap->snapshot_id, snap->branch_id,
                              snap->deep_lane_offset, expected);
            if (memcmp(tile_hash, expected, 16) != 0)
                return -2; /* hash mismatch — Audit reject */
        }
    }

    snap->state = SNAP_CONFIRMED_CERTIFIED;
    return 0;
}

/* Timeout + Branch Healthy → CONFIRMED_AUTO
   ห้ามเรียกถ้า audit_health = DEGRADED */
int pogls_snap_auto_promote(POGLS_SnapshotHeader* snap,
                            audit_health_t audit_health)
{
    if (!snap) return -1;
    if (snap->state != SNAP_PENDING) return -1;

    /* ห้าม promote ถ้า Audit เองก็ไม่ OK */
    if (audit_health == AUDIT_HEALTH_DEGRADED) return -2;

    snap->state                = SNAP_CONFIRMED_AUTO;
    snap->audit_health_at_promo = (uint8_t)audit_health;
    return 0;
}

/* Audit invalidate CONFIRMED_AUTO → VOID (one-shot) */
int pogls_snap_invalidate(POGLS_SnapshotHeader* snap)
{
    if (!snap) return -1;

    /* One-shot rule: ใช้ได้เฉพาะ CONFIRMED_AUTO */
    if (snap->state != SNAP_CONFIRMED_AUTO) return -1;

    snap->state = SNAP_VOID;
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────
   CHECKPOINT
   ─────────────────────────────────────────────────────────────────────── */

int pogls_snap_mark_checkpoint(POGLS_SnapshotHeader* snap)
{
    if (!snap) return -1;
    if (snap->state == SNAP_VOID) return -1; /* VOID ห้าม checkpoint */

    snap->is_checkpoint = 1;
    /* checkpoint_confirmed จะถูก set ตอน Audit certify ทีหลัง */
    return 0;
}

int pogls_snap_is_immune(const POGLS_SnapshotHeader* snap)
{
    if (!snap) return 0;
    /* Immune ถ้ามี checkpoint tag — ไม่ว่าจะ confirmed หรือ pending */
    return (snap->is_checkpoint == 1) ? 1 : 0;
}

/* ───────────────────────────────────────────────────────────────────────
   MIGRATION
   ─────────────────────────────────────────────────────────────────────── */

POGLS_MigrationEvent pogls_migration_begin(uint64_t void_id,
                                           uint64_t certified_source_id,
                                           uint64_t new_snap_id,
                                           uint64_t new_deep_offset,
                                           uint64_t bypassed_offset,
                                           uint8_t  reason)
{
    POGLS_MigrationEvent ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.magic, "MEVT", 4);

    ev.reason               = reason;
    ev.void_snapshot_id     = void_id;
    ev.migrate_from_id      = certified_source_id;
    ev.new_snapshot_id      = new_snap_id;
    ev.new_deep_lane_offset = new_deep_offset;
    ev.bypassed_offset      = bypassed_offset;
    ev.event_at_ms          = now_ms();
    return ev;
}

POGLS_SnapshotHeader pogls_snap_migrated(uint64_t branch_id,
                                         const POGLS_MigrationEvent* ev,
                                         uint32_t effective_timeout_ms)
{
    POGLS_SnapshotHeader s;
    memset(&s, 0, sizeof(s));

    s.snapshot_id          = ev->new_snapshot_id;
    s.branch_id            = branch_id;
    s.state                = SNAP_MIGRATED;
    s.parent_snapshot_id   = ev->migrate_from_id;  /* สืบจาก CERTIFIED */
    s.void_snapshot_id     = ev->void_snapshot_id; /* trace ย้อนกลับได้ */
    s.migrate_from_id      = ev->migrate_from_id;
    s.deep_lane_offset     = ev->new_deep_lane_offset;
    s.created_at_ms        = ev->event_at_ms;
    s.effective_timeout_ms = effective_timeout_ms;

    compute_zone_hash(s.snapshot_id, branch_id,
                      ev->new_deep_lane_offset, s.snapshot_hash);
    return s;
}

/* ───────────────────────────────────────────────────────────────────────
   BRANCH
   ─────────────────────────────────────────────────────────────────────── */

POGLS_BranchHeader pogls_branch_create(uint64_t branch_id,
                                       uint64_t parent_branch_id,
                                       uint64_t zone_start,
                                       uint64_t zone_end,
                                       uint32_t n_bits,
                                       const POGLS_RetentionConfig* cfg)
{
    POGLS_BranchHeader b;
    memset(&b, 0, sizeof(b));
    memcpy(b.magic, "BRNC", 4);

    b.version          = 0x31;   /* V3.1 */
    b.mode             = BRANCH_NORMAL;
    b.branch_id        = branch_id;
    b.parent_branch_id = parent_branch_id;
    b.n_bits           = n_bits;
    b.zone_offset_start = zone_start;
    b.zone_offset_end   = zone_end;

    if (cfg) {
        b.base_timeout_ms = cfg->base_timeout_ms;
        uint64_t zone_size = zone_end - zone_start;
        /* Adaptive timeout คำนวณตอน spawn */
        b.base_timeout_ms = pogls_calc_timeout(cfg, zone_size);
    } else {
        b.base_timeout_ms = 30000; /* fallback */
    }

    /* genesis_snapshot_id และ genesis_hash จะถูก set
       หลังจาก pogls_snap_create + certify เสร็จ */
    return b;
}

int pogls_branch_set_mode(POGLS_BranchHeader* branch, branch_mode_t mode)
{
    if (!branch) return -1;
    branch->mode = (uint8_t)mode;
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────
   AUDIT SIGNAL HANDLER (Shadow side)
   ─────────────────────────────────────────────────────────────────────── */

int pogls_shadow_handle_signal(POGLS_BranchHeader*    branch,
                               POGLS_SnapshotHeader*  snap,
                               const POGLS_AuditSignal* sig)
{
    if (!branch || !snap || !sig) return -1;
    if (sig->branch_id != branch->branch_id) return -1; /* ไม่ใช่ branch นี้ */

    switch (sig->type) {

    case AUDIT_SIG_CERTIFY:
        if (sig->target_snapshot_id != snap->snapshot_id) return -1;
        if (pogls_snap_certify(snap, sig->tile_hash) != 0) return -1;

        /* อัปเดต branch header */
        branch->last_certified_id = snap->snapshot_id;

        /* ถ้าเป็น checkpoint → confirm */
        if (snap->is_checkpoint)
            snap->checkpoint_confirmed = 1;
        return 0;

    case AUDIT_SIG_INVALIDATE_AUTO:
        /* One-shot: ค้าน CONFIRMED_AUTO → VOID */
        if (sig->target_snapshot_id != snap->snapshot_id) return -1;
        if (pogls_snap_invalidate(snap) != 0) return -1;

        /* เข้า Safe Mode รอ Hard Migration */
        pogls_branch_set_mode(branch, BRANCH_SAFE_MODE);
        return 0;

    case AUDIT_SIG_ANOMALY:
        /* Audit แจ้ง anomaly — Shadow ตัดสินใจเองว่าจะ repair ยังไง */
        /* ที่นี่ไม่ force อะไร — caller ต้อง check และ call repair */
        return 1; /* return 1 = anomaly detected, caller ต้อง act */

    case AUDIT_SIG_HEALTH_UPDATE:
        /* รับรู้ว่า Audit ยังมีชีวิตอยู่ — ไม่ต้องทำอะไร */
        return 0;

    default:
        return -1;
    }
}

/* ───────────────────────────────────────────────────────────────────────
   RETENTION / CLEANUP ELIGIBILITY
   Shadow เรียกก่อนลบ snapshot ทุกครั้ง
   ─────────────────────────────────────────────────────────────────────── */

int pogls_snap_eligible_for_cleanup(const POGLS_SnapshotHeader* snap,
                                    uint64_t last_certified_id,
                                    uint64_t window_start_id)
{
    if (!snap) return 0;

    /* CERTIFIED และ MIGRATED เป็น end state ที่ preserve ได้
       แต่ถ้าอยู่นอก window → eligible ยกเว้น checkpoint */
    if (pogls_snap_is_immune(snap)) return 0; /* checkpoint ห้ามลบ */
    if (snap->state == SNAP_VOID)   return 1; /* VOID ลบได้เลย     */
    if (snap->snapshot_id == last_certified_id) return 0; /* last anchor */
    if (snap->snapshot_id >= window_start_id)   return 0; /* อยู่ใน window */

    return 1; /* อยู่นอก window + ไม่ใช่ checkpoint → eligible */
}
