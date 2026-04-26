/*
 * test_chunk_index.c — S2: pogls_chunk_index.h round-trip + random access test
 *
 * Tests:
 *   T1: init + header integrity
 *   T2: add chunks (mix of compressed + passthru) → verify index
 *   T3: O(1) random access — get chunk_id in any order
 *   T4: offset monotonic check (verify catches gap/reorder)
 *   T5: rebuild from raw stream (recovery path)
 *   T6: stats — ratio_bp consistent with compress results
 *   T7: entry CRC tamper detection
 *   T8: capacity overflow guard
 */

#include "pogls_chunk_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

/* ── test helpers ─────────────────────────────────────────────────── */
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name, cond) do { \
    if (cond) { printf("  [PASS] %s\n", name); pass_count++; } \
    else       { printf("  [FAIL] %s  (line %d)\n", name, __LINE__); fail_count++; } \
} while(0)

/* build a deterministic DiamondBlock from a seed */
static void make_block(DiamondBlock *b, uint32_t seed)
{
    uint32_t *w = (uint32_t *)b;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1664525u + 1013904223u;   /* LCG */
        w[i] = seed;
    }
}

/* make an all-zero block (highly compressible) */
static void make_zero_block(DiamondBlock *b)
{
    memset(b, 0, sizeof(*b));
}

/* make a PHI-heavy block */
static void make_phi_block(DiamondBlock *b)
{
    uint32_t *w = (uint32_t *)b;
    for (int i = 0; i < 16; i++)
        w[i] = (i % 2 == 0) ? POGLS_PHI_UP : POGLS_PHI_DOWN;
}

/* compress a block and return total written bytes (hdr+payload) */
static int compress_to(const DiamondBlock *b, uint8_t *out, int out_max,
                        qrpn_ctx_t *qrpn)
{
    return pogls_compress_block(b, out, out_max, qrpn);
}

/* ══════════════════════════════════════════════════════════════════
 * TESTS
 * ══════════════════════════════════════════════════════════════════ */

/* T1: init */
static void t1_init(qrpn_ctx_t *qrpn)
{
    printf("\n=== T1: init + header integrity ===\n");
    uint8_t buf[CHUNK_INDEX_BUF_SIZE(8)];
    ChunkIndex ci;

    int r = chunk_index_init(&ci, buf, sizeof(buf), 8);
    TEST("init returns 0",          r == 0);
    TEST("magic correct",           ci.hdr->magic == CHUNK_INDEX_MAGIC);
    TEST("version correct",         ci.hdr->version == CHUNK_INDEX_VERSION);
    TEST("count=0 after init",      ci.hdr->count == 0);
    TEST("capacity=8",              ci.hdr->capacity == 8);
    TEST("total_orig=0",            ci.hdr->total_orig == 0);
    TEST("qrpn_cg set",             ci.hdr->qrpn_cg != 0);

    int vr = chunk_index_verify(&ci, NULL);
    TEST("verify passes empty index", vr == 0);

    /* bad buffer size */
    int r2 = chunk_index_init(&ci, buf, 4, 8);
    TEST("init rejects small buffer", r2 == -1);

    (void)qrpn;
}

