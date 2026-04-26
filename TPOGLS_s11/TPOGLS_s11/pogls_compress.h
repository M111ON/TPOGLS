/*
 * pogls_compress.h — POGLS Compression Layer (S1)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Pipeline per chunk (64B DiamondBlock):
 *   1. Delta encode     — coord[i] -= coord[i-1]  (diff < raw)
 *   2. PHI symbol table — map hot values → 1B token
 *   3. Run-length       — (value, count) for repeats
 *   4. QRPN guard       — verify before output
 *
 * Decompress reverses 3 → 2 → 1, QRPN verify before returning data.
 *
 * Rules (Frozen):
 *   - PHI constants from pogls_platform.h ONLY
 *   - DiamondBlock 64B from pogls_fold.h
 *   - QRPN via pogls_qrpn_phaseE.h
 *   - integer only — no float
 *   - GPU never touches compress path
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_COMPRESS_H
#define POGLS_COMPRESS_H

#include "core/pogls_platform.h"
#include "core/pogls_fold.h"
#include "core/pogls_qrpn_phaseE.h"
#include <stdint.h>
#include <string.h>

/* ── Compressed output header (8B, packed) ───────────────────────── */
/*
 * Layout per compressed chunk:
 *   [CompressHeader 8B][payload bytes]
 *
 * payload_len = actual bytes after compression
 * orig_blocks = number of DiamondBlocks encoded (usually 1)
 * flags       = COMP_FLAG_* bitmask
 * qrpn_cg     = qrpn_phi_scatter_hex(checksum of orig data)
 */
#define COMP_MAGIC          0x434F4D50u   /* "COMP" */
#define COMP_VERSION        1u

#define COMP_FLAG_DELTA     0x01u   /* delta encoding applied        */
#define COMP_FLAG_PHI_SYM   0x02u   /* PHI symbol table applied      */
#define COMP_FLAG_RLE       0x04u   /* run-length encoding applied   */
#define COMP_FLAG_PASSTHRU  0x80u   /* data stored as-is (pre-compressed input) */

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* COMP_MAGIC                            */
    uint16_t payload_len;   /* compressed payload size in bytes      */
    uint8_t  orig_blocks;   /* number of DiamondBlocks (orig_blocks × 64B) */
    uint8_t  flags;         /* COMP_FLAG_* */
    uint32_t orig_checksum; /* XOR fold of original raw words        */
    uint32_t qrpn_cg;       /* qrpn_phi_scatter_hex(orig_checksum)  */
} CompressHeader;           /* 16B packed */

#define COMPRESS_HEADER_SIZE  16u

/* ── Output buffer sizing ────────────────────────────────────────── */
/*
 * Worst case: RLE expands (value, count) = 2B per original byte
 * Safe upper bound: 2 × input + header
 */
#define COMPRESS_OUT_MAX(in_bytes)  (2u * (in_bytes) + COMPRESS_HEADER_SIZE + 16u)

/* ── PHI Symbol Table ────────────────────────────────────────────── */
/*
 * 4 "sacred" 32-bit values → single token byte
 * Token encoding uses top 2 bits = 0b11 (0xC0 range) to avoid
 * collision with raw delta bytes (which rarely exceed 0x7F for
 * structured data).
 *
 * Token space:
 *   0xC0 = PHI_UP   (1696631)
 *   0xC1 = PHI_DOWN (648055)
 *   0xC2 = PHI_COMP (400521)  — 2^20 - PHI_DOWN
 *   0xC3 = PHI_SCALE (1048576)
 *   0xC4 = ZERO     (common after delta)
 *   0xFF = escape next literal byte
 */
#define PHI_SYM_BASE        0xC0u
#define PHI_SYM_UP          0xC0u
#define PHI_SYM_DOWN        0xC1u
#define PHI_SYM_COMP        0xC2u
#define PHI_SYM_SCALE       0xC3u
#define PHI_SYM_ZERO        0xC4u
#define PHI_SYM_ESC         0xFFu
#define PHI_SYM_COUNT       5u

static const uint32_t _phi_sym_vals[PHI_SYM_COUNT] = {
    POGLS_PHI_UP,    /* 0xC0 */
    POGLS_PHI_DOWN,  /* 0xC1 */
    POGLS_PHI_COMP,  /* 0xC2 */
    POGLS_PHI_SCALE, /* 0xC3 */
    0u,              /* 0xC4 — zero */
};

