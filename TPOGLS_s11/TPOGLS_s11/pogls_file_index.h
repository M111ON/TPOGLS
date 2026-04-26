/*
 * pogls_file_index.h — POGLS File Index Layer (S4)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Role: metadata + chunk map สำหรับ 1 file
 *   FileIndexHeader  → ชื่อ, ขนาด, type, chunk_count
 *   FileChunkRecord  → pair (ScanEntry × ChunkEntry) ต่อ chunk
 *
 * Design:
 *   - Flat array layout — [Header 64B][FileChunkRecord × N]
 *   - O(1) lookup per chunk_id (array index = chunk_id)
 *   - No heap — caller supplies buffer
 *   - QRPN guard บน header
 *   - file_type enum: TEXT/BINARY/PASSTHRU แยก compress path
 *
 * Relationship:
 *   scanner.h  → produces ScanEntry per chunk
 *   compress.h → produces CompressHeader per chunk
 *   chunk_index.h → maps chunk_id → compressed stream offset
 *   file_index.h  → binds all three per file (this layer)
 *
 * Memory layout:
 *   [FileIndexHeader 64B][FileChunkRecord × chunk_count]
 *   sizeof(FileChunkRecord) = 64B (1 cache line)
 *
 * Frozen rules:
 *   - PHI constants from pogls_platform.h only
 *   - DiamondBlock 64B — chunk_size fixed
 *   - QRPN via pogls_qrpn_phaseE.h
 *   - integer only — no float
 *   - GPU never touches file index path
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_FILE_INDEX_H
#define POGLS_FILE_INDEX_H

#include "core/pogls_platform.h"
#include "core/pogls_fold.h"
#include "core/pogls_qrpn_phaseE.h"
#include <stdint.h>
#include <string.h>

/*
 * Requires caller to include before this header:
 *   pogls_scanner.h  → ScanEntry, SCAN_FLAG_*, _scan_is_passthru_magic()
 *   pogls_compress.h → CompressHeader, COMP_MAGIC, COMP_FLAG_PASSTHRU,
 *                       COMPRESS_HEADER_SIZE
 */

/* ── Limits ──────────────────────────────────────────────────────── */
#ifndef FILE_INDEX_CHUNKS_MAX
#  define FILE_INDEX_CHUNKS_MAX  65536u   /* max chunks per file (~4MB) */
#endif

#define FILE_INDEX_MAGIC    0x46494458u   /* "FIDX"                     */
#define FILE_INDEX_VERSION  1u
#define FILE_INDEX_NAME_MAX 240u          /* bytes — fits in header     */

/* ── File type classification ────────────────────────────────────── */
/*
 * TEXT     → delta + PHI symbol + RLE compress
 * BINARY   → delta + RLE compress (no PHI symbol table)
 * PASSTHRU → store as-is (zip/jpg/mp4/etc — already compressed)
 */
typedef enum {
    FTYPE_TEXT     = 0,   /* text, code, structured data   */
    FTYPE_BINARY   = 1,   /* raw binary (weights, blobs)   */
    FTYPE_PASSTHRU = 2,   /* pre-compressed — skip compressor */
} file_type_t;

/* ── Header flags ────────────────────────────────────────────────── */
#define FIDX_FLAG_COMPLETE   0x01u   /* all chunks registered (sealed) */
#define FIDX_FLAG_VERIFIED   0x02u   /* full verify passed             */
#define FIDX_FLAG_PARTIAL    0x04u   /* partial file (streaming mode)  */

