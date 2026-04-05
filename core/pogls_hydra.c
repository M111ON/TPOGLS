#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include "pogls_hydra.h"

/* ───────────────────────────────────────────────────────────────────────
   INTERNAL HELPERS
   ─────────────────────────────────────────────────────────────────────── */

static uint64_t hyd_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t hyd_gen_branch_id(uint8_t head_id) {
    return hyd_now_ms() ^ ((uint64_t)head_id << 56);
}

static POGLS_HydraHead* find_slot(POGLS_HydraCore* hydra) {
    for (int i = 0; i < HYDRA_MAX_HEADS; i++) {
        head_status_t s = (head_status_t)hydra->heads[i].status;
        if (s == HEAD_DORMANT || s == HEAD_DEAD)
            return &hydra->heads[i];
    }
    return NULL;
}

static POGLS_HydraHead* find_head_by_id(POGLS_HydraCore* hydra, uint8_t id) {
    if (id >= HYDRA_MAX_HEADS) return NULL;
    return &hydra->heads[id];
}

/* ─────────────────────────────────────────────────────────────────────
   CORE LIFECYCLE
   ─────────────────────────────────────────────────────────────────────── */

POGLS_HydraCore* pogls_hydra_init(void* core_mmap, uint64_t core_size,
                                   uint64_t spawn_thresh,
                                   uint64_t retract_thresh)
{
    if (!core_mmap || core_size == 0) return NULL;

    POGLS_HydraCore* hydra = calloc(1, sizeof(POGLS_HydraCore));
    if (!hydra) return NULL;

    memcpy(hydra->magic, HYDRA_MAGIC, 4);
    hydra->version              = HYDRA_VERSION;
    hydra->core_mmap            = core_mmap;
    hydra->core_size            = core_size;
    hydra->core_append_offset   = core_size;   /* append ต่อจากปลายไฟล์ */
    hydra->spawn_threshold_bytes   = spawn_thresh   ? spawn_thresh
                                                    : HYDRA_SPAWN_THRESHOLD;
    hydra->retract_threshold_bytes = retract_thresh ? retract_thresh
                                                    : HYDRA_RETRACT_THRESHOLD;

    /* init all head slots as DORMANT */
    for (int i = 0; i < HYDRA_MAX_HEADS; i++) {
        memcpy(hydra->heads[i].magic, HYDRA_MAGIC, 4);
        hydra->heads[i].head_id = (uint8_t)i;
        hydra->heads[i].status  = (uint8_t)HEAD_DORMANT;
        hydra->heads[i].version = HYDRA_VERSION;
    }
    return hydra;
}

void pogls_hydra_destroy(POGLS_HydraCore* hydra) {
    if (!hydra) return;
    memset(hydra, 0, sizeof(*hydra));
    free(hydra);
}

/* ─────────────────────────────────────────────────────────────────────
   SPAWN — "พิธีกรรมการเกิด"
   1. จอง slot
   2. สร้าง BranchHeader
   3. สร้าง Genesis Snapshot
   4. บันทึก SpawnEvent
   5. รอ Audit certify genesis ก่อน active
   ─────────────────────────────────────────────────────────────────────── */

