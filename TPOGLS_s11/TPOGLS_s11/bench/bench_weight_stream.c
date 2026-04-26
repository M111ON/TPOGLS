/*
 * bench_weight_stream.c — S10: ModelIndex + WeightStream throughput
 *
 * Pipeline: raw → scan_buf → compress → FileIndex → ModelIndex → WeightStream
 * Measures: sequential layer load, partial layer access, ram budget check
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "core/pogls_platform.h"
#include "core/pogls_qrpn_phaseE.h"
#include "pogls_compress.h"
#include "pogls_scanner.h"
#include "pogls_file_index.h"
#include "pogls_reconstruct.h"
#include "pogls_stream.h"
#include "pogls_model_index.h"
#include "pogls_weight_stream.h"

#define LAYERS       32
#define CHUNKS_PER   16                          /* 16 × 64B = 1 KB / layer */
#define LAYER_BYTES  (CHUNKS_PER * DIAMOND_BLOCK_SIZE)
#define TOTAL_BYTES  (LAYERS * LAYER_BYTES)
#define MAX_CHUNKS   (LAYERS * CHUNKS_PER)
#define COMP_BUF_SZ  (MAX_CHUNKS * (DIAMOND_BLOCK_SIZE + 64))
#define FIDX_BUF_SZ  FILE_INDEX_BUF_SIZE(MAX_CHUNKS)

/* ── EncodeCtx (mirrors test_e2e pattern) ── */
typedef struct {
    uint8_t        fidx_buf[FIDX_BUF_SZ];
    uint8_t        comp_buf[COMP_BUF_SZ];
    size_t         comp_pos;
    FileIndex      fi;
    qrpn_ctx_t     qrpn;
    const uint8_t *src;
    size_t         src_len;
} EncodeCtx;

static EncodeCtx g_ec;  /* static: too large for stack */

static void _encode_cb(const ScanEntry *e, void *user)
{
    EncodeCtx *ec = (EncodeCtx *)user;
    DiamondBlock blk;
    memset(&blk, 0, sizeof(blk));
    size_t off  = (size_t)e->offset;
    size_t copy = ((ec->src_len - off) > 64) ? 64 : (ec->src_len - off);
    memcpy(&blk, ec->src + off, copy);

    uint8_t tmp[DIAMOND_BLOCK_SIZE + 64];
    int clen = pogls_compress_block(&blk, tmp, (int)sizeof(tmp), &ec->qrpn);
    if (clen <= 0) return;

    CompressHeader hdr;
    memcpy(&hdr, tmp, sizeof(CompressHeader));
    uint64_t comp_offset = (uint64_t)ec->comp_pos;
    memcpy(ec->comp_buf + ec->comp_pos, tmp, (size_t)clen);
    ec->comp_pos += (size_t)clen;
    file_index_add(&ec->fi, e, &hdr, comp_offset, &ec->qrpn);
}

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

/* inference cb — accumulate to prevent dead-code elim */
static volatile uint64_t g_acc = 0;
static int infer_cb(const WeightLayerView *v, void *user) {
    (void)user;
    for (uint32_t i = 0; i < v->size; i++) g_acc += v->data[i];
    return 0;
}

