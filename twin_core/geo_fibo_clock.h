/*
 * geo_fibo_clock.h — Fibo Phase Clock (LEAN / ZERO-WASTE)
 * =======================================================
 * Session 16: GeoSeed + ThirdEye integrated
 *
 * ✔ ไม่มี total_f
 * ✔ ไม่มี drift_count / sig_fail_count
 * ✔ ไม่มี last_drift_type
 * ✔ ใช้ delta trend แทน invariant
 * ✔ [NEW] GeoSeed dual-channel (gen2=spatial, gen3=temporal) in FiboCtx
 * ✔ [NEW] ThirdEye 144-cycle observer co-located with fibo flush boundary
 *
 * GeoSeed wire:
 *   route_sig (uint64_t) → replaced by GeoSeed (2×u64, 2 register, 0 overhead)
 *   fibo_clock_tick uses seed.gen2 for SIG verify channel
 *   te_tick uses seed as cur snapshot arg
 *
 * ThirdEye alignment:
 *   FIBO_PERIOD_FLUSH = 144 = TE_CYCLE → flush and ThirdEye snap co-fire
 *   val_drift = popcount diff between fibo fx consecutive calls (free from accum)
 */

#ifndef GEO_FIBO_CLOCK_H
#define GEO_FIBO_CLOCK_H

#include <stdint.h>
#include "pogls_fold.h"
#include "geo_thirdeye.h"   /* GeoSeed, ThirdEye, te_tick, te_get_mask */

/* ── phase clock periods ─────────────────────────────────────────── */
#define FIBO_PERIOD_SIG       17u
#define FIBO_PERIOD_DRIFT     72u
#define FIBO_PERIOD_FLUSH    144u   /* = TE_CYCLE — co-fire with ThirdEye snap */
#define FIBO_PERIOD_SNAP     720u

/* ── cross-world bridge ──────────────────────────────────────────── */
#define FIBO_BRIDGE_SQ    20736u
#define FIBO_CPU_WORLD      128u
#define FIBO_ICOSA_WORLD    162u

/* ── event flags ─────────────────────────────────────────────────── */
typedef uint8_t FiboEvent;
#define FIBO_EV_NONE        0x00
#define FIBO_EV_SIG_FAIL    0x01
#define FIBO_EV_DRIFT       0x02
#define FIBO_EV_FLUSH       0x04
#define FIBO_EV_SNAP        0x08
#define FIBO_EV_CROSS_DRIFT 0x10
#define FIBO_EV_TE_STRESSED 0x20   /* [NEW] ThirdEye STRESSED at flush boundary */
#define FIBO_EV_TE_ANOMALY  0x40   /* [NEW] ThirdEye ANOMALY at flush boundary  */

/* ── dual-channel (LEAN) ─────────────────────────────────────────── */
typedef struct {
    uint64_t sum8;
    uint64_t sum9;
} FiboDualCh;

/* ── clock ───────────────────────────────────────────────────────── */
typedef struct {
    uint16_t c17;
    uint8_t  c72;
    uint8_t  c144;
    uint16_t c720;
} FiboClockCtx;

/* ── context (LEAN + GeoSeed + ThirdEye) ────────────────────────── */
typedef struct {
    FiboDualCh   dual;
    FiboClockCtx clk;
    uint64_t     prev_delta;      /* trend memory (cumulative)           */
    uint64_t     prev_window_inc; /* per-window increment for DRIFT      */

    /* [NEW] GeoSeed — 2 register, zero overhead vs old uint64_t sig   */
    GeoSeed      seed;        /* gen2=spatial channel, gen3=temporal    */

    /* [NEW] ThirdEye — 144-cycle observer, co-fires with FLUSH        */
    ThirdEye     eye;

    /* [NEW] prev_fx — for val_drift computation (popcount diff)       */
    uint32_t     prev_fx;
} FiboCtx;

/* ════════════════════════════════════════════════════════════════════
 * INIT
 * ════════════════════════════════════════════════════════════════════ */

