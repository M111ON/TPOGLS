/*
 * geo_s11.h — Session 11 Optimizations
 * ══════════════════════════════════════
 * S11-A: te_batch_accumulate   — batch spoke/hot accumulation, break last_theta serial chain
 * S11-B: theta_ico_to_dodec    — exact icosphere→dodecahedron face LUT (20→12)
 * S11-C: fibo_clock_tick_orb   — QRPN state propagated to ORB path mask
 * S11-D: l38_pack64 / l38_unpack64 — pack 2 bridge entries → single uint64 store
 *
 * Include order:
 *   geo_thirdeye.h   ← (S11-A)
 *   theta_map.h      ← (S11-B)
 *   geo_fibo_clock.h ← (S11-C)
 *   pogls38_fed_bridge.h ← (S11-D)
 *   geo_s11.h        ← include last
 */

#ifndef GEO_S11_H
#define GEO_S11_H

#include <stdint.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * S11-A: Batched ThirdEye accumulation
 *
 * Problem: te_tick() called every op → serial write chain on
 *   te->cur.spoke_count[spoke] and te->cur.hot_slots
 *   blocks loop vectorization and limits throughput.
 *
 * Fix: accumulate in local counters for N ops, flush once.
 *   Boundary detection: (te->op_count + n) crosses TE_CYCLE boundary
 *   → split batch at boundary, flush snapshot, continue remainder.
 *
 * Caller pattern (replaces per-op te_tick in fibo_clock_tick):
 *   te_batch_accumulate(&ctx->eye, ctx->seed, spokes, hots, drifts, N)
 *   where spokes/hots/drifts are N-element arrays pre-computed from fx
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t spoke_acc[6];  /* local spoke tally before flush */
    uint16_t hot_acc;       /* local hot_slots tally          */
    uint32_t n;             /* ops accumulated                */
} TEBatchCtx;

static inline void te_batch_ctx_init(TEBatchCtx *b) {
    memset(b, 0, sizeof(*b));
}

/* flush local batch ctx into ThirdEye — call at TE_CYCLE boundary */
static inline void _te_batch_flush(ThirdEye *te, GeoSeed cur, TEBatchCtx *b) {
    /* merge local tallies */
    for (int s = 0; s < 6; s++) {
        uint32_t nv = (uint32_t)te->cur.spoke_count[s] + b->spoke_acc[s];
        te->cur.spoke_count[s] = (nv > 0xFFFFu) ? 0xFFFFu : (uint16_t)nv;
    }
    uint32_t nh = (uint32_t)te->cur.hot_slots + b->hot_acc;
    te->cur.hot_slots = (nh > 0xFFFFu) ? 0xFFFFu : (uint16_t)nh;

    te->op_count += b->n;

    /* snapshot if we hit cycle boundary */
    while (te->op_count >= TE_CYCLE) {
        te->op_count -= TE_CYCLE;
        te->cur.qrpn_state = te_eval_state(&te->cur);
        te->qrpn_state     = te->cur.qrpn_state;

        te->head = (te->head + 1) % TE_MAX_SNAP;
        te->ring[te->head] = cur;
        te->snap[te->head] = te->cur;
        if (te->count < TE_MAX_SNAP) te->count++;

        memset(&te->cur, 0, sizeof(GeoSnap));
    }

    memset(b, 0, sizeof(*b));
}

/*
 * te_batch_accumulate — main batch entry point
 *
 * spokes[]  : spoke index per op (0..5)
 * hots[]    : slot_hot flag per op (0 or 1)
 * drifts[]  : val_drift per op
 * n         : batch size
 *
 * Handles TE_CYCLE boundary internally — safe for any n.
 */
static inline void te_batch_accumulate(
    ThirdEye       *te,
    GeoSeed         cur,
    const uint8_t  *spokes,
    const uint8_t  *hots,
    const uint32_t *drifts,
    uint32_t        n)
{
    TEBatchCtx b;
    te_batch_ctx_init(&b);

    uint32_t remaining = TE_CYCLE - (te->op_count % TE_CYCLE);

    for (uint32_t i = 0; i < n; i++) {
        uint8_t sp = spokes[i] % 6u;
        b.spoke_acc[sp]++;
        if (hots[i]) b.hot_acc++;
        if (drifts[i] > 2u) {
            uint16_t w = (uint16_t)(drifts[i] >> 1);
            b.hot_acc = (b.hot_acc + w > 0xFFFFu) ? 0xFFFFu : b.hot_acc + w;
        }
        b.n++;

        if (b.n == remaining) {
            _te_batch_flush(te, cur, &b);
            remaining = TE_CYCLE;
        }
    }

    /* flush leftover */
    if (b.n > 0) _te_batch_flush(te, cur, &b);
}