/* T2: add + verify */
static void t2_add_verify(qrpn_ctx_t *qrpn)
{
    printf("\n=== T2: add chunks + verify ===\n");

    uint8_t idxbuf[CHUNK_INDEX_BUF_SIZE(16)];
    ChunkIndex ci;
    chunk_index_init(&ci, idxbuf, sizeof(idxbuf), 16);

    /* compressed data stream buffer */
    uint8_t stream[4096];
    size_t  stream_pos = 0;

    DiamondBlock b;
    uint8_t cbuf[COMPRESS_OUT_MAX(64)];

    /* add 4 chunks: zero, phi, random×2 */
    make_zero_block(&b);
    int n0 = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader h0; memcpy(&h0, cbuf, COMPRESS_HEADER_SIZE);
    int id0 = chunk_index_add(&ci, stream_pos, &h0, qrpn);
    memcpy(stream + stream_pos, cbuf, n0); stream_pos += n0;

    make_phi_block(&b);
    int n1 = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader h1; memcpy(&h1, cbuf, COMPRESS_HEADER_SIZE);
    int id1 = chunk_index_add(&ci, stream_pos, &h1, qrpn);
    memcpy(stream + stream_pos, cbuf, n1); stream_pos += n1;

    make_block(&b, 0xDEADBEEF);
    int n2 = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader h2; memcpy(&h2, cbuf, COMPRESS_HEADER_SIZE);
    int id2 = chunk_index_add(&ci, stream_pos, &h2, qrpn);
    memcpy(stream + stream_pos, cbuf, n2); stream_pos += n2;

    make_block(&b, 0x12345678);
    int n3 = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader h3; memcpy(&h3, cbuf, COMPRESS_HEADER_SIZE);
    int id3 = chunk_index_add(&ci, stream_pos, &h3, qrpn);
    memcpy(stream + stream_pos, cbuf, n3); stream_pos += n3;
    chunk_index_mark_last(&ci, (uint32_t)id3);

    TEST("id0=0",           id0 == 0);
    TEST("id1=1",           id1 == 1);
    TEST("id2=2",           id2 == 2);
    TEST("id3=3",           id3 == 3);
    TEST("count=4",         ci.hdr->count == 4);
    TEST("total_orig=256",  ci.hdr->total_orig == 4 * 64);

    int vr = chunk_index_verify(&ci, NULL);
    TEST("verify passes 4-chunk index", vr == 0);

    /* last flag on id3 */
    const ChunkEntry *e3 = chunk_index_get(&ci, 3);
    TEST("id3 has LAST flag", e3 && (e3->flags & CIDX_FLAG_LAST));

    (void)n0; (void)n1; (void)n2; (void)n3; (void)stream_pos;
}

/* T3: O(1) random access */
static void t3_random_access(qrpn_ctx_t *qrpn)
{
    printf("\n=== T3: O(1) random access ===\n");

    const int N = 64;
    uint8_t idxbuf[CHUNK_INDEX_BUF_SIZE(N)];
    ChunkIndex ci;
    chunk_index_init(&ci, idxbuf, sizeof(idxbuf), N);

    uint8_t stream[N * COMPRESS_OUT_MAX(64)];
    size_t  stream_pos = 0;
    uint8_t cbuf[COMPRESS_OUT_MAX(64)];
    DiamondBlock b;

    /* store N chunks with known seeds */
    for (int i = 0; i < N; i++) {
        make_block(&b, (uint32_t)(i * 7919));
        int n = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
        CompressHeader hdr; memcpy(&hdr, cbuf, COMPRESS_HEADER_SIZE);
        chunk_index_add(&ci, stream_pos, &hdr, qrpn);
        memcpy(stream + stream_pos, cbuf, n);
        stream_pos += n;
    }

    TEST("count=64", ci.hdr->count == N);

    /* access in reverse order — should all succeed */
    int all_valid = 1;
    uint64_t prev_off = UINT64_MAX;
    for (int i = N-1; i >= 0; i--) {
        const ChunkEntry *e = chunk_index_get(&ci, (uint32_t)i);
        if (!e) { all_valid = 0; break; }
        /* verify offset is within stream */
        if (e->offset >= stream_pos) { all_valid = 0; break; }
        (void)prev_off;
    }
    TEST("all 64 reverse lookups valid", all_valid);

    /* spot check: decompress chunk 31 from stream using index offset */
    const ChunkEntry *e31 = chunk_index_get(&ci, 31);
    DiamondBlock orig, rebuilt;
    make_block(&orig, (uint32_t)(31 * 7919));
    qrpn_ctx_t q2; qrpn_ctx_init(&q2, 8);
    int dr = pogls_decompress_block(stream + e31->offset,
                                     (int)(stream_pos - e31->offset),
                                     &rebuilt, &q2);
    TEST("spot decompress chunk 31 via index", dr == 0);
    TEST("chunk 31 data matches original",
         memcmp(&orig, &rebuilt, 64) == 0);

    /* spot check chunk 0 */
    const ChunkEntry *e0 = chunk_index_get(&ci, 0);
    make_block(&orig, 0);
    dr = pogls_decompress_block(stream + e0->offset,
                                 (int)(stream_pos - e0->offset),
                                 &rebuilt, &q2);
    TEST("spot decompress chunk 0 via index", dr == 0);
    TEST("chunk 0 data matches original",
         memcmp(&orig, &rebuilt, 64) == 0);
}

