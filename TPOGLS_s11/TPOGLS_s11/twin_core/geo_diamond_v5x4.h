/* ============================================================
 * LEGACY FILE — geo_diamond_v5x4.h (AVX2 path, pre-S21)
 * DO NOT include alongside geo_diamond_field.h
 * Use geo_fast_intersect_x4() via geo_diamond_field.h instead
 * This file is kept for GPU/bench reference only.
 * Define GEO_DIAMOND_V5X4_LEGACY to suppress this warning.
 * ============================================================ */
#ifndef GEO_DIAMOND_V5X4_LEGACY
#pragma message("WARNING: geo_diamond_v5x4.h is a legacy intersect path. Use geo_diamond_field.h")
#endif
/*
 * geo_diamond_v5x4.h — SIMD x4 + SHA256 Integrity  [DEPRECATED — Session 20]
 * ============================================================================
 *
 * ⚠️  DEPRECATED — scalar fallback calls diamond_batch_run_v5 (deprecated path).
 *     AVX2 x4 path uses fold_fibo_intersect, NOT geo_fast_intersect.
 *     route_addr produced here DIFFERS from geo_fused_write_batch.
 *
 * For AVX2/SIMD production use: extend geo_fused_write_batch with x4/x8 lanes
 * using geo_fast_intersect as the intersect kernel (see geo_hardening_whe.h).
 *
 * Retained for reference. Define GEO_ALLOW_V5_DEPRECATED to suppress warnings.
 *
 * Original intent: diamond_batch_temporal_x4 (AVX2 x4 flows) + SHA integrity

#ifndef GEO_DIAMOND_V5X4_H
#define GEO_DIAMOND_V5X4_H

#include "geo_config.h"
#include "pogls_fold.h"
#include "geo_dodeca.h"
#include "geo_diamond_field.h"      /* DiamondFlowCtx4, temporal_x4, invertible route */
#include "geo_diamond_field5.h"     /* diamond_batch_run_v5, flush_v5 */

/* ════════════════════════════════════════════════════════════════════
 * SCALAR FALLBACK CONTEXT (ใช้เมื่อไม่มี AVX2)
 * ════════════════════════════════════════════════════════════════════ */

#ifndef __AVX2__
/* เมื่อไม่มี AVX2 ให้ fallback ไป v5 scalar ทีละ flow */
static inline uint8_t diamond_batch_run_v5x4_scalar(
    DiamondFlowCtx  ctx[4],
    DiamondBlock       cells[4],
    uint32_t           n,
    uint64_t           baseline,
    DodecaTable       *dodeca,
    uint8_t            segment,
    const DiamondSHA  *sha,
    uint32_t          *verify_fail_out)
{
    uint8_t mask = 0;
    for (int f = 0; f < 4; f++) {
        uint32_t before = dodeca->hit_count + dodeca->miss_count;
        uint32_t w = diamond_batch_run_v5(
            &cells[f], n, baseline, &ctx[f], dodeca, segment, sha, verify_fail_out);
        if (w > 0) mask |= (1u << f);
        (void)before;
    }
    return mask;
}
#endif /* !__AVX2__ */

/* ════════════════════════════════════════════════════════════════════
 * AVX2 PATH
 * ════════════════════════════════════════════════════════════════════ */

#ifdef __AVX2__

/* ── per-lane DNA + dodeca + SHA ─────────────────────────────────────
 * เรียกหลัง temporal_x4 คืน mask
 * snap_route/hop/drift = pre-reset state ของ lane ที่ fire
 */
