#ifndef POGLS_GEOMATRIX_H
#define POGLS_GEOMATRIX_H

#include "geomatrix_shared.h"
#include <stdbool.h>

/* 
 * Geomatrix 18-Way Signature Gate (v3)
 * ══════════════════════════════════════════════════════════════════
 * Layer 1: Fast Sig32 Match
 * Layer 2: Hilbert Block Match (Spatial Locality)
 * Verdict: 18-Path Score Window [128..144]
 */

typedef struct {
    uint64_t total_packets;
    uint64_t sig_mismatches;
    uint64_t hilbert_violations;
    uint64_t stable_batches;
} GeomatrixStatsV3;

/* 
 * geomatrix_packet_validate:
 * ตรวจสอบความถูกต้องของ 1 Packet ด้วย Signature และ Hilbert
 */
static inline bool geomatrix_packet_validate(
    const GeoPacket *pkt, 
    const uint64_t *bundle,
    GeomatrixStatsV3 *stats) 
{
    stats->total_packets++;

    /* Layer 1: Signature Check (Ground Truth Reconstruct) */
    uint64_t expected_sig = geo_compute_sig64(bundle, pkt->phase);
    if (pkt->sig != expected_sig) {
        stats->sig_mismatches++;
        return false;
    }

    /* Layer 2: Hilbert Block Consistency */
    /* กฎ: hpos และ idx ต้องอยู่ใน block เดียวกัน
     * boundary = 288 = 576/2 = 2×144 (ThirdEye cycle ✓) */
    bool h_block = (pkt->hpos >= 288);
    bool i_block = (pkt->idx >= 288);
    if (h_block != i_block) {
        stats->hilbert_violations++;
        return false;
    }

    return true;
}

/* 
 * geomatrix_batch_verdict:
 * ตัดสินใจยก Batch (18 เส้นทาง) ตามคะแนน Window
 */
static inline bool geomatrix_batch_verdict(
    const GeoPacket pkts[GEOMATRIX_PATHS],
    const uint64_t *bundle,
    GeomatrixStatsV3 *stats)
{
    int32_t score = 0;
    for (int i = 0; i < GEOMATRIX_PATHS; i++) {
        if (geomatrix_packet_validate(&pkts[i], bundle, stats)) {
            score += 8; /* 17x8 Logic */
        }
    }

    if (score >= GEO_WINDOW_LO && score <= GEO_WINDOW_HI) {
        stats->stable_batches++;
        return true;
    }
    return false;
}

#endif
