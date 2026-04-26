/*
 * pogls_weight_stream.h — POGLS Weight Stream (S8)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Role: inference-aware wrapper around ModelIndex + PoglsStream.
 *       Load one layer at a time into a fixed RAM window,
 *       run inference, clear, load next.
 *
 * Goal: model 7B (14GB) → RAM used < 2GB at any point.
 *
 * Design:
 *   - WeightBuf: caller-supplied flat buffer (stack/mmap/static)
 *     → holds at most `ram_budget` bytes of decompressed weights
 *   - WeightStream drives ModelIndex + PoglsStream per layer
 *   - Layer callback: weights ready → inference → return → clear
 *   - Prefetch slot: optional 1-layer lookahead (same budget window)
 *   - Budget enforcer: pogls_ws_fits_budget() before each layer load
 *   - Zero alloc: all state in WeightStream (stack-safe at ~128B)
 *
 * Usage:
 *   uint8_t  ram_window[WS_DEFAULT_BUDGET];
 *   WeightStream ws;
 *   pogls_ws_init(&ws, &mi, &rc, ram_window, WS_DEFAULT_BUDGET);
 *
 *   // Simple sequential inference loop:
 *   pogls_ws_run(&ws, my_infer_cb, user);
 *
 *   // Manual layer-by-layer:
 *   for (uint32_t i = 0; i < pogls_model_index_total_layers(&mi); i++) {
 *       WeightLayerView v;
 *       int r = pogls_ws_load_layer(&ws, i, &v);
 *       if (r == WS_OK) {
 *           run_inference(v.data, v.size, i);
 *           pogls_ws_clear(&ws);
 *       }
 *   }
 *
 * Frozen rules:
 *   - PHI constants from pogls_platform.h only
 *   - DiamondBlock 64B — DIAMOND_BLOCK_SIZE
 *   - integer only — no float
 *   - GPU never touches weight stream core
 *   - core_c/ ห้ามแตะ
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_WEIGHT_STREAM_H
#define POGLS_WEIGHT_STREAM_H

/*
 * Requires caller to include before this header:
 *   pogls_model_index.h  → ModelIndex, ModelLayerRecord, MIDX_OK, MIDX_ERR_*
 *   pogls_stream.h       → PoglsStream, pogls_stream_init, pogls_stream_next
 *   pogls_reconstruct.h  → ReconContext, DIAMOND_BLOCK_SIZE, RECON_OK
 */

#include "core/pogls_platform.h"
#include <stdint.h>
#include <string.h>

/* ── RAM budget defaults ─────────────────────────────────────────── */
/*
 * WS_DEFAULT_BUDGET = 1.5 GB expressed in bytes.
 * Leaves ~0.5 GB headroom on a 2 GB ceiling for activations + KV cache.
 * Caller may override by passing a different ram_budget to pogls_ws_init.
 */
#define WS_DEFAULT_BUDGET    ((size_t)1536 * 1024 * 1024)   /* 1.5 GB */
#define WS_MIN_BUDGET        ((size_t)DIAMOND_BLOCK_SIZE)   /* 64 B   */

/* ── Error codes ─────────────────────────────────────────────────── */
#define WS_OK               0
#define WS_ERR_NULL        -1   /* null pointer argument               */
#define WS_ERR_BUDGET      -2   /* layer too large for ram_budget      */
#define WS_ERR_RANGE       -3   /* layer_id out of bounds              */
#define WS_ERR_STREAM      -4   /* underlying stream / recon error     */
#define WS_ERR_MIDX        -5   /* model index error (see ws->last_midx_err) */
#define WS_ERR_OVERFLOW    -6   /* buffer overflow (should not happen) */
#define WS_ERR_NOT_CLEARED -7   /* load called before clearing prev layer */
#define WS_DONE             1   /* all layers consumed (pogls_ws_run)  */

/* ── WeightLayerView ─────────────────────────────────────────────── */
/*
 * Returned by pogls_ws_load_layer().
 * `data` points into ws->buf — valid until pogls_ws_clear() is called.
 * DO NOT free; caller must not hold this pointer across ws_clear().
 */
typedef struct {
    const uint8_t  *data;        /* pointer into WeightStream buf       */
    size_t          size;        /* bytes of decompressed layer data    */
    uint32_t        layer_id;    /* which layer                         */
    uint8_t         layer_type;  /* MIDX_LAYER_* tag                    */
    char            name[MIDX_NAME_LEN]; /* layer name from ModelIndex  */
} WeightLayerView;