POGLS_HydraHead* pogls_hydra_spawn(POGLS_HydraCore*        hydra,
                                    const POGLS_SpawnConfig* cfg,
                                    int                      event_fd)
{
    if (!hydra || !cfg) return NULL;
    if (cfg->zone_end <= cfg->zone_start) return NULL;

    POGLS_HydraHead* head = find_slot(hydra);
    if (!head) return NULL;   /* max heads reached */

    uint64_t zone_size = cfg->zone_end - cfg->zone_start;
    uint64_t bid       = hyd_gen_branch_id(head->head_id);

    /* ── 1. Fill head ── */
    memcpy(head->magic, HYDRA_MAGIC, 4);
    head->status             = (uint8_t)HEAD_SPAWNING;
    head->spawn_reason       = (uint8_t)cfg->reason;
    head->zone_offset_start  = cfg->zone_start;
    head->zone_offset_end    = cfg->zone_end;
    head->n_bits_local       = cfg->n_bits  ? cfg->n_bits  : hydra->heads[0].n_bits_local;
    head->topo_level_local   = cfg->topo_level;
    head->branch_id          = bid;
    head->parent_branch_id   = cfg->parent_branch_id;
    head->spawned_at_ms      = hyd_now_ms();
    head->last_active_ms     = head->spawned_at_ms;

    /* Adaptive timeout */
    POGLS_RetentionConfig default_ret = {100, 1000, 30000,
                                          (uint64_t)DEEP_BLOCK_SIZE};
    const POGLS_RetentionConfig* ret = cfg->retention ? cfg->retention : &default_ret;
    head->effective_timeout_ms = pogls_calc_timeout(ret, zone_size);

    /* ── 2. Create Genesis Snapshot (PENDING until certify_genesis called) ── */
    POGLS_SnapshotHeader genesis = pogls_snap_create(
        bid, 0,
        cfg->zone_start,
        (uint32_t)((zone_size + DEEP_BLOCK_SIZE - 1) >> SHIFT_DEEP),
        head->effective_timeout_ms
    );
    /* snapshot_id ใช้เป็น genesis_snapshot_id */
    head->genesis_snapshot_id    = genesis.snapshot_id;
    head->last_certified_snap_id = 0;  /* ยังไม่ certified */

    /* ── 3. Log SpawnEvent ── */
    if (event_fd >= 0) {
        POGLS_SpawnEvent ev;
        memset(&ev, 0, sizeof(ev));
        memcpy(ev.magic, "SPWN", 4);
        ev.head_id             = head->head_id;
        ev.reason              = (uint8_t)cfg->reason;
        ev.branch_id           = bid;
        ev.parent_branch_id    = cfg->parent_branch_id;
        ev.zone_offset_start   = cfg->zone_start;
        ev.zone_offset_end     = cfg->zone_end;
        ev.genesis_snapshot_id = genesis.snapshot_id;
        ev.spawned_at_ms       = head->spawned_at_ms;
        /* genesis_hash จะ fill ตอน certify_genesis */
        pogls_hydra_log_spawn(event_fd, &ev);
    }

    hydra->active_count++;
    hydra->radar_spawn_count++;

    /* Spawn callback */
    if (hydra->on_spawn) hydra->on_spawn(hydra, head);

    return head;
}

/* ─────────────────────────────────────────────────────────────────────
   CERTIFY GENESIS — Audit scan เสร็จ → head เริ่มรับ write ได้
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_certify_genesis(POGLS_HydraCore* hydra, uint8_t head_id,
                                 const uint8_t genesis_hash[16])
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h) return -1;
    if ((head_status_t)h->status != HEAD_SPAWNING) return -1;

    h->last_certified_snap_id = h->genesis_snapshot_id;
    h->last_active_ms         = hyd_now_ms();
    h->status                 = (uint8_t)HEAD_ACTIVE;
    (void)genesis_hash;   /* stored in BranchHeader by caller */

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
   RETRACT — sync final state กลับ Core แล้วยุบหัวลง
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_retract(POGLS_HydraCore* hydra, uint8_t head_id,
                         retract_reason_t reason, int event_fd)
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h || !head_is_alive(h)) return -1;

    h->status = (uint8_t)HEAD_RETRACTING;

    /* Log RetractEvent ก่อน sync */
    if (event_fd >= 0) {
        POGLS_RetractEvent ev;
        memset(&ev, 0, sizeof(ev));
        memcpy(ev.magic, "RETR", 4);
        ev.head_id                = head_id;
        ev.reason                 = (uint8_t)reason;
        ev.branch_id              = h->branch_id;
        ev.last_certified_snap_id = h->last_certified_snap_id;
        ev.final_append_offset    = hydra->core_append_offset;
        ev.retracted_at_ms        = hyd_now_ms();
        ev.total_writes           = (uint32_t)h->write_count;
        ev.total_snapshots        = (uint32_t)h->snapshot_count;
        pogls_hydra_log_retract(event_fd, &ev);
    }

    /* Retract callback ก่อนทำ dead */
    if (hydra->on_retract) hydra->on_retract(hydra, h, reason);

    h->status = (uint8_t)HEAD_DEAD;
    if (hydra->active_count > 0) hydra->active_count--;
    hydra->radar_retract_count++;

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
   WRITE ROUTING
   byte_offset → หา Head ที่ zone ครอบคลุม offset นั้น
   ถ้าไม่มี Head ใดดูแล → return NULL (ใช้ Core ตรง)
   ─────────────────────────────────────────────────────────────────────── */

