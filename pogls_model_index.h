/*
 * pogls_model_index.h  —  S83-B (minimal, derived from WeightStream usage)
 *
 * ModelIndex = ordered map of tensor id → {byte_start, byte_end, name}
 * No layer_type, no chunk addressing — pure file-offset map.
 *
 * Frozen rules: integer only, 64B DiamondBlock align, zero alloc.
 */
#ifndef POGLS_MODEL_INDEX_H
#define POGLS_MODEL_INDEX_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── constants ───────────────────────────────────────────────────── */
#define MIDX_NAME_LEN     48u
#define MIDX_MAX_LAYERS   4096u
#define MIDX_MODEL_LEN    32u

/* ── error codes ─────────────────────────────────────────────────── */
#define MIDX_OK           0
#define MIDX_ERR_FULL    -1
#define MIDX_ERR_SEALED  -2
#define MIDX_ERR_NOTSEAL -3
#define MIDX_ERR_RANGE   -4
#define MIDX_ERR_NULL    -5

/* ── single layer record (64B — 1 DiamondBlock) ──────────────────── */
typedef struct {
    uint64_t byte_start;            /* absolute offset in .gguf file   */
    uint64_t byte_end;              /* exclusive end, 64B-aligned       */
    uint32_t layer_id;
    uint8_t  name[MIDX_NAME_LEN];
    uint8_t  _pad[4];               /* pad to 64B total                 */
} ModelLayerRecord;                 /* sizeof = 64B exactly             */

/* ── header ──────────────────────────────────────────────────────── */
typedef struct {
    char     model_name[MIDX_MODEL_LEN];
    uint32_t capacity;
    uint32_t count;
    uint8_t  sealed;
    uint8_t  _pad[23];
} ModelIndexHdr;                    /* 64B                              */

/* ── ModelIndex (stack/static allocable) ─────────────────────────── */
typedef struct {
    ModelIndexHdr    hdr;
    ModelLayerRecord layers[MIDX_MAX_LAYERS];
} ModelIndex;

/* ── API ─────────────────────────────────────────────────────────── */

static inline int pogls_model_index_init(ModelIndex *mi,
                                          const char *model_name,
                                          uint32_t    capacity)
{
    if (!mi || !model_name) return MIDX_ERR_NULL;
    memset(&mi->hdr, 0, sizeof(mi->hdr));
    strncpy(mi->hdr.model_name, model_name, MIDX_MODEL_LEN - 1);
    mi->hdr.capacity = (capacity < MIDX_MAX_LAYERS) ? capacity : MIDX_MAX_LAYERS;
    mi->hdr.count    = 0;
    mi->hdr.sealed   = 0;
    return MIDX_OK;
}

static inline int pogls_model_index_add(ModelIndex *mi,
                                         uint32_t    layer_id,
                                         const char *name,
                                         uint64_t    byte_start,
                                         uint64_t    byte_end)
{
    if (!mi || !name)              return MIDX_ERR_NULL;
    if (mi->hdr.sealed)            return MIDX_ERR_SEALED;
    if (mi->hdr.count >= mi->hdr.capacity) return MIDX_ERR_FULL;

    ModelLayerRecord *r = &mi->layers[mi->hdr.count++];
    r->layer_id   = layer_id;
    r->byte_start = byte_start;
    r->byte_end   = byte_end;
    strncpy((char *)r->name, name, MIDX_NAME_LEN - 1);
    r->name[MIDX_NAME_LEN - 1] = '\0';
    return MIDX_OK;
}

static inline int pogls_model_index_seal(ModelIndex *mi) {
    if (!mi)           return MIDX_ERR_NULL;
    if (mi->hdr.sealed) return MIDX_OK;   /* idempotent */
    mi->hdr.sealed = 1;
    return MIDX_OK;
}

static inline int pogls_model_index_get(const ModelIndex   *mi,
                                         uint32_t            layer_id,
                                         ModelLayerRecord   *out)
{
    if (!mi || !out)       return MIDX_ERR_NULL;
    if (!mi->hdr.sealed)   return MIDX_ERR_NOTSEAL;
    if (layer_id >= mi->hdr.count) return MIDX_ERR_RANGE;
    *out = mi->layers[layer_id];   /* 64B copy */
    return MIDX_OK;
}

static inline uint32_t pogls_model_index_total_layers(const ModelIndex *mi) {
    return mi ? mi->hdr.count : 0u;
}

#endif /* POGLS_MODEL_INDEX_H */