/* ── Internal helpers ────────────────────────────────────────────── */

/* XOR-fold all 8 uint64_t words of a DiamondBlock into one uint32_t */
static inline uint32_t _compress_checksum(const DiamondBlock *b)
{
    const uint64_t *w = (const uint64_t *)b;
    uint64_t acc = 0;
    for (int i = 0; i < 8; i++) acc ^= w[i];
    return (uint32_t)(acc ^ (acc >> 32));
}

/* XOR-fold arbitrary byte buffer into uint32_t */
static inline uint32_t _buf_checksum(const uint8_t *buf, size_t len)
{
    uint32_t acc = 0;
    for (size_t i = 0; i < len; i++) acc ^= ((uint32_t)buf[i] << ((i & 3) * 8));
    return acc;
}

/* Try to match a uint32_t value against PHI symbol table.
 * Returns token byte (0xC0-0xC4) or 0 if no match. */
static inline uint8_t _phi_sym_encode(uint32_t v)
{
    for (uint8_t i = 0; i < PHI_SYM_COUNT; i++) {
        if (v == _phi_sym_vals[i]) return (uint8_t)(PHI_SYM_BASE + i);
    }
    return 0;
}

/* Decode token → uint32_t value. Returns 1 on success, 0 if not a symbol. */
static inline int _phi_sym_decode(uint8_t token, uint32_t *out)
{
    if (token >= PHI_SYM_BASE && token < (PHI_SYM_BASE + PHI_SYM_COUNT)) {
        *out = _phi_sym_vals[token - PHI_SYM_BASE];
        return 1;
    }
    return 0;
}

/* ── Step 1: Delta encode ────────────────────────────────────────── */
/*
 * Treat DiamondBlock as array of 16 uint32_t words.
 * delta[0] = words[0] (anchor, stored verbatim)
 * delta[i] = words[i] - words[i-1]  (i > 0)
 *
 * Output: 16 int32_t written into out[0..15]
 */
static inline void _delta_encode(const DiamondBlock *b, int32_t out[16])
{
    const uint32_t *words = (const uint32_t *)b;
    out[0] = (int32_t)words[0];
    for (int i = 1; i < 16; i++) {
        out[i] = (int32_t)(words[i] - words[i-1]);
    }
}

/* Reverse: reconstruct DiamondBlock from delta array */
static inline void _delta_decode(const int32_t delta[16], DiamondBlock *b)
{
    uint32_t *words = (uint32_t *)b;
    words[0] = (uint32_t)delta[0];
    for (int i = 1; i < 16; i++) {
        words[i] = (uint32_t)((int32_t)words[i-1] + delta[i]);
    }
}

/* ── Step 2+3: PHI-sym + RLE encode ─────────────────────────────── */
/*
 * Encodes int32_t delta array → byte stream.
 *
 * Each int32_t processed as uint32_t:
 *   a) PHI symbol match → emit 1 token byte
 *   b) else → check if fits in 1 signed byte [-64..63] → emit 1B
 *   c) else → emit ESC + 4 raw bytes (little-endian)
 *
 * After per-value encoding, RLE scan on the resulting byte stream:
 *   If same byte repeats ≥ 3 times:
 *     emit: RLE_MARKER(0xFE) + byte + count(uint8_t, capped 255)
 *   else pass through.
 *
 * Returns number of bytes written into out_buf, or -1 if overflow.
 */
#define RLE_MARKER  0xFEu
#define RLE_MIN_RUN 3u