static inline void fibo_ctx_init(FiboCtx *ctx)
{
    ctx->dual.sum8 = 0;
    ctx->dual.sum9 = 0;

    ctx->clk.c17  = FIBO_PERIOD_SIG;
    ctx->clk.c72  = FIBO_PERIOD_DRIFT;
    ctx->clk.c144 = FIBO_PERIOD_FLUSH;
    ctx->clk.c720 = FIBO_PERIOD_SNAP;

    ctx->prev_delta      = 0;
    ctx->prev_window_inc = 0;
    ctx->prev_fx         = 0;

    /* zero GeoSeed — caller sets via fibo_ctx_set_seed() if needed */
    ctx->seed.gen2 = 0;
    ctx->seed.gen3 = 0;

    te_init(&ctx->eye, ctx->seed);
}

/* ── GeoSeed entropy constants ─────────────────────────────────────
 * GOLDEN  : 2^64 / phi — breaks bit symmetry across all face_id values
 * PAIR_MIX: prime mix for parity lane — keeps A<>a XOR reversible
 * ──────────────────────────────────────────────────────────────── */
#define GEOSEED_GOLDEN   0x9E3779B97F4A7C15ULL
#define GEOSEED_PAIR_MIX 0x6C62272E07BB0142ULL  /* FNV-1a prime */

static inline void fibo_ctx_set_seed(FiboCtx *ctx, GeoSeed seed)
{
    /* ── gen2: spatial channel ─────────────────────────────────────
     * layout: [63:49] entropy(15b) | [48:33] parity(16b) | [32:0] topology+entropy(33b)
     *   topology bits [14:0]:
     *     face_id(4b) | vertex_mask(5b) | edge_mask(5b) | z(1b)
     * ------------------------------------------------------------ */
    uint64_t topo = seed.gen2 & 0x7FFFu;  /* low 15b = topology */

    /* anti-degenerate: if topology is all-zero, system is blind —
     * inject golden ratio constant scaled by a non-zero face slot (1)
     * so face_id=0 still gets a unique non-zero spatial identity    */
    if (topo == 0) {
        topo = (GEOSEED_GOLDEN & 0x7FFFu) | 0x0001u;  /* ensure non-zero */
        seed.gen2 = (seed.gen2 & ~0x7FFFu) | topo;
    }

    /* parity [48:33]: popcount of topology XOR mix — keeps A<>a
     * reversible: f(f(x))=x holds because parity is re-derived,
     * not stored as mutable state                                    */
    uint16_t parity = (uint16_t)(__builtin_popcountll(topo) & 0xFFFFu);
    parity ^= (uint16_t)(GEOSEED_PAIR_MIX & 0xFFFFu);

    /* entropy [63:49]: break remaining symmetry per face_id
     * vertex_edge_bias = (v*8) ^ (e*9)  — 2³:3² ratio as runtime op */
    uint8_t  face_id     = (uint8_t)((topo >> 11) & 0xFu);
    uint8_t  vertex_mask = (uint8_t)((topo >>  6) & 0x1Fu);
    uint8_t  edge_mask   = (uint8_t)((topo >>  1) & 0x1Fu);
    uint64_t ve_bias     = (uint64_t)((vertex_mask * 8u) ^ (edge_mask * 9u));
    uint64_t entropy     = (GEOSEED_GOLDEN ^ (face_id * GEOSEED_GOLDEN)) ^ ve_bias;

    seed.gen2 = topo
              | ((uint64_t)parity  << 33)
              | ((entropy & 0x1FFFFFFFFULL) << 15);  /* 33b entropy in [47:15] */

    /* final guard: if still zero after all transforms (edge case: all
     * masks=0 and face_id=0 after XOR collapse), force minimum identity */
    if (seed.gen2 == 0) seed.gen2 = GEOSEED_GOLDEN;

    /* ── gen3: temporal channel ────────────────────────────────────
     * must be non-zero for fibo clock to sequence correctly —
     * if caller passes 0, derive from gen2 so time is seeded from space */
    if (seed.gen3 == 0) seed.gen3 = seed.gen2 ^ GEOSEED_GOLDEN;

    ctx->seed = seed;
    te_init(&ctx->eye, seed);
}

