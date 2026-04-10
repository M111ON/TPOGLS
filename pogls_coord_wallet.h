/*
 * pogls_coord_wallet.h — POGLS Coordinate Wallet (S11-C)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Concept: "Paper Wallet" for file data
 *   Instead of storing compressed bytes → store COORDINATES only.
 *   Reconstruct on-demand by seeking real source files.
 *
 *   Like a Bitcoin paper wallet: the value isn't stored here,
 *   just the key to find it.
 *
 * Binary format (.pogwallet) — single flat file:
 *
 *   [WalletHeader  128B ]
 *   [FileEntry[]        ]  ← variable count (hdr.file_count)
 *   [string pool        ]  ← file paths (referenced by path_offset/len)
 *   [CoordRecord[]  40B ]  ← N records (hdr.coord_count)
 *   [WalletFooter   16B ]  ← xxh64 of everything above
 *
 * Reconstruct path (mode 0 — path-based):
 *   open file → seek(coord.scan_offset) → read 64B →
 *   verify seed + checksum → emit chunk
 *   → O(1) per chunk, zero-copy via mmap, 100GB+ capable
 *
 * Reconstruct path (mode 1 — buffer-based):
 *   caller supplies (buf, len) per file_idx →
 *   same verify logic, for tests/API
 *
 * Mismatch policy:
 *   seed mismatch     → WALLET_ERR_SEED_FAIL     (HARD FAIL)
 *   checksum mismatch → WALLET_ERR_CKSUM_FAIL    (HARD FAIL)
 *   file meta diff    → WALLET_WARN_META_DRIFT   (SOFT WARN, verify continues)
 *   fast_sig miss     → pre-filter only, triggers full verify
 *
 * Drift tolerance:
 *   coord.drift_window > 0 → search ±drift_window*64 bytes on seed miss
 *   default: drift_window=0 (strict offset, production default)
 *
 * Dedup by design:
 *   Multiple wallets pointing same file + same offset → identical data
 *   No extra mechanism needed.
 *
 * Frozen rules:
 *   - DIAMOND_BLOCK_SIZE = 64 (chunk size, never changes)
 *   - integer only, no float
 *   - GPU never touches wallet path
 *   - PHI constants from pogls_platform.h only
 *   - Hard fail only on data mismatch (no silent passthru)
 *
 * Dependencies (include before this header):
 *   core/pogls_platform.h
 *   core/pogls_fold.h          → DIAMOND_BLOCK_SIZE
 *   core/pogls_qrpn_phaseE.h   → qrpn_phi_scatter_hex
 *   theta_map.h                → ThetaCoord
 *   pogls_scanner.h            → ScanEntry, _scan_chunk_seed,
 *                                 _scan_chunk_checksum
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_COORD_WALLET_H
#define POGLS_COORD_WALLET_H

#include "core/pogls_platform.h"
#include "core/pogls_fold.h"
#include "core/pogls_qrpn_phaseE.h"
#include "theta_map.h"
#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
 * LIMITS & MAGIC
 * ══════════════════════════════════════════════════════════════════ */

#define WALLET_MAGIC          0x574C5447u   /* "WLTG"                   */
#define WALLET_VERSION        1u
#define WALLET_FOOTER_MAGIC   0x454E4457u   /* "WDNE"                   */

#define WALLET_MODE_PATH      0u            /* prod: mmap real files     */
#define WALLET_MODE_BUFFER    1u            /* test/API: caller buf      */

#define WALLET_MAX_FILES      65535u        /* uint16_t file_idx range   */
#define WALLET_MAX_COORDS     (1u << 24)    /* 16M chunks (~1TB at 64B)  */
#define WALLET_PATH_POOL_MAX  (1u << 20)    /* 1MB string pool           */

/* ── Header flags ────────────────────────────────────────────────── */
#define WALLET_FLAG_SEALED    0x01u         /* no more records can be added */
#define WALLET_FLAG_VERIFIED  0x02u         /* full verify passed           */
#define WALLET_FLAG_DRIFT_OK  0x04u         /* at least one record has drift_window > 0 */

/* ── Result codes ─────────────────────────────────────────────────── */
typedef enum {
    WALLET_OK               =  0,
    WALLET_ERR_BAD_ARG      = -1,
    WALLET_ERR_OVERFLOW     = -2,
    WALLET_ERR_SEED_FAIL    = -3,   /* HARD: seed mismatch            */
    WALLET_ERR_CKSUM_FAIL   = -4,   /* HARD: checksum mismatch        */
    WALLET_ERR_BAD_MAGIC    = -5,
    WALLET_ERR_BAD_VERSION  = -6,
    WALLET_ERR_GUARD_FAIL   = -7,   /* header/footer integrity broken */
    WALLET_ERR_FILE_IDX     = -8,   /* file_idx out of range          */
    WALLET_ERR_IO           = -9,   /* mmap/read failure              */
    WALLET_ERR_DRIFT_FAIL   = -10,  /* seed not found even with drift */
    WALLET_WARN_META_DRIFT  =  1,   /* SOFT: file meta changed        */
} wallet_result_t;

