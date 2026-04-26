/*
 * ts_mirror.h — MirrorBlock: 10-byte compressed read view of DiamondBlock
 *
 * DiamondBlock 64B → MirrorBlock 10B = 6.4× cache density on read path
 * Write path untouched — mirror is derived-on-read, never persisted as primary.
 *
 * Layout (80 bits = 10 bytes, packed):
 *   word0 [63:0]  uint64_t:
 *     [63:59]  face_id      (5b)  — dodeca face 0..11 + proxy bits
 *     [58:52]  engine_id    (7b)  — includes world_bit at [58]
 *     [51:28]  vector_pos   (24b) — spatial position in field
 *     [27:24]  fibo_gear    (4b)  — clock gear state
 *     [23:16]  quad_flags   (8b)  — QRPN / mirror control
 *     [15:0]   dna_count    (16b) — from honeycomb, read-path relevance
 *   word1 [15:0] uint16_t:
 *     [15:0]   invert_low   (16b) — low 16b of invert for quick XOR check
 *
 * Total: 8B + 2B = 10B. Static assert enforced.
 *
 * Compress: mirror_compress(DiamondBlock*) → MirrorBlock
 * Expand:   mirror_expand(MirrorBlock*)   → CoreSlot  (active fields only)
 * Audit:    mirror_xor_ok(MirrorBlock*)   → 1 if invert_low consistent
 *
 * Requires: pogls_fold.h (DiamondBlock, CoreSlot, HoneycombSlot)
 */
#ifndef TS_MIRROR_H
#define TS_MIRROR_H

#include <stdint.h>
#include <string.h>
#include "pogls_fold.h"

/* ── MirrorBlock: 10 bytes packed ─────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t word0;       /* face_id|engine_id|vector_pos|fibo_gear|quad_flags|dna_count */
    uint16_t invert_low;  /* low 16b of core.raw ^ 0xFFFF — quick consistency check */
} MirrorBlock;

typedef char _mirror_size_check[sizeof(MirrorBlock) == 10 ? 1 : -1];

/* ── pack helpers ──────────────────────────────────────────────── */
static inline uint64_t mirror_pack(uint8_t  face_id,
                                   uint8_t  engine_id,
                                   uint32_t vector_pos,
                                   uint8_t  fibo_gear,
                                   uint8_t  quad_flags,
                                   uint16_t dna_count)
{
    return ((uint64_t)(face_id   & 0x1F) << 59)
         | ((uint64_t)(engine_id & 0x7F) << 52)
         | ((uint64_t)(vector_pos & 0xFFFFFFu) << 28)
         | ((uint64_t)(fibo_gear  & 0x0F) << 24)
         | ((uint64_t)(quad_flags & 0xFF) << 16)
         | ((uint64_t) dna_count);
}

/* ── unpack helpers ────────────────────────────────────────────── */
static inline uint8_t  mirror_face_id   (MirrorBlock m) { return (uint8_t)((m.word0 >> 59) & 0x1F); }
static inline uint8_t  mirror_engine_id (MirrorBlock m) { return (uint8_t)((m.word0 >> 52) & 0x7F); }
static inline uint32_t mirror_vector_pos(MirrorBlock m) { return (uint32_t)((m.word0 >> 28) & 0xFFFFFFu); }
static inline uint8_t  mirror_fibo_gear (MirrorBlock m) { return (uint8_t)((m.word0 >> 24) & 0x0F); }
static inline uint8_t  mirror_quad_flags(MirrorBlock m) { return (uint8_t)((m.word0 >> 16) & 0xFF); }
static inline uint16_t mirror_dna_count (MirrorBlock m) { return (uint16_t)(m.word0 & 0xFFFFu); }
static inline world_t  mirror_world     (MirrorBlock m) {
    return (mirror_engine_id(m) & ENGINE_WORLD_BIT) ? WORLD_B : WORLD_A;
}

/* ── compress: DiamondBlock → MirrorBlock ─────────────────────── */
static inline MirrorBlock mirror_compress(const DiamondBlock *b)
{
    CoreSlot       c = b->core;
    HoneycombSlot  h = honeycomb_read(b);
    MirrorBlock    m;
    m.word0      = mirror_pack(core_face_id(c),
                               core_engine_id(c),
                               core_vector_pos(c),
                               core_fibo_gear(c),
                               core_quad_flags(c),
                               h.dna_count);
    m.invert_low = (uint16_t)(b->invert & 0xFFFFu);
    return m;
}

/* ── expand: MirrorBlock → CoreSlot (active fields only) ─────── */
static inline CoreSlot mirror_expand(MirrorBlock m)
{
    CoreSlot c;
    c.raw = ((uint64_t)(mirror_face_id(m)    & 0x1F) << 59)
          | ((uint64_t)(mirror_engine_id(m)  & 0x7F) << 52)
          | ((uint64_t)(mirror_vector_pos(m) & 0xFFFFFFu) << 28)
          | ((uint64_t)(mirror_fibo_gear(m)  & 0x0F) << 24)
          | ((uint64_t)(mirror_quad_flags(m) & 0xFF) << 16);
    /* RESERVED[15:0] = 0 — not stored in mirror */
    return c;
}

/* ── quick XOR consistency check (low 16b) ────────────────────── */
/* returns 1 if invert_low == ~(core.raw & 0xFFFF) */
static inline int mirror_xor_ok(MirrorBlock m)
{
    uint16_t core_low = (uint16_t)(mirror_expand(m).raw & 0xFFFFu);
    return (uint16_t)(core_low ^ m.invert_low) == 0xFFFFu;
}

/* ── batch compress: n blocks → n MirrorBlocks ────────────────── */
/* dst must hold n MirrorBlock (10n bytes). No alignment required. */
static inline void mirror_compress_batch(MirrorBlock       *dst,
                                         const DiamondBlock *src,
                                         uint32_t            n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = mirror_compress(&src[i]);
}

#endif /* TS_MIRROR_H */
