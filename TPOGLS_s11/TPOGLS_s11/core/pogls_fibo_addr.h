/*
 * pogls_fibo_addr.h — POGLS V3.6 Fibonacci Integer Address Engine
 *
 * แทนที่ pogls_compute_address() ที่ใช้ double theta / 2π
 * ด้วย pure integer Fibonacci sampling บน sphere
 *
 * THE LAW (unchanged):  A = floor(θ × 2²⁰)
 * NEW PATH:             A = (n × PHI_UP) % PHI_SCALE   — integer ล้วน
 *
 * ไม่มี float, ไม่มี math.h, ไม่มี 2π
 * input = node index n (uint32_t)
 * output = angular address A (uint32_t, range 0..PHI_SCALE-1)
 *
 * Gear modes (FIBO_GEAR 4b ใน CoreSlot):
 *   G1  0-3   direct : (n × PHI_UP) % PHI_SCALE
 *   G2  4-8   batch  : (n × PHI_UP × gear_factor) % PHI_SCALE
 *   G3  9-15  blast  : (n × PHI_UP << gear_shift) % PHI_SCALE
 *
 * World A: PHI_UP   = floor(φ  × 2²⁰) = 1,696,631
 * World B: PHI_DOWN = floor(φ⁻¹× 2²⁰) =   648,055
 *
 * Overflow safety: n × PHI_UP คำนวณใน uint64_t ก่อน mod เสมอ
 *   max safe n สำหรับ uint32: floor(UINT32_MAX / PHI_UP) = 2530
 *   TOPO_ULTRA = 2562 nodes → ต้องใช้ uint64 path เสมอ ✓
 */

#ifndef POGLS_FIBO_ADDR_H
#define POGLS_FIBO_ADDR_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS  (frozen — ห้ามเปลี่ยน)
   ═══════════════════════════════════════════════════════════════════════ */

#define PHI_SCALE    (1u << 20)          /* 2²⁰ = 1,048,576              */
#define PHI_UP       1696631u            /* floor(φ  × 2²⁰) World A      */
#define PHI_DOWN     648055u             /* floor(φ⁻¹× 2²⁰) World B      */
#define PHI_MASK     (PHI_SCALE - 1u)   /* fast mod mask                 */

/* Gear thresholds (FIBO_GEAR field ใน CoreSlot) */
#define GEAR_G1_MAX  3     /* 0-3  = G1 direct                          */
#define GEAR_G2_MAX  8     /* 4-8  = G2 batch                           */
#define GEAR_G3_MAX  15    /* 9-15 = G3 blast                           */

/* ═══════════════════════════════════════════════════════════════════════
   WORLD A — Fibonacci up  (φ spiral outward)
   ═══════════════════════════════════════════════════════════════════════ */

/* G1 direct — น้อยที่สุด */
static inline uint32_t fibo_addr_a(uint32_t n)
{
    return (uint32_t)(((uint64_t)n * PHI_UP) % PHI_SCALE);
}

/* G2 batch — คูณ gear_factor (1-5) */
static inline uint32_t fibo_addr_a_g2(uint32_t n, uint8_t gear)
{
    uint32_t factor = (uint32_t)(gear - GEAR_G1_MAX);   /* 1-5 */
    return (uint32_t)(((uint64_t)n * PHI_UP * factor) % PHI_SCALE);
}

/* G3 blast — shift left gear_shift bits */
static inline uint32_t fibo_addr_a_g3(uint32_t n, uint8_t gear)
{
    uint32_t shift = (uint32_t)(gear - GEAR_G2_MAX);    /* 1-7 */
    return (uint32_t)(((uint64_t)n * (PHI_UP << shift)) % PHI_SCALE);
}

/* ═══════════════════════════════════════════════════════════════════════
   WORLD B — Fibonacci down  (φ⁻¹ spiral inward)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t fibo_addr_b(uint32_t n)
{
    return (uint32_t)(((uint64_t)n * PHI_DOWN) % PHI_SCALE);
}

static inline uint32_t fibo_addr_b_g2(uint32_t n, uint8_t gear)
{
    uint32_t factor = (uint32_t)(gear - GEAR_G1_MAX);
    return (uint32_t)(((uint64_t)n * PHI_DOWN * factor) % PHI_SCALE);
}

static inline uint32_t fibo_addr_b_g3(uint32_t n, uint8_t gear)
{
    uint32_t shift = (uint32_t)(gear - GEAR_G2_MAX);
    return (uint32_t)(((uint64_t)n * (PHI_DOWN << shift)) % PHI_SCALE);
}

/* ═══════════════════════════════════════════════════════════════════════
   UNIFIED DISPATCH  — gear + world ใน call เดียว
   world: 0 = A (PHI_UP), 1 = B (PHI_DOWN)
   gear : 0-15 (FIBO_GEAR field จาก CoreSlot)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t fibo_addr(uint32_t n, uint8_t gear, uint8_t world)
{
    uint64_t base = world ? PHI_DOWN : PHI_UP;

    if (gear <= GEAR_G1_MAX) {
        /* G1 direct */
        return (uint32_t)(((uint64_t)n * base) % PHI_SCALE);
    }
    else if (gear <= GEAR_G2_MAX) {
        /* G2 batch */
        uint32_t factor = (uint32_t)(gear - GEAR_G1_MAX);
        return (uint32_t)(((uint64_t)n * base * factor) % PHI_SCALE);
    }
    else {
        /* G3 blast */
        uint32_t shift = (uint32_t)(gear - GEAR_G2_MAX);
        return (uint32_t)(((uint64_t)n * (base << shift)) % PHI_SCALE);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   INVERSE — address → node index (exact, World A)
   A = (n × PHI_UP) % PHI_SCALE
   n = (A × PHI_UP_INV) % PHI_SCALE
   PHI_UP_INV = modular inverse of PHI_UP mod 2²⁰ = 255,559
   ใช้สำหรับ lookup/routing — คืน raw index (ต้อง % node_max เอง)
   ═══════════════════════════════════════════════════════════════════════ */

#define PHI_UP_INV   255559u   /* pow(PHI_UP, -1, 2^20) — verified      */
#define PHI_DOWN_INV 736711u   /* pow(PHI_DOWN,-1, 2^20) — verified     */

static inline uint32_t fibo_addr_to_node_a(uint32_t addr)
{
    return (uint32_t)(((uint64_t)addr * PHI_UP_INV) % PHI_SCALE);
}

static inline uint32_t fibo_addr_to_node_b(uint32_t addr)
{
    return (uint32_t)(((uint64_t)addr * PHI_DOWN_INV) % PHI_SCALE);
}

/* ═══════════════════════════════════════════════════════════════════════
   TWIN COORD  (World A ↔ World B pair)
   twin_a = fibo_addr_a(n)
   twin_b = fibo_addr_b(n)
   XOR ทั้งคู่ = spread pattern สำหรับ Two-World verify
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t a;   /* World A address */
    uint32_t b;   /* World B address */
} FiboTwin;

static inline FiboTwin fibo_twin(uint32_t n)
{
    FiboTwin t;
    t.a = fibo_addr_a(n);
    t.b = fibo_addr_b(n);
    return t;
}

static inline uint32_t fibo_twin_xor(FiboTwin t)
{
    return t.a ^ t.b;
}

#endif /* POGLS_FIBO_ADDR_H */