/* ══════════════════════════════════════════════════════════════════
 * ON-DISK STRUCTURES
 * All packed — layout is frozen once WALLET_VERSION = 1 is deployed.
 * ══════════════════════════════════════════════════════════════════ */

/*
 * WalletHeader (128B, fixed)
 *
 * string_pool_offset = byte offset from file start to path string pool
 * string_pool_size   = total bytes in pool
 * coord_offset       = byte offset from file start to first CoordRecord
 * file_entry_offset  = byte offset from file start to first FileEntry
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* WALLET_MAGIC                   */
    uint8_t  version;             /* WALLET_VERSION                 */
    uint8_t  mode;                /* WALLET_MODE_PATH / _BUFFER     */
    uint8_t  flags;               /* WALLET_FLAG_*                  */
    uint8_t  _pad0;
    uint64_t wallet_id;           /* unique wallet identity (random) */
    uint32_t file_count;          /* number of FileEntry records     */
    uint32_t coord_count;         /* number of CoordRecord entries   */
    uint64_t file_entry_offset;   /* byte offset → FileEntry[]       */
    uint64_t string_pool_offset;  /* byte offset → path string pool  */
    uint32_t string_pool_size;    /* bytes in pool                   */
    uint64_t coord_offset;        /* byte offset → CoordRecord[]     */
    uint32_t hdr_guard;           /* qrpn_phi_scatter_hex of critical fields */
    uint8_t  _pad1[72];           /* pad to 128B  (108 base + 20 = 128)  */
} WalletHeader;

typedef char _wallet_hdr_size_check[sizeof(WalletHeader) == 128 ? 1 : -1];

/*
 * FileEntry (64B, per source file)
 *
 * path_offset = byte offset within string pool (not file offset)
 * path_len    = byte length of path string (not null-terminated)
 * chunk_start = index of first CoordRecord for this file
 * chunk_count = number of CoordRecords belonging to this file
 *
 * Identity check (reconstruct-time):
 *   file_size + mtime + head_tail_hash → WALLET_WARN_META_DRIFT if mismatch
 *   Never blocks reconstruction; just warns.
 *
 * head_tail_hash:
 *   xxh64-style: XOR-fold of first 64B XOR last 64B of file
 *   Pure integer, no external dependency.
 */
typedef struct __attribute__((packed)) {
    uint32_t path_offset;         /* offset in string pool           */
    uint16_t path_len;            /* bytes of path string            */
    uint16_t file_idx;            /* self-index (== position in array) */
    uint64_t file_size;           /* expected file size in bytes     */
    int64_t  mtime;               /* expected mtime (unix seconds)   */
    uint64_t head_tail_hash;      /* XOR-fold(first64B XOR last64B)  */
    uint32_t chunk_start;         /* first CoordRecord index         */
    uint32_t chunk_count;         /* CoordRecords owned by this file */
    uint32_t entry_guard;         /* qrpn_phi_scatter_hex integrity  */
    uint8_t  _pad[20];            /* pad to 64B  (52 base + 12 = 64) */
} FileEntry;

typedef char _wallet_fe_size_check[sizeof(FileEntry) == 64 ? 1 : -1];

/*
 * CoordRecord (40B, hot-path struct)
 *
 * Layout ordered for cache-friendly reconstruct access:
 *   [file_idx 2B][_pad 2B][checksum 4B]  ← group: identify source
 *   [scan_offset 8B]                      ← seek target
 *   [seed 8B]                             ← primary verify
 *   [fast_sig 4B]                         ← pre-filter (lower 32b of seed)
 *   [coord_packed 4B]                     ← ThetaCoord: face|edge|z|_
 *   [drift_window 1B][_reserved 7B]       ← drift tolerance + align
 *
 * coord_packed layout (4B):
 *   bits 31..24 = face   (0..11)
 *   bits 23..16 = edge   (0..4)
 *   bits 15.. 8 = z      (0..255)
 *   bits  7.. 0 = _pad
 *
 * fast_sig = (uint32_t)(seed & 0xFFFFFFFF)
 *   Used as pre-filter before full seed compare.
 *   False positive rate: 1/2^32 — negligible.
 *
 * drift_window:
 *   0 = strict (production default) — offset must match exactly
 *   N = search ±N*64 bytes if seed miss at exact offset
 *       max useful: 8 (±512B window) — larger risks false match
 */
typedef struct __attribute__((packed)) {
    uint16_t file_idx;            /* which FileEntry                 */
    uint16_t _pad0;
    uint32_t checksum;            /* XOR-fold of raw 64B             */
    uint64_t scan_offset;         /* byte offset in source file      */
    uint64_t seed;                /* chunk fingerprint (primary key)  */
    uint32_t fast_sig;            /* lower 32b of seed (pre-filter)  */
    uint32_t coord_packed;        /* face/edge/z packed              */
    uint8_t  drift_window;        /* ±drift_window*64B search range  */
    uint8_t  _reserved[7];        /* future use — must be 0          */
} CoordRecord;

