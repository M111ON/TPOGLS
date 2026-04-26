/*
 * kv_bridge_gpu.h — GPU flush adapter (Session 19)
 *
 * Bridges kvbridge_flush() callback → kv_insert_gpu() batch kernel.
 *
 * Usage (CUDA translation unit only):
 *   #include "kv_bridge_gpu.h"
 *   GpuKVCtx gctx; gpu_kv_ctx_init(&gctx);
 *   pogls_flush_gpu(ctx, gpu_kv_insert_cb, &gctx);
 *   gpu_kv_ctx_destroy(&gctx);
 *
 * Design:
 *   - kvb_gpu_insert_fn is called per-entry from kvbridge_flush()
 *   - We batch entries into a host staging buffer
 *   - gpu_kv_ctx_commit() sends one cudaMemcpy + kernel launch
 *   - Caller decides when to commit (e.g. every 1024 entries or on timer)
 *
 * Frozen constants (must match geo_kv_gpu.cu):
 *   KVB_GPU_CAP = 65536
 *   MAX_PROBE   = 128
 */

#ifndef KV_BRIDGE_GPU_H
#define KV_BRIDGE_GPU_H

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "kv_bridge.h"

/* pull GPU types + kernel launcher from geo_kv_gpu.cu */
#include "geo_kv_gpu.cu"

#define KVB_GPU_CAP      65536U
#define KVB_STAGE_CAP    4096          /* host staging buffer entries */

/* ── GPU context (one per PoglsCtx) ─────────────────────────────── */
typedef struct {
    KVPair   *d_table;                /* device hash table (persistent) */
    uint32_t *d_overflow;             /* device overflow counter        */
    KVPair   *h_stage;                /* host staging buffer            */
    uint32_t  stage_n;                /* entries staged, not yet sent   */
    uint64_t  total_sent;             /* cumulative entries → GPU       */
    uint32_t  total_overflow;         /* cumulative overflow count      */
} GpuKVCtx;

static inline int gpu_kv_ctx_init(GpuKVCtx *g) {
    memset(g, 0, sizeof(*g));
    g->h_stage = (KVPair *)malloc(KVB_STAGE_CAP * sizeof(KVPair));
    if (!g->h_stage) return -1;
    if (cudaMalloc(&g->d_table,    KVB_GPU_CAP * sizeof(KVPair)) != cudaSuccess) return -1;
    if (cudaMalloc(&g->d_overflow, sizeof(uint32_t))             != cudaSuccess) return -1;
    cudaMemset(g->d_table,    0, KVB_GPU_CAP * sizeof(KVPair));
    cudaMemset(g->d_overflow, 0, sizeof(uint32_t));
    return 0;
}

static inline void gpu_kv_ctx_destroy(GpuKVCtx *g) {
    if (g->d_table)    cudaFree(g->d_table);
    if (g->d_overflow) cudaFree(g->d_overflow);
    if (g->h_stage)    free(g->h_stage);
    memset(g, 0, sizeof(*g));
}

/* ── Commit staged entries → GPU kernel ─────────────────────────── */
static inline void gpu_kv_ctx_commit(GpuKVCtx *g) {
    if (g->stage_n == 0) return;

    KVPair *d_input;
    cudaMalloc(&d_input, g->stage_n * sizeof(KVPair));
    cudaMemcpy(d_input, g->h_stage, g->stage_n * sizeof(KVPair),
               cudaMemcpyHostToDevice);

    /* reset overflow counter for this batch */
    cudaMemset(g->d_overflow, 0, sizeof(uint32_t));

    kv_insert_gpu(g->d_table, KVB_GPU_CAP, d_input, g->stage_n, g->d_overflow);
    cudaDeviceSynchronize();

    uint32_t ov = 0;
    cudaMemcpy(&ov, g->d_overflow, sizeof(uint32_t), cudaMemcpyDeviceToHost);
    g->total_overflow += ov;
    g->total_sent     += g->stage_n;
    g->stage_n         = 0;

    cudaFree(d_input);
}

/* ── kvb_gpu_insert_fn callback: stage one entry ────────────────── */
/*
 * Passed to kvbridge_flush() as the callback.
 * Auto-commits when staging buffer is full.
 */
static void gpu_kv_insert_cb(uint64_t key, uint64_t val, void *ctx) {
    GpuKVCtx *g = (GpuKVCtx *)ctx;
    g->h_stage[g->stage_n++] = (KVPair){key, val};
    if (g->stage_n >= KVB_STAGE_CAP)
        gpu_kv_ctx_commit(g);
}

/* ── Stats ──────────────────────────────────────────────────────── */
static inline void gpu_kv_ctx_stats(const GpuKVCtx *g) {
    printf("[GPU-KV] sent=%" PRIu64 " overflow=%u staged=%u\n",
           g->total_sent, g->total_overflow, g->stage_n);
}

#endif /* KV_BRIDGE_GPU_H */
