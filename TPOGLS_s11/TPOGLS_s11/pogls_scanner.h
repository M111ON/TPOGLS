/*
 * pogls_scanner.h — POGLS File Scanner (S3)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Role: entry point ของ write path
 *   (const uint8_t *buf, size_t len) → 64B chunks → ThetaCoord → callback
 *
 * Design:
 *   - zero-copy: caller owns buffer (file / mmap / network)
 *   - callback per chunk: no heap allocation, stream-friendly
 *   - seed = SIMD-friendly 8-word XOR fold → finalizer mix
 *   - gear=0 / world=0 fixed; entropy hook prepared (commented)
 *   - passthru filter: detect pre-compressed magic → SCAN_FLAG_PASSTHRU
 *   - tail chunk < 64B → zero-padded into DiamondBlock before scan
 *
 * Frozen rules:
 *   - PHI constants from pogls_platform.h only
 *   - DiamondBlock 64B, no float, no heap, GPU never touches scan path
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_SCANNER_H
#define POGLS_SCANNER_H

#include "core/pogls_platform.h"
#include "core/pogls_fold.h"
#include "theta_map.h"
#include "angular_mapper_v36.h"   /* pogls_node_to_address */
#include <stdint.h>
#include <string.h>

/* ── Limits ──────────────────────────────────────────────────────── */
#define SCANNER_CHUNK_SIZE  64u   /* DiamondBlock aligned              */

/* ── ScanEntry flags ─────────────────────────────────────────────── */
#define SCAN_FLAG_VALID      0x01u  /* entry populated                 */
#define SCAN_FLAG_PASSTHRU   0x02u  /* pre-compressed chunk, skip comp */
#define SCAN_FLAG_TAIL       0x04u  /* last (padded) chunk of input    */

/* ── Pre-compressed magic detection table ────────────────────────── */
/*
 * First 4 bytes of common formats we should NOT compress further.
 * Stored as little-endian uint32 for fast compare.
 */
#define _SCAN_MAGIC_ZIP   0x04034B50u  /* PK\x03\x04              */
#define _SCAN_MAGIC_GZIP  0x00088B1Fu  /* \x1F\x8B\x08\x00        */
#define _SCAN_MAGIC_ZSTD  0xFD2FB528u  /* Zstandard frame         */
#define _SCAN_MAGIC_PNG   0x474E5089u  /* \x89PNG                 */
#define _SCAN_MAGIC_JPEG  0xFFD8FFE0u  /* JFIF JPEG               */
#define _SCAN_MAGIC_JFIF  0xFFD8FFE1u  /* EXIF JPEG               */
#define _SCAN_MAGIC_MP4A  0x70797466u  /* ftyp (mp4/mov)          */
#define _SCAN_MAGIC_WEBP  0x46464952u  /* RIFF (webp/avi)         */

/* ── ScanEntry (48B) ─────────────────────────────────────────────── */
/*
 * offset     = byte offset of this chunk in original buffer
 * seed       = fast 8-word XOR fold of 64B chunk (pre-mix)
 * coord      = theta_map(seed) result
 * addr       = angular address (optional; computed when WITH_ADDR flag set)
 * chunk_idx  = 0-based index in stream
 * flags      = SCAN_FLAG_*
 */
typedef struct {
    uint64_t        offset;      /* byte offset in source buf          */
    uint64_t        seed;        /* XOR-fold + finalizer of chunk      */
    ThetaCoord      coord;       /* face/edge/z from theta_map   [3B]  */
    uint8_t         flags;       /* SCAN_FLAG_*                  [1B]  */
    uint8_t         _pad[4];     /* explicit: align coord+flags→u32   */
    uint32_t        chunk_idx;   /* 0-based sequence                   */
    uint32_t        checksum;    /* XOR-fold of raw 64B (for compress) */
} ScanEntry;   /* 32B exact: 8+8+3+1+4+4+4 */

typedef char _scan_entry_size_check[sizeof(ScanEntry) == 32 ? 1 : -1];

/* ── Callback type ───────────────────────────────────────────────── */
typedef void (*scan_cb)(const ScanEntry *e, void *user);

/* ── Scanner config ──────────────────────────────────────────────── */
/*
 * n_bits  : topo precision passed to pogls_node_to_address (12..20)
 * flags   : SCAN_CFG_WITH_ADDR → also compute angular address per entry
 *           SCAN_CFG_SKIP_PASSTHRU → do not emit callback for passthru chunks
 */
#define SCAN_CFG_WITH_ADDR      0x01u
#define SCAN_CFG_SKIP_PASSTHRU  0x02u

typedef struct {
    uint8_t  n_bits;     /* topo precision (default 16)               */
    uint8_t  cfg_flags;  /* SCAN_CFG_*                                */
    uint8_t  _pad[2];
} ScanConfig;

/* ── Internal: fast passthru detection on first chunk only ──────── */
static inline int _scan_is_passthru_magic(const uint8_t *chunk)
{
    uint32_t magic;
    memcpy(&magic, chunk, 4);
    return (magic == _SCAN_MAGIC_ZIP   ||
            magic == _SCAN_MAGIC_GZIP  ||
            magic == _SCAN_MAGIC_ZSTD  ||
            magic == _SCAN_MAGIC_PNG   ||
            magic == _SCAN_MAGIC_JPEG  ||
            magic == _SCAN_MAGIC_JFIF  ||
            magic == _SCAN_MAGIC_MP4A  ||
            magic == _SCAN_MAGIC_WEBP);
}