static inline void _v5x4_dna_lane(
    DiamondFlowCtx4   *ctx4,
    DiamondBlock       cells[4],
    uint8_t            lane_mask,
    uint64_t           baseline __attribute__((unused)),
    DodecaTable       *dodeca,
    uint8_t            segment,
    const DiamondSHA  *sha,
    uint32_t          *verify_fail_out)
{
    for (int f = 0; f < 4; f++) {
        if (!(lane_mask & (1u << f))) continue;

        uint64_t route  = ctx4->snap_route[f];
        uint16_t hops   = (uint16_t)(ctx4->snap_hop[f]   & 0xFFFFu);
        uint8_t  offset = (uint8_t) (ctx4->snap_drift[f]  & 0xFFu);

        /* 1. honeycomb DNA write */
        diamond_dna_write(&cells[f], route, hops, offset);

        /* 2. dodeca lookup + SHA */
        DodecaEntry *e  = NULL;
        DodecaResult dr = dodeca_lookup(dodeca, route, offset, &e);

        if (dr == DODECA_MISS) {
            dodeca_insert_sha(dodeca, route, sha, offset, hops, segment);
        } else {
            /* HIT: verify SHA */
            int vr = dodeca_verify(e, sha);
            if (vr == -1 && verify_fail_out)
                (*verify_fail_out)++;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * diamond_batch_run_v5x4
 * = temporal_x4 (SIMD) + SHA per-lane (scalar on fire)
 *
 * isect4  : [steps × 4] interleaved — caller fills from fold_fibo_intersect
 * cells[4]: 4 DiamondBlock — ใช้ honeycomb write เท่านั้น
 * sha     : NULL = geometry-only, &sha = integrity
 *
 * return: bitmask lanes ที่ fire DNA ใน batch นี้ (cumulative OR)
 * ════════════════════════════════════════════════════════════════════ */
static inline uint8_t diamond_batch_run_v5x4(
    DiamondFlowCtx4   *ctx4,
    const uint64_t    *isect4,    /* [steps * 4] */
    uint32_t           steps,
    uint64_t           baseline,
    DiamondBlock       cells[4],
    DodecaTable       *dodeca,
    uint8_t            segment,
    const DiamondSHA  *sha,
    uint32_t          *verify_fail_out)
{
    uint8_t total_mask = 0;
    uint32_t i = 0;

    /* process SIMD_CHUNK steps at a time, handle DNA fires between chunks */
#define V5X4_CHUNK 16u

    while (i < steps) {
        uint32_t chunk = (steps - i) < V5X4_CHUNK ? (steps - i) : V5X4_CHUNK;

        uint8_t mask = (uint8_t)diamond_batch_temporal_x4(
            ctx4, isect4 + i * 4, chunk, baseline);

        if (mask) {
            _v5x4_dna_lane(ctx4, cells, mask, baseline,
                            dodeca, segment, sha, verify_fail_out);
            total_mask |= mask;
        }

        i += chunk;
    }

#undef V5X4_CHUNK
    return total_mask;
}

/* ════════════════════════════════════════════════════════════════════
 * diamond_batch_run_v5x4_per_lane
 * = เหมือน v5x4 แต่รับ SHA แยกต่อ lane (sha4[4])
 * sha4[f] = NULL → geometry-only สำหรับ lane f
 * ════════════════════════════════════════════════════════════════════ */
static inline uint8_t diamond_batch_run_v5x4_per_lane(
    DiamondFlowCtx4      *ctx4,
    const uint64_t       *isect4,
    uint32_t              steps,
    uint64_t              baseline,
    DiamondBlock          cells[4],
    DodecaTable          *dodeca,
    uint8_t               segment,
    const DiamondSHA     *sha4[4],   /* sha per lane, NULL = skip */
    uint32_t             *verify_fail_out)
{
    uint8_t total_mask = 0;
    uint32_t i = 0;

#define V5X4_CHUNK 16u
    while (i < steps) {
        uint32_t chunk = (steps - i) < V5X4_CHUNK ? (steps - i) : V5X4_CHUNK;

        uint8_t mask = (uint8_t)diamond_batch_temporal_x4(
            ctx4, isect4 + i * 4, chunk, baseline);

        if (mask) {
            for (int f = 0; f < 4; f++) {
                if (!(mask & (1u << f))) continue;

                uint64_t route  = ctx4->snap_route[f];
                uint16_t hops   = (uint16_t)(ctx4->snap_hop[f]  & 0xFFFFu);
                uint8_t  offset = (uint8_t) (ctx4->snap_drift[f] & 0xFFu);

                diamond_dna_write(&cells[f], route, hops, offset);

                DodecaEntry *e  = NULL;
                DodecaResult dr = dodeca_lookup(dodeca, route, offset, &e);

                if (dr == DODECA_MISS) {
                    dodeca_insert_sha(dodeca, route, sha4[f],
                                       offset, hops, segment);
                } else {
                    int vr = dodeca_verify(e, sha4[f]);
                    if (vr == -1 && verify_fail_out)
                        (*verify_fail_out)++;
                }
            }
            total_mask |= mask;
        }

        i += chunk;
    }
#undef V5X4_CHUNK
    return total_mask;
}

/* ════════════════════════════════════════════════════════════════════
 * diamond_flush_v5x4
 * force DNA write ทุก lane ที่มี hop_count > 0
 * เรียกตอน EOF / last chunk
 *
 * return: bitmask lanes ที่ flush เกิดขึ้น
 * ════════════════════════════════════════════════════════════════════ */
static inline uint8_t diamond_flush_v5x4(
    DiamondFlowCtx4   *ctx4,
    uint64_t           baseline __attribute__((unused)),
    DiamondBlock       cells[4],
    DodecaTable       *dodeca,
    uint8_t            segment,
    const DiamondSHA  *sha,
    uint32_t          *verify_fail_out)
{
    uint8_t mask = 0;

    for (int f = 0; f < 4; f++) {
        if (ctx4->hop_count[f] == 0) continue;

        /* ใช้ route_addr ปัจจุบัน (ไม่มี snap เพราะ force flush) */
        uint64_t route  = ctx4->route_addr[f];
        uint16_t hops   = (uint16_t)(ctx4->hop_count[f] & 0xFFFFu);
        uint8_t  offset = (uint8_t) (ctx4->drift_acc[f]  & 0xFFu);

        diamond_dna_write(&cells[f], route, hops, offset);

        DodecaEntry *e  = NULL;
        DodecaResult dr = dodeca_lookup(dodeca, route, offset, &e);

        if (dr == DODECA_MISS) {
            dodeca_insert_sha(dodeca, route, sha, offset, hops, segment);
        } else {
            int vr = dodeca_verify(e, sha);
            if (vr == -1 && verify_fail_out)
                (*verify_fail_out)++;
        }

        /* reset lane */
        ctx4->route_addr[f] = 0;
        ctx4->hop_count[f]  = 0;
        ctx4->drift_acc[f]  = 0;

        mask |= (1u << f);
    }

    return mask;
}

#endif /* __AVX2__ */

/* ════════════════════════════════════════════════════════════════════
 * USAGE SUMMARY
 *
 * --- shared SHA (all 4 lanes same data) ---
 *   DiamondFlowCtx4 ctx4; diamond_flow4_init(&ctx4);
 *   DodecaTable dodeca;   dodeca_init(&dodeca);
 *   DiamondSHA sha = diamond_sha_from_raw(raw);
 *   uint32_t fails = 0;
 *
 *   // fill isect4[steps*4] จาก fold_fibo_intersect ต่อ lane
 *   uint8_t mask = diamond_batch_run_v5x4(
 *       &ctx4, isect4, steps, baseline,
 *       cells, &dodeca, seg, &sha, &fails);
 *
 *   // flush at EOF
 *   diamond_flush_v5x4(&ctx4, baseline, cells, &dodeca, seg, &sha, &fails);
 *
 * --- per-lane SHA ---
 *   const DiamondSHA *sha4[4] = { &sha0, &sha1, NULL, &sha3 };
 *   diamond_batch_run_v5x4_per_lane(..., sha4, &fails);
 *
 * --- fallback (no AVX2) ---
 *   DiamondFlowCtxV2 ctx[4]; // 4 scalar ctxs
 *   diamond_batch_run_v5x4_scalar(ctx, cells, n, baseline,
 *                                  &dodeca, seg, &sha, &fails);
 * ════════════════════════════════════════════════════════════════════ */

#endif /* GEO_DIAMOND_V5X4_H */
