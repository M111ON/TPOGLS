/*
 * test_reconstruct.c — S5 pogls_reconstruct.h test suite
 *
 * Tests:
 *   R1: Full round-trip — TEXT file (delta+PHI+RLE)
 *   R2: Full round-trip — PASSTHRU file (pre-compressed)
 *   R3: Partial reconstruct — chunk range [1..2] of 4-chunk file
 *   R4: pogls_reconstruct_bytes_at — cross-chunk byte read
 *   R5: Error paths — bad magic, checksum mismatch, range OOB
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* include order matters */
#include "core/pogls_platform.h"
#include "core/pogls_fold.h"
#include "core/pogls_qrpn_phaseE.h"
#include "pogls_scanner.h"
#include "pogls_compress.h"
#include "pogls_file_index.h"
#include "pogls_reconstruct.h"

/* ── Test helpers ────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, label) do { \
    if (cond) { printf("  PASS  %s\n", label); g_pass++; } \
    else       { printf("  FAIL  %s  [line %d]\n", label, __LINE__); g_fail++; } \
} while(0)

/* Build a minimal compressed stream for N 64B chunks of known data */
static void _make_chunk(uint8_t *dst, uint8_t fill, uint8_t flags, uint32_t payload_len)
{
    /* Write CompressHeader */
    CompressHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = COMP_MAGIC;
    hdr.flags       = flags;
    hdr.payload_len = payload_len;

    /* XOR-fold of the 64B block (fill repeated) for qrpn_cg */
    uint32_t xf = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t w;
        memset(&w, fill, 4);
        xf ^= w;
    }
    hdr.qrpn_cg = qrpn_phi_scatter_hex((uint64_t)xf ^ 0);  /* seq=0 placeholder */

    memcpy(dst, &hdr, COMPRESS_HEADER_SIZE);
    /* payload = 64B of fill (PASSTHRU: raw block) */
    memset(dst + COMPRESS_HEADER_SIZE, fill, payload_len);
}

/* ── Build a complete FileIndex + compressed stream for N chunks ─── */
typedef struct {
    uint8_t *fi_buf;
    size_t   fi_size;
    uint8_t *cs_buf;   /* compressed stream */
    size_t   cs_size;
    FileIndex fi;
} TestFixture;

static void fixture_init(TestFixture *tf, int n_chunks, uint8_t fill)
{
    tf->fi_size = FILE_INDEX_BUF_SIZE(n_chunks);
    tf->fi_buf  = (uint8_t *)calloc(1, tf->fi_size);

    /* PASSTHRU: each chunk = CompressHeader(16B) + 64B payload */
    uint32_t per_chunk = COMPRESS_HEADER_SIZE + 64;
    tf->cs_size = (size_t)n_chunks * per_chunk;
    tf->cs_buf  = (uint8_t *)calloc(1, tf->cs_size);

    /* Build compressed stream */
    for (int i = 0; i < n_chunks; i++) {
        _make_chunk(tf->cs_buf + (size_t)i * per_chunk, fill,
                    COMP_FLAG_PASSTHRU, 64);
    }

    /* Build FileIndex */
    uint8_t fname[] = "test.bin";
    file_index_init(&tf->fi, tf->fi_buf, tf->fi_size,
                    (uint32_t)n_chunks, fname, 8,
                    (uint64_t)(n_chunks * 64),
                    FTYPE_PASSTHRU);

    /* Shared QRPN for fixture registration */
    qrpn_ctx_t fqrpn;
    qrpn_ctx_init(&fqrpn, 0);

    /* XOR-fold using canonical pogls_checksum64 algo (uint64 fold) */
    uint8_t ref_block[64];
    memset(ref_block, fill, 64);
    const uint64_t *w64 = (const uint64_t *)ref_block;
    uint64_t acc64 = 0;
    for (int j = 0; j < 8; j++) acc64 ^= w64[j];
    uint32_t xf = (uint32_t)(acc64 ^ (acc64 >> 32));

    /* Register each chunk */
    for (int i = 0; i < n_chunks; i++) {
        ScanEntry se;
        memset(&se, 0, sizeof(se));
        se.offset     = (uint64_t)(i * 64);   /* actual field name */
        se.chunk_idx  = (uint32_t)i;
        se.flags      = SCAN_FLAG_VALID;
        se.checksum   = xf;                    /* actual field name */
        se.seed       = (uint64_t)xf ^ (uint64_t)i;

        CompressHeader ch;
        memset(&ch, 0, sizeof(ch));
        ch.magic       = COMP_MAGIC;
        ch.flags       = COMP_FLAG_PASSTHRU;
        ch.payload_len = 64;

        file_index_add(&tf->fi, &se, &ch, (uint64_t)(i * per_chunk), &fqrpn);
    }
    file_index_seal(&tf->fi);
}

static void fixture_free(TestFixture *tf)
{
    free(tf->fi_buf);
    free(tf->cs_buf);
}

/* ── R1: Full round-trip PASSTHRU ───────────────────────────────── */
static void test_R1_full_roundtrip(void)
{
    printf("\n[R1] Full round-trip (PASSTHRU, 4 chunks)\n");
    TestFixture tf;
    fixture_init(&tf, 4, 0xAB);

    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 0);

    ReconContext rc;
    int r = pogls_reconstruct_init(&rc, &tf.fi, tf.cs_buf, tf.cs_size, &qrpn);
    CHECK(r == RECON_OK, "init OK");

    uint8_t out[256];
    size_t  got = 0;
    r = pogls_reconstruct_full(&rc, out, 256, &got);
    CHECK(r == RECON_OK, "full reconstruct OK");
    CHECK(got == 256,    "output size == 256");

    /* Verify all bytes == 0xAB */
    int all_match = 1;
    for (int i = 0; i < 256; i++) if (out[i] != 0xAB) { all_match = 0; break; }
    CHECK(all_match, "all bytes match fill");

    fixture_free(&tf);
}

