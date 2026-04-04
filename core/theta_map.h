/*
 * theta_map.h — Input → Geometry Coordinate (Session 18)
 *
 * Role: entry point ของ write path
 *   raw uint64 → ThetaCoord (face, edge, z)
 *   ห้ามใช้ route_addr (history) เป็น input
 *
 * Design: pure integer mix, zero float, O(1)
 *   face = mix(x) % 12   → 0..11  dodecahedron face
 *   edge = mix(x) % 5    → 0..4   pentagon edge
 *   z    = mix(x) & 0xFF → 0..255 torus layer
 */

#ifndef THETA_MAP_H
#define THETA_MAP_H

#include <stdint.h>

/* ── output struct ──────────────────────────────────────────────── */
typedef struct {
    uint8_t face;   /* 0..11 */
    uint8_t edge;   /* 0..4  */
    uint8_t z;      /* 0..255 torus layer */
} ThetaCoord;

/* ── mix constants ──────────────────────────────────────────────── */
/* murmur finalizer-style: avalanche ทุก bit ก่อน mod */
#define THETA_MIX_A  UINT64_C(0xff51afd7ed558ccd)
#define THETA_MIX_B  UINT64_C(0xc4ceb9fe1a85ec53)

static inline uint64_t theta_mix64(uint64_t x) {
    x ^= x >> 33;
    x *= THETA_MIX_A;
    x ^= x >> 33;
    x *= THETA_MIX_B;
    x ^= x >> 33;
    return x;
}

/*
 * theta_map — main entry point
 *   input : raw data value (pre-encode)
 *   output: ThetaCoord → เข้า geo_route / diamond_flow ต่อ
 *
 * face mod 12: ใช้ Lemire fast reduce (ไม่มี division)
 *   (h * 12) >> 64  — แต่ C ไม่มี 128b โดยตรง ใช้ upper 32b แทน
 */
static inline ThetaCoord theta_map(uint64_t raw) {
    uint64_t h = theta_mix64(raw);

    /* face: Lemire-style reduce: (uint32_t)(h >> 32) * 12 >> 32 */
    uint32_t hi = (uint32_t)(h >> 32);
    uint8_t  face = (uint8_t)(((uint64_t)hi * 12u) >> 32);

    /* edge: ใช้ lower 32b ผ่าน rotate */
    uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
    uint8_t  edge = (uint8_t)(((uint64_t)lo * 5u) >> 32);

    /* z: mid-bits 23..16 หลัง mix — กระจายดีไม่ซ้ำ face/edge */
    uint8_t  z = (uint8_t)((h >> 16) & 0xFFu);

    return (ThetaCoord){ face, edge, z };
}

/*
 * theta_map_batch — แปลง N inputs พร้อมกัน
 *   out[] ต้องมีขนาด >= n
 */
static inline void theta_map_batch(const uint64_t *in, ThetaCoord *out, int n) {
    for (int i = 0; i < n; i++)
        out[i] = theta_map(in[i]);
}

#endif /* THETA_MAP_H */
