/*
 * pogls_reconstruct.h — POGLS Reconstruct Pipeline (S5)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Role: รับ FileIndex + compressed stream → คืน original bytes
 *       รองรับ full reconstruct และ partial (chunk range)
 *
 * Design:
 *   - No heap — caller supplies output buffer
 *   - Per-chunk: comp_offset → pogls_decompress_block → copy to out
 *   - QRPN verify per chunk (before output) — hard fail on mismatch
 *   - orig_checksum verify per chunk (XOR-fold post-decompress)
 *   - Partial mode: specify chunk_start..chunk_end (inclusive)
 *   - Passthru chunks: decompress = memcpy 64B as-is
 *   - Last chunk: may be partial (orig_size % 64 != 0)
 *
 * Caller pattern:
 *   ReconContext rc;
 *   pogls_reconstruct_init(&rc, &fi, comp_stream, comp_size, &qrpn);
 *   int r = pogls_reconstruct_full(&rc, out_buf, out_cap, &bytes_out);
 *
 * Streaming pattern (chunk-by-chunk):
 *   for (uint32_t i = 0; i < fi.hdr->chunk_count; i++) {
 *       uint8_t chunk[64];
 *       uint32_t got;
 *       int r = pogls_reconstruct_chunk(&rc, i, chunk, &got);
 *       // chunk ready immediately — no need to wait for full file
 *   }
 *
 * Frozen rules:
 *   - PHI constants from pogls_platform.h only
 *   - DiamondBlock 64B — DIAMOND_BLOCK_SIZE
 *   - QRPN via pogls_qrpn_phaseE.h
 *   - integer only — no float
 *   - GPU never touches reconstruct path
 *   - core_c/ ห้ามแตะ
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_RECONSTRUCT_H
#define POGLS_RECONSTRUCT_H

/*
 * Requires caller to include before this header:
 *   pogls_scanner.h    → ScanEntry, SCAN_FLAG_*
 *   pogls_compress.h   → pogls_decompress_block, CompressHeader, COMPRESS_HEADER_SIZE
 *   pogls_file_index.h → FileIndex, FileChunkRecord, file_index_get
 */

#include "core/pogls_platform.h"
#include "core/pogls_fold.h"
#include "core/pogls_qrpn_phaseE.h"
#include <stdint.h>
#include <string.h>

/* ── Error codes ─────────────────────────────────────────────────── */
#define RECON_OK              0
#define RECON_ERR_NULL       -1   /* null pointer argument             */
#define RECON_ERR_RANGE      -2   /* chunk_id out of range             */
#define RECON_ERR_MAGIC      -3   /* CompressHeader magic mismatch     */
#define RECON_ERR_DECOMP     -4   /* pogls_decompress_block failed     */
#define RECON_ERR_CHECKSUM   -5   /* orig_checksum mismatch            */
#define RECON_ERR_QRPN       -6   /* QRPN verify failed                */
#define RECON_ERR_OVERFLOW   -7   /* out_buf too small                 */
#define RECON_ERR_STREAM     -8   /* comp_offset beyond stream bounds  */
#define RECON_ERR_SEALED     -9   /* FileIndex not sealed              */

/* ── Reconstruct context ─────────────────────────────────────────── */
typedef struct {
    const FileIndex  *fi;          /* file index (must be sealed)      */
    const uint8_t    *comp_stream; /* entire compressed stream buffer  */
    uint64_t          comp_size;   /* bytes in comp_stream             */
    qrpn_ctx_t       *qrpn;        /* QRPN context (caller-owned)      */
} ReconContext;

/* ── Internal: XOR-fold 64B checksum (matches _compress_checksum) ── */
static inline uint32_t _recon_xorfold64(const uint8_t *data)
{
    uint32_t acc = 0;
    uint32_t w;
    for (int i = 0; i < 16; i++) {
        memcpy(&w, data + i * 4, 4);
        acc ^= w;
    }
    return acc;
}

/* ── pogls_reconstruct_init ──────────────────────────────────────── */
/*
 * Bind context. Does NOT copy data — all pointers must outlive context.
 * Returns RECON_OK or RECON_ERR_NULL / RECON_ERR_SEALED.
 */
