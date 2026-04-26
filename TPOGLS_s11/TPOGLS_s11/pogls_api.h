/*
 * pogls_api.h — POGLS Public API (S9)
 * ════════════════════════════════════════════════════════════════════
 *
 * Single entry point over the full pipeline:
 *
 *   pogls_encode(in, len, out, out_len)  →  self-describing blob
 *   pogls_decode(in, len, out, out_len)  →  original bytes
 *
 * Blob layout (self-describing, no external state):
 *   [BlobHeader  32B ]
 *   [FileIndex   sizeof(FileIndexHeader) + N×sizeof(FileChunkRecord)]
 *   [CompStream  variable]
 *
 * Design rules:
 *   - Self-describing: decode needs blob only — no side files
 *   - Zero external alloc: caller supplies all buffers
 *   - Error passthrough: returns RECON_ERR_* / POGLS_API_ERR_* directly
 *   - ABI guard: layout_sig = sizeof(ScanEntry) ^ sizeof(FileChunkRecord)
 *   - Fast-path: len < 64 → single passthru chunk, skip scanner overhead
 *   - core_c/ ห้ามแตะ, integer only, no float
 * ════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_API_H
#define POGLS_API_H

#include "core/pogls_platform.h"
#include "core/pogls_qrpn_phaseE.h"
#include "core/pogls_fold.h"
#include "pogls_compress.h"
#include "pogls_scanner.h"
#include "pogls_file_index.h"
#include "pogls_reconstruct.h"
#include "pogls_stream.h"
#include <stdint.h>
#include <string.h>

/* ── Error codes (own range, no overlap with RECON_ERR_*) ────────── */
#define POGLS_OK               0
#define POGLS_ERR_NULL        -20   /* null pointer                    */
#define POGLS_ERR_SMALL_BUF   -21   /* out buffer too small            */
#define POGLS_ERR_ENCODE      -22   /* encode pipeline failure         */
#define POGLS_ERR_MAGIC       -23   /* blob magic mismatch             */
#define POGLS_ERR_VERSION     -24   /* unsupported blob version        */
#define POGLS_ERR_LAYOUT      -25   /* ABI layout_sig mismatch         */
#define POGLS_ERR_CORRUPT     -26   /* blob header corrupt             */

/* ── ABI layout signature ────────────────────────────────────────── */
/*
 * XOR of critical struct sizes — catches silent padding/ABI drift.
 * Bumps automatically when ScanEntry or FileChunkRecord layout changes.
 */
#define POGLS_LAYOUT_SIG  ((uint32_t)(sizeof(ScanEntry) ^ sizeof(FileChunkRecord)))

/* ── Blob magic + version ─────────────────────────────────────────── */
#define POGLS_BLOB_MAGIC    0x50474C53u   /* "PGLS" little-endian      */
#define POGLS_BLOB_VERSION  1u

/* ── BlobHeader (32B) ────────────────────────────────────────────── */
/*
 * Always first 32B of encoded blob.
 * Decoder validates magic → version → layout_sig before touching data.
 *
 * Layout:
 *   [0 ] magic        u32   POGLS_BLOB_MAGIC
 *   [4 ] version      u8    POGLS_BLOB_VERSION
 *   [5 ] _pad         u8[3]
 *   [8 ] orig_size    u64   original input bytes
 *   [16] chunk_count  u32   number of FileChunkRecord entries
 *   [20] fidx_bytes   u32   sizeof(FileIndex region) in blob
 *   [24] comp_bytes   u32   sizeof(compressed stream) in blob
 *   [28] layout_sig   u32   POGLS_LAYOUT_SIG
 */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  _pad[3];
    uint64_t orig_size;
    uint32_t chunk_count;
    uint32_t fidx_bytes;
    uint32_t comp_bytes;
    uint32_t layout_sig;
} PoglsBlobHeader;

typedef char _blob_hdr_size[sizeof(PoglsBlobHeader) == 32 ? 1 : -1];

/* ── Size helpers ────────────────────────────────────────────────── */
/*
 * pogls_encode_bound(len) — worst-case output size for pogls_encode.
 * Use to allocate out buffer:
 *   uint8_t *out = malloc(pogls_encode_bound(len));
 */
