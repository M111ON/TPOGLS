/*
 * geo_cylinder.h — Cylinder Geometry for Geomatrix
 * 6 LADOS (spokes), 60° each
 *
 * Number theory closure (full geometry — center restored):
 *   9 face × 64 unit × 6 side = 3456
 *   3456 / 6   = 576  = 24²   (slots per spoke)
 *   3456 / 54  = 64           (units per face)
 *   3456 / 144 = 24           (ThirdEye closure ✓)
 *   144  × 24  = 3456         ✓
 *
 * Face layout per spoke:
 *   8 face outer (freeze era) + 1 face center (restored)
 *   = 9 face × 64 unit = 576 slots per spoke
 *   CENTER_FACE = slot 512..575  (64 units)
 *
 * Side geometry:
 *   visible  3 side × 9 face = 27
 *   invert   3 side × 9 face = 27
 *   full     6 side × 9 face = 54  ✓ sacred number
 *
 * digit_sum chain:
 *   3456→18→9  576→18→9  144→9  24→6  54→9  64→1(origin)
 *
 * Map to GeoSeed bundles:
 *   spoke 0,1,2 = b[0],b[1],b[2]  visible
 *   spoke 3,4,5 = b[3],b[4],b[5]  hidden (invert pair)
 *   b[6],b[7]   = top/bottom ring pointer (free shadow)
 */

#ifndef GEO_CYLINDER_H
#define GEO_CYLINDER_H

#include <stdint.h>
#include <stdio.h>
#include "geomatrix_shared.h"

/* ── Constants (from geo_config.h) ── */
#include "geo_config.h"
#define CYL_SPOKES      GEO_SPOKES
#define CYL_FACES       GEO_FACES
#define CYL_FACE_UNITS  GEO_FACE_UNITS
#define CYL_SLOTS       GEO_SLOTS
#define CYL_FULL_N      GEO_FULL_N
#define CYL_OUTER_SLOTS GEO_OUTER_SLOTS
#define CYL_CENTER_BASE GEO_CENTER_BASE
#define CYL_CENTER_END  (GEO_CENTER_BASE + GEO_FACE_UNITS - 1)
#define CYL_SIDE_FULL   GEO_SIDE_FULL
#define CYL_SIDE_HALF   27u     /* 3 side × 9 face (visible half)     */

#define CYL_EVEN  0
#define CYL_ODD   1

/* ── slot classification ── */
static inline uint8_t geo_slot_is_center(uint16_t slot) {
    return slot >= CYL_CENTER_BASE;  /* 512..575 = center face */
}

/* full index → spoke (0..5)  fast_mod6 */
static inline uint8_t geo_spoke(uint16_t idx) {
    uint32_t q = ((uint32_t)idx * 10923U) >> 16;
    return (uint8_t)(idx - q * 6);
}

/* full index → slot within spoke (0..575) */
static inline uint16_t geo_spoke_slot(uint16_t idx) {
    return idx / CYL_SPOKES;
}

/* spoke + slot → full index */
static inline uint16_t geo_full_idx(uint8_t spoke, uint16_t slot) {
    return (uint16_t)(slot * CYL_SPOKES + spoke);
}

/* invert: cross center O(1)  0↔3  1↔4  2↔5 */
static inline uint8_t geo_spoke_invert(uint8_t spoke) {
    return (spoke + 3) % CYL_SPOKES;
}

/* ring: even=top, odd=bottom */
static inline uint8_t geo_spoke_ring(uint8_t spoke) {
    return spoke & 1;
}

/* slot → face index (0..8, face 8 = center) */
static inline uint8_t geo_slot_face(uint16_t slot) {
    return (uint8_t)(slot / CYL_FACE_UNITS);
}

/* slot → unit within face (0..63) */
static inline uint8_t geo_slot_unit(uint16_t slot) {
    return (uint8_t)(slot % CYL_FACE_UNITS);
}

/* decompose full index */
static inline void geo_idx_to_cyl(
    uint16_t idx,
    uint8_t  *spoke_out,
    uint8_t  *ring_out,
    uint8_t  *cross_out,
    uint8_t  *face_out)
{
    uint16_t slot  = geo_spoke_slot(idx);
    uint8_t  s     = geo_spoke(idx);
    *spoke_out = s;
    *ring_out  = geo_spoke_ring(s);
    *cross_out = geo_spoke_invert(s);
    *face_out  = geo_slot_face(slot);
}

/*
 * geo_hilbert_cylinder(out, n)
 * n = CYL_FULL_N (3456) for full space
 *     CYL_SLOTS  (576)  for per-spoke slice
 *
 * Pattern: 0→3→1→4→2→5 (cross-center pairs)
 * Each step crosses center → locality preserved
 * Center face (slot 512..575) included naturally
 */
static inline void geo_hilbert_cylinder(uint16_t *out, int n) {
    static const uint8_t order[6] = {0, 3, 1, 4, 2, 5};
    for (int i = 0; i < n; i++) {
        uint8_t  spoke = order[i % CYL_SPOKES];
        uint16_t slot  = (uint16_t)(i / CYL_SPOKES);
        out[i] = geo_full_idx(spoke, slot);
    }
}

static inline void geo_fill_hilbert_cylinder(uint16_t *H_inv, int n) {
    geo_hilbert_cylinder(H_inv, n);
}

static inline void geo_cylinder_stats(const uint16_t *H_inv, int n) {
    int spoke_count[CYL_SPOKES] = {0};
    int center_count = 0;
    int cross_ok = 0;

    for (int i = 0; i < n; i++) {
        uint16_t slot = geo_spoke_slot(H_inv[i]);
        spoke_count[geo_spoke(H_inv[i])]++;
        if (geo_slot_is_center(slot)) center_count++;
        if (i % 2 == 0 && i + 1 < n) {
            uint8_t s0 = geo_spoke(H_inv[i]);
            uint8_t s1 = geo_spoke(H_inv[i+1]);
            if (geo_spoke_invert(s0) == s1) cross_ok++;
        }
    }

    printf("[geo_cylinder_stats] n=%d / full=%d\n", n, CYL_FULL_N);
    printf("  geometry: %d side × %d face × %d unit\n",
           CYL_SPOKES, CYL_FACES, CYL_FACE_UNITS);
    for (int s = 0; s < CYL_SPOKES; s++)
        printf("  spoke %d (%s): %d slots (expect %d)\n",
               s, (s&1) ? "bot/odd " : "top/even",
               spoke_count[s], n / CYL_SPOKES);

    printf("  center face slots: %d (expect %d)\n",
           center_count, (n == CYL_FULL_N) ? (int)(CYL_SPOKES * 64) : -1);
    printf("  cross-center pairs: %d/%d\n", cross_ok, n/2);

    int inv_ok = 1;
    for (int s = 0; s < 3; s++) {
        if (spoke_count[s] != spoke_count[s+3]) {
            printf("  FAIL: spoke %d vs %d\n", s, s+3);
            inv_ok = 0;
        }
    }
    if (inv_ok) printf("  invert symmetry: OK ✓\n");
    printf("  closure: 144×24=%d ✓\n", 144*24);
}

#endif /* GEO_CYLINDER_H */