/* ── Internal: 8-word XOR fold → finalizer (SIMD-friendly layout) ── */
/*
 * 64B chunk = 8 × uint64_t
 * seed  = w0 ^ w1 ^ w2 ^ w3 ^ w4 ^ w5 ^ w6 ^ w7
 * apply theta_mix64 finalizer (from theta_map.h) for avalanche
 */
static inline uint64_t _scan_chunk_seed(const uint8_t *chunk)
{
    uint64_t w[8];
    memcpy(w, chunk, 64);
    uint64_t s = w[0] ^ w[1] ^ w[2] ^ w[3] ^
                 w[4] ^ w[5] ^ w[6] ^ w[7];
    /* finalizer from theta_map.h */
    s ^= s >> 33;
    s *= UINT64_C(0xff51afd7ed558ccd);
    s ^= s >> 33;
    s *= UINT64_C(0xc4ceb9fe1a85ec53);
    s ^= s >> 33;
    return s;
}

/* ── Internal: XOR-fold 64B → uint32 checksum (same as compress) ── */
static inline uint32_t _scan_chunk_checksum(const uint8_t *chunk)
{
    /* Must match _recon_xorfold64: XOR sixteen 4B words */
    uint32_t acc = 0, w;
    for (int i = 0; i < 16; i++) {
        memcpy(&w, chunk + i * 4, 4);
        acc ^= w;
    }
    return acc;
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════ */

/*
 * scan_buf — scan a byte buffer, emit ScanEntry per 64B chunk via callback
 *
 * @buf    : source buffer (caller-owned, zero-copy)
 * @len    : total bytes
 * @cb     : called once per chunk (never NULL)
 * @user   : opaque pointer passed to cb
 * @cfg    : scanner config (NULL → defaults: n_bits=16, no addr)
 *
 * Behaviour:
 *   - Splits buf into 64B chunks (last chunk zero-padded if needed)
 *   - First chunk: check magic → set SCAN_FLAG_PASSTHRU for ALL chunks
 *     (entire file treated as passthru if first chunk matches)
 *   - Per chunk: compute seed, theta_map, checksum, populate ScanEntry
 *   - If SCAN_CFG_WITH_ADDR: also compute pogls_node_to_address
 *     (gear=0, world=0 — entropy hook below for future use)
 *   - Calls cb(entry, user) unless SCAN_CFG_SKIP_PASSTHRU + passthru
 *
 * Returns: number of chunks emitted via callback (0 if len==0).
 */
static inline uint32_t scan_buf(const uint8_t *buf, size_t len,
                                 scan_cb cb, void *user,
                                 const ScanConfig *cfg)
{
    if (!buf || !cb || len == 0) return 0;

    /* defaults */
    uint8_t n_bits    = cfg ? cfg->n_bits    : 16u;
    uint8_t cfg_flags = cfg ? cfg->cfg_flags : 0u;
    if (n_bits < 12 || n_bits > 20) n_bits = 16u;

    /* passthru detection on first 64B (or full buf if < 64B) */
    uint8_t  first64[64];
    size_t   first_sz = len < 64 ? len : 64;
    memset(first64, 0, 64);
    memcpy(first64, buf, first_sz);

    int file_passthru = _scan_is_passthru_magic(first64);

    uint32_t total_chunks = (uint32_t)((len + 63u) / 64u);
    uint32_t emitted = 0;

    for (uint32_t idx = 0; idx < total_chunks; idx++) {
        size_t  chunk_off = (size_t)idx * 64u;
        size_t  remain    = len - chunk_off;

        /* prepare 64B (zero-pad tail) */
        uint8_t chunk[64];
        if (remain >= 64) {
            memcpy(chunk, buf + chunk_off, 64);
        } else {
            memset(chunk, 0, 64);
            memcpy(chunk, buf + chunk_off, remain);
        }

        /* build entry */
        ScanEntry e;
        memset(&e, 0, sizeof(e));

        e.offset     = (uint64_t)chunk_off;
        e.chunk_idx  = idx;
        e.seed       = _scan_chunk_seed(chunk);
        e.checksum   = _scan_chunk_checksum(chunk);
        e.coord      = theta_map(e.seed);
        e.flags      = SCAN_FLAG_VALID;

        if (file_passthru) e.flags |= SCAN_FLAG_PASSTHRU;
        if (remain < 64)   e.flags |= SCAN_FLAG_TAIL;

        /* angular address (optional) */
        if (cfg_flags & SCAN_CFG_WITH_ADDR) {
            /* gear/world fixed for stability
             * entropy hook (future): gear = (e.seed >> 8) & 0x3
             *                        world = (e.seed >> 10) & 0x1
             */
            uint8_t gear  = 0;
            uint8_t world = 0;
            (void)pogls_node_to_address((uint32_t)e.coord.face,
                                        gear, world, n_bits);
            /* result available to caller via callback if needed;
             * extend ScanEntry with addr field when promoted from optional */
        }

        /* emit */
        int skip = (cfg_flags & SCAN_CFG_SKIP_PASSTHRU) &&
                   (e.flags & SCAN_FLAG_PASSTHRU);
        if (!skip) {
            cb(&e, user);
            emitted++;
        }
    }

    return emitted;
}

/*
 * scan_buf_count — estimate chunk count without scanning
 *   useful for pre-allocating index buffers
 */
static inline uint32_t scan_buf_count(size_t len)
{
    return (uint32_t)((len + 63u) / 64u);
}

#endif /* POGLS_SCANNER_H */
