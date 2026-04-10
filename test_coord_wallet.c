/*
 * test_coord_wallet.c — Unit tests for pogls_coord_wallet.h (S11-C)
 *
 * Tests:
 *   T01: init + size check
 *   T02: add_file + string pool
 *   T03: add_coord (scan_entry → coord record)
 *   T04: seal + footer integrity
 *   T05: verify_integrity (good path)
 *   T06: verify_integrity (corrupted — expect fail)
 *   T07: reconstruct_buf (buffer mode, happy path)
 *   T08: reconstruct_buf (seed mismatch → HARD FAIL)
 *   T09: reconstruct_buf (checksum mismatch → HARD FAIL)
 *   T10: reconstruct_buf (file_size mismatch → SOFT WARN, continues)
 *   T11: fetch_chunk O(1) single fetch
 *   T12: lookup_by_offset O(1)
 *   T13: multi-file wallet (2 files, interleaved coord lookup)
 *   T14: drift_window search (simulated offset drift)
 *   T15: wallet_stats
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* pull real headers — wallet header includes its own deps */
#include "../core/pogls_platform.h"
#include "../core/pogls_fold.h"
#include "../core/pogls_qrpn_phaseE.h"
#include "../theta_map.h"
#include "../pogls_coord_wallet.h"

/* ── test helpers ────────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        g_fail++; \
    } else { \
        printf("  ok  : %s\n", msg); \
        g_pass++; \
    } \
} while(0)

/* Build a synthetic 64B chunk with known seed/checksum */
static void make_chunk(uint8_t *out64, uint64_t pattern) {
    for (int i = 0; i < 8; i++) {
        uint64_t w = pattern ^ (uint64_t)i * UINT64_C(0x9e3779b97f4a7c15);
        memcpy(out64 + i * 8, &w, 8);
    }
}

static uint64_t chunk_seed(const uint8_t *c) {
    uint64_t w[8]; memcpy(w, c, 64);
    uint64_t s = w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    s ^= s>>33; s *= UINT64_C(0xff51afd7ed558ccd);
    s ^= s>>33; s *= UINT64_C(0xc4ceb9fe1a85ec53);
    s ^= s>>33; return s;
}
static uint32_t chunk_cksum(const uint8_t *c) {
    uint32_t a=0,w; for(int i=0;i<16;i++){memcpy(&w,c+i*4,4);a^=w;} return a;
}

/* callback state for reconstruct tests */
typedef struct { int count; uint64_t seed_xor; } ReconState;
static void recon_cb(const uint8_t *c64, uint32_t cid,
                     const CoordRecord *cr, void *u) {
    (void)cid; (void)cr;
    ReconState *s = (ReconState*)u;
    s->count++;
    s->seed_xor ^= chunk_seed(c64);
}

/* ══════════════════════════════════════════════════════════════════
 * TESTS
 * ══════════════════════════════════════════════════════════════════ */

static void test_t01_size(void) {
    printf("\nT01: struct sizes\n");
    CHECK(sizeof(WalletHeader) == 128, "WalletHeader == 128B");
    CHECK(sizeof(FileEntry)    ==  64, "FileEntry == 64B");
    CHECK(sizeof(CoordRecord)  ==  40, "CoordRecord == 40B");
    CHECK(sizeof(WalletFooter) ==  16, "WalletFooter == 16B");
}

static void test_t02_add_file(void) {
    printf("\nT02: wallet_init + wallet_add_file\n");

    size_t bsz = wallet_buf_size(4, 512, 64);
    uint8_t *buf = calloc(1, bsz);

    WalletCtx ctx;
    wallet_result_t r = wallet_init(&ctx, buf, bsz, 4, 512, 64,
                                    WALLET_MODE_BUFFER, 0xDEADBEEFCAFEBABEull);
    CHECK(r == WALLET_OK, "wallet_init ok");
    CHECK(ctx.hdr->magic == WALLET_MAGIC, "magic set");
    CHECK(ctx.hdr->mode  == WALLET_MODE_BUFFER, "mode set");

    const uint8_t path[] = "/data/myfile.bin";
    int idx = wallet_add_file(&ctx, path, (uint16_t)sizeof(path)-1,
                              4096, 1700000000LL, 0xABCDABCDABCDABCDull);
    CHECK(idx == 0, "first file_idx = 0");
    CHECK(ctx.hdr->file_count == 1, "file_count = 1");
    CHECK(ctx.hdr->string_pool_size == sizeof(path)-1, "pool used");

    uint16_t plen = 0;
    const uint8_t *p = wallet_file_path(&ctx, 0, &plen);
    CHECK(p != NULL, "wallet_file_path not null");
    CHECK(plen == sizeof(path)-1, "path len correct");
    CHECK(memcmp(p, path, plen) == 0, "path content correct");

    free(buf);
}

