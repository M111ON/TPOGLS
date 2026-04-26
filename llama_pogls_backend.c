/*
 * llama_pogls_backend.c  —  S83-B3
 *
 * Standalone POGLS weight-streaming backend for llama.cpp.
 * Does NOT touch llama.cpp internals — caller owns llama_context*.
 *
 * Role:
 *   1. Parse .gguf → ModelIndex         (gguf_parse_to_model_index)
 *   2. Open .gguf  → ReconContext        (pogls_recon_file_open)
 *   3. For each layer: read_at → tensor_set → decode → clear window
 *
 * Design:
 *   - Caller supplies ram_window (size ≥ largest layer bytes)
 *   - Zero alloc beyond caller-supplied buffers
 *   - ModelIndex + ReconContext on stack/static — caller owns
 *   - layer_cb: optional per-layer callback (progress / custom logic)
 *   - Integer only in hot path — no float
 *
 * Frozen rules: DiamondBlock=64B, MAX_PROBE≥128, integer only
 */

#include "geo_headers/pogls_model_index.h"
#include "geo_headers/pogls_recon_file.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── llama.cpp forward decls (caller links against llama.cpp) ─────── */
/* We only call the subset needed — no llama.h include required here.  */
struct llama_context;
struct ggml_tensor;

/* llama.cpp ABI: set raw bytes into a tensor */
extern void ggml_backend_tensor_set(struct ggml_tensor *tensor,
                                     const void         *data,
                                     size_t              offset,
                                     size_t              size);

/* llama.cpp ABI: decode a single transformer layer */
/* NOTE: actual signature depends on llama.cpp version.               */
/* Caller must provide a wrapper matching their build via layer_fn.   */

/* ── error codes ─────────────────────────────────────────────────── */
#define POGLS_BACK_OK           0
#define POGLS_BACK_ERR_NULL    -1
#define POGLS_BACK_ERR_PARSE   -2
#define POGLS_BACK_ERR_IO      -3
#define POGLS_BACK_ERR_LAYER   -4
#define POGLS_BACK_ERR_WINSZ   -5
#define POGLS_BACK_ERR_CB      -6

/* ── layer callback type ─────────────────────────────────────────── */
/*
 * Called after every read_at, before tensor_set.
 * Return 0 to continue, non-zero to abort.
 * user_data: opaque pointer from PoglsBackendCfg.
 */
typedef int (*pogls_layer_cb_t)(uint32_t            layer_id,
                                 uint32_t            total_layers,
                                 const ModelLayerRecord *rec,
                                 void               *user_data);

/* ── layer function type ─────────────────────────────────────────── */
/*
 * Caller provides this to abstract llama.cpp's decode API.
 * Signature: fn(ctx, tensor, layer_id, user_data) → 0 on success
 */
typedef int (*pogls_layer_fn_t)(struct llama_context *ctx,
                                 struct ggml_tensor   *tensor,
                                 uint32_t              layer_id,
                                 void                 *user_data);

/* ── tensor getter: map layer_id → ggml_tensor* ─────────────────── */
/*
 * Caller provides this — they know the model graph structure.
 * Return NULL to skip decode for that layer (embedding tensors etc).
 */
typedef struct ggml_tensor *(*pogls_tensor_get_fn_t)(
        struct llama_context *ctx,
        uint32_t              layer_id,
        void                 *user_data);

/* ══════════════════════════════════════════════════════════════════
 * PoglsBackendCfg — all config in one struct (caller fills)
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* required */
    const char           *gguf_path;       /* path to .gguf file        */
    const char           *model_name;      /* short name for ModelIndex  */
    struct llama_context *llama_ctx;       /* caller's llama context     */

    /* required: caller-supplied window buffer                          */
    void                 *ram_window;      /* scratch for one layer      */
    uint64_t              ram_window_size; /* must be ≥ max layer bytes  */

    /* required: tensor access + decode callbacks                       */
    pogls_tensor_get_fn_t tensor_get_fn;
    pogls_layer_fn_t      layer_fn;
    void                 *fn_user_data;    /* passed to both fns         */

    /* optional: progress callback (NULL = skip)                        */
    pogls_layer_cb_t      layer_cb;
    void                 *cb_user_data;

    /* optional: start/end layer filter (0,0 = all layers)              */
    uint32_t              layer_start;     /* inclusive                  */
    uint32_t              layer_end;       /* exclusive, 0 = all         */
} PoglsBackendCfg;