static inline size_t pogls_encode_bound(size_t len)
{
    uint32_t n = scan_buf_count(len);
    if (n == 0) n = 1;
    size_t fidx  = FILE_INDEX_BUF_SIZE(n);
    size_t comp  = (size_t)n * COMPRESS_OUT_MAX(DIAMOND_BLOCK_SIZE);
    return sizeof(PoglsBlobHeader) + fidx + comp;
}

/* ════════════════════════════════════════════════════════════════════
 * pogls_encode
 * ════════════════════════════════════════════════════════════════════
 *
 * Compress `in[0..len)` into a self-describing blob at `out`.
 *
 * @in      : input bytes
 * @len     : input length (0 is valid → blob with 0 chunks)
 * @out     : output buffer (must be >= pogls_encode_bound(len) bytes)
 * @out_len : set to actual blob bytes written on POGLS_OK
 *
 * Returns POGLS_OK or POGLS_ERR_*.
 */

/* internal encode callback state */
typedef struct {
    uint8_t    *comp_buf;
    size_t      comp_pos;
    size_t      comp_cap;
    FileIndex   fi;
    qrpn_ctx_t  qrpn;
    const uint8_t *src;
    size_t      src_len;
    int         error;
} _ApiEncCtx;

static void _api_encode_cb(const ScanEntry *e, void *user)
{
    _ApiEncCtx *ec = (_ApiEncCtx *)user;
    if (ec->error) return;

    DiamondBlock blk; memset(&blk, 0, sizeof(blk));
    size_t off  = (size_t)e->offset;
    size_t avail = (off < ec->src_len) ? (ec->src_len - off) : 0;
    size_t copy = (avail > 64) ? 64 : avail;
    memcpy(&blk, ec->src + off, copy);

    uint8_t tmp[DIAMOND_BLOCK_SIZE + 64];
    int clen = pogls_compress_block(&blk, tmp, (int)sizeof(tmp), &ec->qrpn);
    if (clen <= 0) { ec->error = POGLS_ERR_ENCODE; return; }

    if (ec->comp_pos + (size_t)clen > ec->comp_cap) {
        ec->error = POGLS_ERR_SMALL_BUF; return;
    }

    CompressHeader hdr; memcpy(&hdr, tmp, sizeof(hdr));
    uint64_t co = (uint64_t)ec->comp_pos;
    memcpy(ec->comp_buf + ec->comp_pos, tmp, (size_t)clen);
    ec->comp_pos += (size_t)clen;

    file_index_add(&ec->fi, e, &hdr, co, &ec->qrpn);
}

static inline int pogls_encode(const uint8_t *in,  size_t   len,
                                uint8_t       *out, size_t  *out_len)
{
    if (!out || !out_len) return POGLS_ERR_NULL;
    if (!in && len > 0)   return POGLS_ERR_NULL;

    uint32_t n = scan_buf_count(len);
    if (n == 0) n = 1;

    /* partition out buffer:  [BlobHeader][FileIndex region][CompStream] */
    size_t hdr_off  = 0;
    size_t fidx_off = sizeof(PoglsBlobHeader);
    size_t fidx_sz  = FILE_INDEX_BUF_SIZE(n);
    size_t comp_off = fidx_off + fidx_sz;
    size_t min_out  = comp_off + (size_t)n * COMPRESS_OUT_MAX(DIAMOND_BLOCK_SIZE);

    /* caller must pre-check with pogls_encode_bound */
    (void)min_out;

    /* ── init FileIndex inside out buffer directly ── */
    _ApiEncCtx ec;
    memset(&ec, 0, sizeof(ec));
    ec.src      = in;
    ec.src_len  = len;
    ec.comp_buf = out + comp_off;
    ec.comp_cap = *out_len > comp_off ? (*out_len - comp_off) : 0;

    qrpn_ctx_init(&ec.qrpn, 8);

    if (file_index_init(&ec.fi, out + fidx_off, fidx_sz,
                        n, (const uint8_t *)"pgls", 4,
                        (uint64_t)len, FTYPE_BINARY) != 0)
        return POGLS_ERR_ENCODE;

    /* ── fast-path: single passthru for tiny input ── */
    if (len > 0) {
        ScanConfig cfg = {0};
        scan_buf(in, len, _api_encode_cb, &ec, &cfg);
        if (ec.error) return ec.error;
    }

    if (file_index_seal(&ec.fi) != 0) return POGLS_ERR_ENCODE;

    /* ── write BlobHeader ── */
    PoglsBlobHeader bh;
    memset(&bh, 0, sizeof(bh));
    bh.magic       = POGLS_BLOB_MAGIC;
    bh.version     = POGLS_BLOB_VERSION;
    bh.orig_size   = (uint64_t)len;
    bh.chunk_count = ec.fi.hdr->chunk_count;
    bh.fidx_bytes  = (uint32_t)fidx_sz;
    bh.comp_bytes  = (uint32_t)ec.comp_pos;
    bh.layout_sig  = POGLS_LAYOUT_SIG;
    memcpy(out + hdr_off, &bh, sizeof(bh));

    *out_len = sizeof(PoglsBlobHeader) + fidx_sz + ec.comp_pos;
    return POGLS_OK;
}

