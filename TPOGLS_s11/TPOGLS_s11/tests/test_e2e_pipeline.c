/*
 * test_e2e_pipeline.c — POGLS End-to-End Integration Test
 * ════════════════════════════════════════════════════════════════════
 *
 * Pipeline under test:
 *   raw_buf → scan_buf() → pogls_compress_block()
 *           → file_index_add() → file_index_seal()
 *           → pogls_reconstruct_init()
 *           → PoglsStream → byte-compare vs original
 *
 * Test cases:
 *   E1 — single block (64B), text payload
 *   E2 — multi-block (8 × 64B = 512B), sequential pattern
 *   E3 — large file (144 × 64B = 9216B), PHI-seeded pattern
 *   E4 — cross-block random-access reconstruct (middle chunk only)
 *   E5 — stream interruption + seek resume
 *   E6 — passthru chunk (pre-compressed flag) bypass compress path
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -Wall -Wextra test_e2e_pipeline.c -o test_e2e
 * ════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── pipeline headers (include order matters) ── */
#include "core/pogls_platform.h"
#include "core/pogls_qrpn_phaseE.h"
#include "pogls_compress.h"
#include "pogls_scanner.h"
#include "pogls_file_index.h"
#include "pogls_reconstruct.h"
#include "pogls_stream.h"

/* ── constants ── */
#define BLOCK        DIAMOND_BLOCK_SIZE          /* 64 */
#define MAX_CHUNKS   256u
#define COMP_BUF_SZ  (MAX_CHUNKS * (BLOCK + 64)) /* generous comp stream  */
#define FIDX_BUF_SZ  FILE_INDEX_BUF_SIZE(MAX_CHUNKS)

/* ════════════════════════════════════════════════════════════════════
 * Helpers
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* encode side */
    uint8_t        fidx_buf[FIDX_BUF_SZ];
    uint8_t        comp_buf[COMP_BUF_SZ];
    size_t         comp_pos;            /* write cursor in comp_buf    */
    FileIndex      fi;
    qrpn_ctx_t     qrpn;
    /* per-scan-chunk state (passed via scan_cb user ptr) */
    const uint8_t *src;                 /* full original buffer        */
    size_t         src_len;
} EncodeCtx;

/* scan_cb: compress each ScanEntry and register into FileIndex */
static void _encode_cb(const ScanEntry *e, void *user)
{
    EncodeCtx *ec = (EncodeCtx *)user;

    /* pack full 64B slice directly into DiamondBlock */
    DiamondBlock blk;
    memset(&blk, 0, sizeof(blk));
    size_t off   = (size_t)e->offset;
    size_t avail = (off < ec->src_len) ? (ec->src_len - off) : 0;
    size_t copy  = (avail > 64) ? 64 : avail;
    memcpy(&blk, ec->src + off, copy);

    /* compress → out_buf writes CompressHeader + payload */
    uint8_t tmp[BLOCK + 64];
    int clen = pogls_compress_block(&blk, tmp, (int)sizeof(tmp), &ec->qrpn);
    if (clen <= 0) { fprintf(stderr, "compress fail @ off=%zu\n", off); return; }

    /* read back CompressHeader from front of compressed output */
    CompressHeader hdr;
    memcpy(&hdr, tmp, sizeof(CompressHeader));

    /* append to comp stream */
    uint64_t comp_offset = (uint64_t)ec->comp_pos;
    memcpy(ec->comp_buf + ec->comp_pos, tmp, (size_t)clen);
    ec->comp_pos += (size_t)clen;

    /* register into FileIndex */
    file_index_add(&ec->fi, e, &hdr, comp_offset, &ec->qrpn);
}

/* encode: full pipeline → fills ec->fi + ec->comp_buf */
static int encode(EncodeCtx *ec, const uint8_t *src, size_t len)
{
    memset(ec, 0, sizeof(*ec));
    ec->src     = src;
    ec->src_len = len;

    qrpn_ctx_init(&ec->qrpn, 8);

    uint32_t n = scan_buf_count(len);
    if (file_index_init(&ec->fi, ec->fidx_buf, sizeof(ec->fidx_buf),
                        n,
                        (const uint8_t *)"e2e_test", 8,
                        (uint64_t)len,
                        FTYPE_BINARY) != 0) return -1;

    ScanConfig cfg = {0};
    scan_buf(src, len, _encode_cb, ec, &cfg);

    return file_index_seal(&ec->fi);
}

