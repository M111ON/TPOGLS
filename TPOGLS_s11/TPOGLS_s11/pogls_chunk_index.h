/*
 * pogls_chunk_index.h — POGLS Chunk Index Layer (S2)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Purpose: แมป chunk_id → byte offset ใน compressed stream
 *          เพื่อให้ reconstruct/stream เข้าถึง chunk ใดก็ได้ใน O(1)
 *
 * Design:
 *   - Index = array of ChunkEntry (32B each, cache-friendly)
 *   - ChunkEntry เก็บ: offset, payload_len, orig_checksum, flags, seq
 *   - QRPN guard บน index header (ป้องกัน index corruption)
 *   - max chunks per index = CHUNK_INDEX_MAX (configurable, default 4096)
 *   - ไม่ allocate heap — caller จัดหา buffer
 *
 * Layout ใน memory / file:
 *   [ChunkIndexHeader 32B][ChunkEntry × N]
 *
 * Frozen rules:
 *   - PHI constants จาก pogls_platform.h เท่านั้น
 *   - DiamondBlock 64B จาก pogls_fold.h
 *   - QRPN via pogls_qrpn_phaseE.h
 *   - integer only — no float
 *   - GPU ไม่แตะ index path
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_CHUNK_INDEX_H
#define POGLS_CHUNK_INDEX_H

#include "core/pogls_platform.h"
#include "core/pogls_fold.h"
#include "core/pogls_qrpn_phaseE.h"
#include "pogls_compress.h"
#include <stdint.h>
#include <string.h>

/* ── Limits ──────────────────────────────────────────────────────── */
#ifndef CHUNK_INDEX_MAX
#  define CHUNK_INDEX_MAX   4096u   /* max chunks per index block       */
#endif

#define CHUNK_INDEX_MAGIC   0x43494458u   /* "CIDX"                     */
#define CHUNK_INDEX_VERSION 1u

/* ── Entry flags (reuse compress flags where applicable) ─────────── */
#define CIDX_FLAG_VALID     0x01u   /* entry populated                  */
#define CIDX_FLAG_PASSTHRU  0x02u   /* chunk was stored passthru        */
#define CIDX_FLAG_LAST      0x04u   /* last chunk of a file/stream      */

/* ── Per-chunk entry (32B, cache-line ×2) ───────────────────────── */
/*
 * offset        = byte position of CompressHeader in the data stream
 * payload_len   = compressed payload bytes (mirrors CompressHeader.payload_len)
 * orig_checksum = XOR-fold of original DiamondBlock (mirrors CompressHeader)
 * seq           = monotonic sequence number (for ordering + gap detect)
 * flags         = CIDX_FLAG_*
 * _pad          = align to 32B
 */
typedef struct __attribute__((packed)) {
    uint64_t offset;        /* byte offset in data stream             */
    uint32_t payload_len;   /* compressed payload bytes               */
    uint32_t orig_checksum; /* XOR-fold of original 64B block         */
    uint32_t seq;           /* sequence number (0-based)              */
    uint8_t  flags;         /* CIDX_FLAG_*                            */
    uint8_t  compress_flags;/* mirrors CompressHeader.flags           */
    uint8_t  _pad[2];       /* reserved, must be 0                    */
    uint32_t entry_crc;     /* XOR-fold of fields above (integrity)   */
} ChunkEntry;               /* 28B — no trailing pad needed */

typedef char _cidx_entry_size_check[sizeof(ChunkEntry) == 28 ? 1 : -1];

/* ── Index header (32B) ──────────────────────────────────────────── */
/*
 * count      = number of valid ChunkEntry records
 * total_orig = sum of all original block sizes (bytes)
 * total_comp = sum of all (COMPRESS_HEADER_SIZE + payload_len)
 * qrpn_cg    = qrpn_phi_scatter_hex over header fields for integrity
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* CHUNK_INDEX_MAGIC                      */
    uint8_t  version;       /* CHUNK_INDEX_VERSION                    */
    uint8_t  _pad[3];
    uint32_t count;         /* populated entries                      */
    uint32_t capacity;      /* max entries (set at init)              */
    uint64_t total_orig;    /* total original bytes indexed           */
    uint64_t total_comp;    /* total compressed bytes (hdr+payload)   */
    uint32_t qrpn_cg;       /* integrity guard over count+totals      */
    uint32_t _pad2;         /* pad to 40B — explicit                  */
} ChunkIndexHeader;         /* 40B */

