/*
 * pogls_node_lut.h — POGLS V3.5 Address→Node LUT
 *
 * Maps 2^20 address space → 162 nodes in O(1)
 *   addr >> 12  →  8-bit bucket  →  node_id (0..161)
 *
 * Properties:
 *   - branchless single load
 *   - L1 resident (256B)
 *   - spatial clustering preserved (locality-preserving wrap)
 *   - first 162 buckets map 1:1, remaining 94 wrap mod 162
 *
 * ห้าม include pogls_hydra.h / pogls_snapshot.h ใน header นี้
 */

#ifndef POGLS_NODE_LUT_H
#define POGLS_NODE_LUT_H

#include <stdint.h>
#include "pogls_node_soa.h"   /* NODE_MAX, NODE_LUT_SIZE */

/* ═══════════════════════════════════════════════════════════════════════
   STATIC LUT (256 entries, 256 bytes — fits 4 cache lines)
   buckets 0..161   → 1:1 node id
   buckets 162..255 → wrap-around mod 162 (locality preserving)
   ═══════════════════════════════════════════════════════════════════════ */

static const uint8_t node_lut[NODE_LUT_SIZE] = {
    /* 0..159 — direct map */
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
     10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
     30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
     40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
     50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
     60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
     70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
     80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
     90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
    100,101,102,103,104,105,106,107,108,109,
    110,111,112,113,114,115,116,117,118,119,
    120,121,122,123,124,125,126,127,128,129,
    130,131,132,133,134,135,136,137,138,139,
    140,141,142,143,144,145,146,147,148,149,
    150,151,152,153,154,155,156,157,158,159,
    /* 160..161 — direct map */
    160,161,
    /* 162..255 — wrap mod 162 (94 entries) */
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
     10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
     30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
     40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
     50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
     60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
     70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
     80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
     90, 91, 92, 93
};

/* ═══════════════════════════════════════════════════════════════════════
   INLINE LOOKUP — hot path, ~2ns
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * node_lut_addr — map 20-bit address → node id
 *
 * addr20 = physical address & 0xFFFFF  (2^20 space)
 * bucket = addr20 >> 12                (0..255)
 * node   = node_lut[bucket]            (0..161)
 *
 * branchless, single memory load after LUT is L1-hot
 */
static inline uint32_t node_lut_addr(uint32_t addr20)
{
    return node_lut[(addr20 >> 12) & 0xFF];
}

/*
 * node_lut_raw — bypass addr masking, use precomputed bucket directly
 * ใช้เมื่อ caller มี bucket แล้ว (ประหยัด 1 shift)
 */
static inline uint32_t node_lut_raw(uint8_t bucket)
{
    return node_lut[bucket];
}

/* ═══════════════════════════════════════════════════════════════════════
   RUNTIME CUSTOM LUT (ถ้าต้องการ remap ตาม workload)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * lut_build_uniform — สร้าง LUT แบบ round-robin (default)
 * เรียกแทน static ถ้าต้องการ custom mapping
 */
static inline void lut_build_uniform(uint8_t *lut, uint32_t lut_size,
                                      uint32_t node_count)
{
    for (uint32_t i = 0; i < lut_size; i++)
        lut[i] = (uint8_t)(i % node_count);
}

/*
 * lut_build_weighted — กระจาย bucket ให้ตรงกับ node ที่มี degree สูงสุด
 * ใช้เมื่อ graph topology ไม่ uniform
 *
 * weight[node_count] : relative weight ต่อ node (sum ไม่จำเป็นต้อง = lut_size)
 */
void lut_build_weighted(uint8_t *lut, uint32_t lut_size,
                         const uint8_t *weight, uint32_t node_count);

#endif /* POGLS_NODE_LUT_H */