static void test_t03_add_coord(void) {
    printf("\nT03: wallet_add_coord\n");

    size_t bsz = wallet_buf_size(2, 256, 16);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 2, 256, 16, WALLET_MODE_BUFFER, 1ull);

    wallet_add_file(&ctx, (const uint8_t*)"/f", 2, 1024, 0, 0);

    uint8_t chunk[64]; make_chunk(chunk, 0x1234567890ABCDEFull);
    uint64_t s = chunk_seed(chunk);
    uint32_t c = chunk_cksum(chunk);
    ThetaCoord tc = theta_map(s);

    int cid = wallet_add_coord(&ctx, 0, 0, s, c, tc, 0);
    CHECK(cid == 0, "first coord_id = 0");
    CHECK(ctx.hdr->coord_count == 1, "coord_count = 1");

    const CoordRecord *cr = &ctx.coords[0];
    CHECK(cr->seed     == s, "seed stored");
    CHECK(cr->checksum == c, "checksum stored");
    CHECK(cr->fast_sig == (uint32_t)(s & 0xFFFFFFFFu), "fast_sig = lo32(seed)");
    CHECK(cr->file_idx == 0, "file_idx = 0");
    CHECK(cr->drift_window == 0, "drift = 0 default");

    ThetaCoord back = wallet_coord_unpack(cr->coord_packed);
    CHECK(back.face == tc.face && back.edge == tc.edge && back.z == tc.z,
          "coord pack/unpack round-trip");

    free(buf);
}

static void test_t04_seal(void) {
    printf("\nT04: wallet_seal + footer\n");

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 42ull);
    wallet_add_file(&ctx, (const uint8_t*)"/x", 2, 128, 0, 0);

    size_t content_sz = bsz - sizeof(WalletFooter);
    wallet_result_t r = wallet_seal(&ctx, buf, content_sz);
    CHECK(r == WALLET_OK, "seal ok");
    CHECK(ctx.hdr->flags & WALLET_FLAG_SEALED, "SEALED flag set");
    CHECK(ctx.footer->magic == WALLET_FOOTER_MAGIC, "footer magic");
    CHECK(ctx.footer->coord_count_check == ctx.hdr->coord_count, "footer count check");
    CHECK(ctx.footer->xxh64_digest != 0, "digest non-zero");

    free(buf);
}

static void test_t05_verify_good(void) {
    printf("\nT05: verify_integrity (good)\n");

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 99ull);
    wallet_add_file(&ctx, (const uint8_t*)"/y", 2, 64, 0, 0);

    uint8_t c[64]; make_chunk(c, 0x1111ull);
    wallet_add_coord(&ctx, 0, 0, chunk_seed(c), chunk_cksum(c), theta_map(chunk_seed(c)), 0);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    wallet_result_t r = wallet_verify_integrity(&ctx, buf, csz, NULL);
    CHECK(r == WALLET_OK, "verify passes on clean wallet");

    free(buf);
}

static void test_t06_verify_corrupt(void) {
    printf("\nT06: verify_integrity (corrupted)\n");

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 77ull);
    wallet_add_file(&ctx, (const uint8_t*)"/z", 2, 64, 0, 0);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    /* corrupt a byte in the coord area */
    buf[ctx.hdr->coord_offset + 1] ^= 0xFF;

    wallet_result_t r = wallet_verify_integrity(&ctx, buf, csz, NULL);
    CHECK(r == WALLET_ERR_GUARD_FAIL, "detects corruption via digest");

    free(buf);
}

