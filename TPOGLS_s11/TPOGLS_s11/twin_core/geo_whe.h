/*
 * geo_whe.h — Wireless Hilbert Entanglement (WHE)
 * ================================================
 * Session 20+ Master Template — "กางครั้งเดียว ใช้ได้ตลอด"
 *
 * Architecture:
 *   Pentagon Hilbert Constraint  →  forbidden zone per step
 *   Violation Detection          →  detect actual ∧ forbidden
 *   Logic Tail (passive)         →  spawn on first deviation only
 *   Fingerprint Accumulator      →  64-bit per flow, zero path storage
 *   Mirror Symmetry Check        →  L/R anomaly via XOR
 *
 * Design Laws (ห้ามแก้):
 *   ❌  no path storage          (เก็บเฉพาะ violation + offset)
 *   ❌  no real entity/object    (tail = state fields ใน ctx เท่านั้น)
 *   ✅  XOR-based everywhere     (reversible + SIMD friendly)
 *   ✅  deterministic            (replay ได้จาก fingerprint)
 *   ✅  split/merge native       (struct copy = perfect clone)
 *
 * Wiring map:
 *   theta_map(raw)   →  ThetaCoord (face, edge, z)   [theta_map.h]
 *   geo_route_init() →  TorusNode                    [geo_route.h]
 *   whe_step()       →  violation + tail update      [this file]
 *   whe_final_fp()   →  64-bit fingerprint           [this file]
 *
 * Constants derive from geo_config.h — no magic numbers here.
 *
 * Number chain (sanity):
 *   5 triangles/pentagon  →  mask width = 5b
 *   12 faces dodeca       →  face field = 4b
 *   64 violation slots    →  violation_bits = 64b  ✓
 *   144 = TE_CYCLE        →  suspicious window K   ✓
 *
 * Usage:
 *   WheCtx ctx;
 *   whe_init(&ctx);
 *   for each step: whe_step(&ctx, actual_pos, expected_route, actual_route, step);
 *   uint64_t fp = whe_final_fp(&ctx);
 */

#ifndef GEO_WHE_H
#define GEO_WHE_H

#include <stdint.h>
#include "geo_config.h"    /* GEO_TE_CYCLE, GEO_SPOKES, etc. */
#include "theta_map.h"     /* ThetaCoord                      */

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 1 — Constants
 * ═══════════════════════════════════════════════════════════════════ */

/* PHI prime mixer — same as geo_hardening.h GEO_SEED_GUARD */
#define WHE_PHI_PRIME       UINT64_C(0x9E3779B97F4A7C15)

/* Suspicious tail window: if tail stays active > K steps → flag */
#define WHE_SUSPICIOUS_K    GEO_TE_CYCLE          /* = 144 */

/* Pentagon mask base: "เดิน 2 เว้น 1" → 3 allowed, 2 forbidden */
#define WHE_PENTA_BASE      0x1Au                 /* 0b11010 */

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 2 — Structs
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * FingerprintCtx — raw violation accumulator
 *   Pure XOR + sum. No path. No history.
 */
typedef struct {
    uint64_t violation_bits;   /* temporal XOR pattern: bit(step%64) */
    uint32_t drift_sum;        /* cumulative drift from route */
    uint32_t hit_count;        /* number of violations */
} FingerprintCtx;

/*
 * TailFP — logic tail state (passive, no object)
 *   Spawns on first deviation. Vanishes on recompose.
 *   Copy-safe → split = memcpy, merge = XOR fp.
 */
typedef struct {
    uint64_t fp;               /* running fingerprint */
    uint64_t tail_base;        /* expected route at first deviation */
    uint32_t tail_start_step;  /* step when tail activated */
    uint8_t  tail_on;          /* 1 = active, 0 = dormant */
    uint8_t  suspicious;       /* 1 = tail lived > WHE_SUSPICIOUS_K */
    uint8_t  _pad[2];
} TailFP;

/*
 * WheCtx — full WHE context per flow
 *   Everything fits in 2 cache lines (≤128B).
 *   Zero init = valid dormant state.
 */
typedef struct {
    FingerprintCtx fp_ctx;      /* violation accumulator      */
    TailFP         tail;        /* passive logic tail         */
    uint64_t       mirror_fp;   /* twin/mirror flow fp        */
    uint32_t       anomaly;     /* L/R mirror mismatch count  */
    uint32_t       step_count;  /* total steps processed      */
} WheCtx;

