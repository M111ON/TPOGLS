/*
 * pogls_delta.h — Delta Lane + Shadow Directory Spec
 * POGLS V3.5  Phase 1: 4-Lane Crash Recovery
 *
 * Design principles:
 *  1. ไม่แตะไฟล์ต้นฉบับ — ทุกอย่างอยู่ใน .pogls/ hidden dir
 *  2. snapshot.merkle = Single Source of Truth
 *     ตราบใด merkle ยังไม่ update = pending ทั้งหมด
 *  3. append-only per lane — ห้าม rewrite block กลางไฟล์
 *  4. msync() ก่อน rename — ป้องกัน data อยู่แค่ใน page cache
 *  5. audit invariant: lane_X + lane_nX = 0  (integer, exact)
 */

#ifndef POGLS_DELTA_H
#define POGLS_DELTA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ══════════════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════════════ */

#define DELTA_MAGIC          0x504C4400u   /* "PLD\0" */
#define DELTA_VERSION        1
#define DELTA_BLOCK_SIZE     256           /* bytes — ตรงกับ Deep block */
#define DELTA_HEADER_SIZE    32            /* bytes */
#define DELTA_MAX_PAYLOAD    (DELTA_BLOCK_SIZE - DELTA_HEADER_SIZE)  /* 224B */

/* PHI constants — integer only, no float in core */
#define PHI_SCALE            (1u << 20)    /* 1,048,576 */
#define PHI_UP               1697054u      /* floor(φ  × 2²⁰) */
#define PHI_DOWN             648609u       /* floor(φ⁻¹ × 2²⁰) */

/* Lane IDs — quad axis */
#define LANE_X               0
#define LANE_NX              1   /* -X */
#define LANE_Y               2
#define LANE_NY              3   /* -Y */
#define LANE_COUNT           4

/* File names inside .pogls/<file>/ */
#define FNAME_LANE_X         "lane_X.delta"
#define FNAME_LANE_NX        "lane_nX.delta"
#define FNAME_LANE_Y         "lane_Y.delta"
#define FNAME_LANE_NY        "lane_nY.delta"
#define FNAME_PENDING_X      "lane_X.pending"
#define FNAME_PENDING_NX     "lane_nX.pending"
#define FNAME_PENDING_Y      "lane_Y.pending"
#define FNAME_PENDING_NY     "lane_nY.pending"
#define FNAME_MERKLE         "snapshot.merkle"
#define FNAME_MERKLE_PENDING "snapshot.merkle.pending"
#define FNAME_MANIFEST       "manifest.json"

/* .pogls/ directory name (hidden on Unix) */
#define POGLS_DIR            ".pogls"

/* ══════════════════════════════════════════════════════════════════════
   ON-DISK STRUCTURES
   ══════════════════════════════════════════════════════════════════════ */

/*
 * Delta Block Header — 32 bytes (packed)
 * เขียนนำหน้า payload ทุก block
 *
 * Layout:
 *   [0..3]   magic     DELTA_MAGIC
 *   [4]      lane_id   LANE_X / LANE_NX / LANE_Y / LANE_NY
 *   [5]      version   DELTA_VERSION
 *   [6..7]   _pad
 *   [8..15]  seq       monotonic per lane
 *   [16..23] addr      angular address = floor(θ × 2²⁰)
 *   [24..27] payload_size  bytes of data after header
 *   [28..31] crc32     CRC32(header[0..27] + payload)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  lane_id;
    uint8_t  version;
    uint8_t  _pad[2];
    uint64_t seq;
    uint64_t addr;        /* angular address — A = floor(θ × 2²⁰) */
    uint32_t payload_size;
    uint32_t crc32;
} Delta_BlockHeader;
/* sizeof = 4+1+1+2+8+8+4+4 = 32 bytes */

/*
 * Merkle Record — 80 bytes (packed)
 * เก็บใน snapshot.merkle — Single Source of Truth
 *
 * ถ้าไฟล์นี้มีอยู่และ valid = committed สมบูรณ์
 * ถ้าไม่มี หรือ CRC fail = pending ทั้งหมด
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* DELTA_MAGIC */
    uint32_t _pad;
    uint64_t epoch;         /* monotonic commit counter */
    uint64_t seq[LANE_COUNT]; /* last committed seq per lane */
    uint8_t  root[32];      /* SHA256 merkle root */
    uint32_t crc32;         /* CRC32(bytes 0..75) */
} Delta_MerkleRecord;
/* sizeof = 4+4+8+(8×4)+32+4 = 80 bytes */

/*
 * Manifest Entry — per source file
 * เก็บใน .pogls/manifest.json (JSON สำหรับ human-readable)
 * และ .pogls/manifest.bin (binary สำหรับ fast scan)
 */
typedef struct __attribute__((packed)) {
    uint8_t  sha256[32];    /* SHA256 ของ source file */
    uint64_t file_size;
    uint64_t ingest_epoch;
    uint8_t  lane_count;    /* ปัจจุบัน = 4 */
    uint8_t  status;        /* MANIFEST_PENDING / MANIFEST_COMMITTED */
    uint8_t  _pad[6];
    uint32_t crc32;
} Delta_ManifestEntry;
/* sizeof = 32+8+8+1+1+6+4 = 60 bytes */

/* status values */
#define MANIFEST_PENDING    0x01
#define MANIFEST_COMMITTED  0x02

