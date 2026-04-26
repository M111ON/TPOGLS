/*
 * pogls_fold.h — POGLS V3.6 Geometric Fold Architecture
 * ======================================================
 *
 * Diamond Block 64B  — 1 CPU cache line
 * Two-World A/B      — Switch Gate via ENGINE_ID bit6
 * HoneycombSlot      — reserved space สำหรับ Tails state
 * 3-Layer verify     — XOR → Fibo Intersect → Merkle
 *
 * กฎที่ห้ามแก้:
 *   DIAMOND_BLOCK_SIZE = 64   (1 cache line — ห้ามขยาย)
 *   Core Law: A = floor(θ × 2²⁰)  ← unchanged from V3.4
 *   V3.5 World A lanes 0-3         ← frozen, never touch
 *
 * Frozen constants (ห้าม change โดยไม่ bump version):
 *   PHI_UP   = 1,696,631
 *   PHI_DOWN =   648,055
 *   PHI_SCALE = 2²⁰ = 1,048,576
 */

#ifndef POGLS_FOLD_H
#define POGLS_FOLD_H

#include <stdint.h>
#include <string.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════
   FROZEN CONSTANTS — V3.4 / V3.5 (unchanged)
   ═══════════════════════════════════════════════════════════════════════ */

#define PHI_SCALE          (1u << 20)      /* 2²⁰ = 1,048,576           */
#define PHI_UP             1696631u        /* floor(φ  × 2²⁰)           */
#define PHI_DOWN           648055u         /* floor(φ⁻¹ × 2²⁰)          */

#define NODE_MAX           162             /* Icosphere L2               */
#define FACES_RAW          32              /* 5-bit FACE_ID              */
#define FACES_LOGICAL      256             /* after 2 folds              */

/* ═══════════════════════════════════════════════════════════════════════
   V3.6 NEW CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define DIAMOND_BLOCK_SIZE   64    /* 1 CPU cache line — FROZEN          */
#define CORE_SLOT_SIZE        8    /* 6B data + 2B reserved              */
#define INVERT_SIZE           8    /* NOT(Core Slot) — always active     */
#define ACTIVE_SIZE          16    /* Core + Invert                      */
#define QUAD_MIRROR_SIZE     32    /* 4 × rotated Core — AVX2 register   */
#define FOLD_SIZE            48    /* Quad Mirror(32) + Fold3 reserved(16)*/

#define WORLD_A_LANES_START   0    /* Lane 0-3: X/NX/Y/NY (V3.5 frozen) */
#define WORLD_A_LANES_END     3
#define WORLD_B_LANES_START   4    /* Lane 4-7: new World B              */
#define WORLD_B_LANES_END     7
#define ENGINE_WORLD_BIT   0x40    /* bit6 of ENGINE_ID: 0=A, 1=B        */

#define FOLD_VERSION_CURRENT  1    /* bump เมื่อ expand fold             */
#define UNIVERSE_HEADER_SIZE  8    /* optional prefix — multi-context    */

/* HoneycombSlot — reserved space ใน Fold3 สำหรับ Tails state           */
#define HONEYCOMB_SLOT_OFFSET 48   /* byte offset จากต้น Diamond Block   */
#define HONEYCOMB_SLOT_SIZE   16   /* 16B ใน Fold3 reserved              */

/* ═══════════════════════════════════════════════════════════════════════
   WORLD TYPE
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    WORLD_A = 0,   /* 2^n binary  — execution / routing                 */
    WORLD_B = 1,   /* 3^n ternary — shadow / witness / evolution        */
} world_t;

/* ═══════════════════════════════════════════════════════════════════════
   CORE SLOT  (8 bytes — packed big-endian)
   ═══════════════════════════════════════════════════════════════════════
   Bit layout (64 bits total):
     [63:59]  FACE_ID    5b  icosphere face 0-31 (→256 logical via fold)
     [58:52]  ENGINE_ID  7b  0-127, bit6=0→World A, bit6=1→World B
     [51:28]  VECTOR_POS 24b A = floor(θ × 2²⁰) per face
     [27:24]  FIBO_GEAR  4b  0-3=G1(direct), 4-8=G2(batch), 9-15=G3(blast)
     [23:16]  QUAD_FLAGS 8b  X/NX/Y/NY axis balance gates
     [15:0]   RESERVED   16b — not yet assigned
*/