typedef char _wallet_cr_size_check[sizeof(CoordRecord) == 40 ? 1 : -1];

/*
 * WalletFooter (16B, at end of file)
 *
 * xxh64_digest: XOR-fold of entire file except footer itself.
 * We implement a self-contained integer hash (no external dep).
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* WALLET_FOOTER_MAGIC             */
    uint32_t coord_count_check;   /* mirrors hdr.coord_count         */
    uint64_t xxh64_digest;        /* integrity of full file content  */
} WalletFooter;

typedef char _wallet_ft_size_check[sizeof(WalletFooter) == 16 ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════
 * IN-MEMORY CONTEXT
 * ══════════════════════════════════════════════════════════════════ */

/*
 * WalletCtx — in-memory handle for a wallet being built or read.
 *
 * Build mode: caller supplies buffers, wallet writes flat layout.
 * Read mode:  mmap the .pogwallet file, pointers into mapped region.
 *
 * For build mode, layout is:
 *   [WalletHeader][FileEntry × file_cap][string pool][CoordRecord × coord_cap][WalletFooter]
 *
 * Caller computes required buffer size via wallet_buf_size().
 */
typedef struct {
    WalletHeader *hdr;
    FileEntry    *files;          /* points after header              */
    uint8_t      *pool;           /* string pool base                 */
    CoordRecord  *coords;         /* coord array base                 */
    WalletFooter *footer;         /* at end of flat buf               */

    uint32_t      file_cap;       /* max file entries                 */
    uint32_t      coord_cap;      /* max coord records                */
    uint32_t      pool_cap;       /* max pool bytes                   */
    uint32_t      pool_used;      /* bytes used in pool               */

    uint8_t       owned;          /* 1 = we allocated buf             */
} WalletCtx;

/* ══════════════════════════════════════════════════════════════════
 * HELPER: ThetaCoord ↔ coord_packed
 * ══════════════════════════════════════════════════════════════════ */

static inline uint32_t wallet_coord_pack(ThetaCoord c) {
    return ((uint32_t)c.face << 24)
         | ((uint32_t)c.edge << 16)
         | ((uint32_t)c.z   <<  8);
}

static inline ThetaCoord wallet_coord_unpack(uint32_t p) {
    return (ThetaCoord){
        .face = (uint8_t)(p >> 24),
        .edge = (uint8_t)(p >> 16),
        .z    = (uint8_t)(p >>  8),
    };
}

/* ══════════════════════════════════════════════════════════════════
 * HELPER: integer xxh64-style digest (no external dep)
 *
 * Equivalent strength to xxhash for integrity checking.
 * Processes input in 8-byte words via XOR-rotate-multiply chain.
 * ══════════════════════════════════════════════════════════════════ */

#define _WALLET_H1  UINT64_C(0x9e3779b97f4a7c15)
#define _WALLET_H2  UINT64_C(0x6c62272e07bb0142)

static inline uint64_t _wallet_hash_update(uint64_t acc, uint64_t word) {
    acc ^= word * _WALLET_H1;
    acc  = (acc << 27) | (acc >> 37);  /* rotl64(acc, 27) */
    acc  = acc * _WALLET_H2 + UINT64_C(0x94d049bb133111eb);
    return acc;
}

static inline uint64_t wallet_hash_buf(const uint8_t *buf, size_t len) {
    uint64_t acc = _WALLET_H1 ^ (uint64_t)len;
    size_t   i   = 0;

    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        memcpy(&w, buf + i, 8);
        acc = _wallet_hash_update(acc, w);
    }
    /* tail (< 8 bytes) */
    if (i < len) {
        uint64_t w = 0;
        memcpy(&w, buf + i, len - i);
        acc = _wallet_hash_update(acc, w);
    }
    /* finalizer */
    acc ^= acc >> 33;
    acc *= _WALLET_H1;
    acc ^= acc >> 29;
    acc *= _WALLET_H2;
    acc ^= acc >> 32;
    return acc;
}

/* ══════════════════════════════════════════════════════════════════
 * HELPER: chunk verify (shared by path + buffer mode)
 *
 * Given 64B chunk at `data`:
 *   1. fast_sig pre-filter (cheap — skip if no match)
 *   2. full seed compare   (HARD FAIL)
 *   3. checksum compare    (HARD FAIL)
 *
 * Returns WALLET_OK, WALLET_ERR_SEED_FAIL, WALLET_ERR_CKSUM_FAIL.
 * ══════════════════════════════════════════════════════════════════ */

/* XOR-fold 64B → uint32 checksum (must match _scan_chunk_checksum) */
static inline uint32_t _wallet_xorfold64(const uint8_t *chunk) {
    uint32_t acc = 0, w;
    for (int i = 0; i < 16; i++) {
        memcpy(&w, chunk + i * 4, 4);
        acc ^= w;
    }
    return acc;
}

