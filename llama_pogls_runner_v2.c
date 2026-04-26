/*
 * llama_pogls_runner_v2.c — S83-B8
 *
 * B8 change: on-demand streaming (zero preload buffer)
 *   - B7 preloaded all 1743 MB → llama.cpp allocated another ~2.8 GB → OOM
 *   - B8 callback does fread directly into t->data (already allocated by llama.cpp)
 *   - RAM footprint = model buffer only, no double allocation
 *
 * Flow:
 *   gguf_init_from_file()           → metadata only
 *   tmap_build()                    → name→offset hashmap (no weight buffer)
 *   llama_model_init_from_user()    → fires set_tensor_cb per tensor
 *     set_tensor_cb_b8()            → fread direct into t->data
 *   llama_init_from_model()         → context
 *   llama_decode() × N              → bench
 */

/* ── Win32 timing shim ──────────────────────────────────────────────
 * Must come before all other includes — avoids timespec redefinition
 * ──────────────────────────────────────────────────────────────────*/
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define CLOCK_MONOTONIC 0
  typedef struct { long tv_sec; long tv_nsec; } PoglsTime;
  static inline int clock_gettime(int _clk, PoglsTime *ts) {
      LARGE_INTEGER freq, cnt;
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&cnt);
      ts->tv_sec  = (long)(cnt.QuadPart / freq.QuadPart);
      ts->tv_nsec = (long)((cnt.QuadPart % freq.QuadPart)
                           * 1000000000LL / freq.QuadPart);
      (void)_clk; return 0;
  }
  #define fseek_64(fp,off,w) _fseeki64((fp),(__int64)(off),(w))
#else
  #include <time.h>
  typedef struct timespec PoglsTime;
  #define fseek_64(fp,off,w) fseeko((fp),(off_t)(off),(w))
#endif
/* ──────────────────────────────────────────────────────────────────*/

#include "llama.h"
#include "ggml-backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── tuning ──────────────────────────────────────────────────────── */
#define RUNNER_CTX_SIZE     2048u
#define RUNNER_N_GPU_LAYERS 0
#define RUNNER_THREADS      4

/* ══════════════════════════════════════════════════════════════════
 * TensorMap — name→file_offset hashmap only, zero weight buffer
 * ══════════════════════════════════════════════════════════════════ */
#define TMAP_MAX  1024
#define TMAP_HASH 512

typedef struct {
    char    name[128];
    size_t  file_offset;
    size_t  size;
} TMapEntry;

typedef struct {
    TMapEntry slots[TMAP_MAX];
    uint32_t  count;
} TensorMap;

static uint32_t tmap_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h;
}

