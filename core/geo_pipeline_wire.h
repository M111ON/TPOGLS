/*
 * geo_pipeline_wire.h — Pipeline Integration: GeoNet + Radial Hilbert
 * ════════════════════════════════════════════════════════════════════
 * Stack position:
 *   [L3 Quad] → [GeoNet] → [RH] → [geo_cylinder / GPU] → [pogls_geomatrix]
 *
 * Replaces raw cidx→spoke in geomatrix_gpu_wire.cu main loop.
 * CPU-side only — produces GeoPacketWire ready for GPU transfer.
 *
 * Usage:
 *   GeoPipeline p;  geo_pipeline_init(&p, seed);
 *   for each op:    geo_pipeline_step(&p, addr, value, slot_hot, &pkt);
 */

#ifndef GEO_PIPELINE_WIRE_H
#define GEO_PIPELINE_WIRE_H

#include <stdint.h>
#include <string.h>
#include "geo_config.h"
#include "geo_net.h"
#include "geo_radial_hilbert.h"

/* ── GeoPacketWire (matches geomatrix_gpu_wire.cu) ───────────────── */
/* Redeclare only if not already included from gpu_wire */
#ifndef GEOPKTWIRE_DEFINED
#define GEOPKTWIRE_DEFINED
typedef struct {
    uint32_t sig32;
    uint16_t idx;
    uint8_t  spoke;
    uint8_t  phase;
} GeoPacketWire;
#endif

/* ── Audit buffer (8 values, one group) ─────────────────────────── */
typedef struct {
    uint8_t  buf[GEO_GROUP_SIZE];
    uint8_t  pos;          /* 0..7 fill position                     */
    uint8_t  spoke;        /* spoke for repair ref                   */
} RHAuditBuf;

static inline void _audit_buf_reset(RHAuditBuf *b, uint8_t spoke) {
    b->pos = 0; b->spoke = spoke;
}

/* ── Pipeline context ────────────────────────────────────────────── */
typedef struct {
    GeoNet     gn;
    GeoSeed    seed;
    RHAuditBuf audit;
    uint32_t   audit_fails;
    uint32_t   total_ops;
} GeoPipeline;

static inline void geo_pipeline_init(GeoPipeline *p, GeoSeed seed) {
    memset(p, 0, sizeof(*p));
    geo_net_init(&p->gn, seed);
    p->seed = seed;
}

/* ── Single step ─────────────────────────────────────────────────── */
/*
 * addr      — logical address (maps to cylinder position)
 * value     — data value (passed to GeoNet, reserved for routing)
 * slot_hot  — ThirdEye heat signal (0 = normal)
 * pkt_out   — filled GeoPacketWire ready for GPU
 *
 * Returns: 0=ok, 1=audit_fail (caller may geo_net_signal_fail)
 */
static inline uint8_t geo_pipeline_step(
    GeoPipeline   *p,
    uint64_t       addr,
    uint64_t       value,
    uint8_t        slot_hot,
    GeoPacketWire *pkt_out)
{
    /* 1. GeoNet route */
    GeoNetAddr a = geo_net_route(&p->gn, addr, value, slot_hot, p->seed);

    /* 2. Radial Hilbert step */
    RHStep rh = rh_step(&a);

    /* 3. Fill packet (spoke + slot from geometry, not raw cidx) */
    pkt_out->sig32 = (uint32_t)(a.slot ^ ((uint32_t)a.spoke << 9));
    pkt_out->idx   = a.slot;
    pkt_out->spoke = a.spoke;
    pkt_out->phase = rh.line.group;   /* group as phase hint     */

     /* 4. Audit buffer accumulate (bounded)
     * NOTE:
     *   rh.do_audit may not fire every 8 ops for every route pattern.
     *   Keep pos bounded to avoid writing past audit.buf[8]. */
    if (p->audit.pos >= GEO_GROUP_SIZE) {
        _audit_buf_reset(&p->audit, a.spoke);
    }
    p->audit.buf[p->audit.pos++] = (uint8_t)(addr & 0xFFu);

    uint8_t fail = 0;
    if (rh.do_audit || p->audit.pos == GEO_GROUP_SIZE) {
        /* group boundary: XOR pair check */
        RHAuditResult r = rh_audit_group(p->audit.buf, a.spoke, 0);
        if (!r.ok) {
            p->audit_fails++;
            geo_net_signal_fail(&p->gn);   /* → ThirdEye hot++ */
            fail = 1;
        }
        _audit_buf_reset(&p->audit, a.spoke);
    }

    p->total_ops++;
    return fail;
}

/* ── Batch fill (replaces gpu_wire main loop) ────────────────────── */
/*
 * Fills n packets from sequential addrs starting at addr_base.
 * Returns count of audit failures.
 */
static inline uint32_t geo_pipeline_fill(
    GeoPipeline   *p,
    GeoPacketWire *pkts,
    uint32_t       n,
    uint64_t       addr_base,
    uint64_t       value,
    uint8_t        slot_hot)
{
    uint32_t fails = 0;
    for (uint32_t i = 0; i < n; i++)
        fails += geo_pipeline_step(p, addr_base + i, value, slot_hot, &pkts[i]);
    return fails;
}

/* ── Status ──────────────────────────────────────────────────────── */
#include <stdio.h>
static inline void geo_pipeline_status(const GeoPipeline *p) {
    printf("[Pipeline] ops=%u  audit_fails=%u  geostate=%s\n",
           p->total_ops, p->audit_fails,
           geo_net_state_name(&p->gn));
}

#endif /* GEO_PIPELINE_WIRE_H */