/* ── WeightStream — zero-alloc state (~128B) ─────────────────────── */
typedef struct {
    /* ── bound context ──────────────────────────────────────────── */
    const ModelIndex   *mi;          /* sealed ModelIndex             */
    const ReconContext *rc;          /* bound reconstruct context     */

    /* ── RAM window (caller-owned) ──────────────────────────────── */
    uint8_t            *buf;         /* flat weight buffer            */
    size_t              ram_budget;  /* capacity of buf in bytes      */
    size_t              buf_used;    /* bytes currently occupied      */

    /* ── current layer state ─────────────────────────────────────── */
    uint32_t            loaded_id;   /* layer_id currently in buf     */
    int                 has_layer;   /* 1 = buf holds a layer         */

    /* ── stats / error detail ───────────────────────────────────── */
    uint32_t            layers_done; /* how many layers processed     */
    int                 last_midx_err;  /* MIDX_ERR_* from last op   */
    int                 last_stream_err;/* STREAM_ERR_* from last op */
} WeightStream;

/* ── Layer callback for pogls_ws_run ────────────────────────────── */
/*
 * Called once per layer with weights loaded into view.
 * @view  : layer data (valid only during this call)
 * @user  : opaque pointer from pogls_ws_run
 * Return : 0 = continue, non-zero = stop streaming early
 */
typedef int (*ws_layer_cb)(const WeightLayerView *view, void *user);

/* ════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ════════════════════════════════════════════════════════════════════ */

/* ── pogls_ws_init ───────────────────────────────────────────────── */
/*
 * Bind WeightStream to a sealed ModelIndex + ReconContext.
 *
 * @ws          : caller-allocated WeightStream
 * @mi          : sealed ModelIndex (pogls_model_index_seal called)
 * @rc          : initialised ReconContext
 * @buf         : caller RAM window (must be >= ram_budget bytes)
 * @ram_budget  : max bytes loadable per layer; 0 → WS_DEFAULT_BUDGET
 *
 * Returns WS_OK or WS_ERR_NULL.
 */
static inline int pogls_ws_init(WeightStream       *ws,
                                 const ModelIndex   *mi,
                                 const ReconContext *rc,
                                 uint8_t            *buf,
                                 size_t              ram_budget)
{
    if (!ws || !mi || !rc || !buf) return WS_ERR_NULL;
    memset(ws, 0, sizeof(*ws));
    ws->mi         = mi;
    ws->rc         = rc;
    ws->buf        = buf;
    ws->ram_budget = (ram_budget == 0) ? WS_DEFAULT_BUDGET : ram_budget;
    ws->buf_used   = 0;
    ws->has_layer  = 0;
    return WS_OK;
}

/* ── pogls_ws_fits_budget ────────────────────────────────────────── */
/*
 * Check whether a layer's byte range fits within ram_budget.
 * Returns 1 (fits) or 0 (too large).
 *
 * Use before pogls_ws_load_layer to decide whether to skip/split.
 */
static inline int pogls_ws_fits_budget(const WeightStream *ws,
                                        uint32_t            layer_id)
{
    if (!ws) return 0;
    ModelLayerRecord rec;
    if (pogls_model_index_get(ws->mi, layer_id, &rec) != MIDX_OK) return 0;
    size_t layer_bytes = (size_t)(rec.byte_end - rec.byte_start);
    return (layer_bytes <= ws->ram_budget) ? 1 : 0;
}

/* ── pogls_ws_clear ──────────────────────────────────────────────── */
/*
 * Release current layer from RAM window (zero for security, then reset).
 * Must be called between successive pogls_ws_load_layer() calls.
 */
static inline void pogls_ws_clear(WeightStream *ws)
{
    if (!ws || !ws->buf) return;
    if (ws->buf_used > 0)
        memset(ws->buf, 0, ws->buf_used);
    ws->buf_used  = 0;
    ws->has_layer = 0;
    ws->loaded_id = 0;
}

/* ── pogls_ws_load_layer ─────────────────────────────────────────── */
/*
 * Decompress and load one layer into ws->buf.
 * Fills view with pointer + metadata on success.
 *
 * @ws       : initialised WeightStream
 * @layer_id : 0-based layer (must be in ModelIndex)
 * @view     : filled on WS_OK (pointer valid until pogls_ws_clear)
 *
 * Returns WS_OK or WS_ERR_*.
 *
 * Enforces: must call pogls_ws_clear() between loads.
 * Budget check: layer_bytes > ram_budget → WS_ERR_BUDGET (no partial load).
 */
