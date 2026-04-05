/*
 * pogls_delta_world_b.h — POGLS V3.6  World B Delta Lanes 4-7
 * ============================================================
 *
 * World B = ternary state space (3^n)
 * Lanes 4-7 mirror lanes 0-3 but carry ternary-addressed blocks
 * Dual Merkle root: merkle_A (lanes 0-3) + merkle_B (lanes 4-7)
 *
 * Design principles (inherited + extended):
 *  1. World A (lanes 0-3) ไม่ถูกแตะเลย — additive only
 *  2. World B lanes แยก subdir ใน .pogls/<file>/world_b/
 *  3. Dual Merkle: merkle_A + merkle_B → combined root = SHA256(A||B)
 *  4. Audit invariant extended:
 *       lane_X_B.seq == lane_nX_B.seq   (World B X pair)
 *       lane_Y_B.seq == lane_nY_B.seq   (World B Y pair)
 *  5. Switch Gate decides A or B — CPU only, no GPU involvement
 *  6. Commit protocol: audit_A → audit_B → merkle_A → merkle_B
 *                      → rename_A → rename_B → merkle_combined (ATOMIC FINAL)
 *
 * ternary address: addr_B = floor(θ × 3^n)  (n=12 default → 531,441)
 * Switch Gate: ENGINE_ID bit6 = 0 → World A / 1 → World B
 */

#ifndef POGLS_DELTA_WORLD_B_H
#define POGLS_DELTA_WORLD_B_H

#include "pogls_delta.h"   /* World A base — ไม่แก้ */

/* ══════════════════════════════════════════════════════════════════════
   WORLD B CONSTANTS
   ══════════════════════════════════════════════════════════════════════ */

/* Lane IDs — World B (ternary axis) */
#define LANE_B_X             4
#define LANE_B_NX            5   /* -X ternary */
#define LANE_B_Y             6
#define LANE_B_NY            7   /* -Y ternary */
#define LANE_B_COUNT         4
#define LANE_TOTAL           8   /* World A(4) + World B(4) */

/* ternary PHI scale — 3^12 = 531,441 */
#define PHI_SCALE_B          531441u          /* 3^12 */
#define PHI_UP_B             859684u          /* floor(φ  × 3^12) */
#define PHI_DOWN_B           328257u          /* floor(φ⁻¹ × 3^12) */

/* World B subdir inside .pogls/<file>/ */
#define POGLS_DIR_B          "world_b"

/* World B file names */
#define FNAME_B_LANE_X       "lane_bX.delta"
#define FNAME_B_LANE_NX      "lane_bnX.delta"
#define FNAME_B_LANE_Y       "lane_bY.delta"
#define FNAME_B_LANE_NY      "lane_bnY.delta"
#define FNAME_B_PENDING_X    "lane_bX.pending"
#define FNAME_B_PENDING_NX   "lane_bnX.pending"
#define FNAME_B_PENDING_Y    "lane_bY.pending"
#define FNAME_B_PENDING_NY   "lane_bnY.pending"

/* Dual Merkle files */
#define FNAME_MERKLE_A       "snapshot.merkle_A"        /* World A lanes 0-3 */
#define FNAME_MERKLE_B       "snapshot.merkle_B"        /* World B lanes 4-7 */
#define FNAME_MERKLE_AB      "snapshot.merkle"          /* combined = SHA256(A||B) */
#define FNAME_MERKLE_A_PEND  "snapshot.merkle_A.pending"
#define FNAME_MERKLE_B_PEND  "snapshot.merkle_B.pending"
#define FNAME_MERKLE_AB_PEND "snapshot.merkle.pending"

/* Switch Gate */
#define SWITCH_GATE_BIT      6                          /* ENGINE_ID bit6 */
#define WORLD_A              0
#define WORLD_B              1

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE RECORD — 144 bytes
   extends Delta_MerkleRecord to cover both worlds
   ══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* DELTA_MAGIC */
    uint32_t _pad;
    uint64_t epoch;              /* monotonic commit counter (shared) */
    uint64_t seq_a[LANE_COUNT];  /* World A lane seqs [0-3] */
    uint64_t seq_b[LANE_B_COUNT];/* World B lane seqs [4-7] */
    uint8_t  root_a[32];         /* SHA256 merkle root World A */
    uint8_t  root_b[32];         /* SHA256 merkle root World B */
    uint8_t  root_ab[32];        /* SHA256(root_a || root_b) combined */
    uint32_t crc32;              /* CRC32(bytes 0..139) */
} Delta_DualMerkleRecord;
/* sizeof = 4+4+8+(8×4)+(8×4)+32+32+32+4 = 180 bytes */