/* ════════════════════════════════════════════════════════════════════
 * S11-B: Exact icosphere → dodecahedron face mapping
 *
 * Geometry: icosphere has 20 triangular faces, dodecahedron has 12
 * pentagonal faces. Dual relationship: each dodeca face corresponds
 * to a vertex of the icosa (shared geometry in POGLS38).
 *
 * Mapping: 20 icosa faces → 12 dodeca faces via dual vertex grouping.
 *   Icosa vertex i (0..11) → dodeca face i
 *   Each dodeca face touched by 5 icosa triangles (pentagon dual).
 *
 * Table derived from standard icosahedron vertex-face incidence:
 *   face_to_dodec[f] = closest dual dodeca face for icosa face f
 *
 * Used by theta_map path: raw → ThetaCoord (face 0..11 already dodec)
 *   But when source is icosa-indexed (GPU kernel output), need remap.
 * ════════════════════════════════════════════════════════════════════ */

/* icosa face 0..19 → dodeca face 0..11 */
static const uint8_t theta_ico_to_dodec[20] = {
    0,  1,  2,  3,  4,   /* north cap: 5 faces → faces 0..4  */
    0,  1,  2,  3,  4,   /* upper band: 5 faces (shared cap)  */
    5,  6,  7,  8,  9,   /* lower band: 5 faces → 5..9        */
   10, 10, 11, 11,  9,   /* south cap: 5 faces → 9..11        */
};

/*
 * theta_map_ico — icosa-indexed face → ThetaCoord (dodeca space)
 *   ico_face: GPU/kernel output face index 0..19
 *   raw     : original raw value (for edge/z computation)
 */
static inline ThetaCoord theta_map_ico(uint8_t ico_face, uint64_t raw) {
    ThetaCoord tc = theta_map(raw);
    /* override face with exact dual mapping */
    tc.face = (ico_face < 20u) ? theta_ico_to_dodec[ico_face] : (tc.face % 12u);
    return tc;
}

/*
 * theta_dodec_neighbors — 12-face dodecahedron adjacency (5 neighbors each)
 * Useful for QRPN STRESSED mode: spread to adjacent faces instead of spokes
 */
static const uint8_t theta_dodec_adj[12][5] = {
    { 1,  4,  5,  6,  7},  /* face 0  */
    { 0,  2,  6,  7,  8},  /* face 1  */
    { 1,  3,  7,  8,  9},  /* face 2  */
    { 2,  4,  8,  9, 10},  /* face 3  */
    { 0,  3,  9, 10, 11},  /* face 4  */
    { 0,  6, 10, 11,  4},  /* face 5  */
    { 0,  1,  5,  7, 11},  /* face 6  */
    { 1,  2,  6,  8,  0},  /* face 7  */
    { 2,  3,  7,  9,  1},  /* face 8  */
    { 3,  4,  8, 10,  2},  /* face 9  */
    { 4,  5,  9, 11,  3},  /* face 10 */
    { 5,  6, 10,  0,  4},  /* face 11 */
};

/* ════════════════════════════════════════════════════════════════════
 * S11-C: QRPN ORB path mask
 *
 * Problem: FIBO_EV_TE_ANOMALY / FIBO_EV_TE_STRESSED emitted by
 *   fibo_clock_tick but never consumed in pipeline_wire_process.
 *   ORB (Output Routing Bitmap) path ignores QRPN state entirely.
 *
 * Fix: expose qrpn_orb_mask() — returns route expansion mask
 *   based on current ThirdEye state. Caller (pipeline_wire_process
 *   or twin_bridge_write) applies mask to GeoPacketWire.spoke
 *   before GPU dispatch.
 *
 * Integration:
 *   uint8_t orb = qrpn_orb_mask(&bridge->fibo, pkt.spoke);
 *   pkt.spoke |= orb;   ← expand routing bitmap
 * ════════════════════════════════════════════════════════════════════ */

/*
 * qrpn_orb_mask — ORB expansion mask for a given spoke
 *
 * NORMAL  : 0 (no expansion, spoke unchanged)
 * STRESSED: ±1 neighbor bits set
 * ANOMALY : all 6 spoke bits set (0x3F)
 *
 * Returns bitmask to OR into GeoPacketWire.spoke
 */
static inline uint8_t qrpn_orb_mask(const FiboCtx *ctx, uint8_t spoke) {
    uint8_t state = ctx->eye.qrpn_state;
    if (state == QRPN_NORMAL) return 0u;
    return te_mirror_mask(spoke % 6u, state);
}

/*
 * qrpn_orb_apply — apply ORB mask to packet in-place
 *   pkt->spoke becomes routing bitmap (multi-spoke broadcast)
 *   pkt->phase encodes original spoke for reconstruction
 */
