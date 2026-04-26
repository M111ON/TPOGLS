/*
 * pogls_compute_lut.h — POGLS V3.5 L1-Packed Compute LUT
 *
 * รวม LUT ทุกตัวไว้ใน struct เดียว → cache block เดียว → L1 hit ทุกครั้ง
 *
 * Layout (10.2KB รวม → พอดี L1 32KB):
 *   rubik_perm [18][256]  = 4608B   state navigation
 *   rubik_inv  [18][256]  = 4608B   inverse / undo
 *   morton_lut [256]      = 1024B   x-bit interleave (precomputed)
 *   node_lut   [256]      = 256B    addr>>12 → node_id (O(1))
 *   ─────────────────────────────
 *   Total                 = 10496B  ✓ < 32KB L1
 *
 * Usage:
 *   clut_init(&g_clut);          // call once at startup (เติม LUT)
 *   clut_perm(&g_clut, s, m)     // rubik perm ~1ns
 *   clut_morton(&g_clut, x, y)   // morton encode ~1ns (LUT, not bitwise)
 *   clut_node(&g_clut, addr)     // addr→node ~0.5ns
 *
 * ไม่แตะ WAL/Snapshot/Hydra/Ntacle
 * Namespace: clut_* / ComputeLUT / g_clut
 */

#ifndef POGLS_COMPUTE_LUT_H
#define POGLS_COMPUTE_LUT_H

#include <stdint.h>
#include <string.h>

#define CLUT_RUBIK_MOVES    18
#define CLUT_RUBIK_STATES   256   /* 8-bit — L1 friendly */
#define CLUT_MORTON_SIZE    256   /* lower 8 bits of each coord */
#define CLUT_NODE_SIZE      256   /* addr[19:12] → node_id      */

/* ═══════════════════════════════════════════════════════════════════════
   STRUCT  — 64-byte aligned so it starts on cache line boundary
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((aligned(64))) {

    /* Rubik permutation tables (18 moves × 256 states) */
    uint8_t  rubik_perm[CLUT_RUBIK_MOVES][CLUT_RUBIK_STATES]; /* 4608B */
    uint8_t  rubik_inv [CLUT_RUBIK_MOVES][CLUT_RUBIK_STATES]; /* 4608B */

    /* Morton x-interleave table — spread8(x) precomputed */
    uint32_t morton_lut[CLUT_MORTON_SIZE];                     /* 1024B */

    /* Node LUT — addr[19:12] (8 bits) → node_id (0..161) */
    uint8_t  node_lut[CLUT_NODE_SIZE];                         /*  256B */

} ComputeLUT;

_Static_assert(sizeof(ComputeLUT) <= 32768,
    "ComputeLUT must fit in L1 cache (32KB)");

/* ═══════════════════════════════════════════════════════════════════════
   GLOBAL INSTANCE  (one per process — all engines share)
   ═══════════════════════════════════════════════════════════════════════ */

extern ComputeLUT g_clut;

/* ═══════════════════════════════════════════════════════════════════════
   INLINE HOT PATH  (~0.5–1ns each — single array lookup)
   ═══════════════════════════════════════════════════════════════════════ */

/* rubik permutation via packed LUT */
static inline uint8_t clut_perm(const ComputeLUT *c,
                                 uint8_t state, uint8_t move)
{
    return c->rubik_perm[move % CLUT_RUBIK_MOVES][state];
}

/* rubik inverse */
static inline uint8_t clut_inv(const ComputeLUT *c,
                                uint8_t state, uint8_t move)
{
    return c->rubik_inv[move % CLUT_RUBIK_MOVES][state];
}

/*
 * clut_morton — 2D morton encode via LUT (~1ns, no bitwise ops)
 *
 * ใช้ precomputed morton_lut[x & 0xFF] แทน morton_spread16()
 * เพียงพอสำหรับ 8-bit per axis (256×256 grid = 65536 nodes)
 * สำหรับ 16-bit coords: ใช้ clut_morton16() ด้านล่าง
 */
static inline uint32_t clut_morton(const ComputeLUT *c,
                                    uint8_t x, uint8_t y)
{
    return (c->morton_lut[x] << 1) | c->morton_lut[y];
}

/*
 * clut_morton16 — 16-bit coords via 2×LUT lookup (ยังเร็วกว่า bitwise)
 * แยก x เป็น lo/hi byte แล้ว combine
 */
static inline uint32_t clut_morton16(const ComputeLUT *c,
                                      uint16_t x, uint16_t y)
{
    uint32_t xlo = c->morton_lut[x & 0xFF];
    uint32_t xhi = c->morton_lut[(x >> 8) & 0xFF];
    uint32_t ylo = c->morton_lut[y & 0xFF];
    uint32_t yhi = c->morton_lut[(y >> 8) & 0xFF];
    return ((xhi << 17) | (xlo << 1)) | ((yhi << 16) | ylo);
}

/* addr→node lookup  (~0.5ns) */
static inline uint8_t clut_node(const ComputeLUT *c, uint32_t addr20)
{
    return c->node_lut[(addr20 >> 12) & 0xFF];
}

/* full address pipeline: rubik → morton → addr → node  (~3ns total) */
static inline uint8_t clut_pipeline(const ComputeLUT *c,
                                     uint8_t  state, uint8_t  move,
                                     uint8_t  x,     uint8_t  y)
{
    uint8_t  ns   = clut_perm(c, state, move);
    uint32_t mort = clut_morton(c, x, y);
    uint32_t addr = (mort ^ ((uint32_t)ns << 12)) & 0xFFFFF;
    return clut_node(c, addr);
}

/* ═══════════════════════════════════════════════════════════════════════
   INIT API  (implemented in pogls_compute_lut.c)
   ═══════════════════════════════════════════════════════════════════════ */

/* build all LUT tables from scratch — call once at startup */
void clut_init(ComputeLUT *c);

/* fill node_lut from existing NodeState adjacency (pass NULL to use default) */
void clut_node_build(ComputeLUT *c, const uint8_t *node_map, uint32_t size);

/* selftest — returns 0 on pass */
int clut_selftest(const ComputeLUT *c);

#endif /* POGLS_COMPUTE_LUT_H */