/* T4: offset monotonic check */
static void t4_monotonic(qrpn_ctx_t *qrpn)
{
    printf("\n=== T4: offset monotonic violation detection ===\n");

    uint8_t idxbuf[CHUNK_INDEX_BUF_SIZE(4)];
    ChunkIndex ci;
    chunk_index_init(&ci, idxbuf, sizeof(idxbuf), 4);

    uint8_t cbuf[COMPRESS_OUT_MAX(64)];
    DiamondBlock b;

    /* add 3 chunks with valid increasing offsets */
    make_zero_block(&b);
    int n = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader hdr; memcpy(&hdr, cbuf, COMPRESS_HEADER_SIZE);
    chunk_index_add(&ci, 0,   &hdr, qrpn);
    chunk_index_add(&ci, 100, &hdr, qrpn);
    chunk_index_add(&ci, 200, &hdr, qrpn);

    int vr = chunk_index_verify(&ci, NULL);
    TEST("monotonic offsets pass verify", vr == 0);

    /* manually corrupt offset of entry[2] to be less than entry[1] */
    ci.entries[2].offset = 50;
    ci.entries[2].entry_crc = _cidx_entry_crc(&ci.entries[2]);  /* fix crc so it passes crc check */

    uint32_t bad_id = 9999;
    vr = chunk_index_verify(&ci, &bad_id);
    TEST("non-monotonic offset caught", vr == -4);
    TEST("bad_id=2 reported", bad_id == 2);

    (void)n;
}

/* T5: rebuild from raw stream */
static void t5_rebuild(qrpn_ctx_t *qrpn)
{
    printf("\n=== T5: rebuild from raw stream ===\n");

    const int N = 8;
    uint8_t stream[N * COMPRESS_OUT_MAX(64)];
    size_t stream_pos = 0;
    uint8_t cbuf[COMPRESS_OUT_MAX(64)];
    DiamondBlock b;

    /* build a stream of N chunks */
    for (int i = 0; i < N; i++) {
        make_block(&b, (uint32_t)(i * 31337));
        int n = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
        memcpy(stream + stream_pos, cbuf, n);
        stream_pos += n;
    }

    /* rebuild index from raw stream (simulating index loss) */
    uint8_t idxbuf[CHUNK_INDEX_BUF_SIZE(N)];
    ChunkIndex ci;
    chunk_index_init(&ci, idxbuf, sizeof(idxbuf), N);
    qrpn_ctx_t q2; qrpn_ctx_init(&q2, 8);
    int found = chunk_index_rebuild(&ci, stream, stream_pos, &q2);

    TEST("rebuild finds 8 chunks",    found == N);
    TEST("count=8 after rebuild",     ci.hdr->count == N);
    TEST("verify passes rebuilt idx", chunk_index_verify(&ci, NULL) == 0);

    /* verify each rebuilt offset points to a valid CompressHeader */
    int all_magic = 1;
    for (int i = 0; i < N; i++) {
        const ChunkEntry *e = chunk_index_get(&ci, (uint32_t)i);
        if (!e) { all_magic = 0; break; }
        CompressHeader hdr;
        memcpy(&hdr, stream + e->offset, COMPRESS_HEADER_SIZE);
        if (hdr.magic != COMP_MAGIC) { all_magic = 0; break; }
    }
    TEST("all rebuilt offsets point to valid COMP_MAGIC", all_magic);
}

/* T6: stats */
static void t6_stats(qrpn_ctx_t *qrpn)
{
    printf("\n=== T6: stats consistency ===\n");

    uint8_t idxbuf[CHUNK_INDEX_BUF_SIZE(4)];
    ChunkIndex ci;
    chunk_index_init(&ci, idxbuf, sizeof(idxbuf), 4);

    uint8_t cbuf[COMPRESS_OUT_MAX(64)];
    DiamondBlock b;
    int total_comp = 0;

    /* zero block → compresses well */
    make_zero_block(&b);
    int n = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader hdr; memcpy(&hdr, cbuf, COMPRESS_HEADER_SIZE);
    chunk_index_add(&ci, (uint64_t)total_comp, &hdr, qrpn); total_comp += n;

    /* random block → likely passthru */
    make_block(&b, 0xCAFEBABE);
    n = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    memcpy(&hdr, cbuf, COMPRESS_HEADER_SIZE);
    chunk_index_add(&ci, (uint64_t)total_comp, &hdr, qrpn); total_comp += n;

    ChunkIndexStats s;
    chunk_index_stats(&ci, &s);

    TEST("stats count=2",       s.count == 2);
    TEST("stats total_orig=128", s.total_orig == 128);
    TEST("stats total_comp consistent",
         (int64_t)s.total_comp == (int64_t)total_comp);
    TEST("stats ratio_bp in range",
         s.ratio_bp > -5000 && s.ratio_bp <= 10000);
}

