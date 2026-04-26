/*
 * pogls_morton.h — POGLS V3.5 Morton / Z-order Spatial Engine
 *
 * แก้ปัญหา address distribution: θ → spatial locality
 *   morton2(x,y) → interleaved bits → address cluster ใน cache
 *
 * Properties:
 *   - ที่อยู่ใกล้ในพื้นที่ 2D → ใกล้กันใน memory
 *   - cache locality ↑, diffusion locality ↑, Ntacle scan เร็วขึ้น
 *   - cost ~2ns (branchless, no divide)
 *
 * วางเหนือ Angular Address — ไม่แตะ WAL/Snapshot/Hydra/Ntacle
 * Namespace: morton_* / Morton*
 */

#ifndef POGLS_MORTON_H
#define POGLS_MORTON_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════
   MORTON ENCODE  (bit interleave — branchless)
   ═══════════════════════════════════════════════════════════════════════ */

/* spread 16-bit x into even bits of 32-bit result
 * 0000_0000_0000_abcd → 0a0b_0c0d (4-bit example)
 */
static inline uint32_t morton_spread16(uint16_t x)
{
    uint32_t v = x;
    v = (v | (v << 8)) & 0x00FF00FFu;
    v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}

/*
 * morton2 — encode (x,y) → Z-order 32-bit key (~2ns)
 * x in even bits, y in odd bits
 * result range: 0 .. (2^32 - 1)
 */
static inline uint32_t morton2(uint16_t x, uint16_t y)
{
    return morton_spread16(x) | (morton_spread16(y) << 1);
}

/*
 * morton2_addr20 — map (x,y) → 20-bit POGLS address
 * folds 32-bit Z-key into 2^20 space via XOR fold
 */
static inline uint32_t morton2_addr20(uint16_t x, uint16_t y)
{
    uint32_t z = morton2(x, y);
    /* XOR fold upper 12 bits into lower 20 */
    return (z ^ (z >> 20)) & ((1u << 20) - 1);
}

/* ═══════════════════════════════════════════════════════════════════════
   DECODE  (unpack morton back to x,y)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint16_t morton_compact16(uint32_t x)
{
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0F0F0F0Fu;
    x = (x | (x >> 4)) & 0x00FF00FFu;
    x = (x | (x >> 8)) & 0x0000FFFFu;
    return (uint16_t)x;
}

static inline void morton2_decode(uint32_t z, uint16_t *x, uint16_t *y)
{
    *x = morton_compact16(z);
    *y = morton_compact16(z >> 1);
}

/* ═══════════════════════════════════════════════════════════════════════
   3D MORTON  (optional — สำหรับ volume compute)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t morton_spread10(uint32_t x)
{
    x &= 0x000003FFu;
    x = (x | (x << 16)) & 0xFF0000FFu;
    x = (x | (x <<  8)) & 0x0300F00Fu;
    x = (x | (x <<  4)) & 0x030C30C3u;
    x = (x | (x <<  2)) & 0x09249249u;
    return x;
}

static inline uint32_t morton3(uint16_t x, uint16_t y, uint16_t z)
{
    return morton_spread10(x)
         | (morton_spread10(y) << 1)
         | (morton_spread10(z) << 2);
}

/* ═══════════════════════════════════════════════════════════════════════
   ANGULAR → MORTON PIPELINE
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * morton_from_theta — แปลง angular coordinate → morton address
 *
 * theta_scaled : θ × 2^n แบบ integer (output จาก POGLS angular engine)
 * n            : bit precision (ปกติ 20)
 *
 * แยก theta_scaled เป็น x/y แล้ว morton encode
 * ให้ spatial locality ที่ angular address ขาด
 */
static inline uint32_t morton_from_theta(uint32_t theta_scaled, uint32_t n)
{
    /* ใช้ sqrt decomposition: x = lower half bits, y = upper half bits */
    uint32_t half = n >> 1;
    uint16_t x    = (uint16_t)(theta_scaled & ((1u << half) - 1));
    uint16_t y    = (uint16_t)(theta_scaled >> half);
    return morton2_addr20(x, y);
}

/*
 * morton_node_bucket — map morton address → 8-bit bucket → NodeLUT
 * ตรงกับ node_lut[addr >> 12] แต่ใช้ morton key ที่มี locality
 */
static inline uint8_t morton_node_bucket(uint32_t morton_addr20)
{
    return (uint8_t)((morton_addr20 >> 12) & 0xFF);
}

/* ═══════════════════════════════════════════════════════════════════════
   FULL ADDRESS PIPELINE (Rubik + Morton + Angular)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * morton_pipeline_addr — full 3-stage address computation
 *
 * rubik_state : Rubik permutation state (0..255)
 * x, y        : spatial coordinates
 * n            : angular precision bits
 *
 * returns 20-bit POGLS address with:
 *   - deterministic navigation (Rubik)
 *   - spatial locality (Morton)
 *   - angular folding (POGLS law)
 */
static inline uint32_t morton_pipeline_addr(uint8_t rubik_state,
                                             uint16_t x, uint16_t y,
                                             uint32_t n)
{
    uint32_t morton  = morton2_addr20(x, y);
    uint32_t angular = morton_from_theta(morton, n);
    /* mix rubik state into address */
    return (angular ^ ((uint32_t)rubik_state << 12)) & ((1u << n) - 1);
}

#endif /* POGLS_MORTON_H */