typedef struct __attribute__((packed)) {
    uint64_t raw;   /* อ่าน/เขียนทั้ง slot ด้วย 1 instruction           */
} CoreSlot;

/* Core Slot field accessors */
static inline uint8_t  core_face_id   (CoreSlot c) { return (uint8_t)((c.raw >> 59) & 0x1F); }
static inline uint8_t  core_engine_id (CoreSlot c) { return (uint8_t)((c.raw >> 52) & 0x7F); }
static inline uint32_t core_vector_pos(CoreSlot c) { return (uint32_t)((c.raw >> 28) & 0xFFFFFF); }
static inline uint8_t  core_fibo_gear (CoreSlot c) { return (uint8_t)((c.raw >> 24) & 0x0F); }
static inline uint8_t  core_quad_flags(CoreSlot c) { return (uint8_t)((c.raw >> 16) & 0xFF); }

static inline world_t core_world(CoreSlot c)
{
    return (core_engine_id(c) & ENGINE_WORLD_BIT) ? WORLD_B : WORLD_A;
}

static inline CoreSlot core_slot_build(uint8_t  face_id,
                                        uint8_t  engine_id,
                                        uint32_t vector_pos,
                                        uint8_t  fibo_gear,
                                        uint8_t  quad_flags)
{
    CoreSlot c;
    c.raw = ((uint64_t)(face_id   & 0x1F) << 59)
          | ((uint64_t)(engine_id & 0x7F) << 52)
          | ((uint64_t)(vector_pos & 0xFFFFFF) << 28)
          | ((uint64_t)(fibo_gear  & 0x0F) << 24)
          | ((uint64_t)(quad_flags & 0xFF) << 16);
    return c;
}

/* ═══════════════════════════════════════════════════════════════════════
   INVERT  (8 bytes)
   XOR(Core, Invert) = 0xFF × 8  ← Layer 1 audit
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint64_t core_invert(CoreSlot c)
{
    return ~c.raw;   /* NOT(Core) — 1 instruction                       */
}

/* ═══════════════════════════════════════════════════════════════════════
   DIAMOND BLOCK  (64 bytes — exactly 1 CPU cache line)
   ═══════════════════════════════════════════════════════════════════════
   Layout:
     [0-7]    Core Slot   8B   active
     [8-15]   Invert      8B   active  — NOT(Core)
     [16-47]  Quad Mirror 32B  folded  — 4 rotated copies (AVX2)
     [48-63]  Fold3/Honey 16B  reserved — HoneycombSlot (Tails state)
*/

typedef struct __attribute__((aligned(64))) {
    /* ── ACTIVE (16B) ──────────────────────────────────────────────── */
    CoreSlot core;            /* 8B — primary data                      */
    uint64_t invert;          /* 8B — NOT(core.raw)                     */

    /* ── FOLDED SPACE (48B) ─────────────────────────────────────────── */
    uint8_t  quad_mirror[32]; /* 32B — 4 rotated Core copies (AVX2)     */
    uint8_t  honeycomb[16];   /* 16B — HoneycombSlot: Tails reserved    */
} DiamondBlock;

/* static assert — ห้าม compile ถ้าขนาดผิด */
typedef char _fold_size_check[sizeof(DiamondBlock) == 64 ? 1 : -1];

/* ═══════════════════════════════════════════════════════════════════════
   QUAD MIRROR BUILD
   4 rotated copies of Core Slot ใน 32B (AVX2 register)
   Copy 0: original          (rot 0)
   Copy 1: rotate_left 1B    (rot 8)
   Copy 2: rotate_left 2B    (rot 16)
   Copy 3: rotate_left 3B    (rot 24)
   ═══════════════════════════════════════════════════════════════════════ */