/* static assert: keep WheCtx ≤ 128B (2 cache lines) */
typedef char _whe_ctx_size_check[sizeof(WheCtx) <= 128 ? 1 : -1];

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 3 — Hilbert Constraint (Pentagon-aware)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_forbidden_mask — forbidden triangles for this step
 *   Pentagon = 5 triangles. Rule: "เดิน 2 เว้น 1"
 *   Fix 1: rotation binds to (step ^ expected_route[hi32])
 *     → constraint coupled to actual data, not time alone
 *     → prevents predictable pattern leak / fp collision
 *   Output: 5-bit mask (bit i = triangle i is forbidden)
 */
static inline uint8_t whe_forbidden_mask(uint32_t step, uint64_t expected_route)
{
    uint32_t s   = step ^ (uint32_t)(expected_route >> 32);
    uint8_t  rot = (uint8_t)(s % 5u);
    /* circular left-rotate 5-bit pattern */
    return (uint8_t)(((WHE_PENTA_BASE << rot) | (WHE_PENTA_BASE >> (5u - rot))) & 0x1Fu);
}

/*
 * whe_geo_to_triangle — GeoPos/ThetaCoord → triangle index
 *   edge 0..4 maps directly to triangle index in pentagon.
 *   O(1), pure field read.
 */
