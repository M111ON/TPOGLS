/*
 * pogls_cli.c — POGLS Command Line Interface (S1)
 * ════════════════════════════════════════════════
 * Usage:
 *   pogls encode  <input>  <output.pogls>
 *   pogls decode  <input.pogls>  <output>
 *   pogls verify  <input.pogls>
 *   pogls read    <input.pogls>  --offset <N> --len <N>
 *
 * killer feature: `read` decodes only the requested byte range
 * — never loads full file into RAM.
 * ════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* pipeline headers (order matters) */
#include "../core/pogls_platform.h"
#include "../core/pogls_qrpn_phaseE.h"
#include "../core/pogls_fold.h"
#include "../pogls_compress.h"
#include "../pogls_scanner.h"
#include "../pogls_file_index.h"
#include "../pogls_reconstruct.h"
#include "../pogls_stream.h"
#include "../pogls_api.h"

/* ── I/O helpers ─────────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *len_out = got;
    return buf;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "error: cannot write '%s': %s\n", path, strerror(errno)); return -1; }
    size_t wrote = fwrite(buf, 1, len, f);
    fclose(f);
    return (wrote == len) ? 0 : -1;
}

/* ── parse --flag value from argv ───────────────────────────────── */
static const char *arg_get(int argc, char **argv, const char *flag)
{
    for (int i = 0; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i+1];
    return NULL;
}

/* ── open .pogls blob for decode (mmap-like: read whole file) ────── */
typedef struct {
    uint8_t   *blob;
    size_t     blob_len;
    FileIndex  fi;
    ReconContext rc;
    qrpn_ctx_t qrpn;
} PoglsFile;

static int pogls_open(PoglsFile *pf, const char *path)
{
    pf->blob = read_file(path, &pf->blob_len);
    if (!pf->blob) return -1;

    if (pf->blob_len < sizeof(PoglsBlobHeader)) {
        fprintf(stderr, "error: file too small to be a .pogls blob\n");
        free(pf->blob); return -1;
    }

    PoglsBlobHeader bh;
    memcpy(&bh, pf->blob, sizeof(bh));

    if (bh.magic != POGLS_BLOB_MAGIC) {
        fprintf(stderr, "error: not a POGLS file (magic mismatch)\n");
        free(pf->blob); return -1;
    }
    if (bh.version != POGLS_BLOB_VERSION) {
        fprintf(stderr, "error: unsupported version %u\n", bh.version);
        free(pf->blob); return -1;
    }
    if (bh.layout_sig != POGLS_LAYOUT_SIG) {
        fprintf(stderr, "error: ABI layout mismatch (layout_sig %08X != %08X)\n",
                bh.layout_sig, POGLS_LAYOUT_SIG);
        free(pf->blob); return -1;
    }

    size_t fidx_off = sizeof(PoglsBlobHeader);
    size_t comp_off = fidx_off + bh.fidx_bytes;

    if (comp_off + bh.comp_bytes > pf->blob_len) {
        fprintf(stderr, "error: blob truncated\n");
        free(pf->blob); return -1;
    }

    /* wire FileIndex directly into blob (zero-copy) */
    pf->fi.hdr      = (FileIndexHeader *)(pf->blob + fidx_off);
    pf->fi.records  = (FileChunkRecord *)(pf->blob + fidx_off + sizeof(FileIndexHeader));
    pf->fi.capacity = bh.chunk_count;

    qrpn_ctx_init(&pf->qrpn, 8);
    int r = pogls_reconstruct_init(&pf->rc, &pf->fi,
                                   pf->blob + comp_off, (uint64_t)bh.comp_bytes,
                                   &pf->qrpn);
    if (r != RECON_OK) {
        fprintf(stderr, "error: reconstruct init failed (%d)\n", r);
        free(pf->blob); return -1;
    }
    return 0;
}

static void pogls_close(PoglsFile *pf) { free(pf->blob); }

/* ════════════════════════════════════════════════════════════════════
 * COMMANDS
 * ════════════════════════════════════════════════════════════════════ */

/* pogls encode <input> <output.pogls> */
static int cmd_encode(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: pogls encode <input> <output.pogls>\n"); return 1; }
    const char *in_path  = argv[2];
    const char *out_path = argv[3];

    size_t in_len;
    uint8_t *in_buf = read_file(in_path, &in_len);
    if (!in_buf) return 1;

    size_t out_cap = pogls_encode_bound(in_len);
    uint8_t *out_buf = malloc(out_cap);
    if (!out_buf) { free(in_buf); return 1; }

    size_t out_len = out_cap;
    int r = pogls_encode(in_buf, in_len, out_buf, &out_len);
    free(in_buf);

    if (r != POGLS_OK) {
        fprintf(stderr, "error: encode failed (%d)\n", r);
        free(out_buf); return 1;
    }

    if (write_file(out_path, out_buf, out_len) != 0) {
        free(out_buf); return 1;
    }
    free(out_buf);

    double ratio = 100.0 * (double)out_len / (double)in_len;
    fprintf(stderr, "encoded: %zu → %zu bytes (blob %.1f%% of orig)\n",
            in_len, out_len, ratio);
    return 0;
}

