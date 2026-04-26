/*
 * pogls_model_index.h — POGLS Model Index (S7)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Role: map layer_id → [chunk_start, chunk_end] within a FileIndex
 *       so pogls_stream_init() can fetch exactly one layer at a time.
 *
 * Design:
 *   - Zero alloc — caller supplies ModelIndex buffer (stack/static/mmap)
 *   - Derived from FileIndex: no separate file format, no extra I/O
 *   - Layer boundary = byte_offset / 64 → chunk arithmetic is O(1)
 *   - "layer" = any named byte range (transformer layer, attention head,
 *     MLP block, tokenizer vocab, etc.)
 *   - Max POGLS_MODEL_MAX_LAYERS layers per index (compile-time constant)
 *   - ModelIndex sealed with MIDX_FLAG_COMPLETE before use
 *
 * Build pattern:
 *   ModelIndex mi;
 *   pogls_model_index_init(&mi, "llama3-8b", total_layers);
 *   for each layer i:
 *       pogls_model_index_add(&mi, i, "attn", byte_start, byte_end);
 *   pogls_model_index_seal(&mi);
 *
 * Stream one layer:
 *   ModelLayerRecord rec;
 *   pogls_model_index_get(&mi, layer_id, &rec);
 *   PoglsStream s;
 *   pogls_stream_init(&s, &rc, rec.chunk_start, rec.chunk_end);
 *   while (pogls_stream_next(&s, chunk, &got) == STREAM_OK) { ... }
 *
 * Frozen rules:
 *   - PHI constants from pogls_platform.h only
 *   - DiamondBlock 64B — DIAMOND_BLOCK_SIZE
 *   - integer only — no float
 *   - GPU never touches index path
 *   - core_c/ ห้ามแตะ
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_MODEL_INDEX_H
#define POGLS_MODEL_INDEX_H

/*
 * Requires caller to include before this header:
 *   pogls_reconstruct.h  → DIAMOND_BLOCK_SIZE, ReconContext
 *   pogls_stream.h       → PoglsStream, pogls_stream_init
 *   pogls_file_index.h   → FileIndex, FileIndexHeader
 */

#include "core/pogls_platform.h"
#include "core/pogls_checksum.h"
#include <stdint.h>
#include <string.h>

/* ── Compile-time limits ─────────────────────────────────────────── */
#ifndef POGLS_MODEL_MAX_LAYERS
#define POGLS_MODEL_MAX_LAYERS  4096u   /* covers 70B+ models            */
#endif

#define MIDX_NAME_LEN  24u              /* layer name (null-terminated) [fixed: 32→24 for 64B struct] */

/* ── Flags ───────────────────────────────────────────────────────── */
#define MIDX_FLAG_COMPLETE  0x01u       /* index sealed — ready for use  */
#define MIDX_FLAG_PARTIAL   0x02u       /* stream incomplete model (OK)  */

/* ── Layer type tags (informational — does not affect chunk logic) ── */
#define MIDX_LAYER_GENERIC   0x00u
#define MIDX_LAYER_EMBED     0x01u   /* token embedding table           */
#define MIDX_LAYER_ATTN      0x02u   /* attention weights               */
#define MIDX_LAYER_MLP       0x03u   /* feed-forward / MLP block        */
#define MIDX_LAYER_NORM      0x04u   /* layer norm / RMS norm           */
#define MIDX_LAYER_HEAD      0x05u   /* LM head / output projection     */
#define MIDX_LAYER_VOCAB     0x06u   /* vocabulary / tokenizer data     */

