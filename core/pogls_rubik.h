/*
 * pogls_rubik.h — POGLS V3.5 Rubik Permutation Engine
 *
 * State navigation ด้วย permutation LUT แทนการ rewrite index
 *   state → perm(state, move) → new state   (~3ns)
 *   reversible, deterministic, parity-auditable
 *
 * วางเหนือ Angular Address Layer — ไม่แตะ WAL/Snapshot/Hydra/Ntacle
 * ใช้ร่วมกับ Morton encode ในขั้นตอน address pipeline
 *
 * Namespace: rubik_* / Rubik*
 */

#ifndef POGLS_RUBIK_H
#define POGLS_RUBIK_H

#include <stdint.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define RUBIK_MOVES       18    /* standard: U U' U2 D D' D2 F F' F2
                                             B B' B2 R R' R2 L L' L2  */
#define RUBIK_STATES      40320 /* 8! corner permutations (compact)    */
#define RUBIK_LUT_DEPTH   6     /* precompute depth for move sequence   */

/* ═══════════════════════════════════════════════════════════════════════
   MOVE CODES
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    RUBIK_U=0, RUBIK_Up=1, RUBIK_U2=2,
    RUBIK_D=3, RUBIK_Dp=4, RUBIK_D2=5,
    RUBIK_F=6, RUBIK_Fp=7, RUBIK_F2=8,
    RUBIK_B=9, RUBIK_Bp=10,RUBIK_B2=11,
    RUBIK_R=12,RUBIK_Rp=13,RUBIK_R2=14,
    RUBIK_L=15,RUBIK_Lp=16,RUBIK_L2=17,
} rubik_move_t;

/* ═══════════════════════════════════════════════════════════════════════
   PERMUTATION TABLE  (extern — built by rubik_init)
   perm_lut[move][state]     → new state after move
   perm_inv[move][state]     → state before move (inverse)
   ═══════════════════════════════════════════════════════════════════════ */

/* Compact form: index into 8-element permutation array (0..40319)
 * ใช้ uint16_t เพราะ 40320 < 65536 → LUT ขนาด 18×40320×2B = 1.4MB
 *
 * สำหรับ POGLS ใช้ state space เล็กลง (8-bit corner index = 256 states)
 * เพื่อให้ LUT อยู่ใน L1 cache (~4KB)
 */
#define RUBIK_COMPACT_STATES 256   /* 8-bit state — L1 friendly        */

extern uint8_t rubik_perm_lut[RUBIK_MOVES][RUBIK_COMPACT_STATES];
extern uint8_t rubik_perm_inv[RUBIK_MOVES][RUBIK_COMPACT_STATES];

/* ═══════════════════════════════════════════════════════════════════════
   INLINE HOT PATH  (~3ns)
   ═══════════════════════════════════════════════════════════════════════ */

/* apply move → new state */
static inline uint8_t rubik_perm(uint8_t state, uint8_t move)
{
    return rubik_perm_lut[move % RUBIK_MOVES][state];
}

/* undo move → previous state */
static inline uint8_t rubik_inv(uint8_t state, uint8_t move)
{
    return rubik_perm_inv[move % RUBIK_MOVES][state];
}

/* parity audit — Ntacle hook สำหรับ integrity check (~1ns) */
static inline uint8_t rubik_parity(uint8_t state)
{
    return __builtin_popcount(state) & 1;
}

/* apply sequence of moves */
static inline uint8_t rubik_apply_seq(uint8_t state,
                                       const uint8_t *moves,
                                       uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        state = rubik_perm(state, moves[i]);
    return state;
}

/* mix state into address (combine with Morton/angular) */
static inline uint32_t rubik_mix_addr(uint8_t state, uint32_t base_addr)
{
    /* XOR fold: state into upper bits of address space */
    return (base_addr ^ ((uint32_t)state << 12)) & ((1u << 20) - 1);
}

/* ═══════════════════════════════════════════════════════════════════════
   API
   ═══════════════════════════════════════════════════════════════════════ */

/* build perm_lut and perm_inv tables — call once at startup */
void rubik_init(void);

/* verify perm_lut integrity (self-test) — returns 0 on pass */
int rubik_selftest(void);

/* encode move sequence → compact uint64 (6 moves × 5bit = 30bit) */
static inline uint64_t rubik_encode_seq(const uint8_t *moves, int n)
{
    uint64_t enc = 0;
    for (int i = 0; i < n && i < 12; i++)
        enc |= ((uint64_t)(moves[i] & 0x1F)) << (i * 5);
    return enc;
}

static inline void rubik_decode_seq(uint64_t enc, uint8_t *moves, int n)
{
    for (int i = 0; i < n; i++)
        moves[i] = (uint8_t)((enc >> (i * 5)) & 0x1F);
}

/* ═══════════════════════════════════════════════════════════════════════
   TEAM-B COMPATIBLE API
   pogls_rubik_move / pogls_rubik_inverse / pogls_rubik_parity
   Maps onto our 8-bit 18-move LUT — ไม่มี extern LUT แยกต่างหาก
   (team-B ใช้ uint32[12][1024]=48KB → ไม่ L1-friendly, ไม่ merge)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t pogls_rubik_move(uint32_t state, uint8_t move_id)
{
    return rubik_perm((uint8_t)(state & 0xFF), move_id);
}

static inline uint32_t pogls_rubik_inverse(uint32_t state, uint8_t move_id)
{
    return rubik_inv((uint8_t)(state & 0xFF), move_id);
}

static inline uint8_t pogls_rubik_parity(uint32_t state)
{
    return rubik_parity((uint8_t)(state & 0xFF));
}

#endif /* POGLS_RUBIK_H */