static void tmap_build(TensorMap *tm, struct gguf_context *gctx,
                       size_t data_offset)
{
    memset(tm, 0, sizeof(*tm));
    int64_t n = gguf_get_n_tensors(gctx);

    for (int64_t i = 0; i < n && tm->count < TMAP_MAX; i++) {
        const char *name = gguf_get_tensor_name(gctx, i);
        if (!name) continue;

        TMapEntry e;
        memset(&e, 0, sizeof(e));
        strncpy(e.name, name, 127);
        e.file_offset = data_offset + gguf_get_tensor_offset(gctx, i);
        e.size        = gguf_get_tensor_size(gctx, i);

        uint32_t idx = tmap_hash(name) & (TMAP_HASH - 1);
        while (tm->slots[idx].name[0])
            idx = (idx + 1) & (TMAP_HASH - 1);
        tm->slots[idx] = e;
        tm->count++;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * B8 callback — on-demand fread directly into t->data
 * llama.cpp already allocated t->data — we just fill it
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    const TensorMap *tmap;
    FILE            *fp;
    uint64_t         tensors_set;
    uint64_t         bytes_read;
} TensorSetCtxB8;

static void set_tensor_cb_b8(struct ggml_tensor *t, void *userdata)
{
    TensorSetCtxB8 *tsc = (TensorSetCtxB8 *)userdata;
    if (!t || !t->name[0] || !t->data) return;

    uint32_t idx = tmap_hash(t->name) & (TMAP_HASH - 1);
    for (int probe = 0; probe < TMAP_HASH; probe++) {
        const TMapEntry *e = &tsc->tmap->slots[idx];
        if (!e->name[0]) return;
        if (strcmp(e->name, t->name) == 0) {
            if (fseek_64(tsc->fp, e->file_offset, SEEK_SET) != 0) {
                fprintf(stderr, "[cb] fseek failed: %s\n", t->name);
                return;
            }
            size_t got = fread(t->data, 1, e->size, tsc->fp);
            if (got != e->size)
                fprintf(stderr, "[cb] short read: %s (%zu/%zu)\n",
                        t->name, got, e->size);
            tsc->bytes_read  += got;
            tsc->tensors_set++;
            return;
        }
        idx = (idx + 1) & (TMAP_HASH - 1);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * pogls_runner_main
 * ══════════════════════════════════════════════════════════════════ */
int pogls_runner_main(const char *gguf_path, int n_bench_tokens)
{
    int rc = 0;

    /* 1. metadata only */
    struct gguf_init_params gparams = { .no_alloc = true, .ctx = NULL };
    struct gguf_context *gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) { fprintf(stderr, "[runner] gguf_init failed\n"); return 1; }

    int64_t n_tensors   = gguf_get_n_tensors(gctx);
    size_t  data_offset = gguf_get_data_offset(gctx);
    fprintf(stderr, "[runner] %lld tensors, data_offset=%zu\n",
            (long long)n_tensors, data_offset);

    /* 2. open file — stays open during model init */
    FILE *fp = fopen(gguf_path, "rb");
    if (!fp) {
        fprintf(stderr, "[runner] fopen failed\n");
        gguf_free(gctx); return 1;
    }

    /* 3. offset map only — zero weight malloc */
    static TensorMap tmap;
    tmap_build(&tmap, gctx, data_offset);
    fprintf(stderr, "[runner] tmap built: %u entries (zero preload)\n",
            tmap.count);

    TensorSetCtxB8 tsc = {
        .tmap        = &tmap,
        .fp          = fp,
        .tensors_set = 0,
        .bytes_read  = 0,
    };

    /* 4. model load — each tensor streamed on demand */
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = RUNNER_N_GPU_LAYERS;

    PoglsTime t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    struct llama_model *model = llama_model_init_from_user(
            gctx, set_tensor_cb_b8, &tsc, mparams);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    fclose(fp); fp = NULL;

    if (!model) {
        fprintf(stderr, "[runner] llama_model_init_from_user failed\n");
        rc = 1; goto done;
    }

    double load_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                   + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fprintf(stderr,
            "[runner] model loaded: %llu tensors  %llu MB  %.0f ms\n",
            (unsigned long long)tsc.tensors_set,
            (unsigned long long)(tsc.bytes_read >> 20),
            load_ms);

    /* 5. context */
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = RUNNER_CTX_SIZE;
    cparams.n_threads = RUNNER_THREADS;

    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        fprintf(stderr, "[runner] llama_init_from_model failed\n");
        rc = 1; goto done_model;
    }

    /* 6. bench */
    if (n_bench_tokens > 0) {
        fprintf(stderr, "[runner] bench %d tokens (CPU, %d threads)...\n",
                n_bench_tokens, RUNNER_THREADS);

        struct llama_batch b0 = llama_batch_init(1, 0, 1);
        b0.n_tokens = 1; b0.token[0] = 1; b0.pos[0] = 0;
        b0.n_seq_id[0] = 1; b0.seq_id[0][0] = 0; b0.logits[0] = 1;
        llama_decode(ctx, b0);
        llama_batch_free(b0);

        PoglsTime tb0, tb1;
        clock_gettime(CLOCK_MONOTONIC, &tb0);
        for (int i = 0; i < n_bench_tokens; i++) {
            struct llama_batch b = llama_batch_init(1, 0, 1);
            b.n_tokens     = 1;
            b.token[0]     = (llama_token)(i % 1000 + 2);
            b.pos[0]       = (int32_t)(i + 1);
            b.n_seq_id[0]  = 1;
            b.seq_id[0][0] = 0;
            b.logits[0]    = 1;
            llama_decode(ctx, b);
            llama_batch_free(b);
        }
        clock_gettime(CLOCK_MONOTONIC, &tb1);

        double inf_ms = (tb1.tv_sec - tb0.tv_sec) * 1000.0
                      + (tb1.tv_nsec - tb0.tv_nsec) / 1e6;
        fprintf(stderr, "[runner] %.2f t/s  (%d tokens / %.0f ms)\n",
                n_bench_tokens / (inf_ms / 1000.0),
                n_bench_tokens, inf_ms);
    }

    llama_free(ctx);
done_model:
    llama_model_free(model);
done:
    if (fp) fclose(fp);
    gguf_free(gctx);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <model.gguf> [bench_tokens=32]\n", argv[0]);
        return 1;
    }
    int bench = (argc >= 3) ? atoi(argv[2]) : 32;

    llama_backend_init();
    ggml_backend_load_all();
    int rc = pogls_runner_main(argv[1], bench);
    llama_backend_free();
    return rc;
}