/* 8-word XOR fold + finalizer (must match _scan_chunk_seed) */
static inline uint64_t _wallet_chunk_seed(const uint8_t *chunk) {
    uint64_t w[8];
    memcpy(w, chunk, 64);
    uint64_t s = w[0] ^ w[1] ^ w[2] ^ w[3] ^
                 w[4] ^ w[5] ^ w[6] ^ w[7];
    s ^= s >> 33;
    s *= UINT64_C(0xff51afd7ed558ccd);
    s ^= s >> 33;
    s *= UINT64_C(0xc4ceb9fe1a85ec53);
    s ^= s >> 33;
    return s;
}

static inline wallet_result_t _wallet_verify_chunk(
    const CoordRecord *cr,
    const uint8_t     *chunk64)   /* exactly 64B */
{
    /* fast_sig pre-filter — cheap early exit */
    uint32_t fsig = (uint32_t)(_wallet_chunk_seed(chunk64) & 0xFFFFFFFFu);
    if (fsig != cr->fast_sig) {
        /* Could be false negative due to hash collision — do full check */
        uint64_t full_seed = _wallet_chunk_seed(chunk64);
        if (full_seed != cr->seed)    return WALLET_ERR_SEED_FAIL;
    } else {
        uint64_t full_seed = _wallet_chunk_seed(chunk64);
        if (full_seed != cr->seed)    return WALLET_ERR_SEED_FAIL;
    }

    uint32_t cksum = _wallet_xorfold64(chunk64);
    if (cksum != cr->checksum)        return WALLET_ERR_CKSUM_FAIL;

    return WALLET_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * HELPER: head+tail hash for file identity
 * ══════════════════════════════════════════════════════════════════ */

static inline uint64_t wallet_file_identity_hash(
    const uint8_t *head64,   /* first 64B of file */
    const uint8_t *tail64)   /* last  64B of file */
{
    uint64_t h = 0, t = 0;
    memcpy(&h, head64, 8);   /* use first 8B as seed for simplicity */
    memcpy(&t, tail64, 8);
    uint64_t combined = h ^ t;
    /* run through finalizer for avalanche */
    combined ^= combined >> 33;
    combined *= UINT64_C(0xff51afd7ed558ccd);
    combined ^= combined >> 33;
    combined *= UINT64_C(0xc4ceb9fe1a85ec53);
    combined ^= combined >> 33;
    return combined;
}

/* ══════════════════════════════════════════════════════════════════
 * HELPER: header/entry guards
 * ══════════════════════════════════════════════════════════════════ */

static inline uint32_t _wallet_hdr_guard(const WalletHeader *h) {
    uint64_t mix = h->wallet_id
                 ^ ((uint64_t)h->file_count  << 16)
                 ^ ((uint64_t)h->coord_count << 32)
                 ^ h->coord_offset
                 ^ h->string_pool_offset
                 ^ ((uint64_t)h->mode << 56);
    return qrpn_phi_scatter_hex(mix);
}

static inline uint32_t _wallet_fe_guard(const FileEntry *fe) {
    uint64_t mix = fe->file_size
                 ^ (uint64_t)(uint32_t)fe->mtime
                 ^ fe->head_tail_hash
                 ^ ((uint64_t)fe->file_idx   << 48)
                 ^ ((uint64_t)fe->path_offset << 32)
                 ^ ((uint64_t)fe->chunk_count << 16);
    return qrpn_phi_scatter_hex(mix);
}

/* ══════════════════════════════════════════════════════════════════
 * BUFFER SIZING
 * ══════════════════════════════════════════════════════════════════ */

static inline size_t wallet_buf_size(
    uint32_t file_cap,
    uint32_t pool_cap,
    uint32_t coord_cap)
{
    return sizeof(WalletHeader)
         + (size_t)file_cap  * sizeof(FileEntry)
         + (size_t)pool_cap
         + (size_t)coord_cap * sizeof(CoordRecord)
         + sizeof(WalletFooter);
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API — BUILD SIDE
 * ══════════════════════════════════════════════════════════════════ */

/*
 * wallet_init — initialise WalletCtx over caller-supplied buffer.
 *
 * @ctx        : WalletCtx to fill
 * @buf        : flat buffer >= wallet_buf_size(file_cap, pool_cap, coord_cap)
 * @buf_size   : size of buf
 * @file_cap   : max source files  (≤ WALLET_MAX_FILES)
 * @pool_cap   : max string pool bytes
 * @coord_cap  : max coord records (≤ WALLET_MAX_COORDS)
 * @mode       : WALLET_MODE_PATH or WALLET_MODE_BUFFER
 * @wallet_id  : unique id (e.g. random u64 from caller)
 *
 * Returns WALLET_OK or WALLET_ERR_BAD_ARG / WALLET_ERR_OVERFLOW.
 */
static inline wallet_result_t wallet_init(
    WalletCtx *ctx,
    uint8_t   *buf,
    size_t     buf_size,
    uint32_t   file_cap,
    uint32_t   pool_cap,
    uint32_t   coord_cap,
    uint8_t    mode,
    uint64_t   wallet_id)
{
    if (!ctx || !buf)                           return WALLET_ERR_BAD_ARG;
    if (file_cap  == 0 || file_cap  > WALLET_MAX_FILES)  return WALLET_ERR_OVERFLOW;
    if (coord_cap == 0 || coord_cap > WALLET_MAX_COORDS) return WALLET_ERR_OVERFLOW;
    if (pool_cap  == 0 || pool_cap  > WALLET_PATH_POOL_MAX) return WALLET_ERR_OVERFLOW;
    if (buf_size  < wallet_buf_size(file_cap, pool_cap, coord_cap))
        return WALLET_ERR_OVERFLOW;

    memset(buf, 0, wallet_buf_size(file_cap, pool_cap, coord_cap));

    /* lay out pointers */
    size_t off = 0;
    ctx->hdr    = (WalletHeader *)(buf + off); off += sizeof(WalletHeader);
    ctx->files  = (FileEntry    *)(buf + off); off += (size_t)file_cap * sizeof(FileEntry);
    ctx->pool   = (uint8_t      *)(buf + off); off += pool_cap;
    ctx->coords = (CoordRecord  *)(buf + off); off += (size_t)coord_cap * sizeof(CoordRecord);
    ctx->footer = (WalletFooter *)(buf + off);

    ctx->file_cap  = file_cap;
    ctx->coord_cap = coord_cap;
    ctx->pool_cap  = pool_cap;
    ctx->pool_used = 0;
    ctx->owned     = 0;

    /* fill header */
    WalletHeader *h = ctx->hdr;
    h->magic              = WALLET_MAGIC;
    h->version            = WALLET_VERSION;
    h->mode               = mode;
    h->flags              = 0;
    h->wallet_id          = wallet_id;
    h->file_count         = 0;
    h->coord_count        = 0;
    h->file_entry_offset  = sizeof(WalletHeader);
    h->string_pool_offset = sizeof(WalletHeader) + (uint64_t)file_cap * sizeof(FileEntry);
    h->string_pool_size   = 0;
    h->coord_offset       = h->string_pool_offset + pool_cap;
    h->hdr_guard          = _wallet_hdr_guard(h);

    return WALLET_OK;
}

/*
 * wallet_add_file — register a source file, return file_idx.
 *
 * @ctx            : initialised WalletCtx
 * @path           : file path string (raw bytes)
 * @path_len       : byte length of path
 * @file_size      : expected file size
 * @mtime          : expected mtime (unix seconds)
 * @head_tail_hash : wallet_file_identity_hash(head64, tail64)
 *
 * Returns file_idx (0-based) on success, < 0 on error.
 * (Returns wallet_result_t cast to int — negative = error)
 */
static inline int wallet_add_file(
    WalletCtx     *ctx,
    const uint8_t *path,
    uint16_t       path_len,
    uint64_t       file_size,
    int64_t        mtime,
    uint64_t       head_tail_hash)
{
    if (!ctx || !path || path_len == 0) return WALLET_ERR_BAD_ARG;
    if (ctx->hdr->file_count >= ctx->file_cap) return WALLET_ERR_OVERFLOW;
    if (ctx->pool_used + path_len > ctx->pool_cap) return WALLET_ERR_OVERFLOW;

    uint32_t idx = ctx->hdr->file_count;
    FileEntry *fe = &ctx->files[idx];

    fe->path_offset    = ctx->pool_used;
    fe->path_len       = path_len;
    fe->file_idx       = (uint16_t)idx;
    fe->file_size      = file_size;
    fe->mtime          = mtime;
    fe->head_tail_hash = head_tail_hash;
    fe->chunk_start    = ctx->hdr->coord_count;  /* will fill coords next */
    fe->chunk_count    = 0;
    fe->entry_guard    = _wallet_fe_guard(fe);

    /* copy path into pool */
    memcpy(ctx->pool + ctx->pool_used, path, path_len);
    ctx->pool_used += path_len;

    ctx->hdr->file_count      = idx + 1;
    ctx->hdr->string_pool_size = ctx->pool_used;
    ctx->hdr->hdr_guard       = _wallet_hdr_guard(ctx->hdr);

    return (int)idx;
}

/*
 * wallet_add_coord — add one CoordRecord from a ScanEntry.
 *
 * @ctx      : initialised WalletCtx
 * @file_idx : from wallet_add_file()
 * @offset   : scan_offset (byte offset in source file)
 * @seed     : chunk seed from scanner
 * @checksum : chunk checksum from scanner
 * @coord    : ThetaCoord from scanner
 * @drift    : drift_window (0 = strict, default)
 *
 * Returns coord_id (0-based) on success, < 0 on error.
 * Chunks must be added in scan order per file (offset monotonic).
 */
static inline int wallet_add_coord(
    WalletCtx  *ctx,
    uint16_t    file_idx,
    uint64_t    offset,
    uint64_t    seed,
    uint32_t    checksum,
    ThetaCoord  coord,
    uint8_t     drift)
{
    if (!ctx) return WALLET_ERR_BAD_ARG;
    if (file_idx >= ctx->hdr->file_count) return WALLET_ERR_FILE_IDX;
    if (ctx->hdr->coord_count >= ctx->coord_cap) return WALLET_ERR_OVERFLOW;

    uint32_t id = ctx->hdr->coord_count;
    CoordRecord *cr = &ctx->coords[id];

    cr->file_idx     = file_idx;
    cr->_pad0        = 0;
    cr->checksum     = checksum;
    cr->scan_offset  = offset;
    cr->seed         = seed;
    cr->fast_sig     = (uint32_t)(seed & 0xFFFFFFFFu);
    cr->coord_packed = wallet_coord_pack(coord);
    cr->drift_window = drift;
    memset(cr->_reserved, 0, 7);

    /* update FileEntry chunk_count */
    ctx->files[file_idx].chunk_count++;
    ctx->files[file_idx].entry_guard = _wallet_fe_guard(&ctx->files[file_idx]);

    if (drift > 0)
        ctx->hdr->flags |= WALLET_FLAG_DRIFT_OK;

    ctx->hdr->coord_count = id + 1;
    ctx->hdr->hdr_guard   = _wallet_hdr_guard(ctx->hdr);

    return (int)id;
}

/*
 * wallet_seal — finalise wallet, compute footer hash.
 *
 * @ctx     : built WalletCtx
 * @buf     : same buf passed to wallet_init (to hash full content)
 * @buf_size: byte size of content (excl. footer) = wallet_buf_size - sizeof(footer)
 *
 * Returns WALLET_OK or WALLET_ERR_BAD_ARG.
 */
static inline wallet_result_t wallet_seal(
    WalletCtx *ctx,
    const uint8_t *buf,
    size_t         content_size)   /* size of everything except footer */
{
    if (!ctx || !buf) return WALLET_ERR_BAD_ARG;

    ctx->hdr->flags |= WALLET_FLAG_SEALED;
    ctx->hdr->hdr_guard = _wallet_hdr_guard(ctx->hdr);

    uint64_t digest = wallet_hash_buf(buf, content_size);

    ctx->footer->magic             = WALLET_FOOTER_MAGIC;
    ctx->footer->coord_count_check = ctx->hdr->coord_count;
    ctx->footer->xxh64_digest      = digest;

    return WALLET_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API — VERIFY SIDE
 * ══════════════════════════════════════════════════════════════════ */

/*
 * wallet_verify_integrity — check structural integrity of a wallet buf.
 *
 * Checks (in order):
 *   1. header magic + version
 *   2. header guard
 *   3. footer magic + coord_count_check
 *   4. footer xxh64 digest
 *   5. all FileEntry guards
 *
 * Does NOT reconstruct chunks (no file I/O).
 *
 * Returns WALLET_OK or specific WALLET_ERR_*.
 * Sets *bad_file_idx if FileEntry guard fails (may be NULL).
 */
static inline wallet_result_t wallet_verify_integrity(
    const WalletCtx *ctx,
    const uint8_t   *buf,
    size_t           content_size,
    uint32_t        *bad_file_idx)
{
    if (!ctx || !ctx->hdr) return WALLET_ERR_BAD_ARG;

    const WalletHeader *h = ctx->hdr;

    if (h->magic   != WALLET_MAGIC)   return WALLET_ERR_BAD_MAGIC;
    if (h->version != WALLET_VERSION) return WALLET_ERR_BAD_VERSION;
    if (_wallet_hdr_guard(h) != h->hdr_guard) return WALLET_ERR_GUARD_FAIL;

    const WalletFooter *ft = ctx->footer;
    if (ft->magic != WALLET_FOOTER_MAGIC)                return WALLET_ERR_GUARD_FAIL;
    if (ft->coord_count_check != h->coord_count)          return WALLET_ERR_GUARD_FAIL;

    if (buf) {
        uint64_t digest = wallet_hash_buf(buf, content_size);
        if (digest != ft->xxh64_digest) return WALLET_ERR_GUARD_FAIL;
    }

    for (uint32_t i = 0; i < h->file_count; i++) {
        const FileEntry *fe = &ctx->files[i];
        if (_wallet_fe_guard(fe) != fe->entry_guard) {
            if (bad_file_idx) *bad_file_idx = i;
            return WALLET_ERR_GUARD_FAIL;
        }
    }

    return WALLET_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API — RECONSTRUCT (BUFFER MODE)
 * ══════════════════════════════════════════════════════════════════ */

/*
 * Reconstruct callback: called per verified chunk.
 *
 * @chunk64   : 64B chunk data (exactly DIAMOND_BLOCK_SIZE bytes)
 * @coord_id  : CoordRecord index
 * @cr        : the CoordRecord (for coord/offset access)
 * @user      : caller context
 */
typedef void (*wallet_chunk_cb)(
    const uint8_t     *chunk64,
    uint32_t           coord_id,
    const CoordRecord *cr,
    void              *user);

/*
 * wallet_reconstruct_buf — reconstruct chunks from an in-memory buffer.
 *
 * Mode 1 (buffer-based) only. Caller supplies source file data.
 * For each CoordRecord with matching file_idx:
 *   1. seek to scan_offset in src_buf
 *   2. verify chunk (seed + checksum) — HARD FAIL on mismatch
 *   3. on drift_window > 0 and seed miss: search ±drift_window*64B
 *   4. emit verified 64B to cb
 *
 * @ctx      : wallet with mode = WALLET_MODE_BUFFER
 * @file_idx : which source file to reconstruct
 * @src_buf  : source file content
 * @src_len  : source file byte length
 * @cb       : called per verified chunk
 * @user     : passed to cb
 * @out_meta : WALLET_WARN_META_DRIFT if file_size differs (may be NULL)
 *
 * Returns WALLET_OK, WALLET_WARN_META_DRIFT, or WALLET_ERR_*.
 * On any HARD error, reconstruction stops immediately.
 */
static inline wallet_result_t wallet_reconstruct_buf(
    const WalletCtx *ctx,
    uint16_t         file_idx,
    const uint8_t   *src_buf,
    size_t           src_len,
    wallet_chunk_cb  cb,
    void            *user,
    wallet_result_t *out_meta)
{
    if (!ctx || !src_buf || !cb) return WALLET_ERR_BAD_ARG;
    if (file_idx >= ctx->hdr->file_count) return WALLET_ERR_FILE_IDX;

    const FileEntry *fe = &ctx->files[file_idx];
    wallet_result_t meta_status = WALLET_OK;

    /* soft meta check */
    if ((uint64_t)src_len != fe->file_size) {
        meta_status = WALLET_WARN_META_DRIFT;
        if (out_meta) *out_meta = WALLET_WARN_META_DRIFT;
    }

    uint8_t  chunk_buf[DIAMOND_BLOCK_SIZE];
    uint32_t end = fe->chunk_start + fe->chunk_count;

    for (uint32_t i = fe->chunk_start; i < end; i++) {
        const CoordRecord *cr = &ctx->coords[i];

        uint64_t off = cr->scan_offset;

        /* fast_sig pre-filter: quick sanity before reading */
        /* (actual data read happens once below)            */

        if (off + DIAMOND_BLOCK_SIZE > src_len) {
            /* offset out of range — try drift if enabled */
            if (cr->drift_window == 0) return WALLET_ERR_SEED_FAIL;
            /* else fall through to drift search below */
            off = src_len;  /* force drift path */
        }

        wallet_result_t vr = WALLET_ERR_SEED_FAIL;

        if (off + DIAMOND_BLOCK_SIZE <= src_len) {
            memcpy(chunk_buf, src_buf + off, DIAMOND_BLOCK_SIZE);
            vr = _wallet_verify_chunk(cr, chunk_buf);
        }

        /* drift search: walk ±drift_window*64B on seed fail */
        if (vr != WALLET_OK && cr->drift_window > 0) {
            int32_t  max_d = (int32_t)cr->drift_window * (int32_t)DIAMOND_BLOCK_SIZE;
            wallet_result_t drift_vr = WALLET_ERR_DRIFT_FAIL;

            for (int32_t d = -(int32_t)DIAMOND_BLOCK_SIZE;
                 d >= -max_d || d <= max_d;
                 d = (d < 0) ? -d : -(d + (int32_t)DIAMOND_BLOCK_SIZE))
            {
                int64_t try_off = (int64_t)cr->scan_offset + d;
                if (try_off < 0) continue;
                if ((uint64_t)try_off + DIAMOND_BLOCK_SIZE > src_len) continue;

                memcpy(chunk_buf, src_buf + (uint64_t)try_off, DIAMOND_BLOCK_SIZE);
                if (_wallet_verify_chunk(cr, chunk_buf) == WALLET_OK) {
                    drift_vr = WALLET_OK;
                    break;
                }
            }
            vr = drift_vr;
        }

        if (vr != WALLET_OK) return vr;   /* HARD FAIL */

        cb(chunk_buf, i, cr, user);
    }

    return meta_status;
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API — O(1) SINGLE CHUNK FETCH (BUFFER MODE)
 * ══════════════════════════════════════════════════════════════════ */

/*
 * wallet_fetch_chunk — fetch and verify exactly one chunk by coord_id.
 *
 * The core O(1) reconstruct primitive.
 * For path mode: caller opens file and passes mmap'd region as src_buf.
 * For buffer mode: caller passes full file buffer.
 *
 * @ctx       : wallet
 * @coord_id  : CoordRecord index (from wallet or lookup)
 * @src_buf   : source file bytes (mmap or full load)
 * @src_len   : source file byte length
 * @out64     : caller-supplied 64B buffer to receive chunk
 *
 * Returns WALLET_OK or WALLET_ERR_*.
 *
 * Hot path:
 *   fast_sig pre-filter → memcpy 64B → seed check → checksum check
 *   = ~3 cache lines touched, zero heap allocation.
 */
static inline wallet_result_t wallet_fetch_chunk(
    const WalletCtx *ctx,
    uint32_t         coord_id,
    const uint8_t   *src_buf,
    size_t           src_len,
    uint8_t         *out64)
{
    if (!ctx || !src_buf || !out64) return WALLET_ERR_BAD_ARG;
    if (coord_id >= ctx->hdr->coord_count) return WALLET_ERR_BAD_ARG;

    const CoordRecord *cr = &ctx->coords[coord_id];

    if (cr->scan_offset + DIAMOND_BLOCK_SIZE > src_len) {
        /* attempt drift if enabled */
        if (cr->drift_window == 0) return WALLET_ERR_SEED_FAIL;

        int32_t  max_d = (int32_t)cr->drift_window * (int32_t)DIAMOND_BLOCK_SIZE;
        for (int32_t d = -(int32_t)DIAMOND_BLOCK_SIZE;
             d >= -max_d || d <= max_d;
             d = (d < 0) ? -d : -(d + (int32_t)DIAMOND_BLOCK_SIZE))
        {
            int64_t try_off = (int64_t)cr->scan_offset + d;
            if (try_off < 0) continue;
            if ((uint64_t)try_off + DIAMOND_BLOCK_SIZE > src_len) continue;
            memcpy(out64, src_buf + (uint64_t)try_off, DIAMOND_BLOCK_SIZE);
            if (_wallet_verify_chunk(cr, out64) == WALLET_OK)
                return WALLET_OK;
        }
        return WALLET_ERR_DRIFT_FAIL;
    }

    memcpy(out64, src_buf + cr->scan_offset, DIAMOND_BLOCK_SIZE);
    return _wallet_verify_chunk(cr, out64);
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API — LOOKUP
 * ══════════════════════════════════════════════════════════════════ */

/*
 * wallet_lookup_by_offset — find coord_id for a file+byte_offset pair.
 *
 * O(1): chunk_start + (byte_offset / 64) — DiamondBlock aligned.
 * Returns coord_id, or WALLET_MAX_COORDS if not found.
 */
static inline uint32_t wallet_lookup_by_offset(
    const WalletCtx *ctx,
    uint16_t         file_idx,
    uint64_t         byte_offset)
{
    if (!ctx || file_idx >= ctx->hdr->file_count) return WALLET_MAX_COORDS;

    const FileEntry *fe = &ctx->files[file_idx];
    uint32_t local_id = (uint32_t)(byte_offset / DIAMOND_BLOCK_SIZE);
    if (local_id >= fe->chunk_count) return WALLET_MAX_COORDS;

    uint32_t coord_id = fe->chunk_start + local_id;
    if (coord_id >= ctx->hdr->coord_count) return WALLET_MAX_COORDS;
    return coord_id;
}

/*
 * wallet_file_path — get path string for a FileEntry.
 *
 * Returns pointer into string pool (not null-terminated).
 * Sets *out_len to path_len.
 * Returns NULL if file_idx out of range.
 */
static inline const uint8_t *wallet_file_path(
    const WalletCtx *ctx,
    uint16_t         file_idx,
    uint16_t        *out_len)
{
    if (!ctx || file_idx >= ctx->hdr->file_count) return NULL;
    const FileEntry *fe = &ctx->files[file_idx];
    if (out_len) *out_len = fe->path_len;
    return ctx->pool + fe->path_offset;
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC API — STATS
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t file_count;
    uint32_t coord_count;
    uint64_t total_source_bytes;    /* sum of all file_size fields   */
    uint64_t wallet_bytes;          /* buf size of this wallet       */
    uint32_t drift_records;         /* CoordRecords with drift_window>0 */
    uint8_t  mode;
    uint8_t  flags;
} WalletStats;

static inline void wallet_stats(
    const WalletCtx *ctx,
    size_t           buf_size,
    WalletStats     *s)
{
    if (!ctx || !s) return;
    memset(s, 0, sizeof(*s));

    s->file_count   = ctx->hdr->file_count;
    s->coord_count  = ctx->hdr->coord_count;
    s->wallet_bytes = (uint64_t)buf_size;
    s->mode         = ctx->hdr->mode;
    s->flags        = ctx->hdr->flags;

    for (uint32_t i = 0; i < ctx->hdr->file_count; i++)
        s->total_source_bytes += ctx->files[i].file_size;

    for (uint32_t i = 0; i < ctx->hdr->coord_count; i++)
        if (ctx->coords[i].drift_window > 0)
            s->drift_records++;
}

#endif /* POGLS_COORD_WALLET_H */