typedef char _cidx_hdr_size_check[sizeof(ChunkIndexHeader) == 40 ? 1 : -1];

/* ── Index context (in-memory, not serialised directly) ─────────── */
typedef struct {
    ChunkIndexHeader *hdr;      /* points into caller-supplied buffer */
    ChunkEntry       *entries;  /* array after header                  */
    uint32_t          capacity; /* max entries                         */
} ChunkIndex;

/* ── Buffer sizing ───────────────────────────────────────────────── */
#define CHUNK_INDEX_BUF_SIZE(n) \
    (sizeof(ChunkIndexHeader) + (n) * sizeof(ChunkEntry))

/* ── Internal: compute entry integrity CRC ───────────────────────── */
static inline uint32_t _cidx_entry_crc(const ChunkEntry *e)
{
    /* XOR-fold over all fields before entry_crc using byte-safe reads */
    uint32_t a, b, c, d, ef;
    memcpy(&a, (const uint8_t*)e + 0,  4);   /* offset lo      */
    memcpy(&b, (const uint8_t*)e + 4,  4);   /* offset hi      */
    memcpy(&c, (const uint8_t*)e + 8,  4);   /* payload_len    */
    memcpy(&d, (const uint8_t*)e + 12, 4);   /* orig_checksum  */
    memcpy(&ef,(const uint8_t*)e + 16, 4);   /* seq            */
    /* flags(1) + compress_flags(1) + _pad(2) — 4B word */
    uint32_t g;
    memcpy(&g, (const uint8_t*)e + 20, 4);
    return a ^ b ^ c ^ d ^ ef ^ g;
}

