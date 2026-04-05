/*
 * geo_read.h — Read Path (Session 21)
 * =====================================
 * Symmetric counterpart to the write path.
 *
 * Write:  raw → theta_map → geo_route → diamond_flow → DNA → DodecaTable
 * Read:   query → route_addr → dodeca_lookup → ReadResult
 *
 * Three query modes (same result type):
 *   geo_read_by_raw()    — raw uint64 → reconstruct route_addr → lookup
 *   geo_read_by_addr()   — route_addr direct lookup (fast path)
 *   geo_read_by_cell()   — read HoneycombSlot from DiamondBlock* (cold path)
 *
 * ReadResult carries everything the caller needs:
 *   found, merkle_root, hop_count, segment, sha_ok, ref_count
 */

#ifndef GEO_READ_H
#define GEO_READ_H

#include <stdint.h>
#include <string.h>

#include "theta_map.h"
#include "geo_route.h"
#include "geo_dodeca.h"
#include "geo_diamond_field.h"
#include "pogls_fold.h"

/* ── result ──────────────────────────────────────────────────────── */
typedef enum {
    READ_HIT_EXACT  = 0,  /* merkle + offset match                  */
    READ_HIT_MERKLE = 1,  /* merkle match, offset differs (variant) */
    READ_HIT_CELL   = 2,  /* found via honeycomb scan, not dodeca   */
    READ_MISS       = 3,  /* not found anywhere                     */
} ReadStatus;

typedef struct {
    ReadStatus status;
    uint64_t   merkle_root;  /* route_addr of the matched flow       */
    uint16_t   hop_count;    /* hops in the matched flow             */
    uint8_t    segment;      /* scroll segment (0-255)               */
    uint8_t    offset;       /* semantic offset from baseline        */
    uint32_t   ref_count;    /* dedup hit count in dodeca            */
    int        sha_ok;       /* 1=verified, 0=no SHA stored, -1=fail */
} ReadResult;

static inline ReadResult read_result_miss(void) {
    ReadResult r;
    memset(&r, 0, sizeof(r));
    r.status = READ_MISS;
    r.sha_ok = 0;
    return r;
}

/* ════════════════════════════════════════════════════════════════════
 * geo_read_by_addr — direct dodeca lookup by route_addr
 * Fast path: O(8) probe, no recompute
 * ════════════════════════════════════════════════════════════════════ */
static inline ReadResult geo_read_by_addr(DodecaTable       *dodeca,
                                           uint64_t           route_addr,
                                           uint8_t            offset,
                                           const DiamondSHA  *sha)
{
    DodecaEntry *entry = NULL;
    DodecaResult dr = dodeca_lookup(dodeca, route_addr, offset, &entry);

    if (dr == DODECA_MISS || entry == NULL)
        return read_result_miss();

    ReadResult r;
    r.merkle_root = entry->merkle_root;
    r.hop_count   = entry->hop_count;
    r.segment     = entry->segment;
    r.offset      = entry->offset;
    r.ref_count   = entry->ref_count;
    r.sha_ok      = dodeca_verify(entry, sha);
    r.status      = (dr == DODECA_HIT_EXACT) ? READ_HIT_EXACT : READ_HIT_MERKLE;
    return r;
}

/* ════════════════════════════════════════════════════════════════════
 * geo_read_by_raw — raw uint64 → reconstruct route_addr → lookup
 *
 * Reconstructs the same initial geometry the writer used,
 * runs one diamond_flow pass to get route_addr, then looks up.
 *
 * cells[]   : the same cell array used during write (caller provides)
 * n         : cell count
 * baseline  : same baseline used during write
 * ════════════════════════════════════════════════════════════════════ */