int main(void)
{
    printf("═══════════════════════════════════════════════════════\n");
    printf("  POGLS WeightStream Bench  (%d layers × %dB = %dKB)\n",
           LAYERS, LAYER_BYTES, TOTAL_BYTES / 1024);
    printf("═══════════════════════════════════════════════════════\n");

    /* ── 1. synthetic weights (PHI-seeded) ── */
    uint8_t *raw = malloc(TOTAL_BYTES);
    for (int i = 0; i < TOTAL_BYTES; i++)
        raw[i] = (uint8_t)((i * 1696631) >> 12);

    /* ── 2. encode via full pipeline ── */
    EncodeCtx *ec = &g_ec;
    memset(ec, 0, sizeof(*ec));
    ec->src     = raw;
    ec->src_len = TOTAL_BYTES;
    qrpn_ctx_init(&ec->qrpn, 8);

    uint32_t n = scan_buf_count(TOTAL_BYTES);
    file_index_init(&ec->fi, ec->fidx_buf, sizeof(ec->fidx_buf),
                    n, (const uint8_t *)"ws_bench", 8,
                    (uint64_t)TOTAL_BYTES, FTYPE_BINARY);
    ScanConfig cfg = {0};
    scan_buf(raw, TOTAL_BYTES, _encode_cb, ec, &cfg);
    file_index_seal(&ec->fi);
    printf("  encode done: %zu chunks, comp_buf=%zu B\n", (size_t)n, ec->comp_pos);

    /* ── 3. ModelIndex: 1 layer = 1 KB ── */
    ModelIndex mi;
    pogls_model_index_init(&mi, "ws_bench",
                           (uint64_t)MAX_CHUNKS, (uint64_t)TOTAL_BYTES);
    for (int i = 0; i < LAYERS; i++) {
        uint64_t bs = (uint64_t)i * LAYER_BYTES;
        pogls_model_index_add(&mi, (uint32_t)i,
                              MIDX_LAYER_ATTN, "layer",
                              bs, bs + LAYER_BYTES);
    }
    int seal_r = pogls_model_index_seal(&mi);
    printf("  model_index seal: %s\n", seal_r == 0 ? "OK" : "FAIL");

    /* ── 4. ReconContext ── */
    ReconContext rc;
    qrpn_ctx_init(&ec->qrpn, 8);
    if (pogls_reconstruct_init(&rc, &ec->fi, ec->comp_buf,
                               (uint64_t)ec->comp_pos, &ec->qrpn) != RECON_OK) {
        printf("  reconstruct_init FAIL\n"); free(raw); return 1;
    }

    /* ── 5. WeightStream sequential run ── */
    size_t ram_budget = (size_t)LAYER_BYTES * 4;  /* 4-layer window */
    uint8_t *ram_win  = malloc(ram_budget);
    WeightStream ws;
    pogls_ws_init(&ws, &mi, &rc, ram_win, ram_budget);

    uint64_t t0 = now_ns();
    int run_r = pogls_ws_run(&ws, infer_cb, NULL);
    uint64_t dt = now_ns() - t0;

    double mb   = TOTAL_BYTES / 1e6;
    double mbps = mb / (dt / 1e9);
    printf("\n── Sequential pogls_ws_run ──\n");
    printf("  result      : %s\n", run_r == WS_DONE ? "OK" : "FAIL");
    printf("  layers done : %u / %d\n", ws.layers_done, LAYERS);
    printf("  time        : %.3f ms\n", dt / 1e6);
    printf("  throughput  : %.1f MB/s\n", mbps);
    printf("  checksum    : 0x%llx\n", (unsigned long long)g_acc);

    /* ── 6. Partial single-layer access ── */
    printf("\n── Partial: load layer 16 only ──\n");
    WeightLayerView v;
    pogls_ws_clear(&ws);
    int load_r = pogls_ws_load_layer(&ws, 16, &v);
    int ok = (load_r == WS_OK && v.size == LAYER_BYTES && v.data != NULL);
    printf("  [%s] load_r=%d  size=%u  ptr=%s\n",
           ok ? "PASS" : "FAIL", load_r, v.size, v.data ? "ok" : "NULL");
    pogls_ws_clear(&ws);

    /* ── 7. Budget check ── */
    printf("\n── Budget ──\n");
    int fits = pogls_ws_fits_budget(&ws, 0);   /* normal budget → should fit */

    /* oversize: tiny budget ws = 1 byte → layer must not fit */
    uint8_t tiny_buf[1];
    WeightStream ws_tiny;
    pogls_ws_init(&ws_tiny, &mi, &rc, tiny_buf, 1);
    int oversize = !pogls_ws_fits_budget(&ws_tiny, 0);
    printf("  [%s] layer fits budget\n", fits    ? "PASS" : "FAIL");
    printf("  [%s] oversized rejected\n", oversize ? "PASS" : "FAIL");

    printf("═══════════════════════════════════════════════════════\n");
    free(raw); free(ram_win);
    return (run_r == WS_DONE && ok && fits && oversize) ? 0 : 1;
}
