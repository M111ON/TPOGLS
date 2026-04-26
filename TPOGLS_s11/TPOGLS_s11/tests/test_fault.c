/*
 * test_fault.c — POGLS Fault Injection Suite
 * F1: flip 1 bit in compressed stream         → RECON_ERR_DECOMP (-4)
 * F2: corrupt rec->orig_checksum in index     → RECON_ERR_CHECKSUM (-5)
 * F3: corrupt rec->comp_offset (wrong seek)   → RECON_ERR_STREAM (-8) or DECOMP
 * F4: corrupt rec->rec_crc (index integrity)  → RECON_ERR_QRPN (-6)
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
#define N_CHUNKS 8u
#define COMP_BUF_SZ (N_CHUNKS * (BLOCK + 64))
#define FIDX_BUF_SZ FILE_INDEX_BUF_SIZE(N_CHUNKS)

typedef struct {
    uint8_t    fidx_buf[FIDX_BUF_SZ];
    uint8_t    comp_buf[COMP_BUF_SZ];
    size_t     comp_pos;
    FileIndex  fi;
    qrpn_ctx_t qrpn;
    const uint8_t *src;
    size_t     src_len;
} Ctx;

static void _cb(const ScanEntry *e, void *user)
{
    Ctx *ec = (Ctx *)user;
    DiamondBlock blk; memset(&blk, 0, sizeof(blk));
    size_t off  = (size_t)e->offset;
    size_t copy = ((ec->src_len - off) > 64) ? 64 : (ec->src_len - off);
    memcpy(&blk, ec->src + off, copy);
    uint8_t tmp[BLOCK + 64];
    int clen = pogls_compress_block(&blk, tmp, (int)sizeof(tmp), &ec->qrpn);
    if (clen <= 0) return;
    CompressHeader hdr; memcpy(&hdr, tmp, sizeof(hdr));
    uint64_t co = (uint64_t)ec->comp_pos;
    memcpy(ec->comp_buf + ec->comp_pos, tmp, (size_t)clen);
    ec->comp_pos += (size_t)clen;
    file_index_add(&ec->fi, e, &hdr, co, &ec->qrpn);
}

static int encode(Ctx *ec, const uint8_t *src, size_t len)
{
    memset(ec, 0, sizeof(*ec));
    ec->src = src; ec->src_len = len;
    qrpn_ctx_init(&ec->qrpn, 8);
    uint32_t n = scan_buf_count(len);
    if (file_index_init(&ec->fi, ec->fidx_buf, sizeof(ec->fidx_buf),
                        n, (const uint8_t *)"fault", 5,
                        (uint64_t)len, FTYPE_BINARY) != 0) return -1;
    ScanConfig cfg = {0};
    scan_buf(src, len, _cb, ec, &cfg);
    return file_index_seal(&ec->fi);
}

/* run full reconstruct, expect specific error on target_chunk */
static int expect_fail(Ctx *ec, uint32_t target_chunk,
                       int expected_err, const char *label)
{
    ReconContext rc;
    qrpn_ctx_init(&ec->qrpn, 8);
    pogls_reconstruct_init(&rc, &ec->fi,
                           ec->comp_buf, (uint64_t)ec->comp_pos, &ec->qrpn);
    uint8_t chunk[BLOCK]; uint32_t got;
    int r = pogls_reconstruct_chunk(&rc, target_chunk, chunk, &got);
    if (r == expected_err) {
        printf("  [PASS] %s: err=%d (expected)\n", label, r);
        return 1;
    }
    /* accept any non-OK as "detected" if expected=-99 (any fail) */
    if (expected_err == -99 && r != RECON_OK) {
        printf("  [PASS] %s: corruption detected err=%d\n", label, r);
        return 1;
    }
    printf("  [FAIL] %s: got=%d expected=%d\n", label, r, expected_err);
    return 0;
}

/* ── build shared test data ── */
static uint8_t g_src[N_CHUNKS * BLOCK];
static void make_src(void) {
    for (size_t i = 0; i < sizeof(g_src); i++)
        g_src[i] = (uint8_t)(i * 31 + 7);
}

/* ── F1: flip 1 bit inside compressed payload (not header) ── */
static int test_F1(void)
{
    puts("F1: flip 1 bit in compressed stream payload");
    Ctx ec; encode(&ec, g_src, sizeof(g_src));

    uint32_t target = 2;
    /* flip inside payload — skip past CompressHeader (16B) */
    uint64_t flip = ec.fi.records[target].comp_offset + 20;
    if (flip < ec.comp_pos) ec.comp_buf[flip] ^= 0x01;

    return expect_fail(&ec, target, RECON_ERR_DECOMP, "F1-bit-flip-payload");
}

/* ── F2: corrupt orig_checksum in FileIndex record ── */
static int test_F2(void)
{
    puts("F2: corrupt orig_checksum in index record");
    Ctx ec; encode(&ec, g_src, sizeof(g_src));

    uint32_t target = 3;
    ec.fi.records[target].orig_checksum ^= 0xDEADBEEF;
    /* rec_crc now stale — reconstruct sees checksum mismatch first */

    return expect_fail(&ec, target, RECON_ERR_CHECKSUM, "F2-checksum-corrupt");
}

/* ── F3: corrupt comp_offset → points outside stream ── */
static int test_F3(void)
{
    puts("F3: corrupt comp_offset → stream out-of-bounds");
    Ctx ec; encode(&ec, g_src, sizeof(g_src));

    uint32_t target = 1;
    ec.fi.records[target].comp_offset = (uint64_t)ec.comp_pos + 9999;
    /* rec_crc stale but stream bounds check fires first */

    return expect_fail(&ec, target, RECON_ERR_STREAM, "F3-offset-oob");
}

/* ── F4: corrupt rec_crc → index integrity fail ── */
static int test_F4(void)
{
    puts("F4: corrupt rec_crc → index integrity fail");
    Ctx ec; encode(&ec, g_src, sizeof(g_src));

    uint32_t target = 0;
    ec.fi.records[target].rec_crc ^= 0x12345678;
    /* checksum verify passes (data unchanged), rec_crc check catches it */

    return expect_fail(&ec, target, RECON_ERR_QRPN, "F4-rec_crc-corrupt");
}

int main(void)
{
    puts("═══════════════════════════════════════════");
    puts("  POGLS Fault Injection Suite");
    puts("═══════════════════════════════════════════");
    make_src();
    int pass = 0, total = 0;
    #define RUN(fn) do { total++; pass += fn(); } while(0)
    RUN(test_F1);
    RUN(test_F2);
    RUN(test_F3);
    RUN(test_F4);
    puts("═══════════════════════════════════════════");
    printf("  Result: %d / %d PASS\n", pass, total);
    puts("═══════════════════════════════════════════");
    return (pass == total) ? 0 : 1;
}
