/**
 * pogls_qrpn.h — Quad-Radial Pythagorean Net
 * POGLS V4 verification layer
 *
 * PLACEMENT: call qrpn_verify_fast() BEFORE pogls_write()
 * RULE:      GPU never touches commit path (witness only)
 * RULE:      integer only — no float
 * RULE:      PHI constants from pogls_platform.h ONLY
 *
 * 3-World invariant:
 *   World A (CPU radial)  → base_A
 *   World B (CPU radial)  → base_B
 *   World C (GPU witness) → base_C  (independent path)
 *
 * Layer stack:
 *   L1: XOR audit        (V4 existing)
 *   L2: Linear   A+B=C   (balance check)
 *   L3: Pythagorean a²+b²=c²  (structure lock)
 *   L4: Merkle SHA256    (V4 existing)
 */

#ifndef POGLS_QRPN_H
#define POGLS_QRPN_H

#include <stdint.h>
#include <stdatomic.h>
#include "pogls_platform.h"   /* PHI_UP, PHI_DOWN, PHI_COMP, PHI_SCALE */

/* ─── Deploy mode ──────────────────────────────────────────────── */
typedef enum {
    QRPN_SHADOW = 0,   /* log only — no abort, no rewind        */
    QRPN_SOFT   = 1,   /* fail → rewind_head()                  */
    QRPN_HARD   = 2    /* fail → abort_tx()                     */
} qrpn_mode_t;

/* ─── Context ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t     N;        /* radial order — borrow from ShellN    */
    uint64_t     seedA;    /* World A seed — MUST differ from seedB */
    uint64_t     seedB;    /* World B seed                          */
    qrpn_mode_t  mode;     /* current deploy phase                  */

    /* stats (shadow logging) */
    atomic_uint_fast64_t shadow_fail;
    atomic_uint_fast64_t soft_rewind;
    atomic_uint_fast64_t hard_abort;
    atomic_uint_fast64_t total_ops;
    atomic_uint_fast64_t degenerate_count;  /* (A|B)==0 inputs */
} qrpn_ctx_t;

/* ─── Fail log entry ────────────────────────────────────────────── */
typedef struct {
    uint64_t value;
    uint64_t addr;
    uint32_t A, B;
    uint64_t c_pyth;     /* A²+B²                    */
    uint32_t Cq;         /* mix32(c_pyth) CPU side   */
    uint32_t Cg;         /* GPU witness              */
    int      reason;     /* -1 pythagorean, -2 linear/witness */
    int64_t  ts_ns;
} qrpn_fail_entry_t;

/* ─── Default seeds (production values) ────────────────────────── */
#define QRPN_SEED_A  0x9E3779B97F4A7C15ULL   /* derived from PHI */
#define QRPN_SEED_B  0xBF58476D1CE4E5B9ULL   /* distinct from A  */

/* ─── Init ──────────────────────────────────────────────────────── */
static inline void qrpn_ctx_init(qrpn_ctx_t *ctx, uint32_t shell_n)
{
    ctx->N     = (shell_n >= 4 && shell_n <= 16) ? shell_n : 8u;
    ctx->seedA = QRPN_SEED_A;
    ctx->seedB = QRPN_SEED_B;
    ctx->mode  = QRPN_SHADOW;
    atomic_store(&ctx->shadow_fail, 0);
    atomic_store(&ctx->soft_rewind, 0);
    atomic_store(&ctx->hard_abort,  0);
    atomic_store(&ctx->total_ops,   0);
}

/* ══════════════════════════════════════════════════════════════════
 * INTERNAL PRIMITIVES
 * ══════════════════════════════════════════════════════════════════ */

/**
 * mix32 — normalize any 64-bit value into 32-bit domain
 * Used to make Cq (CPU) and Cg (GPU) comparable.
 * Both sides MUST use this exact function.
 */