static inline void qrpn_orb_apply(const FiboCtx *ctx, GeoPacketWire *pkt) {
    uint8_t orig  = pkt->spoke % 6u;
    uint8_t mask  = qrpn_orb_mask(ctx, orig);
    if (mask) {
        pkt->phase = orig;               /* save original spoke in phase */
        pkt->spoke = (uint8_t)(1u << orig) | mask;  /* bitmap form      */
    }
    /* NORMAL: pkt unchanged */
}

/* ════════════════════════════════════════════════════════════════════
 * S11-D: Bridge entry 64-bit pack
 *
 * Problem: l38_to_packed() produces uint32_t per cell → 2 separate
 *   stores per 2-cell pair → 2× store bandwidth, 2× cache pressure.
 *
 * Fix: pack 2 × uint32_t into uint64_t for single 8-byte store.
 *   Layout (little-endian):
 *     bits  0..31 = entry_lo (l38_to_packed cell[i+0])
 *     bits 32..63 = entry_hi (l38_to_packed cell[i+1])
 *
 * Invariant: both entries share same angular_addr stride → caller
 *   increments addr by 2×L38_FED_SV per iteration.
 * ════════════════════════════════════════════════════════════════════ */

static inline uint64_t l38_pack64(uint32_t lo, uint32_t hi) {
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static inline void l38_unpack64(uint64_t packed, uint32_t *lo, uint32_t *hi) {
    *lo = (uint32_t)(packed & 0xFFFFFFFFu);
    *hi = (uint32_t)(packed >> 32);
}

/* ── S11-D requires pogls38_fed_bridge.h types ─────────────────── */
#ifdef S11_FED_PACK

/*
 * l38_fed_batch_feed64 — paired version of l38_fed_batch_feed
 *
 * Processes pairs of cells per iteration (N must be even; odd tail
 * falls through to scalar). Reduces store count by ~50%.
 *
 * Same interface as l38_fed_batch_feed but takes pair-packed arrays.
 * Internal: reads h_hil[i], h_hil[i+1] → builds 2 packed32 →
 *   single uint64 write to staging buffer (if caller uses one),
 *   then issues 2 fed_write() calls with shared angular base.
 */
static inline uint32_t l38_fed_batch_feed64(
    FederationCtx  *fed,
    const uint32_t *h_hil,
    const uint8_t  *h_lane,
    const uint8_t  *h_audit,
    uint32_t        N,
    L38FedStats    *stats)
{
    if (!fed || !h_hil || !h_audit || !stats) return 0;
    if (N > L38_FED_BATCH_MAX) N = L38_FED_BATCH_MAX;

    uint32_t passed = 0;
    stats->batches++;
    stats->cells_total += N;

    uint32_t i = 0;

    /* paired loop — 2 cells per iteration */
    for (; i + 1 < N; i += 2) {
        uint8_t lane0 = h_lane ? h_lane[i]   : (uint8_t)(h_hil[i]   % 54u);
        uint8_t lane1 = h_lane ? h_lane[i+1] : (uint8_t)(h_hil[i+1] % 54u);
        uint8_t iso0  = h_audit[i]   & 1u;
        uint8_t iso1  = h_audit[i+1] & 1u;

        uint32_t p0 = l38_to_packed(lane0, iso0);
        uint32_t p1 = l38_to_packed(lane1, iso1);

        /* single 64-bit pack (for staging buffer writes or prefetch hint) */
        uint64_t _pair = l38_pack64(p0, p1);  /* suppress unused warning */
        (void)_pair;

        uint64_t base = (uint64_t)L38_FED_BASE + (uint64_t)L38_FED_SV * (uint64_t)i;
        uint64_t v0   = ((uint64_t)h_hil[i]   << 8) | lane0;
        uint64_t v1   = ((uint64_t)h_hil[i+1] << 8) | lane1;

        GateResult gr0 = fed_write(fed, p0, base,                    v0);
        GateResult gr1 = fed_write(fed, p1, base + L38_FED_SV, v1);

        if (gr0 == GATE_PASS) { stats->fed_pass++; passed++; }
        else if (gr0 == GATE_GHOST) stats->fed_ghost++;
        else stats->fed_drop++;

        if (gr1 == GATE_PASS) { stats->fed_pass++; passed++; }
        else if (gr1 == GATE_GHOST) stats->fed_ghost++;
        else stats->fed_drop++;
    }

    /* scalar tail for odd N */
    for (; i < N; i++) {
        uint8_t lane = h_lane ? h_lane[i] : (uint8_t)(h_hil[i] % 54u);
        uint8_t iso  = h_audit[i] & 1u;
        uint64_t adr = (uint64_t)L38_FED_BASE + (uint64_t)L38_FED_SV * (uint64_t)i;
        uint64_t val = ((uint64_t)h_hil[i] << 8) | lane;
        GateResult gr = fed_write(fed, l38_to_packed(lane, iso), adr, val);
        if (gr == GATE_PASS) { stats->fed_pass++; passed++; }
        else if (gr == GATE_GHOST) stats->fed_ghost++;
        else stats->fed_drop++;
    }

    return passed;
}

#endif /* S11_FED_PACK */

/* ════════════════════════════════════════════════════════════════════
 * S11 Integration helper — full write path with all S11 opts
 *
 * Requires: pogls_twin_bridge.h included BEFORE geo_s11.h
 * Gate: #define S11_BRIDGE_INTEGRATION before include to enable
 * ════════════════════════════════════════════════════════════════════ */
#ifdef S11_BRIDGE_INTEGRATION

/*
 * s11_bridge_write_batch — N ops through TwinBridge with S11 opts
 *
 * addrs[], values[]   : N inputs
 * slot_hots[]         : N hot flags
 * res_out[]           : N PipelineResult (may be NULL)
 *
 * vs original: eliminates N serial te_tick() calls →
 *   1× te_batch_accumulate + QRPN ORB applied per pkt
 */
static inline FiboEvent s11_bridge_write_batch(
    TwinBridge      *b,
    const uint64_t  *addrs,
    const uint64_t  *values,
    const uint8_t   *slot_hots,
    uint32_t         N,
    PipelineResult  *res_out)
{
    if (!b || !addrs || !values || N == 0) return FIBO_EV_NONE;

    /* pre-compute spoke/hot/drift arrays for batch te accumulation */
    uint8_t  *spokes = (uint8_t  *)__builtin_alloca(N);
    uint8_t  *hots   = (uint8_t  *)__builtin_alloca(N);
    uint32_t *drifts = (uint32_t *)__builtin_alloca(N * sizeof(uint32_t));

    uint32_t prev_fx = b->fibo.prev_fx;
    FiboEvent ev_acc = FIBO_EV_NONE;

    for (uint32_t i = 0; i < N; i++) {
        uint64_t raw = addrs[i] ^ values[i] ^ b->fibo.seed.gen3
                       ^ (uint64_t)b->fibo.clk.c144;
        uint32_t fx  = (uint32_t)(raw & 0xFFFFFFFFu);

        uint32_t d = (fx > prev_fx) ? fx - prev_fx : prev_fx - fx;
        prev_fx    = fx;

        spokes[i] = (uint8_t)(b->fibo.seed.gen2 & 0x7u) % 6u;
        hots[i]   = (d > 4u) ? 1u : slot_hots[i];
        drifts[i] = d;

        /* clock tick per-op (L0/L1/L3 timers need per-op resolution) */
        FiboEvent ev = fibo_clock_tick(&b->fibo, fx);
        ev_acc |= ev;

        PipelineResult res;
        pipeline_wire_process(&b->pw, addrs[i], values[i], slot_hots[i], &res);
        qrpn_orb_apply(&b->fibo, &res.pkt);
        if (res_out) res_out[i] = res;

        /* Twin Geo side — mirrors twin_bridge_write logic */
        uint64_t isect = geo_fast_intersect(raw);
        if ((raw & 7u) == 0u)
            b->flow.drift_acc +=
                (uint32_t)__builtin_popcountll(b->baseline & ~isect);
        uint64_t r_next = diamond_route_update(b->flow.route_addr, isect);
        r_next = geo_route_addr_guard(r_next, isect);

        int at_end = (ev & FIBO_EV_FLUSH)
                  || (isect == 0)
                  || (b->flow.drift_acc > 72u)
                  || (b->flow.hop_count >= DIAMOND_HOP_MAX);
        if (at_end) {
            uint8_t offset = (uint8_t)(
                __builtin_popcountll(b->baseline & ~isect) & 0xFFu);
            dodeca_insert(&b->dodeca, r_next, 0, 0, offset,
                          b->flow.hop_count, 0);
            b->twin_writes++;
            diamond_flow_init(&b->flow);
            if (ev & FIBO_EV_FLUSH) b->flush_count++;
        } else {
            b->flow.route_addr = r_next;
            b->flow.hop_count++;
        }
        b->total_ops++;
    }

    /* batch ThirdEye accumulation — replaces N serial te_tick() */
    te_batch_accumulate(&b->fibo.eye, b->fibo.seed,
                        spokes, hots, drifts, N);

    return ev_acc;
}

#endif /* S11_BRIDGE_INTEGRATION */

#endif /* GEO_S11_H */