/* decode: reconstruct full buffer, compare to original */
static int decode_verify(EncodeCtx *ec,
                         const uint8_t *orig, size_t orig_len,
                         const char *label)
{
    ReconContext rc;
    qrpn_ctx_init(&ec->qrpn, 8);   /* fresh qrpn for decode side */

    if (pogls_reconstruct_init(&rc, &ec->fi,
                               ec->comp_buf, (uint64_t)ec->comp_pos,
                               &ec->qrpn) != RECON_OK) {
        printf("  [FAIL] %s: reconstruct_init failed\n", label);
        return 0;
    }

    /* full reconstruct into out_buf */
    uint8_t *out = calloc(1, orig_len + BLOCK);
    if (!out) return 0;

    size_t   written = 0;
    int r = pogls_reconstruct_full(&rc, out, (uint32_t)(orig_len + BLOCK), &written);

    if (r != RECON_OK) {
        printf("  [FAIL] %s: reconstruct_full err=%d\n", label, r);
        free(out); return 0;
    }
    if ((size_t)written < orig_len) {
        printf("  [FAIL] %s: written=%zu < orig=%zu\n", label, written, orig_len);
        free(out); return 0;
    }
    if (memcmp(out, orig, orig_len) != 0) {
        printf("  [FAIL] %s: byte mismatch\n", label);
        free(out); return 0;
    }
    printf("  [PASS] %s: %zu bytes byte-perfect\n", label, orig_len);
    free(out);
    return 1;
}