static inline int pogls_ws_load_layer(WeightStream      *ws,
                                       uint32_t           layer_id,
                                       WeightLayerView   *view)
{
    if (!ws || !view) return WS_ERR_NULL;

    /* ── guard: previous layer must be cleared first ── */
    if (ws->has_layer) return WS_ERR_NOT_CLEARED;

    /* ── lookup layer record ── */
    ModelLayerRecord rec;
    int mr = pogls_model_index_get(ws->mi, layer_id, &rec);
    if (mr != MIDX_OK) {
        ws->last_midx_err = mr;
        return WS_ERR_MIDX;
    }

    /* ── budget check ── */
    size_t layer_bytes = (size_t)(rec.byte_end - rec.byte_start);
    if (layer_bytes > ws->ram_budget) return WS_ERR_BUDGET;

    /* ── init stream for this layer's chunk window ── */
    PoglsStream s;
    int sr = pogls_stream_init(&s, ws->rc, rec.chunk_start, rec.chunk_end);
    if (sr != STREAM_OK) {
        ws->last_stream_err = sr;
        return WS_ERR_STREAM;
    }

    /* ── drain stream into buf ── */
    size_t   pos = 0;
    uint8_t  chunk[DIAMOND_BLOCK_SIZE];
    uint32_t got = 0;
    int      r;

    while ((r = pogls_stream_next(&s, chunk, &got)) == STREAM_OK) {
        if (pos + got > ws->ram_budget) return WS_ERR_OVERFLOW;
        memcpy(ws->buf + pos, chunk, got);
        pos += got;
    }
    if (r != STREAM_DONE) {
        ws->last_stream_err = r;
        return WS_ERR_STREAM;
    }

    /* ── layer loaded ── */
    ws->buf_used  = pos;
    ws->has_layer = 1;
    ws->loaded_id = layer_id;

    /* ── fill view (points into buf — valid until ws_clear) ── */
    view->data       = ws->buf;
    view->size       = pos;
    view->layer_id   = rec.layer_id;
    view->layer_type = rec.layer_type;
    memcpy(view->name, rec.name, MIDX_NAME_LEN);

    return WS_OK;
}

/* ── pogls_ws_run ────────────────────────────────────────────────── */
/*
 * Sequential inference loop: load → callback → clear → next.
 *
 * Iterates all layers in ModelIndex order (0..layer_count-1).
 * If callback returns non-zero, stops early and returns WS_OK
 * (early stop is not an error).
 *
 * Returns WS_DONE on full completion, WS_ERR_* on failure.
 */
static inline int pogls_ws_run(WeightStream *ws,
                                ws_layer_cb   cb,
                                void         *user)
{
    if (!ws || !cb) return WS_ERR_NULL;

    uint32_t total = pogls_model_index_total_layers(ws->mi);

    for (uint32_t i = 0; i < total; i++) {
        /* skip layers that exceed budget (oversized; caller can split) */
        if (!pogls_ws_fits_budget(ws, i)) continue;

        WeightLayerView view;
        int r = pogls_ws_load_layer(ws, i, &view);
        if (r != WS_OK) return r;

        ws->layers_done++;

        int stop = cb(&view, user);

        pogls_ws_clear(ws);   /* always clear, even if cb stops early */

        if (stop) return WS_DONE;
    }

    return WS_DONE;
}

/* ── pogls_ws_ram_used ───────────────────────────────────────────── */
/*
 * How many bytes are currently occupying the RAM window.
 * 0 after ws_clear, > 0 during a loaded layer.
 */
static inline size_t pogls_ws_ram_used(const WeightStream *ws)
{
    return ws ? ws->buf_used : 0;
}

/* ── pogls_ws_budget_pct ─────────────────────────────────────────── */
/*
 * Integer percentage of ram_budget currently in use (0..100).
 * Useful for monitoring / logging without float division.
 *
 *   pct = (buf_used * 100) / ram_budget
 */
static inline uint32_t pogls_ws_budget_pct(const WeightStream *ws)
{
    if (!ws || ws->ram_budget == 0) return 0;
    return (uint32_t)((ws->buf_used * 100u) / ws->ram_budget);
}

#endif /* POGLS_WEIGHT_STREAM_H */
