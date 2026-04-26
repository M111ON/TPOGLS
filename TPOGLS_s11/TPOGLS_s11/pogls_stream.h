/*
 * pogls_stream.h — POGLS Streaming Layer (S6)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Role: dual-interface streaming on top of ReconContext
 *
 *   PULL  — caller drives: pogls_stream_next()
 *   PUSH  — callback driven: pogls_stream_run()
 *   Both share one state machine (PoglsStream) — zero logic duplication.
 *
 * Design:
 *   - Zero alloc — caller owns PoglsStream (stack/static)
 *   - No seek on init — lazy: each next() call drives one chunk
 *   - Range support — stream any chunk_start..chunk_end window
 *   - Progress built-in: current_idx / total_chunks → resume/checkpoint free
 *   - last_bytes: actual bytes in last chunk (for partial tail)
 *   - STREAM_DONE (1) distinguishes "done" from error — clean loop exit
 *
 * Pull pattern (default):
 *   PoglsStream s;
 *   pogls_stream_init(&s, &rc, 0, rc.fi->hdr->chunk_count - 1);
 *   uint8_t chunk[64]; uint32_t got;
 *   while (pogls_stream_next(&s, chunk, &got) == STREAM_OK) {
 *       // chunk ready — use immediately
 *   }
 *
 * Push pattern (pipeline/GPU/network):
 *   void my_cb(const uint8_t *chunk, uint32_t len, void *u) { ... }
 *   pogls_stream_init(&s, &rc, 0, rc.fi->hdr->chunk_count - 1);
 *   int r = pogls_stream_run(&s, my_cb, NULL);
 *
 * Partial / LLM-layer pattern:
 *   pogls_stream_init(&s, &rc, layer_start_chunk, layer_end_chunk);
 *   while (pogls_stream_next(&s, chunk, &got) == STREAM_OK) { ... }
 *
 * Frozen rules:
 *   - PHI constants from pogls_platform.h only
 *   - DiamondBlock 64B — DIAMOND_BLOCK_SIZE
 *   - integer only — no float
 *   - GPU never touches stream core
 *   - core_c/ ห้ามแตะ
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_STREAM_H
#define POGLS_STREAM_H

/*
 * Requires caller to include before this header:
 *   pogls_reconstruct.h → ReconContext, pogls_reconstruct_chunk,
 *                         RECON_OK, RECON_ERR_*, DIAMOND_BLOCK_SIZE
 */

#include "core/pogls_platform.h"
#include <stdint.h>
#include <string.h>

/* ── Return codes ────────────────────────────────────────────────── */
#define STREAM_OK        0   /* chunk written to out, keep going      */
#define STREAM_DONE      1   /* all chunks consumed — not an error    */
#define STREAM_ERR_NULL -1   /* null pointer argument                 */
#define STREAM_ERR_INIT -2   /* bad range (start > end or out of fi)  */
#define STREAM_ERR_RECON -3  /* underlying reconstruct error          */
                             /* inspect s->last_recon_err for detail  */

/* ── PoglsStream — state machine (zero alloc) ────────────────────── */
/*
 * Treat as opaque — use init/next/run API only.
 *
 * current_idx   : next chunk to emit (advances on each next() call)
 * total_chunks  : chunks in this window (end - start + 1)
 * chunk_start   : first chunk of window (absolute, into fi->records)
 * chunk_end     : last  chunk of window (inclusive)
 * last_bytes    : bytes written by most recent next() call
 *                 (64 for full chunks; < 64 for tail)
 * last_recon_err: raw RECON_ERR_* from last failed reconstruct call
 *                 (valid only when next() returns STREAM_ERR_RECON)
 */
typedef struct {
    const ReconContext *rc;           /* bound reconstruct context      */
    uint32_t            chunk_start;  /* window start (absolute idx)    */
    uint32_t            chunk_end;    /* window end   (absolute idx)    */
    uint32_t            current_idx;  /* next chunk to pull (absolute)  */
    uint32_t            total_chunks; /* window size (end-start+1)      */
    uint32_t            last_bytes;   /* bytes from most recent next()  */
    int                 last_recon_err; /* RECON_ERR_* on STREAM_ERR_RECON */
} PoglsStream;

/* ── pogls_stream_init ───────────────────────────────────────────── */
/*
 * Bind stream to a ReconContext window [chunk_start..chunk_end].
 *
 * For full-file stream:
 *   pogls_stream_init(&s, &rc, 0, rc.fi->hdr->chunk_count - 1)
 *
 * Returns STREAM_OK or STREAM_ERR_NULL / STREAM_ERR_INIT.
 */
static inline int pogls_stream_init(PoglsStream        *s,
                                     const ReconContext *rc,
                                     uint32_t            chunk_start,
                                     uint32_t            chunk_end)
{
    if (!s || !rc || !rc->fi) return STREAM_ERR_NULL;
    if (chunk_start > chunk_end)                        return STREAM_ERR_INIT;
    if (chunk_end >= rc->fi->hdr->chunk_count)          return STREAM_ERR_INIT;

    s->rc              = rc;
    s->chunk_start     = chunk_start;
    s->chunk_end       = chunk_end;
    s->current_idx     = chunk_start;
    s->total_chunks    = chunk_end - chunk_start + 1;
    s->last_bytes      = 0;
    s->last_recon_err  = 0;
    return STREAM_OK;
}

