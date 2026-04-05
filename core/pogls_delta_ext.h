/*
 * pogls_delta_ext.h — POGLS V3.6  Dual Merkle Extension
 * =======================================================
 *
 * Task 5: ขยาย pogls_delta.h ให้รู้จัก merkle_A + merkle_B
 *
 * กฎ: pogls_delta.h ไม่ถูกแตะเลย — additive only
 *     include ไฟล์นี้ต่อท้าย pogls_delta.h เสมอ
 *
 * สิ่งที่เพิ่ม:
 *   Delta_DualMerkleRecord  — 180B struct รวม A+B+combined root
 *   delta_dual_*            — API สำหรับ read/write/verify dual merkle
 *   delta_merkle_upgrade()  — upgrade snapshot.merkle (80B) → dual (180B)
 *   delta_merkle_which()    — detect format ของ merkle file ที่มีอยู่
 *
 * Backward compat:
 *   ถ้ามีแค่ snapshot.merkle (80B V3.5) → ยังอ่านได้ via delta.h เดิม
 *   ถ้ามี snapshot.merkle (180B V3.6)   → ใช้ delta_ext.h
 *   ไฟล์เดียวกัน ต่างขนาด — magic + size check แยกได้
 */

#ifndef POGLS_DELTA_EXT_H
#define POGLS_DELTA_EXT_H

#include "pogls_delta.h"           /* World A base — ไม่แก้ */
#include "pogls_delta_world_b.h"   /* World B lanes 4-7 */
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
   VERSION TAG
   ══════════════════════════════════════════════════════════════════════ */

#define DELTA_EXT_VERSION       2     /* V3.6 dual merkle = version 2  */
#define DELTA_MERKLE_V35_SIZE  ((int)sizeof(Delta_MerkleRecord))   /* actual = 84B */
#define DELTA_MERKLE_V36_SIZE 180     /* sizeof(Delta_DualMerkleRecord) */

/*
 * Delta_DualMerkleRecord — defined in pogls_delta_world_b.h (180B)
 * Layout:
 *   magic(4) version(4) epoch(8) seq_a×4(32) seq_b×4(32)
 *   root_a(32) root_b(32) root_ab(32) crc32(4) = 180B
 *
 * ไม่ redefine ที่นี่ — ใช้ struct จาก pogls_delta_world_b.h
 */

/* compile-time size guard */
typedef char _dual_merkle_size_check[
    sizeof(Delta_DualMerkleRecord) == DELTA_MERKLE_V36_SIZE ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════════
   MERKLE FORMAT DETECTION
   ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    MERKLE_FORMAT_UNKNOWN = 0,
    MERKLE_FORMAT_V35     = 1,   /* 80B — World A only          */
    MERKLE_FORMAT_V36     = 2,   /* 180B — Dual A+B             */
    MERKLE_FORMAT_MISSING = 3,   /* ไฟล์ไม่มี                   */
    MERKLE_FORMAT_CORRUPT = -1,  /* ขนาดหรือ magic ผิด          */
} MerkleFormat;

/*
 * delta_merkle_which — detect format ของ snapshot.merkle
 * path_dir: .pogls/<file>/ directory
 */
MerkleFormat delta_merkle_which(const char *path_dir);

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE COMPUTE
   ══════════════════════════════════════════════════════════════════════ */

/*
 * delta_dual_merkle_compute — คำนวณ root_a, root_b, root_ab
 *
 * ctx_a : World A context (lanes 0-3)
 * ctx_b : World B context (lanes 4-7)
 * out   : struct ที่จะเขียนลง
 *
 * root_ab = SHA256(root_a || root_b)  — 64B input
 * คืน 0=ok, -1=error
 */
int delta_dual_merkle_compute(const Delta_Context  *ctx_a,
                               const Delta_ContextB *ctx_b,
                               Delta_DualMerkleRecord *out);

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE WRITE / READ
   ══════════════════════════════════════════════════════════════════════ */