POGLS_HydraHead* pogls_hydra_route(POGLS_HydraCore* hydra,
                                    uint64_t byte_offset)
{
    if (!hydra) return NULL;
    for (int i = 0; i < HYDRA_MAX_HEADS; i++) {
        POGLS_HydraHead* h = &hydra->heads[i];
        if (!head_is_writable(h)) continue;
        if (byte_offset >= h->zone_offset_start &&
            byte_offset <  h->zone_offset_end)
            return h;
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────
   APPEND — the one gate for all writes
   Head != NULL → update head metrics
   Append-only: เขียนต่อท้าย core_append_offset เสมอ
   ─────────────────────────────────────────────────────────────────────── */

uint64_t pogls_hydra_append(POGLS_HydraCore* hydra, POGLS_HydraHead* head,
                             const void* data, uint64_t size)
{
    if (!hydra || !data || size == 0) return (uint64_t)-1;

    uint64_t at = hydra->core_append_offset;

    /* ใน production: pwrite64() หรือ msync กับ mmap
       ที่นี่เป็น model — caller ต้อง handle actual I/O */
    hydra->core_append_offset += size;

    if (head && head_is_alive(head)) {
        head->write_count++;
        head->last_active_ms = hyd_now_ms();
        /* อัปเดต zone end ถ้าเขียนเกิน (append zone expansion) */
        if (hydra->core_append_offset > head->zone_offset_end)
            head->zone_offset_end = hydra->core_append_offset;
        /* block count */
        head->current_block_count = (uint32_t)
            ((head->zone_offset_end - head->zone_offset_start)
             >> SHIFT_DEEP);
        if (head->current_block_count > head->peak_block_count)
            head->peak_block_count = head->current_block_count;
    }
    return at;
}

/* ─────────────────────────────────────────────────────────────────────
   DENSITY CHECK — ตรวจว่า zone ควร spawn หรือ retract
   return  1 = ควร spawn head ใหม่ใน zone นี้
   return -1 = zone เล็กพอ ควร retract
   return  0 = ปกติ
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_check_density(POGLS_HydraCore* hydra, uint8_t head_id)
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h || !head_is_alive(h)) return 0;

    uint64_t sz = head_zone_size(h);
    if (sz > hydra->spawn_threshold_bytes)   return  1;
    if (sz < hydra->retract_threshold_bytes) return -1;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
   LOCAL TOPOLOGY SCALING (Head authority — ไม่ต้อง ask Core)
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_scale_up(POGLS_HydraHead* head, uint32_t new_n_bits)
{
    if (!head || !head_is_alive(head)) return -1;
    if (new_n_bits <= head->n_bits_local) return -1;  /* scale up เท่านั้น */
    if (new_n_bits > 32) return -1;
    head->n_bits_local = new_n_bits;
    return 0;
}

int pogls_hydra_scale_down(POGLS_HydraHead* head, uint32_t new_n_bits)
{
    if (!head || !head_is_alive(head)) return -1;
    if (new_n_bits >= head->n_bits_local) return -1;
    if (new_n_bits < 12) return -1;   /* minimum useful resolution */
    head->n_bits_local = new_n_bits;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
   MODE TRANSITIONS
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_enter_safe(POGLS_HydraCore* hydra, uint8_t head_id)
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h || (head_status_t)h->status != HEAD_ACTIVE) return -1;
    h->status = (uint8_t)HEAD_SAFE;
    return 0;
}

int pogls_hydra_enter_migration(POGLS_HydraCore* hydra, uint8_t head_id)
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h) return -1;
    head_status_t s = (head_status_t)h->status;
    if (s != HEAD_ACTIVE && s != HEAD_SAFE) return -1;
    h->status = (uint8_t)HEAD_MIGRATING;
    h->migration_count++;
    hydra->radar_incident_count++;
    return 0;
}

int pogls_hydra_exit_recovery(POGLS_HydraCore* hydra, uint8_t head_id)
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h) return -1;
    head_status_t s = (head_status_t)h->status;
    if (s != HEAD_SAFE && s != HEAD_MIGRATING) return -1;
    h->status         = (uint8_t)HEAD_ACTIVE;
    h->last_active_ms = hyd_now_ms();
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
   ANOMALY HANDLER — จาก Audit signal → ตัดสินใจ mode transition
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_handle_anomaly(POGLS_HydraCore* hydra, uint8_t head_id,
                                uint8_t anomaly_flags, uint64_t snapshot_id)
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h || !head_is_alive(h)) return -1;
    (void)snapshot_id;

    h->anomaly_count++;
    hydra->radar_incident_count++;

    /* DEEP_UNREADABLE = physical corruption → straight to migration */
    if (anomaly_flags & 0x10) {
        return pogls_hydra_enter_migration(hydra, head_id);
    }

    /* อื่นๆ → safe mode ก่อน ให้ Shadow ประเมินต่อ */
    return pogls_hydra_enter_safe(hydra, head_id);
}

