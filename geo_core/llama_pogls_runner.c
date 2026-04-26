/*
 * llama_pogls_runner.c  —  S83-B4
 *
 * Caller-side implementation for llama_pogls_backend.
 * Provides:
 *   - my_tensor_getter()   : layer_id → ggml_tensor* (via llama model graph)
 *   - my_layer_decoder()   : single-layer decode via llama_decode()
 *   - progress_cb()        : stderr progress bar
 *   - pogls_runner_main()  : full init → preflight → run → bench
 *
 * Target: Qwen3 1.7B Q4_K_M, dual GTX 1050 Ti
 *         Peak RAM < 2 GB, target 8–10+ t/s
 *
 * Build (link against llama.cpp .so + this project):
 *   gcc llama_pogls_runner.c llama_pogls_backend.c pogls_recon_file.c \
 *       gguf_to_model_index.c \
 *       -I. -L/path/to/llama.cpp/build -lllama -lggml \
 *       -o pogls_runner
 */

#include "geo_headers/pogls_model_index.h"
#include "geo_headers/pogls_recon_file.h"
#include "llama_pogls_backend.h"   /* PoglsBackendCfg, pogls_backend_run, etc. */

/* llama.cpp public headers (caller has these) */
#include "llama.h"
#include "ggml.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── tuning ──────────────────────────────────────────────────────── */
#define RUNNER_CTX_SIZE      2048u
#define RUNNER_N_GPU_LAYERS  99
#define RUNNER_THREADS       4

/* ── runner state ────────────────────────────────────────────────── */
typedef struct {
    struct llama_model   *model;
    struct llama_context *ctx;
    uint32_t              n_layers;   /* transformer layers only */
    uint32_t              tensor_count;
} RunnerState;

/* ══════════════════════════════════════════════════════════════════
 * tensor_get_fn
 *
 * Map layer_id (ModelIndex sequential id) → ggml_tensor*.
 *
 * ModelIndex orders tensors as they appear in .gguf:
 *   [0]  token_embd.weight
 *   [1]  blk.0.attn_q.weight
 *   ...  blk.N.* (attn_k, attn_v, attn_output, ffn_*, etc.)
 *   [last] output.weight / output_norm
 *
 * llama.cpp exposes tensors via llama_get_model_tensor(model, name).
 * We pre-built a name→id map in ModelIndex — just look up rec.name.
 *
 * Returns NULL for tensors that don't need per-layer decode
 * (e.g. non-layer metadata tensors — backend will skip decode).
 * ══════════════════════════════════════════════════════════════════ */
static struct ggml_tensor *my_tensor_getter(struct llama_context *ctx,
                                              uint32_t              layer_id,
                                              void                 *user_data)
{
    (void)layer_id;
    RunnerState *rs = (RunnerState *)user_data;

    /* We need the tensor name — stored in ModelIndex rec.             */
    /* Backend passes layer_id; caller retrieves rec from ModelIndex.  */
    /* Here we use a thin wrapper: user_data carries (rs, mi, id→rec). */

    /* NOTE: in production use pogls_model_index_get() in the wrapper  */
    /* struct below (TensorGetCtx). This stub shows the pattern.       */
    (void)rs;
    (void)ctx;
    return NULL;   /* replaced by TensorGetCtx version below */
}

/* ── richer callback context carrying both rs + mi ──────────────── */
typedef struct {
    RunnerState      *rs;
    const ModelIndex *mi;
} TensorGetCtx;

static struct ggml_tensor *tensor_getter_full(struct llama_context *ctx,
                                               uint32_t              layer_id,
                                               void                 *user_data)
{
    TensorGetCtx *tgc = (TensorGetCtx *)user_data;

    /* look up tensor name from ModelIndex */
    ModelLayerRecord rec;
    if (pogls_model_index_get(tgc->mi, layer_id, &rec) != MIDX_OK)
        return NULL;

    /* ask llama.cpp for tensor by name */
    struct ggml_tensor *t =
        llama_get_model_tensor(llama_get_model(ctx),
                               (const char *)rec.name);
    return t;   /* NULL = not found / skip */
}