static inline void fold_build_quad_mirror(DiamondBlock *b)
{
    const uint8_t *src = (const uint8_t *)&b->core.raw;
    uint8_t       *dst = b->quad_mirror;

    /* Copy 0 — rot 0 */
    memcpy(dst,      src, 8);
    /* Copy 1 — rot 8 (rotate left 1 byte) */
    dst[8]  = src[1]; dst[9]  = src[2]; dst[10] = src[3]; dst[11] = src[4];
    dst[12] = src[5]; dst[13] = src[6]; dst[14] = src[7]; dst[15] = src[0];
    /* Copy 2 — rot 16 (rotate left 2 bytes) */
    dst[16] = src[2]; dst[17] = src[3]; dst[18] = src[4]; dst[19] = src[5];
    dst[20] = src[6]; dst[21] = src[7]; dst[22] = src[0]; dst[23] = src[1];
    /* Copy 3 — rot 24 (rotate left 3 bytes) */
    dst[24] = src[3]; dst[25] = src[4]; dst[26] = src[5]; dst[27] = src[6];
    dst[28] = src[7]; dst[29] = src[0]; dst[30] = src[1]; dst[31] = src[2];
}

/* ═══════════════════════════════════════════════════════════════════════
   DIAMOND BLOCK INIT
   ═══════════════════════════════════════════════════════════════════════ */