/* ── Error codes ─────────────────────────────────────────────────── */
#define MIDX_OK              0
#define MIDX_ERR_NULL       -1   /* null pointer                        */
#define MIDX_ERR_FULL       -2   /* layer count exceeds MAX_LAYERS      */
#define MIDX_ERR_RANGE      -3   /* layer_id out of bounds              */
#define MIDX_ERR_SEALED     -4   /* write to sealed index               */
#define MIDX_ERR_NOT_SEALED -5   /* read from unsealed index            */
#define MIDX_ERR_ALIGN      -6   /* byte range not 64B aligned          */
#define MIDX_ERR_ORDER      -7   /* byte_end < byte_start               */
#define MIDX_ERR_OVERFLOW   -8   /* chunk_end exceeds fi->chunk_count   */
#define MIDX_ERR_CHECKSUM   -9   /* record integrity check failed       */

/* ── ModelLayerRecord (64B) ──────────────────────────────────────── */
/*
 * One entry per layer. Chunk range is derived from byte offsets at
 * add-time using DIAMOND_BLOCK_SIZE (64B) arithmetic — O(1), no scan.
 *
 * Layout (64B total — DiamondBlock aligned):
 *   [0 ]  layer_id      u32
 *   [4 ]  chunk_start   u32    ← byte_start / 64
 *   [8 ]  chunk_end     u32    ← (byte_end - 1) / 64
 *   [12]  layer_type    u8     ← MIDX_LAYER_*
 *   [13]  flags         u8
 *   [14]  _pad          u16
 *   [16]  byte_start    u64    ← original model byte offset
 *   [24]  byte_end      u64    ← exclusive end (byte_start + layer_size)
 *   [32]  name          char[32]
 *   [56]  rec_crc       u32    ← XOR-fold of bytes [0..55]
 *   [60]  _pad2         u32
 */
typedef struct {
    uint32_t layer_id;      /* 0-based layer index                      */
    uint32_t chunk_start;   /* first chunk in fi that covers this layer */
    uint32_t chunk_end;     /* last  chunk in fi that covers this layer */
    uint8_t  layer_type;    /* MIDX_LAYER_*                             */
    uint8_t  flags;         /* reserved                                 */
    uint16_t _pad;
    uint64_t byte_start;    /* byte offset in original model file       */
    uint64_t byte_end;      /* exclusive: byte_start + layer_byte_size  */
    char     name[MIDX_NAME_LEN]; /* human-readable ("attn.0", etc.)   */
    uint32_t rec_crc;       /* XOR-fold integrity of this record        */
    uint32_t _pad2;
} ModelLayerRecord;

typedef char _midx_rec_size_check[sizeof(ModelLayerRecord) == 64 ? 1 : -1];

/* ── ModelIndexHeader (32B) ──────────────────────────────────────── */
typedef struct {
    uint32_t layer_count;   /* total layers registered                  */
    uint32_t flags;         /* MIDX_FLAG_*                              */
    uint64_t total_chunks;  /* fi->chunk_count mirror (for bounds check)*/
    uint64_t model_bytes;   /* total original model size in bytes       */
    char     model_name[8]; /* short identifier ("llama3", "phi3", ...) */
} ModelIndexHeader;

/* ── ModelIndex ──────────────────────────────────────────────────── */
typedef struct {
    ModelIndexHeader    hdr;
    ModelLayerRecord    layers[POGLS_MODEL_MAX_LAYERS];
} ModelIndex;

/* ── Internal: compute rec_crc for a record ──────────────────────── */
static inline uint32_t _midx_rec_crc(const ModelLayerRecord *r)
{
    /* XOR-fold first 56 bytes (everything before rec_crc) */
    uint32_t acc = 0, w;
    for (int i = 0; i < 14; i++) {
        memcpy(&w, (const uint8_t *)r + i * 4, 4);
        acc ^= w;
    }
    return acc;
}

/* ── pogls_model_index_init ──────────────────────────────────────── */
/*
 * Zero-initialise ModelIndex and bind metadata.
 *
 * @mi           : caller-allocated ModelIndex
 * @model_name   : short identifier (up to 7 chars + null)
 * @total_chunks : fi->hdr->chunk_count (for bounds validation at add-time)
 * @model_bytes  : original model file size
 *
 * Returns MIDX_OK or MIDX_ERR_NULL.
 */