static inline uint8_t whe_geo_to_triangle(uint8_t edge)
{
    return edge % 5u;      /* clamp to pentagon range 0..4 */
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 4 — Violation Detection
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_detect_violation — is this position in a forbidden zone?
 *   Returns 1 if actual edge lands on forbidden triangle.
 *   Returns 0 if path is legal.
 */
static inline uint8_t whe_detect_violation(uint8_t actual_edge,
                                            uint8_t forbidden_mask)
{
    uint8_t tri = whe_geo_to_triangle(actual_edge);
    return (uint8_t)((forbidden_mask >> tri) & 1u);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 5 — Fingerprint Accumulate
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_fp_update — accumulate violation into FingerprintCtx
 *   violation: 0 or 1
 *   drift:     current route drift score
 *   step:      global step index
 */
static inline void whe_fp_update(FingerprintCtx *fp,
                                  uint8_t         violation,
                                  uint32_t        drift,
                                  uint32_t        step)
{
    if (violation) {
        fp->violation_bits ^= (UINT64_C(1) << (step & 63u));
        fp->hit_count++;
    }
    fp->drift_sum += drift;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 6 — Logic Tail (passive stalker)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_tail_update — core passive tail logic
 *   expected: expected route_addr from theta_map + hilbert
 *   actual:   ctx->route_addr (real pipeline value)
 *   step:     global step
 *
 * Behavior:
 *   normal data     → tail dormant, no work
 *   first deviation → tail_on = 1, record base
 *   while active    → XOR offset accumulates into fp
 *   recomposed      → tail_on = 0 (auto-kill)
 */
static inline void whe_tail_update(TailFP   *t,
                                    uint64_t  expected,
                                    uint64_t  actual,
                                    uint32_t  drift,
                                    uint32_t  step)
{
    /* activate on first violation
     * Fix 2: tail_base = expected ^ step → temporal uniqueness
     *   prevents repeated expected patterns from producing identical offsets */
    if (!t->tail_on && (actual != expected)) {
        t->tail_on         = 1;
        t->tail_base       = expected ^ (uint64_t)step;
        t->tail_start_step = step;
        t->suspicious      = 0;
    }

    if (t->tail_on) {
        uint64_t offset = actual ^ t->tail_base;

        /* Fix 3+4: full influence mix
         *   offset * PHI  → avalanche, collision-resistant across shards
         *   step << 1     → position-aware (even spread)
         *   drift << (step & 31) → separates "still deviation" vs "oscillating deviation" */
        t->fp ^= (offset * WHE_PHI_PRIME)
              ^ ((uint64_t)step << 1)
              ^ ((uint64_t)drift << (step & 31u));

        /* recomposed → kill tail */
        if (offset == 0u) {
            t->tail_on = 0;
            return;
        }

        /* flag suspicious if tail lives too long */
        if ((step - t->tail_start_step) > WHE_SUSPICIOUS_K) {
            t->suspicious = 1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 7 — Split / Merge (shard-safe)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_split — clone parent ctx into child (flow fragments)
 *   Tail state transfers automatically.
 *   Call when data shards.
 */
static inline void whe_split(const WheCtx *parent, WheCtx *child)
{
    *child = *parent;   /* struct copy — tail inherits */
}

/*
 * whe_merge — combine child fp back into parent (flow rejoins)
 *   XOR merge: order-independent, reversible.
 *   Call when shards recombine.
 */
static inline void whe_merge(WheCtx *parent, const WheCtx *child)
{
    /* Fix 3: PHI-mix child fp before XOR — prevents collision when N shards merge
     *   plain XOR: 2 identical children cancel each other → fp = 0 (silent)
     *   PHI-mix:   each child's fp avalanches before merge → unique contribution */
    parent->tail.fp               ^= (child->tail.fp * WHE_PHI_PRIME);
    parent->fp_ctx.violation_bits ^= child->fp_ctx.violation_bits;
    parent->fp_ctx.drift_sum      += child->fp_ctx.drift_sum;
    parent->fp_ctx.hit_count      += child->fp_ctx.hit_count;
    parent->anomaly               += child->anomaly;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 8 — Mirror Symmetry Check (optional, +signal)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_mirror_check — compare L/R twin flow fingerprints
 *   Asymmetry = topology anomaly signal.
 *   fp_left / fp_right: whe_final_fp() from twin flows.
 */
static inline uint8_t whe_mirror_check(WheCtx   *ctx,
                                        uint64_t  fp_left,
                                        uint64_t  fp_right)
{
    if (fp_left != fp_right) {
        ctx->anomaly++;
        ctx->mirror_fp = fp_left ^ fp_right;   /* delta fingerprint */
        return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 9 — Main Step (plug into your loop)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_step — single-step WHE update
 *   actual_edge:    actual position edge (from geo_route or pipeline)
 *   expected_route: expected route_addr from theta_map + hilbert
 *   actual_route:   real route_addr from ctx->route_addr
 *   drift:          current drift_acc from DiamondFlowCtx
 *   step:           global step counter
 *
 * This is the ONLY function you need to call per iteration.
 * Everything else (violation, tail, fp) updates automatically.
 */
static inline void whe_step(WheCtx   *ctx,
                              uint8_t   actual_edge,
                              uint64_t  expected_route,
                              uint64_t  actual_route,
                              uint32_t  drift,
                              uint32_t  step)
{
    /* 1. data-coupled forbidden mask (Fix 1) */
    uint8_t forbidden = whe_forbidden_mask(step, expected_route);

    /* 2. violation detection */
    uint8_t violation = whe_detect_violation(actual_edge, forbidden);

    /* 3. fingerprint accumulate */
    whe_fp_update(&ctx->fp_ctx, violation, drift, step);

    /* 4. passive tail logic */
    whe_tail_update(&ctx->tail, expected_route, actual_route, drift, step);

    ctx->step_count++;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 10 — Final Fingerprint
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * whe_violation_fp — fingerprint from violation accumulator only
 *   64-bit: violation pattern ^ drift ^ hit_count
 */
static inline uint64_t whe_violation_fp(const FingerprintCtx *fp)
{
    return fp->violation_bits
         ^ ((uint64_t)fp->drift_sum  << 32)
         ^ (uint64_t)fp->hit_count;
}

/*
 * whe_final_fp — combined fingerprint: violation + tail + route
 *   This is the SINGLE output of the WHE engine per flow.
 *   Deterministic from inputs — replay friendly.
 *
 *   actual_route: ctx->route_addr at end of flow
 *   hop_count:    ctx->hop_count  at end of flow
 */
static inline uint64_t whe_final_fp(const WheCtx *ctx,
                                     uint64_t      actual_route,
                                     uint32_t      hop_count)
{
    uint64_t v_fp = whe_violation_fp(&ctx->fp_ctx);
    return v_fp
         ^ ctx->tail.fp
         ^ actual_route
         ^ (uint64_t)hop_count;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 11 — Init / Reset
 * ═══════════════════════════════════════════════════════════════════ */

static inline void whe_init(WheCtx *ctx)
{
    /* zero = valid dormant state for all fields */
    __builtin_memset(ctx, 0, sizeof(WheCtx));
}

static inline void whe_reset(WheCtx *ctx)
{
    whe_init(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 12 — Status Query Helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* is tail currently tracking a deviation? */
static inline uint8_t whe_tail_active(const WheCtx *ctx)
{
    return ctx->tail.tail_on;
}

/* did tail live suspiciously long? */
static inline uint8_t whe_is_suspicious(const WheCtx *ctx)
{
    return ctx->tail.suspicious;
}

/* has mirror anomaly been detected? */
static inline uint8_t whe_has_anomaly(const WheCtx *ctx)
{
    return (ctx->anomaly > 0u) ? 1u : 0u;
}

/* violation rate: hit_count / step_count (×256 fixed-point) */
static inline uint32_t whe_violation_rate_fp8(const WheCtx *ctx)
{
    if (ctx->step_count == 0u) return 0u;
    return (ctx->fp_ctx.hit_count << 8) / ctx->step_count;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 13 — Example Loop (reference, not compiled)
 * ═══════════════════════════════════════════════════════════════════
 *
 * #if 0
 *
 * WheCtx whe;
 * whe_init(&whe);
 *
 * for (uint32_t step = 0; step < N; step++) {
 *
 *     // your existing pipeline
 *     ThetaCoord tc       = theta_map(raw[step]);
 *     uint64_t expected_r = geo_route_pack(&geo_route_init(tc));
 *
 *     // ... diamond_flow produces actual_route in ctx ...
 *     uint64_t actual_r = flow_ctx.route_addr;
 *
 *     // single WHE call — everything updates inside
 *     whe_step(&whe,
 *              tc.edge,          // actual_edge (from theta_map)
 *              expected_r,       // expected_route
 *              actual_r,         // actual_route
 *              flow_ctx.drift_acc,
 *              step);
 * }
 *
 * uint64_t fp = whe_final_fp(&whe, flow_ctx.route_addr, flow_ctx.hop_count);
 *
 * // optional: mirror check with twin flow
 * whe_mirror_check(&whe, fp_left, fp_right);
 *
 * #endif
 *
 * ═══════════════════════════════════════════════════════════════════
 * SECTION 14 — Self-Test (call from test harness only)
 * ═══════════════════════════════════════════════════════════════════ */

static inline int whe_selftest(void)
{
    /* Test 1: data-coupled mask — same step but diff expected_route → diff mask */
    uint8_t m_a = whe_forbidden_mask(0, 0x0000000000000000ULL);
    uint8_t m_b = whe_forbidden_mask(0, 0xDEADBEEF00000000ULL);
    if (m_a == m_b) return 0;   /* Fix 1: must differ — constraint is data-driven */

    /* Test 1b: periodicity — step+5 with same expected_route must match */
    uint64_t er = 0xABCD000000000000ULL;
    uint8_t m0  = whe_forbidden_mask(0, er);
    uint8_t m5  = whe_forbidden_mask(5, er);
    if (m0 != m5) return 0;

    /* Test 2: violation on forbidden triangle = 1 */
    uint8_t tri_forbidden = (uint8_t)__builtin_ctz(m0 & 0x1Fu);
    if (!whe_detect_violation(tri_forbidden, m0)) return 0;

    /* Test 3: tail lifecycle — no deviation / deviation / recompose */
    WheCtx ctx; whe_init(&ctx);
    whe_step(&ctx, 0, 0xABCDEFULL, 0xABCDEFULL, 0, 0);   /* normal */
    if (whe_tail_active(&ctx)) return 0;

    whe_step(&ctx, 0, 0xABCDEFULL, 0x000001ULL, 0, 1);    /* deviate */
    if (!whe_tail_active(&ctx)) return 0;

    /* recompose: actual_route must equal expected ^ step to kill tail
     * (Fix 2: tail_base = expected ^ step) */
    uint64_t tail_base_at1 = 0xABCDEFULL ^ (uint64_t)1;
    whe_step(&ctx, 0, 0xABCDEFULL, tail_base_at1, 0, 2);  /* recompose */
    if (whe_tail_active(&ctx)) return 0;

    /* Test 4: two identical shards must NOT cancel after merge (Fix 3) */
    WheCtx parent; whe_init(&parent);
    whe_step(&parent, 1, 0x100ULL, 0x200ULL, 5, 10);
    WheCtx child; whe_split(&parent, &child);

    uint64_t fp_before = parent.tail.fp;
    whe_merge(&parent, &child);
    /* PHI-mix: parent.fp ^= child.fp * PHI → never 0 if child.fp != 0 */
    if (parent.tail.fp == fp_before && child.tail.fp != 0) return 0;

    /* Test 5: drift distinguishes still vs oscillating (Fix 4)
     * two flows same offset but different drift → different fp */
    WheCtx c1; whe_init(&c1);
    WheCtx c2; whe_init(&c2);
    whe_step(&c1, 1, 0x100ULL, 0x200ULL,  0, 5);   /* drift = 0 */
    whe_step(&c2, 1, 0x100ULL, 0x200ULL, 99, 5);   /* drift = 99 */
    if (whe_final_fp(&c1, 0, 0) == whe_final_fp(&c2, 0, 0)) return 0;

    /* Test 6: mirror anomaly */
    WheCtx ctx2; whe_init(&ctx2);
    if (!whe_mirror_check(&ctx2, 0xAAAAULL, 0xBBBBULL)) return 0;

    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* GEO_WHE_H */
