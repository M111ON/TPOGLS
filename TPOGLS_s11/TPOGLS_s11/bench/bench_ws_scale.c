/*
 * bench_ws_scale.c — S11-B: WeightStream scale stress
 *
 * FILE_INDEX_CHUNKS_MAX = 65536 (hard limit ~4MB per FileIndex)
 * Design: 4 segments × 64 layers × 64KB = 256 layers × 64KB = 16MB total
 *
 * Measures:
 *   1. Sequential ws_run per segment → aggregate throughput
 *   2. Degradation curve: vary layer_size (1KB → 32KB, single segment)
 *   3. Window pressure: ram_budget = N × layer_bytes
 *   4. Partial single-layer access (layer 127 = seg1 layer 63)
 *   5. Budget enforcement (exact / oversize)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "core/pogls_platform.h"
#include "core/pogls_qrpn_phaseE.h"
#include "pogls_compress.h"
#include "angular_mapper_v36.h"
/* stub — SCAN_CFG_WITH_ADDR not used */
POGLS_AngularAddress pogls_node_to_address(uint32_t n,
    uint8_t g, uint8_t w, uint32_t nb)
{ (void)n;(void)g;(void)w;(void)nb; POGLS_AngularAddress a={0}; return a; }
#include "pogls_scanner.h"
#include "pogls_file_index.h"
#include "pogls_reconstruct.h"
#include "pogls_stream.h"
#include "pogls_model_index.h"
#include "pogls_weight_stream.h"

/* ═══ CONFIG ════════════════════════════════════════════════════════
 * FILE_INDEX_CHUNKS_MAX = 65536 → max 64 layers × 1024 chunks/layer
 * Split 256 layers into 4 segments of 64 layers each.
 * ═════════════════════════════════════════════════════════════════ */
#define LAYERS_FULL   256
#define LAYER_KB      64
#define LAYER_BYTES   ((size_t)(LAYER_KB) * 1024)            /* 65536B */
#define TOTAL_BYTES   ((size_t)LAYERS_FULL * LAYER_BYTES)    /* 16MB   */
#define CHUNKS_PER    (LAYER_BYTES / DIAMOND_BLOCK_SIZE)     /* 1024   */
#define SEG_LAYERS    64                                      /* per FileIndex */
#define SEGS          (LAYERS_FULL / SEG_LAYERS)             /* 4      */
#define SEG_BYTES     ((size_t)SEG_LAYERS * LAYER_BYTES)     /* 4MB    */
#define SEG_CHUNKS    ((size_t)SEG_LAYERS * CHUNKS_PER)      /* 65536  */

/* ═══ PER-SEGMENT ENCODE STATE ══════════════════════════════════ */
typedef struct {
    uint8_t       *fidx_buf;
    uint8_t       *comp_buf;
    size_t         comp_cap;
    size_t         comp_pos;
    FileIndex      fi;
    qrpn_ctx_t     qrpn;
    const uint8_t *src;
    size_t         src_len;
} SegCtx;

static void _encode_cb(const ScanEntry *e, void *user)
{
    SegCtx *sc = (SegCtx *)user;
    DiamondBlock blk; memset(&blk, 0, sizeof(blk));
    size_t off  = (size_t)e->offset;
    size_t copy = (sc->src_len - off) > 64 ? 64 : (sc->src_len - off);
    memcpy(&blk, sc->src + off, copy);
    uint8_t tmp[DIAMOND_BLOCK_SIZE + 64];
    int clen = pogls_compress_block(&blk, tmp, (int)sizeof(tmp), &sc->qrpn);
    if (clen <= 0) return;
    CompressHeader hdr; memcpy(&hdr, tmp, sizeof(hdr));
    uint64_t co = (uint64_t)sc->comp_pos;
    if (sc->comp_pos + (size_t)clen <= sc->comp_cap) {
        memcpy(sc->comp_buf + sc->comp_pos, tmp, (size_t)clen);
        sc->comp_pos += (size_t)clen;
    }
    file_index_add(&sc->fi, e, &hdr, co, &sc->qrpn);
}

static int seg_encode(SegCtx *sc, const uint8_t *src, size_t len)
{
    size_t fidx_sz  = FILE_INDEX_BUF_SIZE(SEG_CHUNKS);
    size_t comp_cap = SEG_CHUNKS * (DIAMOND_BLOCK_SIZE + 64);
    sc->fidx_buf = malloc(fidx_sz);
    sc->comp_buf = malloc(comp_cap);
    if (!sc->fidx_buf || !sc->comp_buf) return -1;
    sc->comp_cap = comp_cap;
    sc->comp_pos = 0;
    sc->src      = src;
    sc->src_len  = len;
    qrpn_ctx_init(&sc->qrpn, 8);
    uint32_t n   = scan_buf_count(len);
    file_index_init(&sc->fi, sc->fidx_buf, fidx_sz, n,
                    (const uint8_t *)"seg", 3, (uint64_t)len, FTYPE_BINARY);
    ScanConfig cfg = {0};
    scan_buf(src, len, _encode_cb, sc, &cfg);
    file_index_seal(&sc->fi);
    return 0;
}

