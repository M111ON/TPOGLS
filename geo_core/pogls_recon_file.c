/*
 * pogls_recon_file.c  —  S83-B2
 * FILE* I/O → ReconContext (file-backed weight reader)
 *
 * Role: WeightStream calls rc->read_at(offset, buf, size) →
 *       this translates to fseek+fread on the .gguf file.
 *
 * Design: zero alloc, caller owns ReconContext on stack/static.
 *         read_at is the only hot path — everything else is setup.
 *
 * Frozen rules: 64B DiamondBlock align, integer only, no float.
 */

#include "geo_headers/pogls_recon_file.h"
#include <stdio.h>
#include <string.h>

/* ── open ─────────────────────────────────────────────────────────── */
int pogls_recon_file_open(ReconContext *rc, const char *path)
{
    if (!rc || !path) return RECON_ERR_NULL;
    memset(rc, 0, sizeof(*rc));

    rc->fp = fopen(path, "rb");
    if (!rc->fp) return RECON_ERR_IO;

    /* cache file size */
    if (fseek(rc->fp, 0, SEEK_END) != 0) { fclose(rc->fp); return RECON_ERR_IO; }
    rc->file_size = (uint64_t)ftell(rc->fp);
    rewind(rc->fp);

    strncpy(rc->path, path, RECON_PATH_LEN - 1);
    rc->ready = 1;
    return RECON_OK;
}

/* ── core read — hot path ─────────────────────────────────────────── */
/*
 * read_at(): fseek → fread exactly `size` bytes from `offset`.
 * Returns RECON_OK or RECON_ERR_*.
 * WeightStream calls this once per layer — not byte-by-byte.
 */
int pogls_recon_read_at(ReconContext *rc,
                         uint64_t      offset,
                         void         *buf,
                         uint64_t      size)
{
    if (!rc || !buf)   return RECON_ERR_NULL;
    if (!rc->ready)    return RECON_ERR_IO;
    if (size == 0)     return RECON_OK;

    /* bounds check */
    if (offset + size > rc->file_size) return RECON_ERR_BOUNDS;

    if (fseek(rc->fp, (long)offset, SEEK_SET) != 0) return RECON_ERR_IO;

    uint64_t got = (uint64_t)fread(buf, 1, (size_t)size, rc->fp);
    if (got != size) return RECON_ERR_IO;

    rc->total_reads++;
    rc->total_bytes += size;
    return RECON_OK;
}

/* ── close ────────────────────────────────────────────────────────── */
void pogls_recon_file_close(ReconContext *rc)
{
    if (!rc || !rc->fp) return;
    fclose(rc->fp);
    rc->fp    = NULL;
    rc->ready = 0;
}