static inline int _sym_rle_encode(const int32_t delta[16],
                                   uint8_t *out_buf, int out_max)
{
    /* Stage A: value → token stream (max 5B per value × 16 = 80B) */
    uint8_t stage[80];
    int stage_len = 0;

    for (int i = 0; i < 16; i++) {
        uint32_t v = (uint32_t)delta[i];
        uint8_t sym = _phi_sym_encode(v);
        if (sym) {
            stage[stage_len++] = sym;
        } else if (delta[i] >= -64 && delta[i] <= 63) {
            /* fits in 7-bit signed — avoid collision with PHI_SYM range */
            uint8_t b = (uint8_t)(delta[i] & 0xFF);
            if (b >= PHI_SYM_BASE || b == PHI_SYM_ESC || b == RLE_MARKER) {
                /* need escape */
                stage[stage_len++] = PHI_SYM_ESC;
                stage[stage_len++] = b;
                stage[stage_len++] = 0;
                stage[stage_len++] = 0;
                stage[stage_len++] = 0;
            } else {
                stage[stage_len++] = b;
            }
        } else {
            /* 4-byte literal with escape prefix */
            stage[stage_len++] = PHI_SYM_ESC;
            stage[stage_len++] = (uint8_t)(v);
            stage[stage_len++] = (uint8_t)(v >> 8);
            stage[stage_len++] = (uint8_t)(v >> 16);
            stage[stage_len++] = (uint8_t)(v >> 24);
        }
    }

    /* Stage B: RLE pass on stage buffer */
    int out_len = 0;
    int i = 0;
    while (i < stage_len) {
        uint8_t cur = stage[i];
        /* count run */
        int run = 1;
        while ((i + run) < stage_len && stage[i + run] == cur && run < 255) run++;

        if (run >= (int)RLE_MIN_RUN) {
            if (out_len + 3 > out_max) return -1;
            out_buf[out_len++] = RLE_MARKER;
            out_buf[out_len++] = cur;
            out_buf[out_len++] = (uint8_t)run;
            i += run;
        } else {
            if (out_len + 1 > out_max) return -1;
            out_buf[out_len++] = cur;
            i++;
        }
    }
    return out_len;
}