/*
 * delta_dual_merkle_write — เขียน DualMerkleRecord ลง snapshot.merkle
 * path_dir: .pogls/<file>/ directory
 * ใช้ protocol: write .pending → fsync → rename (ATOMIC FINAL)
 */
int delta_dual_merkle_write(const char *path_dir,
                             const Delta_DualMerkleRecord *rec);

/*
 * delta_dual_merkle_read — อ่าน snapshot.merkle
 * คืน 0=ok (V3.6 found), 1=V3.5 format (caller ใช้ delta.h เดิม)
 * คืน -1=missing, -2=corrupt
 */
int delta_dual_merkle_read(const char *path_dir,
                            Delta_DualMerkleRecord *out);

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE VERIFY
   ══════════════════════════════════════════════════════════════════════ */

/*
 * delta_dual_merkle_verify — ตรวจ CRC + root_ab = SHA256(root_a||root_b)
 * คืน 0=valid, -1=CRC fail, -2=root_ab mismatch
 */
int delta_dual_merkle_verify(const Delta_DualMerkleRecord *rec);

/* ══════════════════════════════════════════════════════════════════════
   UPGRADE PATH: V3.5 → V3.6
   ══════════════════════════════════════════════════════════════════════ */

/*
 * delta_merkle_upgrade — แปลง snapshot.merkle V3.5 (80B) → V3.6 (180B)
 *
 * อ่าน V3.5 record → copy seq_a + root_a ไป V3.6
 * World B: seq_b = 0, root_b = SHA256("empty") — B ยังไม่มี data
 * root_ab = SHA256(root_a || root_b_empty)
 *
 * path_dir: .pogls/<file>/ directory
 * คืน 0=ok, -1=error, 1=already V3.6 (ไม่ต้อง upgrade)
 */
int delta_merkle_upgrade(const char *path_dir);

/* ══════════════════════════════════════════════════════════════════════
   COMBINED RECOVERY
   ══════════════════════════════════════════════════════════════════════ */

/*
 * delta_ext_recover — boot scan รู้จัก V3.5 + V3.6 format
 *
 * ถ้าพบ V3.5: คืน Delta_DualRecovery.world_b = DELTA_RECOVERY_NEW
 * ถ้าพบ V3.6: ตรวจ A และ B แยกกัน
 *
 * path_dir: .pogls/<file>/ directory
 */
Delta_DualRecovery delta_ext_recover(const char *source_path);

/* ══════════════════════════════════════════════════════════════════════
   UTILITY
   ══════════════════════════════════════════════════════════════════════ */

/* human-readable format name */
static inline const char *delta_merkle_format_str(MerkleFormat f) {
    switch (f) {
        case MERKLE_FORMAT_V35:     return "V3.5 (80B World A)";
        case MERKLE_FORMAT_V36:     return "V3.6 (180B Dual A+B)";
        case MERKLE_FORMAT_MISSING: return "MISSING";
        case MERKLE_FORMAT_CORRUPT: return "CORRUPT";
        default:                    return "UNKNOWN";
    }
}

/* root_ab helper — combine A+B ด้วย SHA256 stub (ใช้ CRC chain) */
static inline void delta_combine_roots(const uint8_t root_a[32],
                                        const uint8_t root_b[32],
                                        uint8_t root_ab[32])
{
    /* SHA256(root_a || root_b) — CRC chain stub */
    uint8_t combined[64];
    memcpy(combined,      root_a, 32);
    memcpy(combined + 32, root_b, 32);

    uint32_t h[8];
    for (int i = 0; i < 8; i++) h[i] = 0x6a09e667u + (uint32_t)i;
    uint32_t c = delta_crc32(0, combined, 64);
    for (int i = 0; i < 8; i++) {
        h[i] ^= c;
        c = delta_crc32(c, (uint8_t*)&h[i], 4);
    }
    memcpy(root_ab, h, 32);
}

#endif /* POGLS_DELTA_EXT_H */