/* ══════════════════════════════════════════════════════════════════════
   DIRECTORY LAYOUT
   ══════════════════════════════════════════════════════════════════════

   vault/
     model.safetensors          ← source file (ไม่แตะ)
     image.png
     .pogls/                    ← POGLS_DIR (hidden)
       manifest.json            ← human-readable registry
       manifest.bin             ← fast-scan binary
       model.safetensors/       ← 1 subdir ต่อ 1 source file
         lane_X.delta           ← committed lane data
         lane_nX.delta
         lane_Y.delta
         lane_nY.delta
         snapshot.merkle        ← THE TRUTH — committed iff exists+valid
         lane_X.pending         ← exists = write in progress
         lane_nX.pending        ← (ทุกตัวหายไปหลัง commit สำเร็จ)
         lane_Y.pending
         lane_nY.pending
         snapshot.merkle.pending← compute ก่อน rename เป็น snapshot.merkle
       image.png/
         ...

   ══════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════════
   COMMIT PROTOCOL  (ลำดับห้ามสลับ)
   ══════════════════════════════════════════════════════════════════════

   Step 1  append delta blocks → lane_*.pending  (append-only)
   Step 2  compute CRC32 ทุก block ใน .pending
   Step 3  audit: sum(lane_X.seq) - sum(lane_nX.seq) == 0
                  sum(lane_Y.seq) - sum(lane_nY.seq) == 0
   Step 4  compute merkle root จาก 4 lanes
   Step 5  write snapshot.merkle.pending
   Step 6  msync(lane_*.pending)       ← flush page cache ก่อน rename
   Step 7  msync(merkle.pending)
   Step 8  rename lane_X.pending   → lane_X.delta
           rename lane_nX.pending  → lane_nX.delta
           rename lane_Y.pending   → lane_Y.delta
           rename lane_nY.pending  → lane_nY.delta
   Step 9  rename snapshot.merkle.pending → snapshot.merkle  ← ATOMIC FINAL
   Step 10 update manifest entry → MANIFEST_COMMITTED

   กฎ: ถ้าไฟดับก่อน Step 9 → merkle ยังเป็น version เก่า
       Boot scanner เห็น merkle เก่า + .pending ค้าง → discard → fallback

   ══════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════════
   BOOT RECOVERY  (deterministic — 3 กรณีเท่านั้น)
   ══════════════════════════════════════════════════════════════════════

   scan .pogls/<file>/

   กรณี A — CLEAN
     snapshot.merkle exists + CRC valid
     merkle.seq[i] == lane_*.delta last seq
     ไม่มี .pending เหลือ
     → load committed state ✅

   กรณี B — TORN (ไฟดับระหว่าง commit)
     snapshot.merkle exists แต่ seq ไม่ตรงกับ .delta
     หรือมี .pending ค้างอยู่
     → discard .pending ทั้งหมด
     → fallback merkle.epoch - 1 (shadow N-1)
     → reload lane_*.delta version ก่อนหน้า ✅

   กรณี C — NEW
     ไม่มี snapshot.merkle
     → source file ใหม่ ยังไม่เคย commit
     → ingest ปกติ ✅

   ══════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════════
   API DECLARATIONS
   ══════════════════════════════════════════════════════════════════════ */

/* Context ของ 1 source file */
typedef struct {
    char     source_path[512];    /* path ของ source file */
    char     pogls_dir[512];      /* .pogls/<filename>/ */
    int      lane_fd[LANE_COUNT]; /* fd ของ .pending files */
    uint64_t lane_seq[LANE_COUNT];/* seq ปัจจุบันของแต่ละ lane */
    uint64_t epoch;               /* commit epoch */
    bool     is_open;
} Delta_Context;

/* Recovery result */
typedef enum {
    DELTA_RECOVERY_CLEAN  = 0,   /* กรณี A */
    DELTA_RECOVERY_TORN   = 1,   /* กรณี B — fallback เรียบร้อย */
    DELTA_RECOVERY_NEW    = 2,   /* กรณี C */
    DELTA_RECOVERY_ERROR  = -1,
} Delta_RecoveryResult;

/* ── Writer API ──────────────────────────────────────────────────────── */

/* เปิด/สร้าง context สำหรับ 1 source file */
int delta_open(Delta_Context *ctx, const char *source_path);

/* append 1 block ลง lane (append-only) */
int delta_append(Delta_Context *ctx, uint8_t lane_id,
                 uint64_t addr, const void *data, uint32_t size);

/* commit: audit → merkle → msync → rename (ลำดับตาม protocol) */
int delta_commit(Delta_Context *ctx);

/* close context (flush + close fds) */
int delta_close(Delta_Context *ctx);

/* ── Scanner API ─────────────────────────────────────────────────────── */

/* boot recovery scan สำหรับ 1 source file */
Delta_RecoveryResult delta_recover(const char *source_path);

/* scan vault directory ทั้งหมด */
int delta_scan_vault(const char *vault_path,
                     void (*callback)(const char *file,
                                      Delta_RecoveryResult result,
                                      void *userdata),
                     void *userdata);

/* ── Audit API ───────────────────────────────────────────────────────── */

/* ตรวจ invariant: lane_X.seq_sum - lane_nX.seq_sum == 0 */
int delta_audit(const Delta_Context *ctx);

/* ── Utility ─────────────────────────────────────────────────────────── */

uint32_t delta_crc32(uint32_t crc, const void *data, size_t len);
int      delta_merkle_compute(Delta_Context *ctx, uint8_t root_out[32]);
const char *delta_lane_name(uint8_t lane_id);   /* "X" / "-X" / "Y" / "-Y" */
const char *delta_recovery_str(Delta_RecoveryResult r);

#endif /* POGLS_DELTA_H */
