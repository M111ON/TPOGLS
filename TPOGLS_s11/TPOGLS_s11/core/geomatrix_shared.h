/*
 * geomatrix_shared.h — GeoPacket + Phase constants (CPU + GPU shared)
 * v3: Standardized from Geomatrix Handoff
 */
#ifndef GEOMATRIX_SHARED_H
#define GEOMATRIX_SHARED_H

#include <stdint.h>

/* ── Frozen constants (from geo_config.h) ── */
#include "geo_config.h"
/* GEO_BUNDLE_WORDS = 8u  → geo_config.h  (do NOT redefine — was self-ref bug) */
/* GEO_PHASE_COUNT  = 4u  → geo_config.h  (do NOT redefine — was self-ref bug) */
/* GEO_HILBERT_N = GEO_SLOTS = 576        → geo_config.h                       */
#define GEOMATRIX_PATHS    18
#define GEO_WINDOW_LO      128
#define GEO_WINDOW_HI      144

/* ── Phase IDs ── */
#define PHASE_PROBE    0
#define PHASE_MAIN     1
#define PHASE_MIRROR   2
#define PHASE_CANCEL   3

/* ── Phase masks (NEW v2/v3) ── */
static const uint64_t PHASE_MASK64[4] = {
    0xAAAAAAAA00000000ULL,   /* PROBE  */
    0x5555555500000000ULL,   /* MAIN   */
    0xF0F0F0F000000000ULL,   /* MIRROR */
    0x0F0F0F0F00000000ULL,   /* CANCEL */
};

/* ── GeoPacket 16B (full, GPU-safe) ── */
typedef struct {
    uint64_t sig;       /* sig64: fold(bundle) ^ PHASE_MASK64[phase] */
    uint16_t hpos;      /* Hilbert position                           */
    uint16_t idx;       /* bit index [0..575] = CYL_SLOTS v3.2        */
    uint8_t  bit;       /* fetched bit value                          */
    uint8_t  phase;     /* PHASE_PROBE / MAIN / MIRROR / CANCEL       */
    uint8_t  _pad[2];   /* align to 16B                               */
} GeoPacket;

/* ── GeoPacketSmall 8B (compressed, PCIe-optimized) ── */
typedef struct {
    uint32_t sig32;     /* xor_fold(sig64): (sig64>>32)^(sig64&0xFFFFFFFF) */
    uint16_t idx;
    uint8_t  bit;
    uint8_t  phase;
} GeoPacketSmall;

/* ── sig helpers (inline, CPU only) ── */
static inline uint64_t geo_bundle_fold64(const uint64_t *bundle) {
    uint64_t f = 0;
    for (int i = 0; i < GEO_BUNDLE_WORDS; i++) f ^= bundle[i];
    return f;
}

static inline uint64_t geo_compute_sig64(const uint64_t *bundle, uint8_t phase) {
    return geo_bundle_fold64(bundle) ^ PHASE_MASK64[phase & 3];
}

static inline uint32_t geo_sig64_to_sig32(uint64_t sig64) {
    return (uint32_t)(sig64 >> 32) ^ (uint32_t)(sig64 & 0xFFFFFFFFU);
}

static inline uint32_t geo_compute_sig32(const uint64_t *bundle, uint8_t phase) {
    return geo_sig64_to_sig32(geo_compute_sig64(bundle, phase));
}

#endif /* GEOMATRIX_SHARED_H */