static void test_t07_reconstruct_good(void) {
    printf("\nT07: reconstruct_buf (happy path)\n");

    /* build a fake source file: 3 chunks = 192 bytes */
    uint8_t src[192];
    uint8_t chunks[3][64];
    uint64_t seeds[3]; uint32_t cksums[3];
    for (int i = 0; i < 3; i++) {
        make_chunk(chunks[i], (uint64_t)(i+1) * 0xFEDCBA9876543210ull);
        memcpy(src + i*64, chunks[i], 64);
        seeds[i]  = chunk_seed(chunks[i]);
        cksums[i] = chunk_cksum(chunks[i]);
    }

    size_t bsz = wallet_buf_size(1, 64, 8);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 8, WALLET_MODE_BUFFER, 1234ull);
    wallet_add_file(&ctx, (const uint8_t*)"/src", 4, 192, 0, 0);

    for (int i = 0; i < 3; i++)
        wallet_add_coord(&ctx, 0, (uint64_t)i*64,
                         seeds[i], cksums[i], theta_map(seeds[i]), 0);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    ReconState rs = {0, 0};
    wallet_result_t r = wallet_reconstruct_buf(&ctx, 0, src, 192,
                                               recon_cb, &rs, NULL);
    CHECK(r == WALLET_OK, "reconstruct returns OK");
    CHECK(rs.count == 3, "3 chunks emitted");

    uint64_t expected_xor = seeds[0] ^ seeds[1] ^ seeds[2];
    CHECK(rs.seed_xor == expected_xor, "chunk data matches seeds");

    free(buf);
}

static void test_t08_seed_fail(void) {
    printf("\nT08: reconstruct_buf (seed mismatch → HARD FAIL)\n");

    uint8_t src[64]; make_chunk(src, 0xAAAAull);
    uint64_t good_seed = chunk_seed(src);

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 5ull);
    wallet_add_file(&ctx, (const uint8_t*)"/a", 2, 64, 0, 0);

    /* register with wrong seed */
    wallet_add_coord(&ctx, 0, 0, good_seed ^ 0xDEADull, chunk_cksum(src),
                     theta_map(good_seed), 0);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    ReconState rs = {0, 0};
    wallet_result_t r = wallet_reconstruct_buf(&ctx, 0, src, 64,
                                               recon_cb, &rs, NULL);
    CHECK(r == WALLET_ERR_SEED_FAIL, "HARD FAIL on seed mismatch");
    CHECK(rs.count == 0, "no chunks emitted after fail");

    free(buf);
}

static void test_t09_cksum_fail(void) {
    printf("\nT09: reconstruct_buf (checksum mismatch → HARD FAIL)\n");

    uint8_t src[64]; make_chunk(src, 0xBBBBull);
    uint64_t s = chunk_seed(src);
    uint32_t c = chunk_cksum(src);

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 6ull);
    wallet_add_file(&ctx, (const uint8_t*)"/b", 2, 64, 0, 0);

    /* register with wrong checksum */
    wallet_add_coord(&ctx, 0, 0, s, c ^ 0xFFFFFFFF, theta_map(s), 0);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    ReconState rs = {0, 0};
    wallet_result_t r = wallet_reconstruct_buf(&ctx, 0, src, 64,
                                               recon_cb, &rs, NULL);
    CHECK(r == WALLET_ERR_CKSUM_FAIL, "HARD FAIL on checksum mismatch");

    free(buf);
}

static void test_t10_meta_warn(void) {
    printf("\nT10: reconstruct_buf (file_size mismatch → SOFT WARN)\n");

    uint8_t src[64]; make_chunk(src, 0xCCCCull);
    uint64_t s = chunk_seed(src);
    uint32_t c = chunk_cksum(src);

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 7ull);

    /* register expected size as 128, but actual = 64 */
    wallet_add_file(&ctx, (const uint8_t*)"/c", 2, 128, 0, 0);
    wallet_add_coord(&ctx, 0, 0, s, c, theta_map(s), 0);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    wallet_result_t meta_out = WALLET_OK;
    ReconState rs = {0, 0};
    wallet_result_t r = wallet_reconstruct_buf(&ctx, 0, src, 64,
                                               recon_cb, &rs, &meta_out);
    CHECK(r == WALLET_WARN_META_DRIFT, "SOFT WARN on file_size mismatch");
    CHECK(meta_out == WALLET_WARN_META_DRIFT, "out_meta set");
    CHECK(rs.count == 1, "chunk still emitted despite warn");

    free(buf);
}