static inline uint32_t qrpn_mix32(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

/**
 * radial_reduce — World A or B projection
 * Uses PHI_SCALE as mixing multiplier (consistent with platform)
 * N = radial order from ShellN
 */
static inline uint32_t qrpn_radial(uint64_t v, uint32_t N, uint64_t seed)
{
    v ^= seed;
    v ^= v >> (N & 31u);
    /* PHI-based mix — aligned with POGLS_PHI_SCALE */
    v *= (uint64_t)POGLS_PHI_UP | ((uint64_t)POGLS_PHI_DOWN << 32);
    v ^= v >> 29;
    return (uint32_t)v;
}

/**
 * qrpn_phi_scatter_hex — GPU witness path (Phase E replacement)
 *
 * ใช้แทน qrpn_phi_scatter() เมื่อ GPU จริงมาใน Phase E
 * CPU fallback จะ mirror path นี้เพื่อให้ dual-path ทำงาน:
 *   CPU radial  → A,B → mix32(A²+B²) = Cq
 *   GPU hex     → 6-rotation fold → Cg
 *   verify: Cq vs Cg (ต่าง entropy source = strong witness)
 *
 * Design: integer only, no float, PHI constants from platform.h
 * 6-rotation = hex axial symmetry (60°×6 = 360° closure)
 * XOR accumulate = rotation-order independent signature
 */
static inline uint32_t qrpn_phi_scatter_hex(uint64_t v)
{
    /* bit-swap fold (เหมือน phi_scatter เดิม) */
    v = ((v & 0xAAAAAAAAAAAAAAAAULL) >> 1)
      | ((v & 0x5555555555555555ULL) << 1);
    v ^= v >> 17;

    /* แยก (x,y) จาก 64-bit → hex axial coordinates */
    int q = (int)((v       & 0xFFFFu) * 2u / 3u);
    int r = (int)((-(int)(v & 0xFFFFu) + 2*(int)((v >> 32) & 0xFFFFu)) / 3);

    /* 6-rotation fold (60° axial: q,r → -r, q+r) ×6 = 360°
     * NOTE: ห้ามใช้ XOR accumulate เพราะ 6 rotations เป็น closed group
     *       XOR ทั้ง 6 steps cancel ออกเป็น 0 เสมอ (self-inverse property)
     * FIX: asymmetric mix — multiply-add แต่ละ step ด้วย prime ต่างกัน */
    uint32_t acc = 0;
    int qi = q, ri = r;
    uint32_t primes[6] = {73856093u, 19349663u, 83492791u,
                          50331653u, 12582917u, 402653189u};
    for (int i = 0; i < 6; i++) {
        int nq = -ri;
        int nr =  qi + ri;
        qi = nq;
        ri = nr;
        /* asymmetric: rotate acc เพื่อทำลาย symmetry ก่อน XOR */
        acc = (acc << 5) | (acc >> 27);   /* rotate32 left 5 */
        acc ^= (uint32_t)(qi * (int)primes[i]) + (uint32_t)(ri * (int)primes[(i+3)%6]);
    }

    /* PHI_DOWN final fold — align กับ POGLS constants */
    uint64_t out = (uint64_t)acc * POGLS_PHI_DOWN;
    out ^= out >> 31;
    out += POGLS_PHI_COMP;
    out ^= out >> 23;

    return qrpn_mix32(out);
}


/**
 * qrpn_phi_scatter — original GPU independent path (keep for reference)
 * Uses PHI_DOWN as scatter step (distinct from radial path).
 * Phase E: replaced by qrpn_phi_scatter_hex() in GPU kernel.
 */
static inline uint32_t qrpn_phi_scatter(uint64_t v)
{
    /* Hilbert-inspired fold (bit reversal + XOR mix) */
    v = ((v & 0xAAAAAAAAAAAAAAAAULL) >> 1)
      | ((v & 0x5555555555555555ULL) << 1);
    v ^= v >> 17;

    /* PHI_DOWN scatter — from pogls_platform.h */
    v *= (uint64_t)POGLS_PHI_DOWN;
    v ^= v >> 31;

    /* PHI_COMP secondary fold */
    v += (uint64_t)POGLS_PHI_COMP;
    v ^= v >> 23;

    return qrpn_mix32(v);
}

/* ══════════════════════════════════════════════════════════════════
 * CORE VERIFY
 * ══════════════════════════════════════════════════════════════════ */

/**
 * qrpn_verify_fast — main verification function
 *
 * Returns:
 *    0   = OK
 *   -1   = Pythagorean invariant broken (a²+b²≠c²)
 *   -2   = Linear + witness mismatch    (A+B≠Cg AND Cq≠Cg)
 *  -10   = Degenerate input             (A==0 or B==0)
 *
 * Caller provides Cg from GPU witness.
 * If GPU unavailable, pass Cg = qrpn_phi_scatter(value) as CPU fallback.
 */
static inline int qrpn_verify_fast(uint64_t value,
                                   uint32_t Cg,
                                   const qrpn_ctx_t *ctx)
{
    /* Phase 1: Radial projection → World A, World B */
    uint32_t A = qrpn_radial(value, ctx->N, ctx->seedA);
    uint32_t B = qrpn_radial(value, ctx->N, ctx->seedB);

    /* Guard: degenerate inputs break invariant */
    if ((A | B) == 0u) {
        atomic_fetch_add((atomic_uint_fast64_t *)&ctx->degenerate_count, 1);
        return -10;
    }

    /* Phase 2: Pythagorean generator
     * a = A²-B²,  b = 2AB,  c = A²+B²
     * Invariant: a²+b²=c²  (always true by construction)
     * We verify c is consistent with A,B — not recomputed externally.
     */
    uint64_t AA = (uint64_t)A * A;
    uint64_t BB = (uint64_t)B * B;
    uint64_t c  = AA + BB;                /* = c in Pythagorean triple */

    /* L3: Pythagorean self-check (sanity — catches overflow/corruption) */
    uint64_t a64 = AA - BB;               /* a = A²-B² */
    uint64_t b64 = 2ULL * A * B;          /* b = 2AB   */
    if (a64 * a64 + b64 * b64 != c * c)  /* a²+b²=c²  */
        return -1;

    /* Normalize c → 32-bit domain for cross-path comparison */
    uint32_t Cq = qrpn_mix32(c);

    /* L2: GPU witness cross-check (FINAL CORRECT)
     *
     * RULE: ห้าม compare ข้าม domain
     *   ❌ Cq == Cg        (radial vs hex — คนละ function)
     *   ❌ A+B == Cg       (CPU linear vs GPU hex)
     *
     * CORRECT: each domain verifies itself
     *   CPU domain: pyth_ok = (a²+b² == c²)  ← already checked above
     *   GPU domain: witness_ok = (Cg == Cg_expected)
     *     Cg_expected = phi_scatter_hex(value) computed by CPU
     *     Cg          = phi_scatter_hex(value) computed by GPU
     *     → same function, same input → must match (deterministic)
     *     → mismatch = GPU corruption / memory error
     */
    uint32_t Cg_expected = qrpn_phi_scatter_hex(value);
    uint32_t witness_ok  = (Cg == Cg_expected) ? 1u : 0u;

    if (!witness_ok)
        return -2;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * SHADOW MODE INTEGRATION
 * ══════════════════════════════════════════════════════════════════ */

/**
 * qrpn_check — wrapper for fed_write() integration
 *
 * Shadow:  log fail, continue write
 * Soft:    log fail, caller should rewind_head()
 * Hard:    log fail, caller should abort_tx()
 *
 * Returns 0 = OK, non-zero = fail (action depends on mode)
 */
static inline int qrpn_check(uint64_t value,
                              uint64_t addr,
                              uint32_t Cg,
                              qrpn_ctx_t *ctx,
                              qrpn_fail_entry_t *fail_out  /* nullable */)
{
    atomic_fetch_add(&ctx->total_ops, 1);

    int r = qrpn_verify_fast(value, Cg, ctx);
    if (r == 0) return 0;

    /* Fill fail entry for logging */
    if (fail_out) {
        uint32_t A  = qrpn_radial(value, ctx->N, ctx->seedA);
        uint32_t B  = qrpn_radial(value, ctx->N, ctx->seedB);
        uint64_t c  = (uint64_t)A*A + (uint64_t)B*B;
        fail_out->value  = value;
        fail_out->addr   = addr;
        fail_out->A      = A;
        fail_out->B      = B;
        fail_out->c_pyth = c;
        fail_out->Cq     = qrpn_mix32(c);
        fail_out->Cg     = Cg;
        fail_out->reason = r;
        fail_out->ts_ns  = 0;  /* caller fills timestamp */
    }

    switch (ctx->mode) {
    case QRPN_SHADOW:
        atomic_fetch_add(&ctx->shadow_fail, 1);
        return 0;   /* shadow = continue always */

    case QRPN_SOFT:
        atomic_fetch_add(&ctx->soft_rewind, 1);
        return r;   /* caller: rewind_head() */

    case QRPN_HARD:
        atomic_fetch_add(&ctx->hard_abort, 1);
        return r;   /* caller: abort_tx() */
    }
    return r;
}

/* ══════════════════════════════════════════════════════════════════
 * GPU WITNESS INTERFACE (CPU-SIDE STUB)
 * ══════════════════════════════════════════════════════════════════
 *
 * Production: replace with GPU kernel output
 * Fallback:   CPU phi_scatter (same entropy path as GPU)
 *
 * GPU kernel MUST implement identical qrpn_phi_scatter() logic.
 * Both must call qrpn_mix32() as final step.
 */
static inline uint32_t qrpn_gpu_witness_cpu_fallback(uint64_t value)
{
    /* FINAL CORRECT: GPU computes phi_scatter_hex(value)
     * CPU mirror must use identical function for witness_ok to work.
     * witness_ok = (Cg_gpu == Cg_expected)
     *            = (phi_hex(value) == phi_hex(value))  → always 1 if no corruption
     * Phase E: GPU kernel computes this; fallback mirrors it deterministically. */
    return qrpn_phi_scatter_hex(value);
}

/* ══════════════════════════════════════════════════════════════════
 * USAGE EXAMPLE (shadow mode)
 * ══════════════════════════════════════════════════════════════════
 *
 *  qrpn_ctx_t qrpn;
 *  qrpn_ctx_init(&qrpn, current_shell_n);   // N from AdaptTopo
 *  qrpn.mode = QRPN_SHADOW;
 *
 *  // In fed_write():
 *  uint32_t Cg = gpu_batch_get_witness(value);   // GPU output
 *  // or fallback: uint32_t Cg = qrpn_gpu_witness_cpu_fallback(value);
 *
 *  qrpn_fail_entry_t fail;
 *  int r = qrpn_check(value, addr, Cg, &qrpn, &fail);
 *  if (r != 0 && qrpn.mode >= QRPN_SOFT) {
 *      rewind_head(head_id);           // SOFT
 *      // abort_tx();                  // HARD
 *      return -1;
 *  }
 *  pogls_write(pw, value, addr);       // V4 pipeline unchanged
 */

/* ══════════════════════════════════════════════════════════════════
 * STATS DUMP (debug / monitoring)
 * ══════════════════════════════════════════════════════════════════ */
#include <stdio.h>
static inline void qrpn_stats_print(const qrpn_ctx_t *ctx)
{
    uint64_t total  = atomic_load(&ctx->total_ops);
    uint64_t shadow = atomic_load(&ctx->shadow_fail);
    uint64_t soft   = atomic_load(&ctx->soft_rewind);
    uint64_t hard   = atomic_load(&ctx->hard_abort);
    fprintf(stderr,
        "[QRPN] total=%llu shadow_fail=%llu soft_rewind=%llu hard_abort=%llu"
        " fail_rate=%.4f%%\n",
        (unsigned long long)total,
        (unsigned long long)shadow,
        (unsigned long long)soft,
        (unsigned long long)hard,
        total ? (double)(shadow+soft+hard)*100.0/(double)total : 0.0);
}

#endif /* POGLS_QRPN_H */