/* ════════════════════════════════════════════════════════════════════
 * f(x)
 * ════════════════════════════════════════════════════════════════════ */

static inline uint32_t fibo_fx_full(const DiamondBlock *b, uint16_t hop)
{
    uint32_t bits = (uint32_t)__builtin_popcountll(b->core.raw);
    uint32_t gear = (uint32_t)core_fibo_gear(b->core);
    return bits + hop + gear;
}

static inline uint32_t fibo_fx_fast(const DiamondBlock *b)
{
    return (uint32_t)__builtin_popcountll(b->core.raw);
}

/* ════════════════════════════════════════════════════════════════════
 * ACCUM
 * ════════════════════════════════════════════════════════════════════ */

static inline void fibo_accum(FiboDualCh *d, uint32_t fx)
{
    d->sum8 += (uint64_t)fx << 3;
    d->sum9 += (uint64_t)fx * 9u;
}

/* ════════════════════════════════════════════════════════════════════
 * DRIFT CLASSIFY (stateless)
 * ════════════════════════════════════════════════════════════════════ */

static inline uint8_t fibo_drift_type(uint64_t delta, uint64_t prev)
{
    if (delta >= prev) return 0;

    int64_t diff = (int64_t)(prev - delta);

    int t = (diff % 9 == 0);
    int s = (diff % 8 == 0);

    return (t && !s) ? 1 :
           (s && !t) ? 2 : 3;
}

/* ════════════════════════════════════════════════════════════════════
 * SIG (17 system) — uses seed.gen2 as spatial channel
 * ════════════════════════════════════════════════════════════════════ */

static inline uint64_t fibo_sig_encode(uint64_t N)
{
    return (N << 4) + N + 0xFFFFFFFFFFFFFFF7ULL;
}

static inline int fibo_sig_verify(uint64_t sig)
{
    return ((sig + 9) % 17) == 0;
}

static inline uint64_t fibo_sig_decode(uint64_t sig)
{
    return (sig + 9) / 17;
}

/* ════════════════════════════════════════════════════════════════════
 * CROSS WORLD
 * ════════════════════════════════════════════════════════════════════ */

static inline int fibo_cross_check(uint64_t cpu_val, uint64_t icosa_val)
{
    return (cpu_val * FIBO_ICOSA_WORLD) == (icosa_val * FIBO_CPU_WORLD);
}

static inline uint64_t fibo_cpu_to_icosa(uint64_t cpu_val)
{
    return cpu_val * FIBO_ICOSA_WORLD / FIBO_CPU_WORLD;
}

static inline uint64_t fibo_icosa_to_cpu(uint64_t icosa_val)
{
    return icosa_val * FIBO_CPU_WORLD / FIBO_ICOSA_WORLD;
}

/* ════════════════════════════════════════════════════════════════════
 * CLOCK TICK (LEAN CORE + ThirdEye co-fire at L2 FLUSH)
 *
 * spoke: derived from seed.gen2 low bits (face % 6) — no extra arg
 * val_drift: |fx - prev_fx| >> 1 — free from prev_fx tracking
 * ════════════════════════════════════════════════════════════════════ */

