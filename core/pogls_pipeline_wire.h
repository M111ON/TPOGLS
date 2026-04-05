/*
 * pogls_pipeline_wire.h — Master Pipeline Wire
 * ═════════════════════════════════════════════
 * Stack (one direction only):
 *   [L3 Quad]
 *       → [GeoNet + Radial Hilbert]   geo_pipeline_step()
 *       → [Geomatrix FILTER]          geomatrix_batch_verdict()
 *       → [GeoNet VERIFY]            sig32 + audit_fail → geo_net_signal_fail()
 *   fail → geo_net_signal_fail() → ThirdEye ANOMALY
 *
 * Note: pogls_qrpn superseded by GeoNet — verify IS geo_net_signal_fail()
 *
 * Usage (single op):
 *   PipelineWire pw;  pipeline_wire_init(&pw, seed, bundle);
 *   PipelineResult r = pipeline_wire_process(&pw, addr, value, slot_hot);
 *
 * Usage (batch, 18 paths = 1 Geomatrix verdict):
 *   pipeline_wire_batch(&pw, addrs, values, N, bundle, &verdict);
 */

#ifndef POGLS_PIPELINE_WIRE_H
#define POGLS_PIPELINE_WIRE_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "geo_config.h"
#include "geo_pipeline_wire.h"   /* GeoPipeline, geo_pipeline_step()    */
#include "pogls_geomatrix.h"     /* geomatrix_batch_verdict(), GeoPacket */
#include "pogls_platform.h"      /* PHI constants */
#include "pogls_qrpn_phaseE.h"   /* qrpn_ctx_t, qrpn_check(), qrpn_gpu_witness_cpu_fallback() */

/* ── Master pipeline context ─────────────────────────────────────── */
typedef struct {
    GeoPipeline       geo;        /* GeoNet + RH                        */
    GeomatrixStatsV3  gm_stats;
    const uint64_t   *bundle;     /* sig reference (caller owns)        */
    qrpn_ctx_t        qrpn;       /* P2: QRPN verify context            */
    uint32_t          qrpn_fails;
    uint32_t          total_ops;
} PipelineWire;

static inline void pipeline_wire_init(
    PipelineWire   *pw,
    GeoSeed         seed,
    const uint64_t *bundle)
{
    memset(pw, 0, sizeof(*pw));
    geo_pipeline_init(&pw->geo, seed);
    pw->bundle = bundle;
    qrpn_ctx_init(&pw->qrpn, 8u);   /* P2: init QRPN, N=8 (ShellN default) */
}

/* ── Single op result ────────────────────────────────────────────── */
typedef struct {
    GeoNetAddr  addr;        /* routed geometry position               */
    GeoPacketWire pkt;       /* ready for GPU / downstream             */
    uint8_t     audit_fail;  /* RH group audit mismatch                */
    uint8_t     sig_ok;      /* geomatrix sig32 quick check            */
} PipelineResult;

/*
 * pipeline_wire_process — single op
 * Fills result; caller feeds pkt into batch accumulator.
 * Returns 1 if any layer flagged fail.
 */