static inline ReadResult geo_read_by_raw(DodecaTable       *dodeca,
                                          const uint64_t    *cells_raw,
                                          uint32_t           n,
                                          uint64_t           baseline,
                                          const DiamondSHA  *sha)
{
    if (n == 0) return read_result_miss();

    /*
     * Reconstruct route_addr using the SAME path as geo_fused_write_batch:
     *   theta_mix64 → core_raw → geo_fast_intersect → diamond_route_update
     *   + geo_route_addr_guard + drift sampling + boundary detection
     *
     * IMPORTANT: the old implementation used fold_build_quad_mirror +
     * diamond_batch_run_v5 which produces a DIFFERENT route_addr than the
     * fused path (10.9× slower and different intersect calculation).
     * This version mirrors fused_write exactly so lookups always match.
     */
    DiamondFlowCtx ctx; diamond_flow_init(&ctx);

    /* PHI prime for geo_fast_intersect (same constant as hardening_whe.h) */
#   define _GEO_READ_TOPO_MASK UINT64_C(0x0000FFFFFFFFFFFF)
#   define _GEO_READ_PHI       UINT64_C(0x9E3779B97F4A7C15)

    for (uint32_t i = 0; i < n; i++) {
        /* inline theta_map — identical to fused_write */
        uint64_t h  = theta_mix64(cells_raw[i]);
        uint32_t hi = (uint32_t)(h >> 32);
        uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
        uint8_t  face = (uint8_t)(((uint64_t)hi * 12u) >> 32);
        uint8_t  edge = (uint8_t)(((uint64_t)lo *  5u) >> 32);

        /* pack core_raw */
        uint64_t core_raw = ((uint64_t)face << 59)
                          | ((uint64_t)edge << 52)
                          | (cells_raw[i] & UINT64_C(0x000FFFFFFFFFFFFF));

        /* geo_fast_intersect: 4-rotation AND */
        uint64_t r8  = (core_raw >> 8)  | (core_raw << 56);
        uint64_t r16 = (core_raw >> 16) | (core_raw << 48);
        uint64_t r24 = (core_raw >> 24) | (core_raw << 40);
        uint64_t isect = core_raw & r8 & r16 & r24;

        /* drift sample (1/8) */
        if ((core_raw & 7u) == 0u)
            ctx.drift_acc += (uint32_t)__builtin_popcountll(baseline & ~isect);

        /* route accumulate + seed guard */
        uint64_t r_next = diamond_route_update(ctx.route_addr, isect);
        if ((r_next & _GEO_READ_TOPO_MASK) == 0)
            r_next ^= _GEO_READ_PHI ^ isect;

        /* boundary: same conditions as fused_write */
        int at_end = (isect == 0) || (ctx.drift_acc > 72u)
                  || (ctx.hop_count >= DIAMOND_HOP_MAX);

        if (at_end) {
            /* attempt lookup at this boundary */
            uint8_t offset = (uint8_t)(__builtin_popcountll(baseline & ~isect) & 0xFF);
            ReadResult r = geo_read_by_addr(dodeca, r_next, offset, sha);
            if (r.status != READ_MISS) return r;
            /* boundary but not found — reset and keep scanning */
            ctx.route_addr = 0;
            ctx.hop_count  = 0;
            ctx.drift_acc  = 0;
            continue;
        }

        ctx.route_addr = r_next;
        ctx.hop_count++;
    }

    /* check any partial (non-boundary) flow at end of stream */
    if (ctx.route_addr != 0) {
        uint8_t offset = (uint8_t)(ctx.drift_acc & 0xFF);
        ReadResult r = geo_read_by_addr(dodeca, ctx.route_addr, offset, sha);
        if (r.status != READ_MISS) return r;
    }

#   undef _GEO_READ_TOPO_MASK
#   undef _GEO_READ_PHI

    return read_result_miss();
}

/* ════════════════════════════════════════════════════════════════════
 * geo_read_by_cell — cold path: read HoneycombSlot directly
 *
 * Does NOT require DodecaTable — reads from cell's honeycomb field.
 * Use when dodeca is cold/evicted or for single-cell verification.
 * ════════════════════════════════════════════════════════════════════ */
static inline ReadResult geo_read_by_cell(const DiamondBlock *b,
                                           DodecaTable        *dodeca,
                                           const DiamondSHA   *sha)
{
    HoneycombSlot s = honeycomb_read(b);

    /* empty cell: dna_count=0 and merkle_root=0 */
    if (s.merkle_root == 0 && s.dna_count == 0)
        return read_result_miss();

    ReadResult r;
    r.merkle_root = s.merkle_root;
    r.hop_count   = s.dna_count;
    r.offset      = s.reserved[0];
    r.segment     = 0;
    r.ref_count   = 0;
    r.sha_ok      = 0;
    r.status      = READ_HIT_CELL;

    /* optionally cross-check with dodeca if available */
    if (dodeca) {
        DodecaEntry *entry = NULL;
        DodecaResult dr = dodeca_lookup(dodeca, s.merkle_root,
                                         s.reserved[0], &entry);
        if (dr != DODECA_MISS && entry) {
            r.segment   = entry->segment;
            r.ref_count = entry->ref_count;
            r.sha_ok    = dodeca_verify(entry, sha);
            r.status    = (dr == DODECA_HIT_EXACT)
                        ? READ_HIT_EXACT : READ_HIT_MERKLE;
        }
    }

    return r;
}

/* ════════════════════════════════════════════════════════════════════
 * geo_read_scan — batch scan: find all cells with DNA in array
 *   results[] must be >= n
 *   returns count of cells with DNA (status != READ_MISS)
 * ════════════════════════════════════════════════════════════════════ */
static inline uint32_t geo_read_scan(const DiamondBlock *cells,
                                      uint32_t            n,
                                      DodecaTable        *dodeca,
                                      const DiamondSHA   *sha,
                                      ReadResult         *results)
{
    uint32_t found = 0;
    for (uint32_t i = 0; i < n; i++) {
        results[i] = geo_read_by_cell(&cells[i], dodeca, sha);
        if (results[i].status != READ_MISS) found++;
    }
    return found;
}

#endif /* GEO_READ_H */
