/*
 * pogls_recon_file.h  —  S83-B2
 * ReconContext: file-backed I/O for WeightStream
 *
 * Interface derived from WeightStream consumer needs:
 *   open → read_at (many times) → close
 */
#ifndef POGLS_RECON_FILE_H
#define POGLS_RECON_FILE_H

#include <stdint.h>
#include <stdio.h>

/* ── error codes ─────────────────────────────────────────────────── */
#define RECON_OK          0
#define RECON_ERR_NULL   -1
#define RECON_ERR_IO     -2
#define RECON_ERR_BOUNDS -3

#define RECON_PATH_LEN   256u

/* ── ReconContext (stack/static, ~280B) ──────────────────────────── */
typedef struct {
    FILE    *fp;
    uint64_t file_size;
    uint64_t total_reads;   /* stats only */
    uint64_t total_bytes;
    char     path[RECON_PATH_LEN];
    uint8_t  ready;
    uint8_t  _pad[7];
} ReconContext;

/* ── API ─────────────────────────────────────────────────────────── */
int  pogls_recon_file_open  (ReconContext *rc, const char *path);
int  pogls_recon_read_at    (ReconContext *rc, uint64_t offset,
                              void *buf, uint64_t size);
void pogls_recon_file_close (ReconContext *rc);

#endif /* POGLS_RECON_FILE_H */