/* T7: entry CRC tamper detection */
static void t7_tamper(qrpn_ctx_t *qrpn)
{
    printf("\n=== T7: entry CRC tamper detection ===\n");

    uint8_t idxbuf[CHUNK_INDEX_BUF_SIZE(4)];
    ChunkIndex ci;
    chunk_index_init(&ci, idxbuf, sizeof(idxbuf), 4);

    uint8_t cbuf[COMPRESS_OUT_MAX(64)];
    DiamondBlock b;
    make_zero_block(&b);
    int n = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader hdr; memcpy(&hdr, cbuf, COMPRESS_HEADER_SIZE);
    chunk_index_add(&ci, 0, &hdr, qrpn);

    TEST("get entry[0] valid before tamper",
         chunk_index_get(&ci, 0) != NULL);

    /* tamper: flip a bit in orig_checksum without fixing entry_crc */
    ci.entries[0].orig_checksum ^= 0xFFFFFFFF;

    TEST("get entry[0] returns NULL after tamper",
         chunk_index_get(&ci, 0) == NULL);

    /* verify also catches it */
    uint32_t bad_id = 9999;
    int vr = chunk_index_verify(&ci, &bad_id);
    TEST("verify returns -3 on tampered entry", vr == -3);
    TEST("bad_id=0 reported", bad_id == 0);

    (void)n;
}

/* T8: capacity overflow */
static void t8_overflow(qrpn_ctx_t *qrpn)
{
    printf("\n=== T8: capacity overflow guard ===\n");

    uint8_t idxbuf[CHUNK_INDEX_BUF_SIZE(2)];
    ChunkIndex ci;
    chunk_index_init(&ci, idxbuf, sizeof(idxbuf), 2);

    uint8_t cbuf[COMPRESS_OUT_MAX(64)];
    DiamondBlock b;
    make_zero_block(&b);
    int n = compress_to(&b, cbuf, sizeof(cbuf), qrpn);
    CompressHeader hdr; memcpy(&hdr, cbuf, COMPRESS_HEADER_SIZE);

    int r0 = chunk_index_add(&ci, 0,   &hdr, qrpn);
    int r1 = chunk_index_add(&ci, 100, &hdr, qrpn);
    int r2 = chunk_index_add(&ci, 200, &hdr, qrpn);   /* should fail */

    TEST("r0=0 (first slot)",   r0 == 0);
    TEST("r1=1 (second slot)",  r1 == 1);
    TEST("r2=-1 (overflow)",    r2 == -1);
    TEST("count stays at 2",    ci.hdr->count == 2);

    (void)n;
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== pogls_chunk_index.h  S2 random-access test ===\n");

    /* struct size assertions */
    printf("\nChunkEntry       size = %zu B  (expected 28) %s\n",
           sizeof(ChunkEntry),
           sizeof(ChunkEntry) == 28 ? "✓" : "✗");
    printf("ChunkIndexHeader size = %zu B  (expected 40) %s\n",
           sizeof(ChunkIndexHeader),
           sizeof(ChunkIndexHeader) == 40 ? "✓" : "✗");

    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 8);

    t1_init(&qrpn);
    t2_add_verify(&qrpn);
    t3_random_access(&qrpn);
    t4_monotonic(&qrpn);
    t5_rebuild(&qrpn);
    t6_stats(&qrpn);
    t7_tamper(&qrpn);
    t8_overflow(&qrpn);

    printf("\n=== %d/%d passed ===\n", pass_count, pass_count + fail_count);

    /* QRPN summary */
    printf("QRPN shadow_fail=%" PRIu64 "  soft_rewind=%" PRIu64
           "  hard_abort=%" PRIu64 "\n",
           (uint64_t)atomic_load(&qrpn.shadow_fail),
           (uint64_t)atomic_load(&qrpn.soft_rewind),
           (uint64_t)atomic_load(&qrpn.hard_abort));

    return fail_count > 0 ? 1 : 0;
}