static inline int pogls_model_index_init(ModelIndex *mi,
                                          const char *model_name,
                                          uint64_t    total_chunks,
                                          uint64_t    model_bytes)
{
    if (!mi) return MIDX_ERR_NULL;
    memset(mi, 0, sizeof(*mi));
    mi->hdr.layer_count  = 0;
    mi->hdr.flags        = 0;
    mi->hdr.total_chunks = total_chunks;
    mi->hdr.model_bytes  = model_bytes;
    if (model_name)
        strncpy(mi->hdr.model_name, model_name, sizeof(mi->hdr.model_name) - 1);
    return MIDX_OK;
}

/* ── pogls_model_index_add ───────────────────────────────────────── */
/*
 * Register one layer by its byte range in the original model file.
 * Chunk range is computed as:
 *   chunk_start = byte_start / DIAMOND_BLOCK_SIZE
 *   chunk_end   = (byte_end - 1) / DIAMOND_BLOCK_SIZE
 *
 * @layer_id   : 0-based (caller must order correctly)
 * @layer_type : MIDX_LAYER_* tag
 * @name       : human-readable (up to MIDX_NAME_LEN-1 chars)
 * @byte_start : byte offset in model file (must be 64B-aligned)
 * @byte_end   : exclusive end (byte_start + size); must be 64B-aligned
 *               EXCEPT last layer where byte_end == model_bytes (tail ok)
 *
 * Returns MIDX_OK or MIDX_ERR_*.
 */
static inline int pogls_model_index_add(ModelIndex *mi,
                                         uint32_t    layer_id,
                                         uint8_t     layer_type,
                                         const char *name,
                                         uint64_t    byte_start,
                                         uint64_t    byte_end)
{
    if (!mi)                                  return MIDX_ERR_NULL;
    if (mi->hdr.flags & MIDX_FLAG_COMPLETE)   return MIDX_ERR_SEALED;
    if (mi->hdr.layer_count >= POGLS_MODEL_MAX_LAYERS) return MIDX_ERR_FULL;
    if (byte_end <= byte_start)               return MIDX_ERR_ORDER;
    /* byte_start must be 64B-aligned */
    if (byte_start % DIAMOND_BLOCK_SIZE != 0) return MIDX_ERR_ALIGN;

    uint32_t cs = (uint32_t)(byte_start / DIAMOND_BLOCK_SIZE);
    uint32_t ce = (uint32_t)((byte_end - 1) / DIAMOND_BLOCK_SIZE);

    if (ce >= mi->hdr.total_chunks)           return MIDX_ERR_OVERFLOW;

    ModelLayerRecord *r = &mi->layers[mi->hdr.layer_count];
    memset(r, 0, sizeof(*r));

    r->layer_id    = layer_id;
    r->chunk_start = cs;
    r->chunk_end   = ce;
    r->layer_type  = layer_type;
    r->byte_start  = byte_start;
    r->byte_end    = byte_end;
    if (name)
        strncpy(r->name, name, MIDX_NAME_LEN - 1);
    r->rec_crc = _midx_rec_crc(r);

    mi->hdr.layer_count++;
    return MIDX_OK;
}

/* ── pogls_model_index_seal ──────────────────────────────────────── */
/*
 * Mark index as complete. After this call, add() is rejected.
 * Returns MIDX_OK or MIDX_ERR_NULL.
 */
static inline int pogls_model_index_seal(ModelIndex *mi)
{
    if (!mi) return MIDX_ERR_NULL;
    mi->hdr.flags |= MIDX_FLAG_COMPLETE;
    return MIDX_OK;
}