/* ══════════════════════════════════════════════════════════════════
 * layer_fn
 *
 * Single-layer decode: push tensor data → run one decode step.
 *
 * llama_decode() normally runs the full forward pass.
 * For layer-by-layer streaming we use llama_set_n_threads() +
 * a single-token batch per layer decode cycle.
 *
 * If llama.cpp version supports llama_decode_layer() use that.
 * Fallback: full llama_decode() with batch size 1 is still fast
 * because RAM stays bounded — swap eliminated = t/s gain.
 * ══════════════════════════════════════════════════════════════════ */
static int my_layer_decoder(struct llama_context *ctx,
                              struct ggml_tensor   *tensor,
                              uint32_t              layer_id,
                              void                 *user_data)
{
    (void)tensor;   /* data already set via ggml_backend_tensor_set   */
    (void)user_data;

    /* Only decode on transformer layers, not embeddings/norms.       */
    /* Caller can refine this filter via tensor_getter returning NULL. */
    /* Here we decode every non-NULL tensor layer.                    */

    /* Build minimal single-token batch */
    struct llama_batch batch = llama_batch_init(1, 0, 1);
    batch.n_tokens      = 1;
    batch.token[0]      = 0;          /* dummy token id               */
    batch.pos[0]        = (int32_t)layer_id;
    batch.n_seq_id[0]   = 1;
    batch.seq_id[0][0]  = 0;
    batch.logits[0]     = 0;

    int rc = llama_decode(ctx, batch);
    llama_batch_free(batch);

    return rc;   /* 0 = OK */
}

/* ══════════════════════════════════════════════════════════════════
 * progress_cb — stderr progress bar
 * ══════════════════════════════════════════════════════════════════ */