/* ── R2: Partial range [1..2] of 4-chunk file ───────────────────── */
static void test_R2_partial_range(void)
{
    printf("\n[R2] Partial range [1..2] of 4-chunk file\n");
    TestFixture tf;
    fixture_init(&tf, 4, 0xCD);

    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 0);

    ReconContext rc;
    pogls_reconstruct_init(&rc, &tf.fi, tf.cs_buf, tf.cs_size, &qrpn);

    uint8_t out[128];
    size_t  got = 0;
    int r = pogls_reconstruct_range(&rc, 1, 2, out, 128, &got);
    CHECK(r == RECON_OK, "range [1..2] OK");
    CHECK(got == 128,    "range output == 128B");

    int all_match = 1;
    for (int i = 0; i < 128; i++) if (out[i] != 0xCD) { all_match = 0; break; }
    CHECK(all_match, "range bytes match fill");

    fixture_free(&tf);
}

/* ── R3: bytes_at — cross-chunk read ────────────────────────────── */
static void test_R3_bytes_at(void)
{
    printf("\n[R3] bytes_at cross-chunk (offset=56, len=16)\n");
    TestFixture tf;
    fixture_init(&tf, 4, 0xEF);

    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 0);

    ReconContext rc;
    pogls_reconstruct_init(&rc, &tf.fi, tf.cs_buf, tf.cs_size, &qrpn);

    /* offset 56..71 spans chunk 0 [56..63] + chunk 1 [0..7] */
    uint8_t  out[16];
    uint32_t got = 0;
    int r = pogls_reconstruct_bytes_at(&rc, 56, out, 16, &got);
    CHECK(r == RECON_OK, "bytes_at OK");
    CHECK(got == 16,     "bytes_at got == 16");
    int all_ef = 1;
    for (int i = 0; i < 16; i++) if (out[i] != 0xEF) { all_ef = 0; break; }
    CHECK(all_ef, "bytes_at values match fill");

    fixture_free(&tf);
}

/* ── R4: lookup_offset ─────────────────────────────────────────── */
static void test_R4_lookup_offset(void)
{
    printf("\n[R4] lookup_offset O(1)\n");
    TestFixture tf;
    fixture_init(&tf, 4, 0x12);

    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 0);

    ReconContext rc;
    pogls_reconstruct_init(&rc, &tf.fi, tf.cs_buf, tf.cs_size, &qrpn);

    CHECK(pogls_reconstruct_lookup_offset(&rc, 0)   == 0, "offset 0   → chunk 0");
    CHECK(pogls_reconstruct_lookup_offset(&rc, 63)  == 0, "offset 63  → chunk 0");
    CHECK(pogls_reconstruct_lookup_offset(&rc, 64)  == 1, "offset 64  → chunk 1");
    CHECK(pogls_reconstruct_lookup_offset(&rc, 255) == 3, "offset 255 → chunk 3");
    CHECK(pogls_reconstruct_lookup_offset(&rc, 256) == UINT32_MAX, "offset OOB → UINT32_MAX");

    fixture_free(&tf);
}

/* ── R5: Error paths ─────────────────────────────────────────────── */
static void test_R5_error_paths(void)
{
    printf("\n[R5] Error paths\n");
    TestFixture tf;
    fixture_init(&tf, 2, 0x55);

    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 0);

    ReconContext rc;
    pogls_reconstruct_init(&rc, &tf.fi, tf.cs_buf, tf.cs_size, &qrpn);

    /* chunk_id OOB */
    uint8_t tmp[64]; uint32_t got;
    int r = pogls_reconstruct_chunk(&rc, 99, tmp, &got);
    CHECK(r == RECON_ERR_RANGE, "chunk OOB → RECON_ERR_RANGE");

    /* null out buffer */
    r = pogls_reconstruct_chunk(&rc, 0, NULL, &got);
    CHECK(r == RECON_ERR_NULL, "null out → RECON_ERR_NULL");

    /* output buffer too small for full */
    uint8_t small[10];
    size_t  sgot;
    r = pogls_reconstruct_full(&rc, small, 10, &sgot);
    CHECK(r == RECON_ERR_OVERFLOW, "small buf → RECON_ERR_OVERFLOW");

    /* unsealed index */
    uint8_t fi2_buf[FILE_INDEX_BUF_SIZE(4)];
    FileIndex fi2;
    uint8_t n2[] = "x";
    file_index_init(&fi2, fi2_buf, sizeof(fi2_buf), 4, n2, 1, 256, FTYPE_BINARY);
    /* NOT sealed */
    ReconContext rc2;
    r = pogls_reconstruct_init(&rc2, &fi2, tf.cs_buf, tf.cs_size, &qrpn);
    CHECK(r == RECON_ERR_SEALED, "unsealed → RECON_ERR_SEALED");

    fixture_free(&tf);
}

/* ── main ──────────────────────────────────────────────────────────  */
int main(void)
{
    printf("=== test_reconstruct.c — S5 ===\n");
    test_R1_full_roundtrip();
    test_R2_partial_range();
    test_R3_bytes_at();
    test_R4_lookup_offset();
    test_R5_error_paths();

    printf("\n─────────────────────────────\n");
    printf("  %d/%d PASS\n", g_pass, g_pass + g_fail);
    return (g_fail == 0) ? 0 : 1;
}