static void seg_free(SegCtx *sc)
{
    free(sc->fidx_buf); sc->fidx_buf = NULL;
    free(sc->comp_buf); sc->comp_buf = NULL;
}

/* ═══ TIMING ══════════════════════════════════════════════════════ */
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

/* ═══ INFER CB ════════════════════════════════════════════════════ */
static volatile uint64_t g_acc = 0;
static int infer_cb(const WeightLayerView *v, void *user) {
    (void)user;
    for (uint32_t i = 0; i < v->size; i++) g_acc += v->data[i];
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  POGLS WeightStream Scale Bench  S11-B                  ║\n");
    printf("║  %d layers × %dKB = %zuMB  (%d segs × %d layers)%*s║\n",
           LAYERS_FULL, LAYER_KB, TOTAL_BYTES/(1024*1024),
           SEGS, SEG_LAYERS, 10, "");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── synthetic weights ── */
    uint8_t *raw = malloc(TOTAL_BYTES);
    if (!raw) { fprintf(stderr, "malloc TOTAL_BYTES failed\n"); return 1; }
    for (size_t i = 0; i < TOTAL_BYTES; i++)
        raw[i] = (uint8_t)((i * 1696631ULL) >> 12);

    /* ── encode 4 segments ── */
    printf("── Encoding 4 × 4MB segments ...\n");
    SegCtx segs[SEGS];
    memset(segs, 0, sizeof(segs));
    uint64_t t_enc0 = now_ns();
    for (int s = 0; s < SEGS; s++) {
        if (seg_encode(&segs[s], raw + s * SEG_BYTES, SEG_BYTES) != 0) {
            fprintf(stderr, "seg_encode[%d] failed\n", s);
            return 1;
        }
        printf("   seg[%d]: %zu chunks  comp=%zuKB\n",
               s, (size_t)scan_buf_count(SEG_BYTES),
               segs[s].comp_pos / 1024);
    }
    uint64_t t_enc1 = now_ns();
    printf("   encode total: %.0f MB/s\n\n",
           (TOTAL_BYTES / 1e6) / ((t_enc1 - t_enc0) / 1e9));

    /* ══════════════════════════════════════════════════════════════
     * 1. Sequential ws_run — all 4 segments
     * ══════════════════════════════════════════════════════════════ */
    printf("── 1. Sequential ws_run (4 segs × 64 layers × 64KB) ──\n");
    {
        uint64_t total_dt = 0;
        uint32_t total_layers = 0;
        g_acc = 0;

        for (int s = 0; s < SEGS; s++) {
            ModelIndex   mi; ReconContext rc;
            /* model index: SEG_LAYERS layers per segment */
            pogls_model_index_init(&mi, "seg",
                (uint64_t)SEG_CHUNKS, (uint64_t)SEG_BYTES);
            for (int i = 0; i < SEG_LAYERS; i++) {
                uint64_t bs = (uint64_t)i * LAYER_BYTES;
                pogls_model_index_add(&mi, (uint32_t)i,
                    MIDX_LAYER_ATTN, "layer", bs, bs + LAYER_BYTES);
            }
            pogls_model_index_seal(&mi);
            qrpn_ctx_init(&segs[s].qrpn, 8);
            pogls_reconstruct_init(&rc, &segs[s].fi, segs[s].comp_buf,
                                   (uint64_t)segs[s].comp_pos, &segs[s].qrpn);

            size_t   rbsz = LAYER_BYTES * 4;
            uint8_t *rbuf = malloc(rbsz);
            WeightStream ws;
            pogls_ws_init(&ws, &mi, &rc, rbuf, rbsz);

            uint64_t t0 = now_ns();
            int rr = pogls_ws_run(&ws, infer_cb, NULL);
            total_dt     += now_ns() - t0;
            total_layers += ws.layers_done;

            printf("   seg[%d]: %s  layers=%u  %.1f MB/s\n",
                   s, rr == WS_DONE ? "OK" : "FAIL",
                   ws.layers_done,
                   (SEG_BYTES / 1e6) / ((now_ns() - t0 + 1) / 1e9));
            free(rbuf);
        }

        printf("   ──────────────────────────────────────────────\n");
        printf("   TOTAL  layers=%u  time=%.1f ms  throughput=%.1f MB/s\n",
               total_layers,
               total_dt / 1e6,
               (TOTAL_BYTES / 1e6) / (total_dt / 1e9));
        printf("   checksum: 0x%llx\n", (unsigned long long)g_acc);
    }

    /* ══════════════════════════════════════════════════════════════
     * 2. Degradation curve: vary layer_size (single segment, 64 layers)
     * ══════════════════════════════════════════════════════════════ */
    printf("\n── 2. Degradation curve (64 layers, vary KB/layer) ──\n");
    printf("  %-10s  %-10s  %-14s  %s\n",
           "layer_KB", "total_MB", "throughput", "result");
    printf("  %-10s  %-10s  %-14s  %s\n",
           "────────","────────","──────────────","──────");
    {
        size_t sizes_kb[] = {1, 2, 4, 8, 16, 32};
        int    n_sizes    = (int)(sizeof(sizes_kb)/sizeof(sizes_kb[0]));

        for (int si = 0; si < n_sizes; si++) {
            size_t lkb    = sizes_kb[si];
            size_t lbytes = lkb * 1024;
            int    layers = 64;
            size_t chunks = lbytes / DIAMOND_BLOCK_SIZE;
            size_t tbytes = (size_t)layers * lbytes;

            /* must fit FILE_INDEX_CHUNKS_MAX */
            if ((size_t)layers * chunks > 65536) {
                printf("  %-10zu  SKIP (exceeds CHUNKS_MAX)\n", lkb);
                continue;
            }

            SegCtx sc; memset(&sc, 0, sizeof(sc));
            size_t fidx_sz  = FILE_INDEX_BUF_SIZE((size_t)layers * chunks);
            size_t comp_cap = (size_t)layers * chunks * (DIAMOND_BLOCK_SIZE + 64);
            sc.fidx_buf = malloc(fidx_sz);
            sc.comp_buf = malloc(comp_cap);
            sc.comp_cap = comp_cap;
            sc.src      = raw;
            sc.src_len  = tbytes;
            qrpn_ctx_init(&sc.qrpn, 8);
            uint32_t nc = scan_buf_count(tbytes);
            file_index_init(&sc.fi, sc.fidx_buf, fidx_sz, nc,
                            (const uint8_t *)"crv", 3,
                            (uint64_t)tbytes, FTYPE_BINARY);
            ScanConfig cfg = {0};
            scan_buf(raw, tbytes, _encode_cb, &sc, &cfg);
            file_index_seal(&sc.fi);

            ModelIndex mi; ReconContext rc;
            pogls_model_index_init(&mi, "crv",
                (uint64_t)nc, (uint64_t)tbytes);
            for (int i = 0; i < layers; i++) {
                uint64_t bs = (uint64_t)i * lbytes;
                pogls_model_index_add(&mi, (uint32_t)i,
                    MIDX_LAYER_ATTN, "layer", bs, bs + lbytes);
            }
            pogls_model_index_seal(&mi);
            qrpn_ctx_init(&sc.qrpn, 8);
            pogls_reconstruct_init(&rc, &sc.fi, sc.comp_buf,
                                   (uint64_t)sc.comp_pos, &sc.qrpn);

            size_t   rbsz = lbytes * 2;
            uint8_t *rbuf = malloc(rbsz);
            WeightStream ws;
            pogls_ws_init(&ws, &mi, &rc, rbuf, rbsz);
            g_acc = 0;
            uint64_t t0 = now_ns();
            int rr = pogls_ws_run(&ws, infer_cb, NULL);
            uint64_t dt = now_ns() - t0;

            printf("  %-10zu  %-10.2f  %-10.1f MB/s  %s\n",
                   lkb, tbytes / 1e6,
                   (tbytes / 1e6) / (dt / 1e9),
                   rr == WS_DONE ? "OK" : "FAIL");

            free(rbuf); seg_free(&sc);
        }
    }

    /* ══════════════════════════════════════════════════════════════
     * 3. Window pressure: vary ram_budget = N × layer_bytes (seg0)
     * ══════════════════════════════════════════════════════════════ */
    printf("\n── 3. Window pressure (64 layers × 64KB, vary window) ──\n");
    printf("  %-14s  %-14s  %s\n", "win_layers", "MB/s", "result");
    printf("  %-14s  %-14s  %s\n", "──────────","──────","──────");
    {
        int wins[] = {1, 2, 4, 8, 16, 32, 64};
        int nw     = (int)(sizeof(wins)/sizeof(wins[0]));

        for (int wi = 0; wi < nw; wi++) {
            ModelIndex mi; ReconContext rc;
            pogls_model_index_init(&mi, "win",
                (uint64_t)SEG_CHUNKS, (uint64_t)SEG_BYTES);
            for (int i = 0; i < SEG_LAYERS; i++) {
                uint64_t bs = (uint64_t)i * LAYER_BYTES;
                pogls_model_index_add(&mi, (uint32_t)i,
                    MIDX_LAYER_ATTN, "layer", bs, bs + LAYER_BYTES);
            }
            pogls_model_index_seal(&mi);
            qrpn_ctx_init(&segs[0].qrpn, 8);
            ReconContext rc0;
            pogls_reconstruct_init(&rc0, &segs[0].fi, segs[0].comp_buf,
                                   (uint64_t)segs[0].comp_pos, &segs[0].qrpn);
            (void)rc;

            size_t   rbsz = (size_t)wins[wi] * LAYER_BYTES;
            uint8_t *rbuf = malloc(rbsz);
            WeightStream ws;
            pogls_ws_init(&ws, &mi, &rc0, rbuf, rbsz);
            g_acc = 0;
            uint64_t t0 = now_ns();
            int rr = pogls_ws_run(&ws, infer_cb, NULL);
            uint64_t dt = now_ns() - t0;

            printf("  %-14d  %-10.1f MB/s  %s\n",
                   wins[wi],
                   (SEG_BYTES / 1e6) / (dt / 1e9),
                   rr == WS_DONE ? "OK" : "FAIL");
            free(rbuf);
        }
    }

    /* ══════════════════════════════════════════════════════════════
     * 4. Partial: load single layer (layer 31 from seg0)
     * ══════════════════════════════════════════════════════════════ */
    printf("\n── 4. Partial: load layer 31 (seg0) ──\n");
    {
        ModelIndex mi; ReconContext rc;
        pogls_model_index_init(&mi, "par",
            (uint64_t)SEG_CHUNKS, (uint64_t)SEG_BYTES);
        for (int i = 0; i < SEG_LAYERS; i++) {
            uint64_t bs = (uint64_t)i * LAYER_BYTES;
            pogls_model_index_add(&mi, (uint32_t)i,
                MIDX_LAYER_ATTN, "layer", bs, bs + LAYER_BYTES);
        }
        pogls_model_index_seal(&mi);
        qrpn_ctx_init(&segs[0].qrpn, 8);
        pogls_reconstruct_init(&rc, &segs[0].fi, segs[0].comp_buf,
                               (uint64_t)segs[0].comp_pos, &segs[0].qrpn);

        uint8_t *rbuf = malloc(LAYER_BYTES * 2);
        WeightStream ws;
        pogls_ws_init(&ws, &mi, &rc, rbuf, LAYER_BYTES * 2);
        WeightLayerView v;
        uint64_t t0 = now_ns();
        int lr = pogls_ws_load_layer(&ws, 31, &v);
        uint64_t dt = now_ns() - t0;
        int ok = (lr == WS_OK && v.size == LAYER_BYTES && v.data != NULL);
        printf("  [%s] load_r=%d  size=%zuB  time=%.2fms\n",
               ok ? "PASS" : "FAIL", lr, (size_t)v.size, dt / 1e6);
        pogls_ws_clear(&ws);
        free(rbuf);
    }

    /* ══════════════════════════════════════════════════════════════
     * 5. Budget enforcement
     * ══════════════════════════════════════════════════════════════ */
    printf("\n── 5. Budget enforcement ──\n");
    {
        ModelIndex mi; ReconContext rc;
        pogls_model_index_init(&mi, "bud",
            (uint64_t)SEG_CHUNKS, (uint64_t)SEG_BYTES);
        for (int i = 0; i < SEG_LAYERS; i++) {
            uint64_t bs = (uint64_t)i * LAYER_BYTES;
            pogls_model_index_add(&mi, (uint32_t)i,
                MIDX_LAYER_ATTN, "layer", bs, bs + LAYER_BYTES);
        }
        pogls_model_index_seal(&mi);
        qrpn_ctx_init(&segs[0].qrpn, 8);
        pogls_reconstruct_init(&rc, &segs[0].fi, segs[0].comp_buf,
                               (uint64_t)segs[0].comp_pos, &segs[0].qrpn);

        uint8_t *exact = malloc(LAYER_BYTES);
        WeightStream ws_ok;
        pogls_ws_init(&ws_ok, &mi, &rc, exact, LAYER_BYTES);
        int fits = pogls_ws_fits_budget(&ws_ok, 0);
        printf("  [%s] exact budget (64KB) fits\n", fits ? "PASS" : "FAIL");
        free(exact);

        uint8_t tiny[1];
        WeightStream ws_tiny;
        pogls_ws_init(&ws_tiny, &mi, &rc, tiny, 1);
        int over = !pogls_ws_fits_budget(&ws_tiny, 0);
        printf("  [%s] 1-byte budget rejected\n", over ? "PASS" : "FAIL");
    }

    /* ── summary ── */
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  S11-B WeightStream scale bench complete     ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    for (int s = 0; s < SEGS; s++) seg_free(&segs[s]);
    free(raw);
    return 0;
}