static inline uint8_t pipeline_wire_process(
    PipelineWire  *pw,
    uint64_t       addr,
    uint64_t       value,
    uint8_t        slot_hot,
    PipelineResult *out)
{
    /* 1. GeoNet route + RH (audit inside) */
    out->audit_fail = geo_pipeline_step(&pw->geo, addr, value, slot_hot, &out->pkt);

    /* 2. Re-read routed addr for downstream (reconstruct from pkt) */
    out->addr = geo_net_route(&pw->geo.gn, addr, value, slot_hot,
                              pw->geo.seed);

    /* 3. Quick sig32 check against bundle (single-packet layer) */
    uint32_t expected = geo_compute_sig32(pw->bundle, out->pkt.phase);
    out->sig_ok = (out->pkt.sig32 == expected) ? 1u : 0u;

    /* 4. P2: QRPN verify — after sig32 check
     * value = addr (logical), Cg = cpu_fallback until GPU kernel wired
     * fail → geo_net_signal_fail() via existing path below */
    uint64_t qrpn_value = addr ^ (uint64_t)out->pkt.idx;
    uint32_t Cg = qrpn_gpu_witness_cpu_fallback(qrpn_value);
    int qr = qrpn_check(qrpn_value, addr, Cg, &pw->qrpn, NULL);

    /* 5. GeoNet verify — sig fail OR audit fail OR qrpn fail → ThirdEye */
    if (!out->sig_ok || out->audit_fail || qr != 0) {
        pw->qrpn_fails++;
        geo_net_signal_fail(&pw->geo.gn);   /* → hot++ → ANOMALY @97 */
    }

    pw->total_ops++;
    return (uint8_t)(out->audit_fail | (!out->sig_ok) | (qr != 0));
}

/* ── Batch: N ops → 1 Geomatrix verdict (18-path score) ─────────── */
/*
 * Fills up to GEOMATRIX_PATHS=18 GeoPacket from N ops,
 * runs geomatrix_batch_verdict(), returns pass/fail.
 * Remaining ops (N > 18) processed but not scored in this batch.
 */
typedef struct {
    bool    verdict;          /* geomatrix_batch_verdict result         */
    uint32_t audit_fails;
    uint32_t sig_fails;
    int32_t  score_paths;     /* how many of 18 passed sig check        */
} BatchVerdict;

static inline BatchVerdict pipeline_wire_batch(
    PipelineWire   *pw,
    const uint64_t *addrs,
    const uint64_t *values,
    uint32_t        n,
    const uint64_t *bundle)
{
    BatchVerdict bv = {0};
    GeoPacket    gm_pkts[GEOMATRIX_PATHS];
    uint32_t     gm_count = 0;

    for (uint32_t i = 0; i < n; i++) {
        PipelineResult r;
        pipeline_wire_process(pw, addrs[i], values ? values[i] : 0, 0, &r);

        bv.audit_fails += r.audit_fail;
        if (!r.sig_ok) bv.sig_fails++;

        /* accumulate into GeoPacket batch */
        if (gm_count < GEOMATRIX_PATHS) {
            gm_pkts[gm_count].sig   = geo_compute_sig64(bundle, r.pkt.phase);
            gm_pkts[gm_count].hpos  = r.addr.slot;
            gm_pkts[gm_count].idx   = r.addr.slot;
            gm_pkts[gm_count].phase = r.pkt.phase;
            gm_count++;
        }
    }

    /* pad remaining paths if n < 18 */
    while (gm_count < GEOMATRIX_PATHS) {
        gm_pkts[gm_count] = gm_pkts[0];
        gm_count++;
    }

    bv.verdict     = geomatrix_batch_verdict(gm_pkts, bundle, &pw->gm_stats);
    bv.score_paths = (int32_t)pw->gm_stats.stable_batches;
    return bv;
}

/* ── Status ──────────────────────────────────────────────────────── */
#include <stdio.h>
static inline void pipeline_wire_status(const PipelineWire *pw) {
    printf("[PipelineWire] ops=%u  qrpn_fails=%u  geostate=%s\n",
           pw->total_ops, pw->qrpn_fails,
           geo_net_state_name(&pw->geo.gn));
    printf("  geomatrix: total=%llu sig_miss=%llu hilbert_viol=%llu stable=%llu\n",
           (unsigned long long)pw->gm_stats.total_packets,
           (unsigned long long)pw->gm_stats.sig_mismatches,
           (unsigned long long)pw->gm_stats.hilbert_violations,
           (unsigned long long)pw->gm_stats.stable_batches);
    qrpn_stats_print(&pw->qrpn);   /* P2: QRPN stats */
}

#endif /* POGLS_PIPELINE_WIRE_H */