/* ── Internal: compute header QRPN guard value ───────────────────── */
static inline uint32_t _cidx_hdr_guard(const ChunkIndexHeader *h)
{
    uint64_t mix = (uint64_t)h->count
                 ^ ((uint64_t)h->capacity << 20)
                 ^ h->total_orig
                 ^ h->total_comp;
    return qrpn_phi_scatter_hex(mix);
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════ */

/*
 * chunk_index_init — initialise a ChunkIndex over caller-supplied buffer
 *
 * @ci       : ChunkIndex to initialise
 * @buf      : buffer of size >= CHUNK_INDEX_BUF_SIZE(capacity)
 * @buf_size : size of buf in bytes
 * @capacity : max number of chunks (≤ CHUNK_INDEX_MAX)
 *
 * Returns 0 on success, -1 if buffer too small or capacity > max.
 */
static inline int chunk_index_init(ChunkIndex *ci,
                                    uint8_t *buf, size_t buf_size,
                                    uint32_t capacity)
{
    if (!ci || !buf) return -1;
    if (capacity == 0 || capacity > CHUNK_INDEX_MAX) return -1;
    if (buf_size < CHUNK_INDEX_BUF_SIZE(capacity)) return -1;

    memset(buf, 0, CHUNK_INDEX_BUF_SIZE(capacity));

    ci->hdr      = (ChunkIndexHeader *)buf;
    ci->entries  = (ChunkEntry *)(buf + sizeof(ChunkIndexHeader));
    ci->capacity = capacity;

    ci->hdr->magic    = CHUNK_INDEX_MAGIC;
    ci->hdr->version  = CHUNK_INDEX_VERSION;
    ci->hdr->count    = 0;
    ci->hdr->capacity = capacity;
    ci->hdr->total_orig = 0;
    ci->hdr->total_comp = 0;
    ci->hdr->qrpn_cg  = _cidx_hdr_guard(ci->hdr);

    return 0;
}

/*
 * chunk_index_add — register one compressed chunk into the index
 *
 * @ci        : initialised ChunkIndex
 * @offset    : byte offset of this chunk's CompressHeader in data stream
 * @hdr       : the CompressHeader of the compressed chunk
 * @qrpn      : QRPN context for guard update
 *
 * Returns chunk_id (0-based) on success, -1 on overflow or bad args.
 *
 * Rules:
 *   - seq = current count before insert
 *   - entry_crc computed and stored
 *   - header guard refreshed after every add
 */
static inline int chunk_index_add(ChunkIndex *ci,
                                   uint64_t offset,
                                   const CompressHeader *hdr,
                                   qrpn_ctx_t *qrpn)
{
    if (!ci || !hdr || !qrpn) return -1;
    if (ci->hdr->count >= ci->capacity) return -1;
    if (hdr->magic != COMP_MAGIC) return -1;

    uint32_t id = ci->hdr->count;
    ChunkEntry *e = &ci->entries[id];

    e->offset         = offset;
    e->payload_len    = hdr->payload_len;
    e->orig_checksum  = hdr->orig_checksum;
    e->seq            = id;
    e->flags          = CIDX_FLAG_VALID;
    e->compress_flags = hdr->flags;
    e->_pad[0]        = 0;
    e->_pad[1]        = 0;
    e->entry_crc      = _cidx_entry_crc(e);

    /* update totals */
    ci->hdr->total_orig += (uint64_t)hdr->orig_blocks * DIAMOND_BLOCK_SIZE;
    ci->hdr->total_comp += (uint64_t)COMPRESS_HEADER_SIZE + hdr->payload_len;
    ci->hdr->count       = id + 1;

    /* refresh QRPN guard on header */
    ci->hdr->qrpn_cg = _cidx_hdr_guard(ci->hdr);

    /* QRPN shadow log for the new entry */
    uint64_t entry_sig = (uint64_t)e->orig_checksum ^ ((uint64_t)id << 32);
    uint32_t cg = qrpn_phi_scatter_hex(entry_sig);
    qrpn_check(entry_sig, (uint64_t)offset, cg, qrpn, NULL);

    return (int)id;
}

/*
 * chunk_index_mark_last — set CIDX_FLAG_LAST on a chunk entry
 *
 * @ci       : initialised ChunkIndex
 * @chunk_id : id returned from chunk_index_add
 *
 * Returns 0 on success, -1 if chunk_id out of range or entry invalid.
 */
static inline int chunk_index_mark_last(ChunkIndex *ci, uint32_t chunk_id)
{
    if (!ci || chunk_id >= ci->hdr->count) return -1;
    ChunkEntry *e = &ci->entries[chunk_id];
    if (!(e->flags & CIDX_FLAG_VALID)) return -1;
    e->flags |= CIDX_FLAG_LAST;
    e->entry_crc = _cidx_entry_crc(e);
    return 0;
}

/*
 * chunk_index_get — O(1) lookup of ChunkEntry by chunk_id
 *
 * @ci       : initialised ChunkIndex
 * @chunk_id : 0-based id
 *
 * Returns pointer to ChunkEntry (read-only), or NULL if invalid.
 * Verifies entry_crc before returning — corrupted entry → NULL.
 */
static inline const ChunkEntry *chunk_index_get(const ChunkIndex *ci,
                                                  uint32_t chunk_id)
{
    if (!ci || chunk_id >= ci->hdr->count) return NULL;
    const ChunkEntry *e = &ci->entries[chunk_id];
    if (!(e->flags & CIDX_FLAG_VALID)) return NULL;

    /* integrity check */
    if (_cidx_entry_crc(e) != e->entry_crc) return NULL;

    return e;
}

/*
 * chunk_index_verify — full integrity check of entire index
 *
 * Checks:
 *   1. magic + version
 *   2. header qrpn_cg matches recomputed guard
 *   3. every entry's entry_crc is valid
 *   4. offsets are monotonically increasing (stream order)
 *
 * Returns 0 if clean, negative error code:
 *   -1  bad magic/version
 *   -2  header guard mismatch
 *   -3  entry crc mismatch (entry id in *bad_id if non-NULL)
 *   -4  offset not monotonic (entry id in *bad_id if non-NULL)
 */
static inline int chunk_index_verify(const ChunkIndex *ci, uint32_t *bad_id)
{
    if (!ci || !ci->hdr) return -1;

    if (ci->hdr->magic   != CHUNK_INDEX_MAGIC)   return -1;
    if (ci->hdr->version != CHUNK_INDEX_VERSION)  return -1;

    if (_cidx_hdr_guard(ci->hdr) != ci->hdr->qrpn_cg) return -2;

    uint64_t prev_offset = 0;
    for (uint32_t i = 0; i < ci->hdr->count; i++) {
        const ChunkEntry *e = &ci->entries[i];

        if (_cidx_entry_crc(e) != e->entry_crc) {
            if (bad_id) *bad_id = i;
            return -3;
        }

        if (i > 0 && e->offset <= prev_offset) {
            if (bad_id) *bad_id = i;
            return -4;
        }
        prev_offset = e->offset;
    }

    return 0;
}

/*
 * chunk_index_rebuild — reconstruct a ChunkIndex by scanning a raw
 * compressed data buffer (recovery path — walks CompressHeaders)
 *
 * @ci        : pre-initialised ChunkIndex (capacity must be sufficient)
 * @data      : pointer to start of compressed stream
 * @data_len  : total bytes available
 * @qrpn      : QRPN context
 *
 * Returns number of chunks found, or -1 on fatal error.
 * Useful when index was lost but data stream is intact.
 */
static inline int chunk_index_rebuild(ChunkIndex *ci,
                                       const uint8_t *data, size_t data_len,
                                       qrpn_ctx_t *qrpn)
{
    if (!ci || !data || !qrpn) return -1;

    /* reset */
    ci->hdr->count      = 0;
    ci->hdr->total_orig = 0;
    ci->hdr->total_comp = 0;

    size_t pos = 0;
    int    found = 0;

    while (pos + COMPRESS_HEADER_SIZE <= data_len) {
        CompressHeader hdr;
        memcpy(&hdr, data + pos, COMPRESS_HEADER_SIZE);

        if (hdr.magic != COMP_MAGIC) break;   /* end of valid stream */
        if (hdr.payload_len == 0)    break;

        size_t chunk_total = (size_t)COMPRESS_HEADER_SIZE + hdr.payload_len;
        if (pos + chunk_total > data_len) break;

        int id = chunk_index_add(ci, (uint64_t)pos, &hdr, qrpn);
        if (id < 0) break;   /* capacity exceeded */

        found++;
        pos += chunk_total;
    }

    return found;
}

/*
 * chunk_index_stats — fill human-readable stats
 * (integers only — ratio in basis points, 10000 = 100%)
 */
typedef struct {
    uint32_t count;
    uint64_t total_orig;
    uint64_t total_comp;
    int32_t  ratio_bp;      /* (orig-comp)*10000/orig */
    uint32_t passthru_count;
} ChunkIndexStats;

static inline void chunk_index_stats(const ChunkIndex *ci,
                                      ChunkIndexStats *s)
{
    if (!ci || !s) return;
    memset(s, 0, sizeof(*s));

    s->count      = ci->hdr->count;
    s->total_orig = ci->hdr->total_orig;
    s->total_comp = ci->hdr->total_comp;

    if (s->total_orig > 0) {
        int64_t saved = (int64_t)s->total_orig - (int64_t)s->total_comp;
        s->ratio_bp = (int32_t)(saved * 10000 / (int64_t)s->total_orig);
    }

    for (uint32_t i = 0; i < ci->hdr->count; i++) {
        if (ci->entries[i].compress_flags & COMP_FLAG_PASSTHRU)
            s->passthru_count++;
    }
}

#endif /* POGLS_CHUNK_INDEX_H */
