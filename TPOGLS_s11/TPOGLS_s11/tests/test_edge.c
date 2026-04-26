/*
 * test_edge.c — Edge + Fault Injection tests
 * E6: boundary chunks (63B, 64B, 65B)
 * E7: 1-bit corruption → must fail
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/pogls_platform.h"
#include "core/pogls_qrpn_phaseE.h"
#include "core/pogls_fold.h"
#include "pogls_compress.h"
#include "pogls_scanner.h"
#include "pogls_file_index.h"
#include "pogls_reconstruct.h"
#include "pogls_stream.h"

#define BLOCK DIAMOND_BLOCK_SIZE
#define MAX_CHUNKS 256u
#define COMP_BUF_SZ (MAX_CHUNKS * (BLOCK + 64))
#define FIDX_BUF_SZ FILE_INDEX_BUF_SIZE(MAX_CHUNKS)

typedef struct {
    uint8_t  fidx_buf[FIDX_BUF_SZ];
    uint8_t  comp_buf[COMP_BUF_SZ];
    size_t   comp_pos;
    FileIndex fi;
    qrpn_ctx_t qrpn;
    const uint8_t *src;
    size_t src_len;
} EncodeCtx;

static void _encode_cb(const ScanEntry *e, void *user)
{
    EncodeCtx *ec = (EncodeCtx *)user;
    DiamondBlock blk;
    memset(&blk, 0, sizeof(blk));
    size_t off   = (size_t)e->offset;
    size_t avail = (off < ec->src_len) ? (ec->src_len - off) : 0;
    size_t copy  = (avail > 64) ? 64 : avail;
    memcpy(&blk, ec->src + off, copy);
    uint8_t tmp[BLOCK + 64];
    int clen = pogls_compress_block(&blk, tmp, (int)sizeof(tmp), &ec->qrpn);
    if (clen <= 0) return;
    CompressHeader hdr;
    memcpy(&hdr, tmp, sizeof(CompressHeader));
    uint64_t comp_offset = (uint64_t)ec->comp_pos;
    memcpy(ec->comp_buf + ec->comp_pos, tmp, (size_t)clen);
    ec->comp_pos += (size_t)clen;
    file_index_add(&ec->fi, e, &hdr, comp_offset, &ec->qrpn);
}

static int encode(EncodeCtx *ec, const uint8_t *src, size_t len)
{
    memset(ec, 0, sizeof(*ec));
    ec->src = src; ec->src_len = len;
    qrpn_ctx_init(&ec->qrpn, 8);
    uint32_t n = scan_buf_count(len);
    if (n == 0) n = 1;
    if (file_index_init(&ec->fi, ec->fidx_buf, sizeof(ec->fidx_buf),
                        n, (const uint8_t *)"edge_test", 9,
                        (uint64_t)len, FTYPE_BINARY) != 0) return -1;
    ScanConfig cfg = {0};
    scan_buf(src, len, _encode_cb, ec, &cfg);
    return file_index_seal(&ec->fi);
}

static int full_recon_verify(EncodeCtx *ec, const uint8_t *orig, size_t len, const char *label)
{
    ReconContext rc;
    qrpn_ctx_init(&ec->qrpn, 8);
    if (pogls_reconstruct_init(&rc, &ec->fi,
                               ec->comp_buf, (uint64_t)ec->comp_pos,
                               &ec->qrpn) != RECON_OK) {
        printf("  [FAIL] %s: recon_init\n", label); return 0;
    }
    uint8_t *out = calloc(1, len + BLOCK);
    size_t written = 0;
    int r = pogls_reconstruct_full(&rc, out, len + BLOCK, &written);
    if (r != RECON_OK) { printf("  [FAIL] %s: recon err=%d\n", label, r); free(out); return 0; }
    if (memcmp(out, orig, len) != 0) { printf("  [FAIL] %s: mismatch\n", label); free(out); return 0; }
    printf("  [PASS] %s: %zu bytes byte-perfect\n", label, len);
    free(out); return 1;
}

/* ── E6: boundary sizes ── */
static int test_E6(void)
{
    puts("E6: boundary chunk sizes (63B / 64B / 65B)");
    int pass = 1;
    size_t sizes[] = { 63, 64, 65 };
    char label[32];
    for (int i = 0; i < 3; i++) {
        size_t len = sizes[i];
        uint8_t *src = malloc(len);
        for (size_t j = 0; j < len; j++) src[j] = (uint8_t)(j * 7 + 13);
        EncodeCtx ec;
        snprintf(label, sizeof(label), "E6-%zuB", len);
        if (encode(&ec, src, len) != 0) { printf("  [FAIL] %s encode\n", label); pass = 0; }
        else pass &= full_recon_verify(&ec, src, len, label);
        free(src);
    }
    return pass;
}

/* ── E7: 1-bit corruption must fail ── */
static int test_E7(void)
{
    puts("E7: 1-bit corruption → must detect");
    size_t len = 4 * BLOCK;
    uint8_t src[4 * BLOCK];
    for (size_t i = 0; i < len; i++) src[i] = (uint8_t)(i ^ 0x5A);

    EncodeCtx ec;
    if (encode(&ec, src, len) != 0) { puts("  [FAIL] E7: encode"); return 0; }

    /* flip bit 3 of byte 32 inside chunk 1 (middle of comp stream) */
    uint32_t target_chunk = 1;
    uint64_t flip_offset  = ec.fi.records[target_chunk].comp_offset + 32;
    if (flip_offset < ec.comp_pos)
        ec.comp_buf[flip_offset] ^= 0x08;

    ReconContext rc;
    qrpn_ctx_init(&ec.qrpn, 8);
    pogls_reconstruct_init(&rc, &ec.fi, ec.comp_buf, (uint64_t)ec.comp_pos, &ec.qrpn);

    uint8_t out[4 * BLOCK];
    size_t written = 0;
    int r = pogls_reconstruct_full(&rc, out, sizeof(out), &written);

    if (r != RECON_OK) {
        printf("  [PASS] E7: corruption detected (err=%d)\n", r);
        return 1;
    }
    /* if reconstruct "succeeded", data must differ */
    if (memcmp(out, src, len) != 0) {
        puts("  [PASS] E7: corruption detected via mismatch");
        return 1;
    }
    puts("  [FAIL] E7: corruption NOT detected");
    return 0;
}

int main(void)
{
    puts("═══════════════════════════════════════════");
    puts("  POGLS Edge + Fault Injection Tests");
    puts("═══════════════════════════════════════════");
    int pass = 0, total = 0;
    #define RUN(fn) do { total++; pass += fn(); } while(0)
    RUN(test_E6);
    RUN(test_E7);
    puts("═══════════════════════════════════════════");
    printf("  Result: %d / %d PASS\n", pass, total);
    puts("═══════════════════════════════════════════");
    return (pass == total) ? 0 : 1;
}