static inline FiboEvent fibo_clock_tick(FiboCtx *ctx, uint32_t fx)
{
    FiboEvent ev = FIBO_EV_NONE;

    /* accumulate dual channel every tick — DRIFT depends on this */
    fibo_accum(&ctx->dual, fx);

    /* val_drift: popcount diff between consecutive fx (spatial drift signal) */
    uint32_t drift = (fx > ctx->prev_fx) ? (fx - ctx->prev_fx)
                                         : (ctx->prev_fx - fx);
    ctx->prev_fx = fx;

    /* spoke from seed.gen2 low bits: stable, 0 cost */
    uint8_t spoke    = (uint8_t)(ctx->seed.gen2 & 0x7u) % 6u;
    uint8_t slot_hot = (uint8_t)(drift > 4u ? 1u : 0u);

    /* L0: SIG — verify seed.gen2 (spatial channel) */
    if (--ctx->clk.c17 == 0) {
        ctx->clk.c17 = FIBO_PERIOD_SIG;
        if (!fibo_sig_verify(ctx->seed.gen2)) {
            ev |= FIBO_EV_SIG_FAIL;
        }
    }

    /* L1: DRIFT (trend-based) — per-window increment comparison */
    if (--ctx->clk.c72 == 0) {
        ctx->clk.c72 = FIBO_PERIOD_DRIFT;

        uint64_t delta     = ctx->dual.sum9 - ctx->dual.sum8;
        uint64_t increment = delta - ctx->prev_delta;  /* this window's net */

        /* DRIFT: this window contributed less than previous window */
        if (ctx->prev_window_inc > 0 && increment < ctx->prev_window_inc) {
            ev |= FIBO_EV_DRIFT;
        }

        ctx->prev_window_inc = increment;
        ctx->prev_delta      = delta;
    }

    /* L2: FLUSH — co-fires with ThirdEye 144-cycle snap */
    if (--ctx->clk.c144 == 0) {
        ctx->clk.c144 = FIBO_PERIOD_FLUSH;
        ev |= FIBO_EV_FLUSH;

        /* ThirdEye snap at flush boundary */
        te_tick(&ctx->eye, ctx->seed, spoke, slot_hot, drift);

        /* propagate ThirdEye state into event flags */
        if (ctx->eye.qrpn_state == QRPN_ANOMALY)
            ev |= FIBO_EV_TE_ANOMALY;
        else if (ctx->eye.qrpn_state == QRPN_STRESSED)
            ev |= FIBO_EV_TE_STRESSED;
    } else {
        /* tick every op for spoke_count accumulation */
        te_tick(&ctx->eye, ctx->seed, spoke, slot_hot, drift);
    }

    /* L3: SNAP */
    if (--ctx->clk.c720 == 0) {
        ctx->clk.c720 = FIBO_PERIOD_SNAP;
        ev |= FIBO_EV_SNAP;
    }

    return ev;
}

/* ════════════════════════════════════════════════════════════════════
 * ThirdEye query helpers (inline, 0 cost)
 * ════════════════════════════════════════════════════════════════════ */

/* mirror mask for current spoke — drives torus XRay face selection */
static inline uint8_t fibo_mirror_mask(const FiboCtx *ctx)
{
    uint8_t spoke = (uint8_t)(ctx->seed.gen2 & 0x7u) % 6u;
    return te_get_mask(&ctx->eye, spoke);
}

static inline uint8_t fibo_qrpn_state(const FiboCtx *ctx)
{
    return ctx->eye.qrpn_state;
}

/* ════════════════════════════════════════════════════════════════════
 * RESET
 * ════════════════════════════════════════════════════════════════════ */

static inline void fibo_dual_reset(FiboDualCh *d)
{
    d->sum8 = 0;
    d->sum9 = 0;
}

/* ════════════════════════════════════════════════════════════════════
 * HOP — updated: route_sig removed, GeoSeed used internally
 * ════════════════════════════════════════════════════════════════════ */

static inline FiboEvent fibo_hop(FiboCtx           *ctx,
                                  const DiamondBlock *b,
                                  uint16_t            hop)
{
    uint32_t fx = fibo_fx_full(b, hop);
    return fibo_clock_tick(ctx, fx);
}

static inline FiboEvent fibo_hop_fast(FiboCtx           *ctx,
                                       const DiamondBlock *b)
{
    uint32_t fx = fibo_fx_fast(b);
    return fibo_clock_tick(ctx, fx);
}

#endif /* GEO_FIBO_CLOCK_H */