static void test_t11_fetch_single(void) {
    printf("\nT11: wallet_fetch_chunk O(1)\n");

    uint8_t src[128];
    uint8_t c0[64], c1[64];
    make_chunk(c0, 0x1111ull); make_chunk(c1, 0x2222ull);
    memcpy(src,    c0, 64);
    memcpy(src+64, c1, 64);

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 8ull);
    wallet_add_file(&ctx, (const uint8_t*)"/d", 2, 128, 0, 0);
    wallet_add_coord(&ctx, 0,  0, chunk_seed(c0), chunk_cksum(c0), theta_map(chunk_seed(c0)), 0);
    wallet_add_coord(&ctx, 0, 64, chunk_seed(c1), chunk_cksum(c1), theta_map(chunk_seed(c1)), 0);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    uint8_t out[64];
    wallet_result_t r0 = wallet_fetch_chunk(&ctx, 0, src, 128, out);
    CHECK(r0 == WALLET_OK, "fetch coord_id=0 ok");
    CHECK(memcmp(out, c0, 64) == 0, "coord_id=0 data matches");

    wallet_result_t r1 = wallet_fetch_chunk(&ctx, 1, src, 128, out);
    CHECK(r1 == WALLET_OK, "fetch coord_id=1 ok");
    CHECK(memcmp(out, c1, 64) == 0, "coord_id=1 data matches");

    free(buf);
}

static void test_t12_lookup(void) {
    printf("\nT12: wallet_lookup_by_offset O(1)\n");

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 9ull);
    wallet_add_file(&ctx, (const uint8_t*)"/e", 2, 256, 0, 0);

    uint8_t dummy[64] = {0};
    uint64_t s = chunk_seed(dummy); uint32_t c = chunk_cksum(dummy);
    ThetaCoord tc = theta_map(s);
    for (int i = 0; i < 4; i++)
        wallet_add_coord(&ctx, 0, (uint64_t)i*64, s^i, c, tc, 0);

    uint32_t cid = wallet_lookup_by_offset(&ctx, 0, 128);  /* chunk 2 */
    CHECK(cid == 2, "offset 128 → coord_id 2");

    uint32_t bad = wallet_lookup_by_offset(&ctx, 0, 9999);
    CHECK(bad == WALLET_MAX_COORDS, "out-of-range → WALLET_MAX_COORDS");

    free(buf);
}

static void test_t13_multi_file(void) {
    printf("\nT13: multi-file wallet\n");

    /* 2 files, 2 chunks each */
    uint8_t src0[128], src1[128];
    uint8_t c[4][64];
    uint64_t seeds[4]; uint32_t cksums[4];
    for (int i = 0; i < 4; i++) {
        make_chunk(c[i], (uint64_t)(0xF0F0 + i) << 32);
        seeds[i]  = chunk_seed(c[i]);
        cksums[i] = chunk_cksum(c[i]);
    }
    memcpy(src0,    c[0], 64); memcpy(src0+64, c[1], 64);
    memcpy(src1,    c[2], 64); memcpy(src1+64, c[3], 64);

    size_t bsz = wallet_buf_size(2, 128, 8);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 2, 128, 8, WALLET_MODE_PATH, 10ull);

    /* correct contract: add file, then its coords, then next file */
    int f0 = wallet_add_file(&ctx, (const uint8_t*)"/file0", 6, 128, 0, 0);
    wallet_add_coord(&ctx, 0,  0, seeds[0], cksums[0], theta_map(seeds[0]), 0);
    wallet_add_coord(&ctx, 0, 64, seeds[1], cksums[1], theta_map(seeds[1]), 0);

    int f1 = wallet_add_file(&ctx, (const uint8_t*)"/file1", 6, 128, 0, 0);
    wallet_add_coord(&ctx, 1,  0, seeds[2], cksums[2], theta_map(seeds[2]), 0);
    wallet_add_coord(&ctx, 1, 64, seeds[3], cksums[3], theta_map(seeds[3]), 0);

    CHECK(f0 == 0 && f1 == 1, "two files registered");

    CHECK(ctx.hdr->coord_count == 4, "4 coords total");
    CHECK(ctx.files[0].chunk_count == 2, "file0 has 2 chunks");
    CHECK(ctx.files[1].chunk_count == 2, "file1 has 2 chunks");
    CHECK(ctx.files[1].chunk_start == 2, "file1 starts at coord_id 2");

    /* reconstruct file0 */
    ReconState rs0 = {0, 0};
    wallet_reconstruct_buf(&ctx, 0, src0, 128, recon_cb, &rs0, NULL);
    CHECK(rs0.count == 2, "file0: 2 chunks reconstructed");

    /* reconstruct file1 */
    ReconState rs1 = {0, 0};
    wallet_reconstruct_buf(&ctx, 1, src1, 128, recon_cb, &rs1, NULL);
    CHECK(rs1.count == 2, "file1: 2 chunks reconstructed");

    /* dedup: both files share chunk at seed[0] == seed[0] is trivial
     * — real dedup: two wallets pointing same (file,offset) */
    uint32_t cid = wallet_lookup_by_offset(&ctx, 1, 64);
    CHECK(cid == 3, "file1 offset 64 → coord_id 3");

    free(buf);
}