/* ── FileIndexHeader (64B = 1 cache line) ────────────────────────── */
/*
 * name_len      = actual bytes used in name[] (not null-term length)
 * orig_size     = total original file size in bytes
 * chunk_count   = number of FileChunkRecord entries
 * file_type     = FTYPE_*
 * flags         = FIDX_FLAG_*
 * qrpn_cg       = phi_scatter_hex guard over critical fields
 * name[240]     = file path/name (raw bytes, no null required)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* FILE_INDEX_MAGIC                      */
    uint8_t  version;        /* FILE_INDEX_VERSION                    */
    uint8_t  file_type;      /* file_type_t                           */
    uint8_t  flags;          /* FIDX_FLAG_*                           */
    uint8_t  _pad0;
    uint32_t chunk_count;    /* number of FileChunkRecord entries     */
    uint32_t capacity;       /* max entries in this index             */
    uint64_t orig_size;      /* original file size (bytes)            */
    uint64_t total_comp;     /* total compressed bytes stored         */
    uint32_t qrpn_cg;        /* integrity guard                       */
    uint8_t  name_len;       /* bytes used in name[]                  */
    uint8_t  _pad1[3];
    uint8_t  name[240];      /* file name/path (raw, not null-term)   */
} FileIndexHeader;           /* 280B — explicit, not 64B */

/* Trim: header kept readable; not cache-line constrained (written once) */

/* ── FileChunkRecord (64B = 1 cache line, accessed hot) ─────────── */
/*
 * Per-chunk: binds scan result + compressed stream position
 *
 * scan_offset   = byte offset of this chunk in original file
 * comp_offset   = byte offset of CompressHeader in compressed stream
 * seed          = XOR-fold seed from scanner (identity fingerprint)
 * orig_checksum = XOR-fold of raw 64B (from scanner + compress)
 * coord_face    = ThetaCoord.face
 * coord_edge    = ThetaCoord.edge
 * coord_z       = ThetaCoord.z
 * payload_len   = compressed payload bytes
 * seq           = chunk sequence (0-based, must match array index)
 * scan_flags    = SCAN_FLAG_* from scanner
 * comp_flags    = COMP_FLAG_* from compressor
 * rec_crc       = XOR-fold integrity of this record
 */
typedef struct __attribute__((packed)) {
    uint64_t scan_offset;    /* byte offset in original file          */
    uint64_t comp_offset;    /* byte offset in compressed stream      */
    uint64_t seed;           /* chunk fingerprint from scanner        */
    uint32_t orig_checksum;  /* XOR-fold of raw 64B block             */
    uint32_t payload_len;    /* compressed payload size               */
    uint32_t seq;            /* chunk sequence (== array index)       */
    uint8_t  coord_face;     /* ThetaCoord.face  0..11               */
    uint8_t  coord_edge;     /* ThetaCoord.edge  0..4                */
    uint8_t  coord_z;        /* ThetaCoord.z     0..255              */
    uint8_t  scan_flags;     /* SCAN_FLAG_*                          */
    uint8_t  comp_flags;     /* COMP_FLAG_*                          */
    uint8_t  _pad[3];
    uint32_t rec_crc;        /* integrity: XOR-fold of fields above  */
} FileChunkRecord;           /* 48B — packed */

typedef char _fidx_rec_size_check[sizeof(FileChunkRecord) == 48 ? 1 : -1];

/* ── Index context (in-memory handle) ───────────────────────────── */
typedef struct {
    FileIndexHeader  *hdr;       /* points into caller-supplied buffer */
    FileChunkRecord  *records;   /* array after header                  */
    uint32_t          capacity;  /* max records                         */
} FileIndex;

/* ── Buffer sizing ───────────────────────────────────────────────── */
#define FILE_INDEX_BUF_SIZE(n) \
    (sizeof(FileIndexHeader) + (n) * sizeof(FileChunkRecord))

/* ── Internal: record integrity CRC ─────────────────────────────── */
static inline uint32_t _fidx_rec_crc(const FileChunkRecord *r)
{
    /* XOR-fold over all fields before rec_crc */
    uint32_t a, b, c, d, e, f, g, h, j, k;
    memcpy(&a, (const uint8_t*)r +  0, 4);  /* scan_offset lo  */
    memcpy(&b, (const uint8_t*)r +  4, 4);  /* scan_offset hi  */
    memcpy(&c, (const uint8_t*)r +  8, 4);  /* comp_offset lo  */
    memcpy(&d, (const uint8_t*)r + 12, 4);  /* comp_offset hi  */
    memcpy(&e, (const uint8_t*)r + 16, 4);  /* seed lo         */
    memcpy(&f, (const uint8_t*)r + 20, 4);  /* seed hi         */
    memcpy(&g, (const uint8_t*)r + 24, 4);  /* orig_checksum   */
    memcpy(&h, (const uint8_t*)r + 28, 4);  /* payload_len     */
    memcpy(&j, (const uint8_t*)r + 32, 4);  /* seq             */
    memcpy(&k, (const uint8_t*)r + 36, 4);  /* coord+flags+pad */
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ j ^ k;
}