/* ── pogls_model_index_get ───────────────────────────────────────── */
/*
 * Look up a layer record by layer_id.
 * Verifies rec_crc before returning — catches in-memory corruption.
 *
 * @layer_id : 0-based, must be < hdr.layer_count
 * @out      : caller-supplied record pointer (filled on MIDX_OK)
 *
 * Returns MIDX_OK, MIDX_ERR_NOT_SEALED, MIDX_ERR_RANGE,
 *         or MIDX_ERR_CHECKSUM on corruption.
 *
 * Note: linear scan by layer_count.
 * For models < 4096 layers this is < 1µs — acceptable.
 * Promote to hash if needed for extreme layer counts.
 */
static inline int pogls_model_index_get(const ModelIndex    *mi,
                                         uint32_t             layer_id,
                                         ModelLayerRecord    *out)
{
    if (!mi || !out)                           return MIDX_ERR_NULL;
    if (!(mi->hdr.flags & MIDX_FLAG_COMPLETE)) return MIDX_ERR_NOT_SEALED;
    if (layer_id >= mi->hdr.layer_count)       return MIDX_ERR_RANGE;

    /* O(1) direct index if layer_id matches position (common case) */
    const ModelLayerRecord *r = &mi->layers[layer_id];
    if (r->layer_id == layer_id) {
        uint32_t expected = _midx_rec_crc(r);
        if (expected != r->rec_crc) return MIDX_ERR_CHECKSUM;
        *out = *r;
        return MIDX_OK;
    }

    /* Fallback: linear scan (layer_id != position — sparse registrations) */
    for (uint32_t i = 0; i < mi->hdr.layer_count; i++) {
        if (mi->layers[i].layer_id == layer_id) {
            uint32_t expected = _midx_rec_crc(&mi->layers[i]);
            if (expected != mi->layers[i].rec_crc) return MIDX_ERR_CHECKSUM;
            *out = mi->layers[i];
            return MIDX_OK;
        }
    }
    return MIDX_ERR_RANGE;
}

/* ── pogls_model_index_layer_chunks ─────────────────────────────── */
/*
 * Convenience: get chunk_start + chunk_end for a layer_id directly.
 * Returns MIDX_OK or first error. Skips copying full record.
 */
static inline int pogls_model_index_layer_chunks(const ModelIndex *mi,
                                                   uint32_t          layer_id,
                                                   uint32_t         *chunk_start,
                                                   uint32_t         *chunk_end)
{
    if (!chunk_start || !chunk_end) return MIDX_ERR_NULL;
    ModelLayerRecord rec;
    int r = pogls_model_index_get(mi, layer_id, &rec);
    if (r != MIDX_OK) return r;
    *chunk_start = rec.chunk_start;
    *chunk_end   = rec.chunk_end;
    return MIDX_OK;
}

/* ── pogls_model_index_stream_layer ─────────────────────────────── */
/*
 * Convenience: init a PoglsStream for a single layer.
 *
 * Combines pogls_model_index_layer_chunks + pogls_stream_init.
 * One call → stream ready.
 *
 *   PoglsStream s;
 *   int r = pogls_model_index_stream_layer(&mi, &rc, layer_id, &s);
 *   if (r == MIDX_OK) {
 *       while (pogls_stream_next(&s, chunk, &got) == STREAM_OK) { ... }
 *   }
 *
 * Returns MIDX_OK on success, MIDX_ERR_* on index failure,
 * or STREAM_ERR_* (negative) on stream-init failure.
 */
static inline int pogls_model_index_stream_layer(const ModelIndex   *mi,
                                                   const ReconContext *rc,
                                                   uint32_t            layer_id,
                                                   PoglsStream        *s)
{
    if (!mi || !rc || !s) return MIDX_ERR_NULL;
    uint32_t cs, ce;
    int r = pogls_model_index_layer_chunks(mi, layer_id, &cs, &ce);
    if (r != MIDX_OK) return r;
    return pogls_stream_init(s, rc, cs, ce);
}

/* ── pogls_model_index_total_layers ─────────────────────────────── */
static inline uint32_t pogls_model_index_total_layers(const ModelIndex *mi)
{
    return mi ? mi->hdr.layer_count : 0;
}

#endif /* POGLS_MODEL_INDEX_H */