/* ─────────────────────────────────────────────────────────────────────
   FAULT ISOLATION
   Fault ใน Branch X → ไม่กระทบ Head อื่น (ตาม Isolation Principle)
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_isolate(POGLS_HydraCore* hydra, uint8_t head_id)
{
    POGLS_HydraHead* h = find_head_by_id(hydra, head_id);
    if (!h) return -1;
    /* Isolation = enter safe mode ของ head นั้นเท่านั้น
       Head อื่น ไม่ถูกแตะต้องเลย */
    h->status = (uint8_t)HEAD_SAFE;
    hydra->radar_incident_count++;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
   EVENT LOG
   ─────────────────────────────────────────────────────────────────────── */

int pogls_hydra_log_spawn(int fd, const POGLS_SpawnEvent* ev)
{
    if (fd < 0 || !ev) return -1;
    return (write(fd, ev, sizeof(*ev)) == (ssize_t)sizeof(*ev)) ? 0 : -1;
}

int pogls_hydra_log_retract(int fd, const POGLS_RetractEvent* ev)
{
    if (fd < 0 || !ev) return -1;
    return (write(fd, ev, sizeof(*ev)) == (ssize_t)sizeof(*ev)) ? 0 : -1;
}

/* ─────────────────────────────────────────────────────────────────────
   VISUALIZER
   ─────────────────────────────────────────────────────────────────────── */

static const char* status_str(uint8_t s) {
    switch ((head_status_t)s) {
    case HEAD_DORMANT:    return "DORMANT   ";
    case HEAD_SPAWNING:   return "SPAWNING  ";
    case HEAD_ACTIVE:     return "ACTIVE    ";
    case HEAD_SAFE:       return "SAFE_MODE ";
    case HEAD_MIGRATING:  return "MIGRATING ";
    case HEAD_RETRACTING: return "RETRACTING";
    case HEAD_DEAD:       return "DEAD      ";
    default:              return "UNKNOWN   ";
    }
}

void pogls_hydra_print_head(const POGLS_HydraHead* h) {
    if (!h) return;
    double mb = (double)head_zone_size(h) / (1024.0 * 1024.0);
    printf("| Head[%2u] %s | zone[%8llu-%8llu] %.1fMB"
           " | n=%u blk=%u/%u w=%llu anom=%u\n",
           h->head_id,
           status_str(h->status),
           (unsigned long long)h->zone_offset_start,
           (unsigned long long)h->zone_offset_end,
           mb,
           h->n_bits_local,
           h->current_block_count,
           h->peak_block_count,
           (unsigned long long)h->write_count,
           h->anomaly_count);
}

void pogls_hydra_print_all(const POGLS_HydraCore* hydra) {
    if (!hydra) return;
    double core_gb = (double)hydra->core_size / (1024.0*1024.0*1024.0);
    printf("+-- HYDRA Core [%c%c%c%c v%02X] "
           "-------------------------------------------+\n",
           hydra->magic[0], hydra->magic[1],
           hydra->magic[2], hydra->magic[3], hydra->version);
    printf("| Core: %.2fGB | ActiveHeads: %u/%u"
           " | Spawn: %llu | Retract: %llu | Incident: %llu\n",
           core_gb, hydra->active_count, HYDRA_MAX_HEADS,
           (unsigned long long)hydra->radar_spawn_count,
           (unsigned long long)hydra->radar_retract_count,
           (unsigned long long)hydra->radar_incident_count);
    printf("+-- Heads -------------------------------------------------------+\n");
    for (int i = 0; i < HYDRA_MAX_HEADS; i++) {
        const POGLS_HydraHead* h = &hydra->heads[i];
        if ((head_status_t)h->status != HEAD_DORMANT &&
            (head_status_t)h->status != HEAD_DEAD)
            pogls_hydra_print_head(h);
    }
    printf("+----------------------------------------------------------------+\n");
}