/* ── Internal: header QRPN guard ────────────────────────────────── */
static inline uint32_t _fidx_hdr_guard(const FileIndexHeader *h)
{
    uint64_t mix = (uint64_t)h->chunk_count
                 ^ ((uint64_t)h->capacity << 20)
                 ^ h->orig_size
                 ^ h->total_comp
                 ^ ((uint64_t)h->file_type << 56);
    return qrpn_phi_scatter_hex(mix);
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════ */

/*
 * file_index_init — initialise a FileIndex over caller-supplied buffer
 *
 * @fi        : FileIndex handle to fill
 * @buf       : buffer of size >= FILE_INDEX_BUF_SIZE(capacity)
 * @buf_size  : size of buf in bytes
 * @capacity  : max number of chunk records (<= FILE_INDEX_CHUNKS_MAX)
 * @name      : file name/path (raw bytes)
 * @name_len  : byte length of name (capped at FILE_INDEX_NAME_MAX)
 * @orig_size : total original file size in bytes
 * @ftype     : FTYPE_TEXT / FTYPE_BINARY / FTYPE_PASSTHRU
 *
 * Returns 0 on success, -1 on bad args or buffer too small.
 */
static inline int file_index_init(FileIndex *fi,
                                   uint8_t *buf, size_t buf_size,
                                   uint32_t capacity,
                                   const uint8_t *name, uint8_t name_len,
                                   uint64_t orig_size,
                                   file_type_t ftype)
{
    if (!fi || !buf) return -1;
    if (capacity == 0 || capacity > FILE_INDEX_CHUNKS_MAX) return -1;
    if (buf_size < FILE_INDEX_BUF_SIZE(capacity)) return -1;

    memset(buf, 0, FILE_INDEX_BUF_SIZE(capacity));

    fi->hdr     = (FileIndexHeader *)buf;
    fi->records = (FileChunkRecord *)(buf + sizeof(FileIndexHeader));
    fi->capacity = capacity;

    fi->hdr->magic       = FILE_INDEX_MAGIC;
    fi->hdr->version     = FILE_INDEX_VERSION;
    fi->hdr->file_type   = (uint8_t)ftype;
    fi->hdr->flags       = 0;
    fi->hdr->chunk_count = 0;
    fi->hdr->capacity    = capacity;
    fi->hdr->orig_size   = orig_size;
    fi->hdr->total_comp  = 0;

    /* copy name */
    uint8_t nlen = name_len > FILE_INDEX_NAME_MAX ? FILE_INDEX_NAME_MAX
                                                   : name_len;
    if (name && nlen > 0)
        memcpy(fi->hdr->name, name, nlen);
    fi->hdr->name_len = nlen;

    fi->hdr->qrpn_cg = _fidx_hdr_guard(fi->hdr);

    return 0;
}

/*
 * file_index_add — register one chunk (scan + compress info) into index
 *
 * @fi          : initialised FileIndex
 * @scan        : ScanEntry from scan_buf callback (this chunk)
 * @comp_hdr    : CompressHeader of the compressed chunk
 * @comp_offset : byte offset of comp_hdr in the compressed data stream
 * @qrpn        : QRPN context for shadow log
 *
 * Returns chunk_id (0-based) on success, -1 on overflow or bad args.
 *
 * Rules:
 *   - seq must equal current chunk_count (ordered insert only)
 *   - rec_crc computed and stored
 *   - header guard refreshed after every add
 */
static inline int file_index_add(FileIndex *fi,
                                  const ScanEntry *scan,
                                  const CompressHeader *comp_hdr,
                                  uint64_t comp_offset,
                                  qrpn_ctx_t *qrpn)
{
    if (!fi || !scan || !comp_hdr || !qrpn) return -1;
    if (fi->hdr->chunk_count >= fi->capacity)   return -1;
    if (!(scan->flags & SCAN_FLAG_VALID))        return -1;
    if (comp_hdr->magic != COMP_MAGIC)           return -1;

    uint32_t id = fi->hdr->chunk_count;
    FileChunkRecord *r = &fi->records[id];

    r->scan_offset   = scan->offset;
    r->comp_offset   = comp_offset;
    r->seed          = scan->seed;
    r->orig_checksum = scan->checksum;
    r->payload_len   = comp_hdr->payload_len;
    r->seq           = id;
    r->coord_face    = scan->coord.face;
    r->coord_edge    = scan->coord.edge;
    r->coord_z       = scan->coord.z;
    r->scan_flags    = scan->flags;
    r->comp_flags    = comp_hdr->flags;
    r->_pad[0]       = 0;
    r->_pad[1]       = 0;
    r->_pad[2]       = 0;
    r->rec_crc       = _fidx_rec_crc(r);

    /* update header totals */
    fi->hdr->total_comp  += (uint64_t)COMPRESS_HEADER_SIZE + comp_hdr->payload_len;
    fi->hdr->chunk_count  = id + 1;
    fi->hdr->qrpn_cg      = _fidx_hdr_guard(fi->hdr);

    /* QRPN shadow — bind seed + coord + comp_offset */
    uint64_t sig = scan->seed ^ ((uint64_t)r->orig_checksum << 32) ^ comp_offset;
    uint32_t cg  = qrpn_phi_scatter_hex(sig);
    qrpn_check(sig, comp_offset, cg, qrpn, NULL);

    return (int)id;
}

/*
 * file_index_seal — mark index as complete (all chunks registered)
 *   Sets FIDX_FLAG_COMPLETE, refreshes guard.
 *   After seal, file_index_add will still work (guard refreshed again).
 *
 * Returns 0 on success, -1 if fi is NULL.
 */
static inline int file_index_seal(FileIndex *fi)
{
    if (!fi || !fi->hdr) return -1;
    fi->hdr->flags  |= FIDX_FLAG_COMPLETE;
    fi->hdr->qrpn_cg = _fidx_hdr_guard(fi->hdr);
    return 0;
}

/*
 * file_index_get — O(1) lookup of FileChunkRecord by chunk_id
 *
 * @fi       : initialised FileIndex
 * @chunk_id : 0-based id
 *
 * Returns const pointer to FileChunkRecord, or NULL if:
 *   - chunk_id out of range
 *   - rec_crc mismatch (corrupted)
 */
static inline const FileChunkRecord *file_index_get(const FileIndex *fi,
                                                      uint32_t chunk_id)
{
    if (!fi || chunk_id >= fi->hdr->chunk_count) return NULL;
    const FileChunkRecord *r = &fi->records[chunk_id];
    if (_fidx_rec_crc(r) != r->rec_crc) return NULL;
    return r;
}

/*
 * file_index_verify — full integrity check
 *
 * Checks (in order):
 *   1. magic + version
 *   2. header qrpn_cg
 *   3. every record rec_crc
 *   4. scan_offset monotonically increasing
 *   5. comp_offset monotonically increasing
 *
 * Returns  0 = clean
 *         -1 = bad magic/version
 *         -2 = header guard mismatch
 *         -3 = record crc mismatch   (*bad_id set if non-NULL)
 *         -4 = scan_offset not monotonic
 *         -5 = comp_offset not monotonic
 */
static inline int file_index_verify(const FileIndex *fi, uint32_t *bad_id)
{
    if (!fi || !fi->hdr) return -1;
    if (fi->hdr->magic   != FILE_INDEX_MAGIC)   return -1;
    if (fi->hdr->version != FILE_INDEX_VERSION)  return -1;
    if (_fidx_hdr_guard(fi->hdr) != fi->hdr->qrpn_cg) return -2;

    uint64_t prev_scan = 0, prev_comp = 0;
    for (uint32_t i = 0; i < fi->hdr->chunk_count; i++) {
        const FileChunkRecord *r = &fi->records[i];

        if (_fidx_rec_crc(r) != r->rec_crc) {
            if (bad_id) *bad_id = i;
            return -3;
        }
        if (i > 0 && r->scan_offset <= prev_scan) {
            if (bad_id) *bad_id = i;
            return -4;
        }
        if (i > 0 && r->comp_offset <= prev_comp) {
            if (bad_id) *bad_id = i;
            return -5;
        }
        prev_scan = r->scan_offset;
        prev_comp = r->comp_offset;
    }

    if (fi->hdr->flags & FIDX_FLAG_COMPLETE)
        ((FileIndex*)fi)->hdr->flags |= FIDX_FLAG_VERIFIED;

    return 0;
}

/*
 * file_index_lookup_by_orig_offset — find chunk_id that contains
 * a given byte offset in the original file (for partial reconstruct)
 *
 * Uses the fact that scan_offset = chunk_id * 64  (DiamondBlock aligned)
 * → direct division, O(1)
 *
 * Returns chunk_id, or FILE_INDEX_CHUNKS_MAX if out of range.
 */
static inline uint32_t file_index_lookup_by_orig_offset(const FileIndex *fi,
                                                          uint64_t byte_offset)
{
    if (!fi || fi->hdr->chunk_count == 0) return FILE_INDEX_CHUNKS_MAX;
    uint32_t id = (uint32_t)(byte_offset / DIAMOND_BLOCK_SIZE);
    if (id >= fi->hdr->chunk_count) return FILE_INDEX_CHUNKS_MAX;
    return id;
}

/*
 * file_index_classify — detect file_type from first 4 bytes of raw data
 * (mirrors scanner passthru detection — single source of truth wrapper)
 *
 * Returns FTYPE_PASSTHRU if magic matches known compressed format,
 *         FTYPE_BINARY   if data contains non-printable bytes (heuristic),
 *         FTYPE_TEXT     otherwise.
 *
 * @sample     : pointer to first bytes of file (min 4 bytes)
 * @sample_len : bytes available in sample
 */
static inline file_type_t file_index_classify(const uint8_t *sample,
                                               size_t sample_len)
{
    if (!sample || sample_len < 4) return FTYPE_BINARY;

    /* reuse scanner passthru table */
    if (_scan_is_passthru_magic(sample)) return FTYPE_PASSTHRU;

    /* text heuristic: scan up to 256 bytes — if > 10% non-printable → binary */
    size_t   check = sample_len < 256 ? sample_len : 256;
    uint32_t non_print = 0;
    for (size_t i = 0; i < check; i++) {
        uint8_t b = sample[i];
        if (b < 0x09 || (b > 0x0D && b < 0x20) || b == 0x7F)
            non_print++;
    }
    return (non_print * 10 > check) ? FTYPE_BINARY : FTYPE_TEXT;
}

/* ── Stats ───────────────────────────────────────────────────────── */
typedef struct {
    uint32_t    chunk_count;
    uint64_t    orig_size;
    uint64_t    total_comp;
    int32_t     ratio_bp;       /* (orig-comp)*10000/orig, basis points */
    uint32_t    passthru_count;
    file_type_t file_type;
    uint8_t     flags;
} FileIndexStats;

static inline void file_index_stats(const FileIndex *fi, FileIndexStats *s)
{
    if (!fi || !s) return;
    memset(s, 0, sizeof(*s));

    s->chunk_count = fi->hdr->chunk_count;
    s->orig_size   = fi->hdr->orig_size;
    s->total_comp  = fi->hdr->total_comp;
    s->file_type   = (file_type_t)fi->hdr->file_type;
    s->flags       = fi->hdr->flags;

    if (s->orig_size > 0) {
        int64_t saved = (int64_t)s->orig_size - (int64_t)s->total_comp;
        s->ratio_bp   = (int32_t)(saved * 10000 / (int64_t)s->orig_size);
    }
    for (uint32_t i = 0; i < fi->hdr->chunk_count; i++) {
        if (fi->records[i].scan_flags & SCAN_FLAG_PASSTHRU)
            s->passthru_count++;
    }
}

#endif /* POGLS_FILE_INDEX_H */