static void test_t14_drift(void) {
    printf("\nT14: drift_window search\n");

    /* chunk is at offset 64 in wallet, but physically at offset 128 (drifted +64) */
    uint8_t src[256]; memset(src, 0xAB, 256);
    uint8_t chunk[64]; make_chunk(chunk, 0x9999ull);
    uint64_t s = chunk_seed(chunk);
    uint32_t c = chunk_cksum(chunk);
    /* place chunk at offset 128 (drifted by +64 from registered offset 64) */
    memcpy(src + 128, chunk, 64);

    size_t bsz = wallet_buf_size(1, 64, 4);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 1, 64, 4, WALLET_MODE_BUFFER, 11ull);
    wallet_add_file(&ctx, (const uint8_t*)"/drifted", 8, 256, 0, 0);

    /* register at offset 64, but chunk is actually at 128 */
    wallet_add_coord(&ctx, 0, 64, s, c, theta_map(s), 2 /* ±2*64=±128B */);

    size_t csz = bsz - sizeof(WalletFooter);
    wallet_seal(&ctx, buf, csz);

    ReconState rs = {0, 0};
    wallet_result_t r = wallet_reconstruct_buf(&ctx, 0, src, 256,
                                               recon_cb, &rs, NULL);
    CHECK(r == WALLET_OK, "drift search finds chunk at drifted offset");
    CHECK(rs.count == 1, "1 chunk emitted via drift");

    /* strict mode (drift=0) should fail */
    ctx.coords[0].drift_window = 0;
    rs.count = 0;
    r = wallet_reconstruct_buf(&ctx, 0, src, 256, recon_cb, &rs, NULL);
    CHECK(r == WALLET_ERR_SEED_FAIL, "strict mode fails when chunk not at registered offset");

    free(buf);
}

static void test_t15_stats(void) {
    printf("\nT15: wallet_stats\n");

    size_t bsz = wallet_buf_size(2, 128, 6);
    uint8_t *buf = calloc(1, bsz);
    WalletCtx ctx;
    wallet_init(&ctx, buf, bsz, 2, 128, 6, WALLET_MODE_PATH, 12ull);
    wallet_add_file(&ctx, (const uint8_t*)"/a", 2, 1000, 0, 0);
    wallet_add_file(&ctx, (const uint8_t*)"/b", 2, 2000, 0, 0);

    uint8_t dummy[64] = {0};
    uint64_t s = chunk_seed(dummy); uint32_t c = chunk_cksum(dummy);
    wallet_add_coord(&ctx, 0, 0, s, c, theta_map(s), 0);
    wallet_add_coord(&ctx, 1, 0, s, c, theta_map(s), 3); /* drift */

    WalletStats st;
    wallet_stats(&ctx, bsz, &st);

    CHECK(st.file_count  == 2, "stats: 2 files");
    CHECK(st.coord_count == 2, "stats: 2 coords");
    CHECK(st.total_source_bytes == 3000, "stats: total_source_bytes");
    CHECK(st.drift_records == 1, "stats: 1 drift record");
    CHECK(st.mode == WALLET_MODE_PATH, "stats: mode=PATH");

    free(buf);
}

/* ══════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== pogls_coord_wallet tests (S11-C) ===\n");

    test_t01_size();
    test_t02_add_file();
    test_t03_add_coord();
    test_t04_seal();
    test_t05_verify_good();
    test_t06_verify_corrupt();
    test_t07_reconstruct_good();
    test_t08_seed_fail();
    test_t09_cksum_fail();
    test_t10_meta_warn();
    test_t11_fetch_single();
    test_t12_lookup();
    test_t13_multi_file();
    test_t14_drift();
    test_t15_stats();

    printf("\n=== Results: %d pass / %d fail ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