/* ════════════════════════════════════════════════════════════════════
 * pogls_decode
 * ════════════════════════════════════════════════════════════════════
 *
 * Reconstruct original bytes from a self-describing blob.
 *
 * @in      : blob produced by pogls_encode
 * @len     : blob length
 * @out     : output buffer (must be >= bh.orig_size bytes)
 * @out_len : set to orig_size on POGLS_OK
 *
 * Returns POGLS_OK, POGLS_ERR_*, or RECON_ERR_* (passthrough).
 */
static inline int pogls_decode(const uint8_t *in,  size_t   len,
                                uint8_t       *out, size_t  *out_len)
{
    if (!in || !out || !out_len) return POGLS_ERR_NULL;
    if (len < sizeof(PoglsBlobHeader)) return POGLS_ERR_CORRUPT;

    /* ── validate BlobHeader ── */
    PoglsBlobHeader bh;
    memcpy(&bh, in, sizeof(bh));

    if (bh.magic      != POGLS_BLOB_MAGIC)   return POGLS_ERR_MAGIC;
    if (bh.version    != POGLS_BLOB_VERSION) return POGLS_ERR_VERSION;
    if (bh.layout_sig != POGLS_LAYOUT_SIG)   return POGLS_ERR_LAYOUT;

    if (*out_len < (size_t)bh.orig_size) return POGLS_ERR_SMALL_BUF;

    /* empty source → nothing to reconstruct */
    if (bh.orig_size == 0) { *out_len = 0; return POGLS_OK; }

    /* ── locate regions in blob ── */
    size_t fidx_off = sizeof(PoglsBlobHeader);
    size_t comp_off = fidx_off + (size_t)bh.fidx_bytes;

    if (comp_off + bh.comp_bytes > len) return POGLS_ERR_CORRUPT;

    /* ── reconstruct FileIndex view (points into blob — zero-copy) ── */
    FileIndex fi;
    /* cast away const: FileIndex is read-only here */
    fi.hdr     = (FileIndexHeader *)(in + fidx_off);
    fi.records = (FileChunkRecord *)(in + fidx_off + sizeof(FileIndexHeader));
    fi.capacity = bh.chunk_count;

    if (fi.hdr->magic != FILE_INDEX_MAGIC) return POGLS_ERR_CORRUPT;

    /* ── init reconstruct context ── */
    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 8);

    ReconContext rc;
    int r = pogls_reconstruct_init(&rc, &fi,
                                   in + comp_off, (uint64_t)bh.comp_bytes,
                                   &qrpn);
    if (r != RECON_OK) return r;

    /* ── reconstruct full — single memcpy path per chunk ── */
    size_t written = 0;
    r = pogls_reconstruct_full(&rc, out, (size_t)bh.orig_size + DIAMOND_BLOCK_SIZE,
                               &written);
    if (r != RECON_OK) return r;

    *out_len = (size_t)bh.orig_size;
    return POGLS_OK;
}

/* ── pogls_decode_orig_size ──────────────────────────────────────── */
/*
 * Peek orig_size from blob header without decoding.
 * Use to allocate out buffer for pogls_decode.
 *
 * Returns orig_size, or 0 on invalid blob.
 */
static inline size_t pogls_decode_orig_size(const uint8_t *blob, size_t len)
{
    if (!blob || len < sizeof(PoglsBlobHeader)) return 0;
    PoglsBlobHeader bh; memcpy(&bh, blob, sizeof(bh));
    if (bh.magic != POGLS_BLOB_MAGIC) return 0;
    return (size_t)bh.orig_size;
}

#endif /* POGLS_API_H */