static int progress_cb(uint32_t                layer_id,
                        uint32_t                total,
                        const ModelLayerRecord *rec,
                        void                   *user_data)
{
    (void)user_data;
    if (layer_id % 8 == 0 || layer_id == total - 1) {
        uint32_t pct = (layer_id + 1) * 100u / total;
        fprintf(stderr, "\r[pogls] streaming %3u%%  layer %4u/%-4u  %s   ",
                pct, layer_id + 1, total, (const char *)rec->name);
        fflush(stderr);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * pogls_runner_main()
 *
 * Full pipeline:
 *   1. preflight  → required window size
 *   2. alloc window
 *   3. llama_model_load (mmap=false, no_alloc=true variant)
 *   4. backend_run → stream weights layer by layer
 *   5. inference benchmark (N tokens)
 *   6. report t/s
 * ══════════════════════════════════════════════════════════════════ */
int pogls_runner_main(const char *gguf_path, int n_bench_tokens)
{
    int exit_rc = 0;
    uint8_t *window = NULL;

    /* ── 1. preflight ─────────────────────────────────────────── */
    uint64_t win_sz   = 0;
    uint32_t n_layers = 0;

    int pf = pogls_backend_preflight(gguf_path, "qwen3-1.7b",
                                      0, &win_sz, &n_layers);
    if (pf != POGLS_BACK_ERR_WINSZ && pf != POGLS_BACK_OK) {
        fprintf(stderr, "[runner] preflight parse failed\n");
        return 1;
    }

    fprintf(stderr, "[runner] %u layers, max layer = %llu KB\n",
            n_layers, (unsigned long long)(win_sz / 1024));

    /* ── 2. alloc window ──────────────────────────────────────── */
    window = (uint8_t *)malloc((size_t)win_sz);
    if (!window) {
        fprintf(stderr, "[runner] malloc failed (need %llu MB)\n",
                (unsigned long long)(win_sz / (1024*1024)));
        return 1;
    }

    /* ── 3. load model (no weight preload) ────────────────────── */
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = RUNNER_N_GPU_LAYERS;
    mparams.use_mmap     = false;   /* we stream manually — no mmap  */

    struct llama_model *model = llama_model_load_from_file(gguf_path,
                                                            mparams);
    if (!model) {
        fprintf(stderr, "[runner] llama_model_load failed\n");
        exit_rc = 1; goto done;
    }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = RUNNER_CTX_SIZE;
    cparams.n_threads = RUNNER_THREADS;

    struct llama_context *ctx = llama_new_context_with_model(model, cparams);
    if (!ctx) {
        fprintf(stderr, "[runner] llama_new_context failed\n");
        exit_rc = 1; goto done_model;
    }

    /* ── 4. build shared ModelIndex for tensor_getter ─────────── */
    static ModelIndex mi_shared;
    memset(&mi_shared, 0, sizeof(mi_shared));
    gguf_parse_to_model_index(gguf_path, &mi_shared, "qwen3-1.7b");

    RunnerState rs = { .model = model, .ctx = ctx,
                       .n_layers = n_layers, .tensor_count = n_layers };

    TensorGetCtx tgc = { .rs = &rs, .mi = &mi_shared };

    /* ── 5. stream weights ────────────────────────────────────── */
    PoglsBackendCfg cfg = {
        .gguf_path       = gguf_path,
        .model_name      = "qwen3-1.7b",
        .llama_ctx       = ctx,
        .ram_window      = window,
        .ram_window_size = win_sz,
        .tensor_get_fn   = tensor_getter_full,
        .layer_fn        = my_layer_decoder,
        .fn_user_data    = &tgc,
        .layer_cb        = progress_cb,
        .cb_user_data    = NULL,
        .layer_start     = 0,
        .layer_end       = 0,   /* all */
    };

    PoglsBackendStats st;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int run_rc = pogls_backend_run(&cfg, &st);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "\n");

    if (run_rc != POGLS_BACK_OK) {
        fprintf(stderr, "[runner] backend_run failed rc=%d\n", run_rc);
        exit_rc = 1; goto done_ctx;
    }

    double stream_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                     + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fprintf(stderr, "[runner] stream done: %.0f ms  ", stream_ms);
    pogls_backend_print_stats(&st);

    /* ── 6. inference bench ───────────────────────────────────── */
    if (n_bench_tokens > 0) {
        fprintf(stderr, "[runner] bench: %d tokens...\n", n_bench_tokens);

        /* warm-up: 1 token */
        struct llama_batch batch = llama_batch_init(1, 0, 1);
        batch.n_tokens = 1; batch.token[0] = 1;
        batch.pos[0] = 0; batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0; batch.logits[0] = 1;
        llama_decode(ctx, batch);
        llama_batch_free(batch);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < n_bench_tokens; i++) {
            struct llama_batch b = llama_batch_init(1, 0, 1);
            b.n_tokens = 1; b.token[0] = (llama_token)(i % 1000 + 2);
            b.pos[0] = (int32_t)(i + 1);
            b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
            llama_decode(ctx, b);
            llama_batch_free(b);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        double inf_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                      + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        double tps = n_bench_tokens / (inf_ms / 1000.0);
        fprintf(stderr, "[runner] %.2f t/s  (%d tokens / %.0f ms)\n",
                tps, n_bench_tokens, inf_ms);
    }

done_ctx:
    llama_free(ctx);
done_model:
    llama_model_free(model);
done:
    free(window);
    return exit_rc;
}

/* ══════════════════════════════════════════════════════════════════
 * main
 * Usage: ./pogls_runner <model.gguf> [n_bench_tokens=32]
 * ══════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <model.gguf> [bench_tokens]\n", argv[0]);
        return 1;
    }
    int bench = (argc >= 3) ? atoi(argv[2]) : 32;
    return pogls_runner_main(argv[1], bench);
}