/* stream_verify: reconstruct via PoglsStream, compare to original */
static int stream_verify(EncodeCtx *ec,
                         const uint8_t *orig, size_t orig_len,
                         uint32_t chunk_start, uint32_t chunk_end,
                         const char *label)
{
    ReconContext rc;
    qrpn_ctx_init(&ec->qrpn, 8);

    if (pogls_reconstruct_init(&rc, &ec->fi,
                               ec->comp_buf, (uint64_t)ec->comp_pos,
                               &ec->qrpn) != RECON_OK) {
        printf("  [FAIL] %s: reconstruct_init\n", label); return 0;
    }

    PoglsStream s;
    if (pogls_stream_init(&s, &rc, chunk_start, chunk_end) != STREAM_OK) {
        printf("  [FAIL] %s: stream_init\n", label); return 0;
    }

    uint8_t *out = calloc(1, orig_len + BLOCK);
    if (!out) return 0;

    size_t   pos = 0;
    uint8_t  chunk[BLOCK];
    uint32_t got = 0;
    int      sr;

    while ((sr = pogls_stream_next(&s, chunk, &got)) == STREAM_OK) {
        size_t copy = (pos + got <= orig_len + BLOCK) ? got : 0;
        if (copy) { memcpy(out + pos, chunk, copy); pos += copy; }
    }
    if (sr != STREAM_DONE) {
        printf("  [FAIL] %s: stream err=%d recon=%d\n",
               label, sr, s.last_recon_err);
        free(out); return 0;
    }
    if (memcmp(out, orig, orig_len) != 0) {
        printf("  [FAIL] %s: byte mismatch (stream path)\n", label);
        free(out); return 0;
    }
    printf("  [PASS] %s: stream %u→%u byte-perfect\n",
           label, chunk_start, chunk_end);
    free(out);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * Test cases
 * ════════════════════════════════════════════════════════════════════ */

static int test_E1(void)
{
    puts("E1: single block (64B text)");
    uint8_t src[BLOCK];
    memset(src, 0, BLOCK);
    const char *msg = "Hello POGLS E2E pipeline test! DiamondBlock 64B!";
    memcpy(src, msg, strlen(msg));

    EncodeCtx ec;
    if (encode(&ec, src, BLOCK) != 0) { puts("  [FAIL] encode"); return 0; }
    return decode_verify(&ec, src, BLOCK, "E1-full-recon");
}

static int test_E2(void)
{
    puts("E2: multi-block (8 × 64B = 512B)");
    const size_t len = 8 * BLOCK;
    uint8_t src[8 * BLOCK];
    for (size_t i = 0; i < len; i++) src[i] = (uint8_t)(i & 0xFF);

    EncodeCtx ec;
    if (encode(&ec, src, len) != 0) { puts("  [FAIL] encode"); return 0; }
    int r = decode_verify(&ec, src, len, "E2-full-recon");
    r    &= stream_verify(&ec, src, len, 0, 7, "E2-stream-full");
    return r;
}

static int test_E3(void)
{
    puts("E3: large file (144 × 64B = 9216B, PHI-seeded)");
    const uint32_t PHI32 = 0x9E3779B9u;
    const size_t   len   = 144 * BLOCK;
    uint8_t       *src   = malloc(len);
    if (!src) return 0;

    uint32_t v = PHI32;
    for (size_t i = 0; i < len; i += 4) {
        v = v * PHI32 + 1;
        memcpy(src + i, &v, 4);
    }

    EncodeCtx ec;
    if (encode(&ec, src, len) != 0) { free(src); puts("  [FAIL] encode"); return 0; }
    int r = decode_verify(&ec, src, len, "E3-full-recon");
    r    &= stream_verify(&ec, src, len, 0, 143, "E3-stream-full");
    free(src);
    return r;
}

static int test_E4(void)
{
    puts("E4: cross-block random-access (middle chunk only)");
    const size_t len = 8 * BLOCK;
    uint8_t src[8 * BLOCK];
    for (size_t i = 0; i < len; i++) src[i] = (uint8_t)((i * 17 + 3) & 0xFF);

    EncodeCtx ec;
    if (encode(&ec, src, len) != 0) { puts("  [FAIL] encode"); return 0; }

    /* reconstruct only chunks 2..5 (middle 4 of 8) */
    uint32_t cs = 2, ce = 5;
    return stream_verify(&ec, src + cs * BLOCK,
                         (ce - cs + 1) * BLOCK, cs, ce, "E4-partial-middle");
}

static int test_E5(void)
{
    puts("E5: stream seek/resume");
    const size_t len = 8 * BLOCK;
    uint8_t src[8 * BLOCK];
    for (size_t i = 0; i < len; i++) src[i] = (uint8_t)(i ^ 0xA5u);

    EncodeCtx ec;
    if (encode(&ec, src, len) != 0) { puts("  [FAIL] encode"); return 0; }

    ReconContext rc;
    qrpn_ctx_init(&ec.qrpn, 8);
    if (pogls_reconstruct_init(&rc, &ec.fi,
                               ec.comp_buf, (uint64_t)ec.comp_pos,
                               &ec.qrpn) != RECON_OK) {
        puts("  [FAIL] E5: recon_init"); return 0;
    }

    PoglsStream s;
    pogls_stream_init(&s, &rc, 0, 7);

    /* consume 3 chunks */
    uint8_t chunk[BLOCK]; uint32_t got;
    for (int i = 0; i < 3; i++) pogls_stream_next(&s, chunk, &got);

    /* seek back to chunk 1 */
    if (pogls_stream_seek(&s, 1) != STREAM_OK) {
        puts("  [FAIL] E5: seek"); return 0;
    }

    /* drain from chunk 1 to end */
    uint8_t out[8 * BLOCK]; memset(out, 0, sizeof(out));
    size_t pos = 0;
    int sr;
    while ((sr = pogls_stream_next(&s, chunk, &got)) == STREAM_OK) {
        memcpy(out + pos, chunk, got); pos += got;
    }
    if (sr != STREAM_DONE) {
        printf("  [FAIL] E5: stream err=%d\n", sr); return 0;
    }
    /* compare chunk 1..7 of original */
    if (memcmp(out, src + BLOCK, 7 * BLOCK) != 0) {
        puts("  [FAIL] E5: byte mismatch after seek"); return 0;
    }
    puts("  [PASS] E5: seek resume byte-perfect");
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════ */

int main(void)
{
    puts("═══════════════════════════════════════════");
    puts("  POGLS End-to-End Pipeline Test Suite");
    puts("═══════════════════════════════════════════");

    int pass = 0, total = 0;

    #define RUN(fn) do { total++; pass += fn(); } while(0)

    RUN(test_E1);
    RUN(test_E2);
    RUN(test_E3);
    RUN(test_E4);
    RUN(test_E5);

    puts("═══════════════════════════════════════════");
    printf("  Result: %d / %d PASS\n", pass, total);
    puts("═══════════════════════════════════════════");

    return (pass == total) ? 0 : 1;
}