/* ══════════════════════════════════════════════════════════════════
 * PoglsBackendStats — filled on return
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t layers_read;     /* layers actually streamed          */
    uint32_t layers_skipped;  /* NULL tensor_get_fn → skip         */
    uint64_t bytes_read;      /* total bytes from file             */
    uint64_t peak_layer_bytes;/* largest single layer read         */
} PoglsBackendStats;

/* ══════════════════════════════════════════════════════════════════
 * INTERNAL — validate config
 * ══════════════════════════════════════════════════════════════════ */
static int _validate_cfg(const PoglsBackendCfg *cfg)
{
    if (!cfg)                    return POGLS_BACK_ERR_NULL;
    if (!cfg->gguf_path)         return POGLS_BACK_ERR_NULL;
    if (!cfg->model_name)        return POGLS_BACK_ERR_NULL;
    if (!cfg->llama_ctx)         return POGLS_BACK_ERR_NULL;
    if (!cfg->ram_window)        return POGLS_BACK_ERR_NULL;
    if (cfg->ram_window_size==0) return POGLS_BACK_ERR_WINSZ;
    if (!cfg->tensor_get_fn)     return POGLS_BACK_ERR_NULL;
    if (!cfg->layer_fn)          return POGLS_BACK_ERR_NULL;
    return POGLS_BACK_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * pogls_backend_run()
 *
 * Main entry point.
 * Streams every layer from .gguf → ram_window → tensor → decode.
 * On return, stats is populated (may be NULL).
 *
 * Returns POGLS_BACK_OK or POGLS_BACK_ERR_*
 * ══════════════════════════════════════════════════════════════════ */
int pogls_backend_run(const PoglsBackendCfg *cfg,
                       PoglsBackendStats     *stats)
{
    int rc = _validate_cfg(cfg);
    if (rc != POGLS_BACK_OK) return rc;

    /* zero stats */
    PoglsBackendStats st;
    memset(&st, 0, sizeof(st));

    /* ── 1. parse .gguf → ModelIndex ──────────────────────────── */
    static ModelIndex mi;      /* static: ~264KB, single instance   */
    memset(&mi, 0, sizeof(mi));

    int parse_rc = gguf_parse_to_model_index(cfg->gguf_path, &mi,
                                              cfg->model_name);
    if (parse_rc != 0) return POGLS_BACK_ERR_PARSE;

    uint32_t total = pogls_model_index_total_layers(&mi);

    /* ── 2. open file → ReconContext ───────────────────────────── */
    ReconContext rc_ctx;
    if (pogls_recon_file_open(&rc_ctx, cfg->gguf_path) != RECON_OK)
        return POGLS_BACK_ERR_IO;

    /* ── 3. resolve layer range ────────────────────────────────── */
    uint32_t id_start = cfg->layer_start;
    uint32_t id_end   = (cfg->layer_end == 0 || cfg->layer_end > total)
                        ? total : cfg->layer_end;

    /* ── 4. hot loop: read → optional cb → tensor_set → decode ── */
    for (uint32_t id = id_start; id < id_end; id++) {

        ModelLayerRecord rec;
        if (pogls_model_index_get(&mi, id, &rec) != MIDX_OK) {
            rc = POGLS_BACK_ERR_LAYER; goto done;
        }

        uint64_t layer_bytes = rec.byte_end - rec.byte_start;

        /* window size check */
        if (layer_bytes > cfg->ram_window_size) {
            rc = POGLS_BACK_ERR_WINSZ; goto done;
        }

        /* ── read layer into caller's window ─────────────────── */
        if (pogls_recon_read_at(&rc_ctx,
                                 rec.byte_start,
                                 cfg->ram_window,
                                 layer_bytes) != RECON_OK) {
            rc = POGLS_BACK_ERR_IO; goto done;
        }

        /* stats */
        st.bytes_read += layer_bytes;
        if (layer_bytes > st.peak_layer_bytes)
            st.peak_layer_bytes = layer_bytes;

        /* ── optional progress callback ─────────────────────── */
        if (cfg->layer_cb) {
            int cb_rc = cfg->layer_cb(id, total, &rec,
                                       cfg->cb_user_data);
            if (cb_rc != 0) { rc = POGLS_BACK_ERR_CB; goto done; }
        }

        /* ── get tensor for this layer ───────────────────────── */
        struct ggml_tensor *tensor =
            cfg->tensor_get_fn(cfg->llama_ctx, id, cfg->fn_user_data);

        if (!tensor) {
            /* no tensor → skip decode (embeddings, norms, etc.)  */
            st.layers_skipped++;
            /* clear window regardless */
            memset(cfg->ram_window, 0, (size_t)layer_bytes);
            continue;
        }

        /* ── push bytes into tensor ──────────────────────────── */
        ggml_backend_tensor_set(tensor,
                                 cfg->ram_window,
                                 0,
                                 (size_t)layer_bytes);

        /* ── decode layer ────────────────────────────────────── */
        int fn_rc = cfg->layer_fn(cfg->llama_ctx, tensor, id,
                                   cfg->fn_user_data);
        if (fn_rc != 0) { rc = POGLS_BACK_ERR_LAYER; goto done; }

        st.layers_read++;

        /* ── clear window (peak RAM stays bounded) ───────────── */
        memset(cfg->ram_window, 0, (size_t)layer_bytes);
    }

    rc = POGLS_BACK_OK;

done:
    pogls_recon_file_close(&rc_ctx);
    if (stats) *stats = st;
    return rc;
}

/* ══════════════════════════════════════════════════════════════════
 * pogls_backend_preflight()
 *
 * Dry-run: parse .gguf → ModelIndex, find max layer size,
 * report if caller's ram_window is large enough.
 * Does NOT open llama context — safe to call before model load.
 *
 * Returns POGLS_BACK_OK if window is sufficient.
 * Returns POGLS_BACK_ERR_WINSZ if window too small
 *   (out_required_bytes set to minimum needed).
 * ══════════════════════════════════════════════════════════════════ */
int pogls_backend_preflight(const char *gguf_path,
                              const char *model_name,
                              uint64_t    window_size,
                              uint64_t   *out_required_bytes,
                              uint32_t   *out_total_layers)
{
    if (!gguf_path || !model_name) return POGLS_BACK_ERR_NULL;

    static ModelIndex mi;
    memset(&mi, 0, sizeof(mi));

    int parse_rc = gguf_parse_to_model_index(gguf_path, &mi, model_name);
    if (parse_rc != 0) return POGLS_BACK_ERR_PARSE;

    uint32_t total   = pogls_model_index_total_layers(&mi);
    uint64_t max_sz  = 0;

    for (uint32_t id = 0; id < total; id++) {
        ModelLayerRecord rec;
        if (pogls_model_index_get(&mi, id, &rec) == MIDX_OK) {
            uint64_t sz = rec.byte_end - rec.byte_start;
            if (sz > max_sz) max_sz = sz;
        }
    }

    if (out_required_bytes) *out_required_bytes = max_sz;
    if (out_total_layers)   *out_total_layers   = total;

    return (window_size >= max_sz)
           ? POGLS_BACK_OK
           : POGLS_BACK_ERR_WINSZ;
}

/* ── diagnostic ─────────────────────────────────────────────────── */
void pogls_backend_print_stats(const PoglsBackendStats *st) {
    if (!st) return;
    printf("[pogls_back] layers_read=%u  skipped=%u"
           "  bytes=%llu MB  peak_layer=%llu KB\n",
           st->layers_read,
           st->layers_skipped,
           (unsigned long long)(st->bytes_read   / (1024*1024)),
           (unsigned long long)(st->peak_layer_bytes / 1024));
}

/* ══════════════════════════════════════════════════════════════════
 * USAGE EXAMPLE  (for reference — not compiled)
 *
 *   // 1. preflight — find window size needed
 *   uint64_t win_sz; uint32_t n_layers;
 *   pogls_backend_preflight(path, "qwen3-1.7b", 0, &win_sz, &n_layers);
 *
 *   // 2. alloc window (caller owns — heap/stack/mmap)
 *   uint8_t *window = malloc(win_sz);
 *
 *   // 3. configure
 *   PoglsBackendCfg cfg = {
 *       .gguf_path       = path,
 *       .model_name      = "qwen3-1.7b",
 *       .llama_ctx       = ctx,
 *       .ram_window      = window,
 *       .ram_window_size = win_sz,
 *       .tensor_get_fn   = my_tensor_getter,
 *       .layer_fn        = my_layer_decoder,
 *       .fn_user_data    = NULL,
 *       .layer_cb        = my_progress_cb,  // optional
 *   };
 *
 *   // 4. run
 *   PoglsBackendStats st;
 *   int rc = pogls_backend_run(&cfg, &st);
 *   pogls_backend_print_stats(&st);
 *
 *   free(window);
 * ══════════════════════════════════════════════════════════════════ */