/* ══════════════════════════════════════════════════════════════════════
   WORLD B CONTEXT — wraps Delta_Context for B lanes
   ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    char     source_path[512];
    char     pogls_dir_b[512];         /* .pogls/<file>/world_b/ */
    int      lane_fd[LANE_B_COUNT];    /* fd ของ .pending files (index 0-3 = B lanes) */
    uint64_t lane_seq[LANE_B_COUNT];   /* seq per B lane */
    uint64_t epoch;                    /* shared epoch กับ World A */
    bool     is_open;
} Delta_ContextB;

/* ══════════════════════════════════════════════════════════════════════
   COMBINED CONTEXT — A + B together
   ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    Delta_Context  a;   /* World A — lanes 0-3 */
    Delta_ContextB b;   /* World B — lanes 4-7 */
} Delta_ContextAB;

/* ══════════════════════════════════════════════════════════════════════
   SWITCH GATE
   ══════════════════════════════════════════════════════════════════════ */

/*
 * pogls_switch_gate — dispatch block ไป World A หรือ B
 * ใช้ ENGINE_ID bit6 ของ core[0]
 * ~1-2 ns, no branch prediction miss
 */
static inline int pogls_switch_gate(const uint8_t core[8]) {
    uint8_t engine_id = (core[0] >> 1) & 0x7F;
    return (engine_id & (1 << SWITCH_GATE_BIT)) ? WORLD_B : WORLD_A;
}

/* ══════════════════════════════════════════════════════════════════════
   RECOVERY RESULT — extended for dual world
   ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int world_a;    /* DELTA_RECOVERY_CLEAN/TORN/NEW */
    int world_b;    /* DELTA_RECOVERY_CLEAN/TORN/NEW */
} Delta_DualRecovery;

/* ══════════════════════════════════════════════════════════════════════
   API — World B
   ══════════════════════════════════════════════════════════════════════ */

/* open World B context (creates world_b/ subdir if needed) */
int delta_b_open(Delta_ContextB *ctx, const char *source_path);

/* append 1 block ลง World B lane (lane_id must be LANE_B_X..LANE_B_NY) */
int delta_b_append(Delta_ContextB *ctx, uint8_t lane_id,
                   uint64_t addr, const void *data, uint32_t size);

/* audit World B: lane_B_X.seq == lane_B_NX.seq && Y pair */
int delta_b_audit(const Delta_ContextB *ctx);

/* close World B context */
int delta_b_close(Delta_ContextB *ctx);

/* ══════════════════════════════════════════════════════════════════════
   API — Combined A+B commit (atomic dual merkle)
   ══════════════════════════════════════════════════════════════════════ */

/* open both worlds */
int delta_ab_open(Delta_ContextAB *ctx, const char *source_path);

/*
 * atomic dual commit:
 *   1. audit_A  (lane 0-3 balanced)
 *   2. audit_B  (lane 4-7 balanced)
 *   3. merkle_A  ← SHA256 lanes 0-3
 *   4. merkle_B  ← SHA256 lanes 4-7
 *   5. merkle_AB ← SHA256(A||B)
 *   6. msync all pending
 *   7. rename A lanes
 *   8. rename B lanes
 *   9. rename merkle_AB.pending → snapshot.merkle  ← ATOMIC FINAL
 */
int delta_ab_commit(Delta_ContextAB *ctx);

/* boot recovery scan — both worlds */
Delta_DualRecovery delta_ab_recover(const char *source_path);

/* close both worlds */
int delta_ab_close(Delta_ContextAB *ctx);

/* utility */
const char *delta_b_lane_name(uint8_t lane_id);  /* "B_X" / "B_-X" / "B_Y" / "B_-Y" */
int         delta_b_merkle_compute(Delta_ContextB *ctx, uint8_t root_out[32]);

#endif /* POGLS_DELTA_WORLD_B_H */