static inline DiamondBlock fold_block_init(uint8_t  face_id,
                                            uint8_t  engine_id,
                                            uint32_t vector_pos,
                                            uint8_t  fibo_gear,
                                            uint8_t  quad_flags)
{
    DiamondBlock b;
    memset(&b, 0, sizeof(b));
    b.core   = core_slot_build(face_id, engine_id,
                                vector_pos, fibo_gear, quad_flags);
    b.invert = core_invert(b.core);
    fold_build_quad_mirror(&b);
    /* honeycomb[16] = 0 — Tails เขียนเอง */
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════
   HONEYCOMB SLOT  — Tails reserved (16B ใน honeycomb[])
   ═══════════════════════════════════════════════════════════════════════
   Layout (16B):
     [0-7]   merkle_root   8B  last committed Merkle root (truncated)
     [8]     algo_id       1B  hash algo: 0=md5, 1=sha256
     [9]     migration     1B  MIGRATION_STATE: 0=IDLE,1=RUNNING,2=COMMITTED
     [10-11] dna_count     2B  จำนวน DNA entries ที่รู้จัก
     [12-15] reserved      4B  Tails ใช้ได้
   Tails เขียน — Entangle อ่านตอน TAILS_SPAWN
   Core ไม่แตะ
*/

typedef struct __attribute__((packed)) {
    uint64_t merkle_root;   /* last committed Merkle root (8B truncated) */
    uint8_t  algo_id;       /* 0=md5, 1=sha256                           */
    uint8_t  migration;     /* MIGRATION_STATE enum                      */
    uint16_t dna_count;     /* DNA entries ที่ Tails รู้จัก              */
    uint8_t  reserved[4];   /* Tails ใช้ได้                              */
} HoneycombSlot;

typedef char _hcomb_size_check[sizeof(HoneycombSlot) == 16 ? 1 : -1];

/* อ่าน/เขียน HoneycombSlot จาก DiamondBlock */
static inline HoneycombSlot honeycomb_read(const DiamondBlock *b)
{
    HoneycombSlot s;
    memcpy(&s, b->honeycomb, sizeof(s));
    return s;
}

static inline void honeycomb_write(DiamondBlock *b, const HoneycombSlot *s)
{
    memcpy(b->honeycomb, s, sizeof(*s));
}

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 1 — XOR AUDIT
   XOR(Core 8B, Invert 8B) == 0xFFFFFFFFFFFFFFFF
   Cost: ~0.3ns (1 XOR + 1 CMP)
   ═══════════════════════════════════════════════════════════════════════ */

static inline int fold_xor_audit(const DiamondBlock *b)
{
    return (b->core.raw ^ b->invert) == 0xFFFFFFFFFFFFFFFFull;
}

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 2 — FIBONACCI INTERSECT
   invariant = C0 AND C1 AND C2 AND C3
   bits ที่รอดจาก AND ทุก copy = geometric constants
   Cost: ~5-10ns (4 AND instructions หรือ 1 AVX2)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint64_t fold_fibo_intersect(const DiamondBlock *b)
{
    const uint64_t *q = (const uint64_t *)b->quad_mirror;
    /* C0 AND C1 AND C2 AND C3 */
    return q[0] & q[1] & q[2] & q[3];
}

/* entropy ต่ำเกินไป = block ต้องส่งต่อ Layer 3 */
static inline int fold_fibo_needs_merkle(const DiamondBlock *b)
{
    uint64_t inv = fold_fibo_intersect(b);
    /* นับ bits ที่รอด — ถ้า < 4 bits = entropy ต่ำเกินไป */
    return __builtin_popcountll(inv) == 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   SWITCH GATE  — dispatch World A / B
   bit6 ของ ENGINE_ID: 0 → World A (2^n), 1 → World B (3^n)
   Cost: ~1-2ns (shift + AND)
   ═══════════════════════════════════════════════════════════════════════ */

static inline world_t fold_switch_gate(const DiamondBlock *b)
{
    return core_world(b->core);
}

/* ═══════════════════════════════════════════════════════════════════════
   TWIN COORDINATE  — Two-World paired address
   twin = pos XOR invert_mask
   B promotes → A เมื่อ A ถูก eject
   ═══════════════════════════════════════════════════════════════════════ */

#define TWIN_INVERT_MASK   0x40   /* flip bit6 ของ ENGINE_ID = flip world */

static inline uint32_t fold_twin_engine_id(const DiamondBlock *b)
{
    uint8_t eid = core_engine_id(b->core);
    return (uint32_t)(eid ^ TWIN_INVERT_MASK);
}

/* ═══════════════════════════════════════════════════════════════════════
   UNIVERSE HEADER  (8B — optional prefix)
   เสียบข้างนอก DiamondBlock เพื่อขยาย context โดยไม่แตะ core
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t universe_id;    /* context ID                               */
    uint8_t  topo_level;     /* icosphere level (0-4)                    */
    uint8_t  fold_version;   /* expansion version                        */
    uint8_t  reserved[2];    /* future                                   */
} UniverseHeader;

typedef char _uhdr_size_check[sizeof(UniverseHeader) == 8 ? 1 : -1];

/* ═══════════════════════════════════════════════════════════════════════
   ENTANGLE OP CODE — TAILS_SPAWN
   เพิ่มใน entangle_graph.h op codes
   ═══════════════════════════════════════════════════════════════════════ */

#define EENTANGLE_OP_WRITE        1
#define EENTANGLE_OP_READ         2
#define EENTANGLE_OP_AUDIT        3
#define EENTANGLE_OP_DETACH       4
#define EENTANGLE_OP_TAILS_SPAWN  5   /* อ่าน HoneycombSlot → spawn Tails  */

/*
 * fold_tails_spawn_data — เตรียมข้อมูลสำหรับ Entangle TAILS_SPAWN
 *
 * Entangle worker เรียก function นี้หลัง pop EENTANGLE_OP_TAILS_SPAWN
 * คืน HoneycombSlot ที่ Tails ต้องการ boot กลับมา
 *
 * block  : Diamond Block ที่เก็บ HoneycombSlot ของ Tails
 * out    : ข้อมูลที่ส่งให้ spawn process ใหม่
 * คืน    : 1=มีข้อมูล, 0=slot ว่าง (Tails ยังไม่เคย commit)
 */
static inline int fold_tails_spawn_data(const DiamondBlock *block,
                                         HoneycombSlot      *out)
{
    HoneycombSlot s = honeycomb_read(block);
    if (s.merkle_root == 0 && s.dna_count == 0)
        return 0;   /* ว่าง — Tails ใหม่ ยังไม่เคย commit */
    *out = s;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
   3-LAYER VERIFY PIPELINE  (full path)
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    FOLD_VERIFY_PASS    =  0,   /* ผ่านทุก layer                         */
    FOLD_VERIFY_EJECT_1 = -1,   /* Layer 1 fail — XOR ผิด               */
    FOLD_VERIFY_EJECT_2 = -2,   /* Layer 2 fail — Fibo entropy ต่ำ       */
    FOLD_VERIFY_NEED_L3 =  1,   /* ต้องการ Merkle (Layer 3)              */
} FoldVerifyResult;

static inline FoldVerifyResult fold_verify(const DiamondBlock *b)
{
    /* Layer 1 — XOR: ~0.3ns */
    if (!fold_xor_audit(b))
        return FOLD_VERIFY_EJECT_1;

    /* Layer 2 — Fibo Intersect: ~5-10ns */
    if (fold_fibo_needs_merkle(b))
        return FOLD_VERIFY_NEED_L3;

    return FOLD_VERIFY_PASS;
}

#endif /* POGLS_FOLD_H */