static inline int pogls_reconstruct_init(ReconContext      *rc,
                                          const FileIndex   *fi,
                                          const uint8_t     *comp_stream,
                                          uint64_t           comp_size,
                                          qrpn_ctx_t        *qrpn)
{
    if (!rc || !fi || !comp_stream || !qrpn) return RECON_ERR_NULL;
    if (!(fi->hdr->flags & FIDX_FLAG_COMPLETE))  return RECON_ERR_SEALED;

    rc->fi          = fi;
    rc->comp_stream = comp_stream;
    rc->comp_size   = comp_size;
    rc->qrpn        = qrpn;
    return RECON_OK;
}

/* ── pogls_reconstruct_chunk ─────────────────────────────────────── */
/*
 * Reconstruct one chunk by chunk_id.
 *
 * @rc        : initialised ReconContext
 * @chunk_id  : 0-based index (must be < fi->hdr->chunk_count)
 * @out       : caller buffer, must be >= 64B
 * @bytes_out : set to actual bytes written (64 normally, < 64 last chunk)
 *
 * Returns RECON_OK on success, RECON_ERR_* on failure.
 * Output is available immediately on return — caller can stream it.
 *
 * Verify order:
 *   1. comp_offset bounds check
 *   2. pogls_decompress_block (includes CompressHeader magic)
 *   3. orig_checksum XOR-fold
 *   4. QRPN check on orig_checksum (uses seq as addr)
 */
static inline int pogls_reconstruct_chunk(const ReconContext *rc,
                                           uint32_t            chunk_id,
                                           uint8_t            *out,
                                           uint32_t           *bytes_out)
{
    if (!rc || !out || !bytes_out) return RECON_ERR_NULL;

    const FileIndex *fi = rc->fi;
    if (chunk_id >= fi->hdr->chunk_count) return RECON_ERR_RANGE;

    /* O(1) record lookup */
    const FileChunkRecord *rec = &fi->records[chunk_id];

    /* ── 1. Bounds check: comp_offset + payload must fit stream ── */
    uint64_t comp_end = rec->comp_offset
                      + (uint64_t)COMPRESS_HEADER_SIZE
                      + (uint64_t)rec->payload_len;
    if (comp_end > rc->comp_size) return RECON_ERR_STREAM;

    const uint8_t *in_buf = rc->comp_stream + rec->comp_offset;
    int            in_len = (int)(COMPRESS_HEADER_SIZE + rec->payload_len);

    /* ── 2. Decompress block ────────────────────────────────────── */
    DiamondBlock blk;
    int dr = pogls_decompress_block(in_buf, in_len, &blk, rc->qrpn);
    if (dr != 0) return RECON_ERR_DECOMP;

    /* ── 3. orig_checksum verify ────────────────────────────────── */
    uint32_t chk = _recon_xorfold64((const uint8_t *)&blk);
    if (chk != rec->orig_checksum) return RECON_ERR_CHECKSUM;

    /* ── 4. Record integrity verify (rec_crc = XOR-fold of record fields) ── */
    uint32_t expected_crc = _fidx_rec_crc(rec);
    if (expected_crc != rec->rec_crc) {
        return RECON_ERR_QRPN;
    }

    /* ── 5. Determine output size ───────────────────────────────── */
    /*
     * Last chunk may be partial: orig_size % 64 gives actual bytes.
     * All other chunks are exactly 64B.
     */
    uint32_t out_bytes = DIAMOND_BLOCK_SIZE;
    if (chunk_id == fi->hdr->chunk_count - 1) {
        uint64_t rem = fi->hdr->orig_size % DIAMOND_BLOCK_SIZE;
        if (rem != 0) out_bytes = (uint32_t)rem;
    }

    memcpy(out, &blk, out_bytes);
    *bytes_out = out_bytes;
    return RECON_OK;
}

/* ── pogls_reconstruct_range ─────────────────────────────────────── */
/*
 * Reconstruct chunk_start..chunk_end (inclusive) into a flat buffer.
 * Enables partial reconstruct: video frame, LLM layer slice, etc.
 *
 * @out_buf   : caller buffer
 * @out_cap   : capacity of out_buf in bytes
 * @bytes_out : total bytes written on success
 *
 * Returns RECON_OK, or RECON_ERR_OVERFLOW if out_cap is too small,
 * or first chunk error encountered.
 */