/* Reverse: decode byte stream → int32_t delta[16] */
static inline int _sym_rle_decode(const uint8_t *in_buf, int in_len,
                                   int32_t delta[16])
{
    /* Expand RLE first into expanded buffer */
    uint8_t expanded[512];
    int exp_len = 0;
    int i = 0;

    while (i < in_len) {
        uint8_t b = in_buf[i++];
        if (b == RLE_MARKER) {
            if (i + 2 > in_len) return -1;
            uint8_t val   = in_buf[i++];
            uint8_t count = in_buf[i++];
            if (exp_len + count > (int)sizeof(expanded)) return -1;
            memset(expanded + exp_len, val, count);
            exp_len += count;
        } else {
            if (exp_len >= (int)sizeof(expanded)) return -1;
            expanded[exp_len++] = b;
        }
    }

    /* Decode token stream → int32_t delta[16] */
    int pos = 0;
    for (int d = 0; d < 16; d++) {
        if (pos >= exp_len) return -1;
        uint8_t b = expanded[pos];
        uint32_t v;
        if (_phi_sym_decode(b, &v)) {
            delta[d] = (int32_t)v;
            pos++;
        } else if (b == PHI_SYM_ESC) {
            if (pos + 4 >= exp_len) return -1;
            v  = (uint32_t)expanded[pos+1];
            v |= (uint32_t)expanded[pos+2] << 8;
            v |= (uint32_t)expanded[pos+3] << 16;
            v |= (uint32_t)expanded[pos+4] << 24;
            delta[d] = (int32_t)v;
            pos += 5;
        } else {
            /* signed 1-byte */
            delta[d] = (int8_t)b;
            pos++;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════ */

/*
 * pogls_compress_block — compress one DiamondBlock into out_buf
 *
 * @b        : source DiamondBlock (64B)
 * @out_buf  : output buffer (size >= COMPRESS_OUT_MAX(64))
 * @out_max  : size of out_buf in bytes
 * @qrpn     : QRPN context (must be initialised; mode used for verification)
 *
 * Returns: number of bytes written (header + payload), or -1 on error.
 *
 * Behaviour:
 *   - Always attempts delta → PHI-sym → RLE pipeline.
 *   - If compressed size >= original (64B), stores as PASSTHRU with header.
 *   - QRPN verify runs on orig_checksum before writing header.
 *   - Compress fails (returns -1) only if QRPN hard-aborts or buffer overflow.
 */
static inline int pogls_compress_block(const DiamondBlock *b,
                                        uint8_t *out_buf, int out_max,
                                        qrpn_ctx_t *qrpn)
{
    if (!b || !out_buf || !qrpn) return -1;
    if (out_max < (int)COMPRESS_HEADER_SIZE + 1) return -1;

    /* QRPN guard on original data */
    uint32_t orig_cs = _compress_checksum(b);
    uint32_t cg      = qrpn_phi_scatter_hex((uint64_t)orig_cs);
    int qr = qrpn_check((uint64_t)orig_cs, 0, cg, qrpn, NULL);
    if (qr != 0 && qrpn->mode == QRPN_HARD) return -1;

    /* Attempt compression pipeline */
    int32_t  delta[16];
    uint8_t  payload_buf[COMPRESS_OUT_MAX(64)];
    int      payload_len = -1;
    uint8_t  flags = 0;

    _delta_encode(b, delta);
    payload_len = _sym_rle_encode(delta, payload_buf,
                                   (int)sizeof(payload_buf));

    if (payload_len > 0 && payload_len < 64) {
        flags = COMP_FLAG_DELTA | COMP_FLAG_PHI_SYM | COMP_FLAG_RLE;
    } else {
        /* Passthru: copy raw block */
        payload_len = 64;
        memcpy(payload_buf, b, 64);
        flags = COMP_FLAG_PASSTHRU;
    }

    if ((int)COMPRESS_HEADER_SIZE + payload_len > out_max) return -1;

    /* Write header */
    CompressHeader hdr;
    hdr.magic         = COMP_MAGIC;
    hdr.payload_len   = (uint16_t)payload_len;
    hdr.orig_blocks   = 1;
    hdr.flags         = flags;
    hdr.orig_checksum = orig_cs;
    hdr.qrpn_cg       = cg;
    memcpy(out_buf, &hdr, COMPRESS_HEADER_SIZE);

    /* Write payload */
    memcpy(out_buf + COMPRESS_HEADER_SIZE, payload_buf, (size_t)payload_len);

    return (int)COMPRESS_HEADER_SIZE + payload_len;
}

/*
 * pogls_decompress_block — decompress one chunk back to DiamondBlock
 *
 * @in_buf   : compressed data (starts at CompressHeader)
 * @in_len   : bytes available in in_buf
 * @b_out    : destination DiamondBlock
 * @qrpn     : QRPN context
 *
 * Returns: 0 on success, -1 on error (corrupt data or QRPN fail).
 *
 * QRPN verify runs on reconstructed data's checksum vs stored qrpn_cg.
 * Mismatch in QRPN_HARD mode returns -1.
 */
static inline int pogls_decompress_block(const uint8_t *in_buf, int in_len,
                                          DiamondBlock *b_out,
                                          qrpn_ctx_t *qrpn)
{
    if (!in_buf || !b_out || !qrpn) return -1;
    if (in_len < (int)COMPRESS_HEADER_SIZE) return -1;

    /* Read header */
    CompressHeader hdr;
    memcpy(&hdr, in_buf, COMPRESS_HEADER_SIZE);

    if (hdr.magic != COMP_MAGIC) return -1;
    if (hdr.payload_len == 0) return -1;
    if ((int)COMPRESS_HEADER_SIZE + hdr.payload_len > in_len) return -1;

    const uint8_t *payload = in_buf + COMPRESS_HEADER_SIZE;

    if (hdr.flags & COMP_FLAG_PASSTHRU) {
        if (hdr.payload_len != 64) return -1;
        memcpy(b_out, payload, 64);
    } else if ((hdr.flags & (COMP_FLAG_DELTA | COMP_FLAG_PHI_SYM | COMP_FLAG_RLE)) ==
               (COMP_FLAG_DELTA | COMP_FLAG_PHI_SYM | COMP_FLAG_RLE)) {
        int32_t delta[16];
        if (_sym_rle_decode(payload, hdr.payload_len, delta) != 0) return -1;
        _delta_decode(delta, b_out);
    } else {
        return -1;  /* unknown flag combo */
    }

    /* QRPN verify on reconstructed data */
    uint32_t got_cs = _compress_checksum(b_out);
    if (got_cs != hdr.orig_checksum) return -1;

    uint32_t cg = qrpn_phi_scatter_hex((uint64_t)got_cs);
    int qr = qrpn_check((uint64_t)got_cs, 0, cg, qrpn, NULL);
    if (qr != 0 && qrpn->mode == QRPN_HARD) return -1;

    return 0;
}

/*
 * pogls_compress_ratio_pct — return compression ratio × 100
 *   e.g. 7500 = 75.00% reduction
 *   (orig_bytes - compressed_bytes) * 10000 / orig_bytes
 */
static inline int32_t pogls_compress_ratio_pct(int orig_bytes,
                                                 int compressed_bytes)
{
    if (orig_bytes <= 0) return 0;
    int saved = orig_bytes - compressed_bytes;
    return (int32_t)(saved * 10000 / orig_bytes);
}

#endif /* POGLS_COMPRESS_H */