/* ── pogls_stream_next ── CORE PULL ENGINE ───────────────────────── */
/*
 * Pull one chunk from the stream.
 *
 * @s        : initialised PoglsStream
 * @out      : caller buffer, must be >= 64B (DIAMOND_BLOCK_SIZE)
 * @bytes_out: set to actual bytes written (64 normally; < 64 last chunk)
 *
 * Returns:
 *   STREAM_OK         — chunk in out, bytes_out set, current_idx advanced
 *   STREAM_DONE       — window exhausted, out unchanged (clean exit)
 *   STREAM_ERR_NULL   — null argument
 *   STREAM_ERR_RECON  — reconstruct failure; inspect s->last_recon_err
 *
 * This is the single source of truth for both pull and push paths.
 */
static inline int pogls_stream_next(PoglsStream *s,
                                     uint8_t     *out,
                                     uint32_t    *bytes_out)
{
    if (!s || !out || !bytes_out) return STREAM_ERR_NULL;

    /* window exhausted → clean done */
    if (s->current_idx > s->chunk_end) {
        s->last_bytes = 0;
        return STREAM_DONE;
    }

    uint32_t got = 0;
    int r = pogls_reconstruct_chunk(s->rc, s->current_idx, out, &got);
    if (r != RECON_OK) {
        s->last_recon_err = r;
        return STREAM_ERR_RECON;
    }

    s->last_bytes = got;
    *bytes_out    = got;
    s->current_idx++;
    return STREAM_OK;
}

/* ── Push callback type ──────────────────────────────────────────── */
/*
 * chunk : pointer to 64B output buffer (valid only for duration of call)
 * len   : actual bytes (64 normally; < 64 for tail chunk)
 * user  : opaque pointer from pogls_stream_run
 */
typedef void (*pogls_stream_cb)(const uint8_t *chunk, uint32_t len, void *user);

/* ── pogls_stream_run ── PUSH WRAPPER ───────────────────────────── */
/*
 * Drive the stream to completion, invoking cb per chunk.
 * Internally: while (next()==OK) cb(...)
 *
 * Returns STREAM_DONE on success, or STREAM_ERR_* on failure.
 * s->last_recon_err holds detail on STREAM_ERR_RECON.
 *
 * Usage — GPU / network / pipeline integration:
 *   void send_chunk(const uint8_t *chunk, uint32_t len, void *u) {
 *       network_send(u, chunk, len);
 *   }
 *   pogls_stream_run(&s, send_chunk, socket);
 */
static inline int pogls_stream_run(PoglsStream     *s,
                                    pogls_stream_cb  cb,
                                    void            *user)
{
    if (!s || !cb) return STREAM_ERR_NULL;

    uint8_t  chunk[DIAMOND_BLOCK_SIZE];
    uint32_t got = 0;
    int      r;

    while ((r = pogls_stream_next(s, chunk, &got)) == STREAM_OK) {
        cb(chunk, got, user);
    }

    /* STREAM_DONE is success — propagate errors only */
    return (r == STREAM_DONE) ? STREAM_DONE : r;
}

/* ── pogls_stream_progress ───────────────────────────────────────── */
/*
 * How far through the window are we?
 * Returns chunks already emitted (0..total_chunks).
 *
 *   progress / total_chunks → fraction (integer: multiply by 100 for %)
 *   Use for: CLI progress bar, checkpoint, resume marker
 */
static inline uint32_t pogls_stream_progress(const PoglsStream *s)
{
    if (!s) return 0;
    return s->current_idx - s->chunk_start;
}

/* ── pogls_stream_reset ──────────────────────────────────────────── */
/*
 * Rewind to chunk_start without re-init.
 * Useful for: retry on error, replay, test.
 */
static inline void pogls_stream_reset(PoglsStream *s)
{
    if (!s) return;
    s->current_idx    = s->chunk_start;
    s->last_bytes     = 0;
    s->last_recon_err = 0;
}

/* ── pogls_stream_seek ───────────────────────────────────────────── */
/*
 * Jump to absolute chunk_id within the window.
 * Enables resume from checkpoint: save current_idx, restore with seek.
 *
 * Returns STREAM_OK or STREAM_ERR_INIT if chunk_id outside window.
 */
static inline int pogls_stream_seek(PoglsStream *s, uint32_t chunk_id)
{
    if (!s) return STREAM_ERR_NULL;
    if (chunk_id < s->chunk_start || chunk_id > s->chunk_end)
        return STREAM_ERR_INIT;
    s->current_idx    = chunk_id;
    s->last_bytes     = 0;
    s->last_recon_err = 0;
    return STREAM_OK;
}

#endif /* POGLS_STREAM_H */