/* pogls decode <input.pogls> <output> */
static int cmd_decode(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: pogls decode <input.pogls> <output>\n"); return 1; }

    size_t blob_len;
    uint8_t *blob = read_file(argv[2], &blob_len);
    if (!blob) return 1;

    size_t orig_size = pogls_decode_orig_size(blob, blob_len);
    if (orig_size == 0 && blob_len > sizeof(PoglsBlobHeader)) {
        fprintf(stderr, "error: invalid blob\n"); free(blob); return 1;
    }

    uint8_t *out = malloc(orig_size + DIAMOND_BLOCK_SIZE);
    if (!out) { free(blob); return 1; }

    size_t out_len = orig_size + DIAMOND_BLOCK_SIZE;
    int r = pogls_decode(blob, blob_len, out, &out_len);
    free(blob);

    if (r != POGLS_OK) {
        fprintf(stderr, "error: decode failed (%d)\n", r);
        free(out); return 1;
    }

    if (write_file(argv[3], out, out_len) != 0) { free(out); return 1; }
    free(out);
    fprintf(stderr, "decoded: %zu bytes\n", out_len);
    return 0;
}

/* pogls verify <input.pogls> */
static int cmd_verify(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: pogls verify <input.pogls>\n"); return 1; }

    PoglsFile pf;
    if (pogls_open(&pf, argv[2]) != 0) return 1;

    uint32_t total   = pf.fi.hdr->chunk_count;
    uint32_t ok      = 0;
    uint32_t bad     = 0;
    uint8_t  chunk[DIAMOND_BLOCK_SIZE];
    uint32_t got;

    for (uint32_t i = 0; i < total; i++) {
        int r = pogls_reconstruct_chunk(&pf.rc, i, chunk, &got);
        if (r == RECON_OK) ok++;
        else {
            fprintf(stderr, "  chunk %u: err=%d\n", i, r);
            bad++;
        }
    }

    PoglsBlobHeader bh; memcpy(&bh, pf.blob, sizeof(bh));
    fprintf(stderr, "verify: %u/%u chunks OK  |  orig_size=%llu  |  %s\n",
            ok, total, (unsigned long long)bh.orig_size,
            bad == 0 ? "PASS" : "FAIL");
    pogls_close(&pf);
    return bad > 0 ? 1 : 0;
}

/* pogls read <input.pogls> --offset <N> --len <N> */
static int cmd_read(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: pogls read <input.pogls> --offset <bytes> --len <bytes>\n");
        return 1;
    }

    const char *off_s = arg_get(argc, argv, "--offset");
    const char *len_s = arg_get(argc, argv, "--len");
    if (!off_s || !len_s) {
        fprintf(stderr, "error: --offset and --len required\n"); return 1;
    }

    uint64_t byte_offset = (uint64_t)strtoull(off_s, NULL, 0);
    uint32_t read_len    = (uint32_t)strtoul(len_s,  NULL, 0);

    PoglsFile pf;
    if (pogls_open(&pf, argv[2]) != 0) return 1;

    PoglsBlobHeader bh; memcpy(&bh, pf.blob, sizeof(bh));
    if (byte_offset + read_len > bh.orig_size) {
        fprintf(stderr, "error: range [%llu+%u] exceeds orig_size %llu\n",
                (unsigned long long)byte_offset, read_len,
                (unsigned long long)bh.orig_size);
        pogls_close(&pf); return 1;
    }

    uint8_t *out = malloc(read_len);
    uint32_t bytes_out = 0;
    int r = pogls_reconstruct_bytes_at(&pf.rc, byte_offset, out, read_len, &bytes_out);

    if (r != RECON_OK) {
        fprintf(stderr, "error: read failed (%d)\n", r);
        free(out); pogls_close(&pf); return 1;
    }

    /* write raw bytes to stdout */
    fwrite(out, 1, bytes_out, stdout);
    fflush(stdout);

    fprintf(stderr, "read: offset=%llu len=%u → %u bytes\n",
            (unsigned long long)byte_offset, read_len, bytes_out);
    free(out);
    pogls_close(&pf);
    return 0;
}

/* ── usage ─────────────────────────────────────────────────────── */
static void usage(void)
{
    fprintf(stderr,
        "pogls — POGLS Compressed Storage CLI\n"
        "\n"
        "  pogls encode  <input>        <output.pogls>\n"
        "  pogls decode  <input.pogls>  <output>\n"
        "  pogls verify  <input.pogls>\n"
        "  pogls read    <input.pogls>  --offset <bytes> --len <bytes>\n"
        "\n"
        "  read decodes only the requested byte range — never full file.\n"
    );
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "encode") == 0) return cmd_encode(argc, argv);
    if (strcmp(argv[1], "decode") == 0) return cmd_decode(argc, argv);
    if (strcmp(argv[1], "verify") == 0) return cmd_verify(argc, argv);
    if (strcmp(argv[1], "read")   == 0) return cmd_read(argc, argv);
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    usage(); return 1;
}