static inline int pogls_reconstruct_range(const ReconContext *rc,
                                           uint32_t            chunk_start,
                                           uint32_t            chunk_end,
                                           uint8_t            *out_buf,
                                           size_t              out_cap,
                                           size_t             *bytes_out)
{
    if (!rc || !out_buf || !bytes_out) return RECON_ERR_NULL;
    if (chunk_start > chunk_end)       return RECON_ERR_RANGE;
    if (chunk_end >= rc->fi->hdr->chunk_count) return RECON_ERR_RANGE;

    size_t   pos  = 0;
    uint8_t  tmp[DIAMOND_BLOCK_SIZE];

    for (uint32_t id = chunk_start; id <= chunk_end; id++) {
        uint32_t got = 0;
        int r = pogls_reconstruct_chunk(rc, id, tmp, &got);
        if (r != RECON_OK) return r;

        if (pos + got > out_cap) return RECON_ERR_OVERFLOW;
        memcpy(out_buf + pos, tmp, got);
        pos += got;
    }

    *bytes_out = pos;
    return RECON_OK;
}

/* ── pogls_reconstruct_full ──────────────────────────────────────── */
/*
 * Reconstruct entire file.  Convenience wrapper over _range.
 *
 * out_cap must be >= fi->hdr->orig_size.
 */
static inline int pogls_reconstruct_full(const ReconContext *rc,
                                          uint8_t            *out_buf,
                                          size_t              out_cap,
                                          size_t             *bytes_out)
{
    if (!rc) return RECON_ERR_NULL;
    if (rc->fi->hdr->chunk_count == 0) {
        if (bytes_out) *bytes_out = 0;
        return RECON_OK;
    }
    return pogls_reconstruct_range(rc,
                                   0,
                                   rc->fi->hdr->chunk_count - 1,
                                   out_buf, out_cap, bytes_out);
}

/* ── pogls_reconstruct_lookup_offset ────────────────────────────── */
/*
 * Find which chunk_id covers a given byte offset in original file.
 * Returns chunk_id, or UINT32_MAX if offset >= orig_size.
 *
 * Usage: random-access into large files, LLM weight offset lookup.
 *   chunk_id = offset / 64  (O(1) — no scan)
 */
static inline uint32_t pogls_reconstruct_lookup_offset(
    const ReconContext *rc, uint64_t byte_offset)
{
    if (!rc) return UINT32_MAX;
    if (byte_offset >= rc->fi->hdr->orig_size) return UINT32_MAX;
    uint32_t cid = (uint32_t)(byte_offset / DIAMOND_BLOCK_SIZE);
    if (cid >= rc->fi->hdr->chunk_count) return UINT32_MAX;
    return cid;
}

/* ── pogls_reconstruct_bytes_at ─────────────────────────────────── */
/*
 * Convenience: read exactly `len` bytes starting at `byte_offset`
 * from the original file into out_buf.
 *
 * Handles cross-chunk reads transparently.
 * Returns RECON_OK or first error.
 */
static inline int pogls_reconstruct_bytes_at(const ReconContext *rc,
                                              uint64_t  byte_offset,
                                              uint8_t  *out_buf,
                                              uint32_t  len,
                                              uint32_t *bytes_out)
{
    if (!rc || !out_buf || !bytes_out) return RECON_ERR_NULL;
    if (byte_offset + len > rc->fi->hdr->orig_size) return RECON_ERR_RANGE;

    uint32_t chunk_start = (uint32_t)(byte_offset / DIAMOND_BLOCK_SIZE);
    uint32_t chunk_end   = (uint32_t)((byte_offset + len - 1) / DIAMOND_BLOCK_SIZE);

    uint8_t  tmp[DIAMOND_BLOCK_SIZE];
    uint32_t written = 0;

    for (uint32_t id = chunk_start; id <= chunk_end; id++) {
        uint32_t got = 0;
        int r = pogls_reconstruct_chunk(rc, id, tmp, &got);
        if (r != RECON_OK) return r;

        /* slice within this chunk */
        uint64_t chunk_base = (uint64_t)id * DIAMOND_BLOCK_SIZE;
        uint32_t slice_start = (byte_offset > chunk_base)
                               ? (uint32_t)(byte_offset - chunk_base) : 0;
        uint32_t slice_end   = (slice_start + (len - written) < got)
                               ? (slice_start + (len - written)) : got;
        uint32_t copy_len    = slice_end - slice_start;

        memcpy(out_buf + written, tmp + slice_start, copy_len);
        written += copy_len;
    }

    *bytes_out = written;
    return RECON_OK;
}

#endif /* POGLS_RECONSTRUCT_H */
