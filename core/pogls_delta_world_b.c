/*
 * pogls_delta_world_b.c — POGLS V3.6  World B Delta Lanes 4-7
 * ============================================================
 *
 * World A (lanes 0-3) ไม่ถูกแตะ — ใช้ delta.c/h เดิมทั้งหมด
 * ไฟล์นี้เพิ่ม World B และ dual commit protocol เท่านั้น
 */

#include "pogls_delta_world_b.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ══════════════════════════════════════════════════════════════════════
   INTERNAL HELPERS
   ══════════════════════════════════════════════════════════════════════ */

/* World B lane id → index 0-3 */
static inline int _b_idx(uint8_t lane_id) {
    return (int)(lane_id - LANE_B_X);   /* 4→0, 5→1, 6→2, 7→3 */
}

/* World B file names (parallel ของ World A) */
static const char *B_DELTA_NAMES[LANE_B_COUNT] = {
    FNAME_B_LANE_X, FNAME_B_LANE_NX, FNAME_B_LANE_Y, FNAME_B_LANE_NY
};
static const char *B_PENDING_NAMES[LANE_B_COUNT] = {
    FNAME_B_PENDING_X, FNAME_B_PENDING_NX, FNAME_B_PENDING_Y, FNAME_B_PENDING_NY
};

static void _b_pending_path(const Delta_ContextB *ctx, int idx, char *out, size_t sz) {
    snprintf(out, sz, "%s/%s", ctx->pogls_dir_b, B_PENDING_NAMES[idx]);
}
static void _b_delta_path(const Delta_ContextB *ctx, int idx, char *out, size_t sz) {
    snprintf(out, sz, "%s/%s", ctx->pogls_dir_b, B_DELTA_NAMES[idx]);
}

/* SHA256 stub — ใช้ delta_crc32 chain แทนจนกว่าจะ link libcrypto */
static void _sha256_stub(const uint8_t *data, size_t len, uint8_t out[32]) {
    /* deterministic CRC chain — production ใช้ SHA256 จริง */
    uint32_t h[8];
    for (int i = 0; i < 8; i++) h[i] = 0x6a09e667u + i;
    uint32_t c = delta_crc32(0, data, len);
    for (int i = 0; i < 8; i++) {
        h[i] ^= c;
        c = delta_crc32(c, (uint8_t*)&h[i], 4);
    }
    memcpy(out, h, 32);
}

/* ══════════════════════════════════════════════════════════════════════
   WORLD B OPEN
   ══════════════════════════════════════════════════════════════════════ */

int delta_b_open(Delta_ContextB *ctx, const char *source_path) {
    if (!ctx || !source_path) return -1;
    memset(ctx, 0, sizeof(*ctx));

    strncpy(ctx->source_path, source_path, sizeof(ctx->source_path)-1);

    /* build world_b path: .pogls/<filename>/world_b/ */
    const char *base = strrchr(source_path, '/');
    base = base ? base + 1 : source_path;

    char pogls_root[600];
    const char *dir = source_path;
    char dir_buf[512] = ".";
    const char *slash = strrchr(source_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - source_path);
        if (dlen < sizeof(dir_buf)) {
            strncpy(dir_buf, source_path, dlen);
            dir_buf[dlen] = '\0';
        }
        dir = dir_buf;
    }

    snprintf(pogls_root,   sizeof(pogls_root),   "%s/.pogls/%s", dir, base);
    snprintf(ctx->pogls_dir_b, sizeof(ctx->pogls_dir_b), "%s/world_b", pogls_root);

    /* mkdir -p .pogls/<file>/world_b/ */
    {
        /* ensure parent dirs exist */
        char tmp[700]; snprintf(tmp, sizeof(tmp), "%s", pogls_root);
        for (char *p = tmp+1; *p; p++) {
            if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
        }
        mkdir(pogls_root, 0755);
    }
    if (mkdir(ctx->pogls_dir_b, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[delta_b] mkdir %s failed: %s\n",
                ctx->pogls_dir_b, strerror(errno));
        return -1;
    }

    /* open .pending files (append mode) */
    for (int i = 0; i < LANE_B_COUNT; i++) {
        char path[700];
        _b_pending_path(ctx, i, path, sizeof(path));
        ctx->lane_fd[i] = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
        if (ctx->lane_fd[i] < 0) {
            fprintf(stderr, "[delta_b] open %s failed: %s\n", path, strerror(errno));
            return -1;
        }
    }

    ctx->is_open = true;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   WORLD B APPEND
   ══════════════════════════════════════════════════════════════════════ */

int delta_b_append(Delta_ContextB *ctx, uint8_t lane_id,
                   uint64_t addr, const void *data, uint32_t size) {
    if (!ctx || !ctx->is_open) return -1;
    if (lane_id < LANE_B_X || lane_id > LANE_B_NY) return -1;
    if (!data || size == 0 || size > DELTA_MAX_PAYLOAD) return -1;

    int idx = _b_idx(lane_id);

    /* build delta block header (same format as World A) */
    Delta_BlockHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic        = DELTA_MAGIC;
    hdr.lane_id      = lane_id;
    hdr.version      = DELTA_VERSION;
    hdr.seq          = ++ctx->lane_seq[idx];
    hdr.addr         = addr;
    hdr.payload_size = size;

    /* CRC32 over header (without crc field) + payload */
    uint32_t crc = delta_crc32(0, &hdr, offsetof(Delta_BlockHeader, crc32));
    crc = delta_crc32(crc, data, size);
    hdr.crc32 = crc;

    int fd = ctx->lane_fd[idx];
    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (write(fd, data, size)        != (ssize_t)size) return -1;

    return (int)ctx->lane_seq[idx];
}

/* ══════════════════════════════════════════════════════════════════════
   WORLD B AUDIT
   ══════════════════════════════════════════════════════════════════════ */

int delta_b_audit(const Delta_ContextB *ctx) {
    if (!ctx || !ctx->is_open) return -1;

    /* X pair: lane_B_X.seq == lane_B_NX.seq */
    int64_t diff_x = (int64_t)ctx->lane_seq[0]   /* LANE_B_X  idx=0 */
                   - (int64_t)ctx->lane_seq[1];   /* LANE_B_NX idx=1 */

    /* Y pair: lane_B_Y.seq == lane_B_NY.seq */
    int64_t diff_y = (int64_t)ctx->lane_seq[2]   /* LANE_B_Y  idx=2 */
                   - (int64_t)ctx->lane_seq[3];   /* LANE_B_NY idx=3 */

    if (diff_x != 0 || diff_y != 0) {
        fprintf(stderr, "[delta_b] audit FAIL  diff_X=%lld  diff_Y=%lld\n",
                (long long)diff_x, (long long)diff_y);
        return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   WORLD B MERKLE COMPUTE
   ══════════════════════════════════════════════════════════════════════ */

int delta_b_merkle_compute(Delta_ContextB *ctx, uint8_t root_out[32]) {
    if (!ctx || !root_out) return -1;

    /* chain CRC32 ของ lane seqs → hash stub */
    uint8_t seed[32];
    memset(seed, 0, 32);
    for (int i = 0; i < LANE_B_COUNT; i++) {
        uint32_t c = delta_crc32(0, &ctx->lane_seq[i], sizeof(uint64_t));
        memcpy(seed + i*4, &c, 4);   /* interleave into seed */
    }
    _sha256_stub(seed, 32, root_out);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   WORLD B CLOSE
   ══════════════════════════════════════════════════════════════════════ */

int delta_b_close(Delta_ContextB *ctx) {
    if (!ctx) return -1;
    for (int i = 0; i < LANE_B_COUNT; i++) {
        if (ctx->lane_fd[i] > 0) {
            close(ctx->lane_fd[i]);
            ctx->lane_fd[i] = -1;
        }
    }
    ctx->is_open = false;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   COMBINED A+B OPEN / CLOSE
   ══════════════════════════════════════════════════════════════════════ */

int delta_ab_open(Delta_ContextAB *ctx, const char *source_path) {
    if (!ctx || !source_path) return -1;
    if (delta_open(&ctx->a, source_path) != 0) return -1;
    if (delta_b_open(&ctx->b, source_path) != 0) {
        delta_close(&ctx->a);
        return -1;
    }
    /* sync epoch */
    ctx->b.epoch = ctx->a.epoch;
    return 0;
}

int delta_ab_close(Delta_ContextAB *ctx) {
    if (!ctx) return -1;
    delta_close(&ctx->a);
    delta_b_close(&ctx->b);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   DUAL ATOMIC COMMIT
   ══════════════════════════════════════════════════════════════════════ */

int delta_ab_commit(Delta_ContextAB *ctx) {
    if (!ctx) return -1;

    /* ── Step 1-2  audit both worlds ──────────────────────────────── */
    if (delta_audit(&ctx->a) != 0) {
        fprintf(stderr, "[delta_ab] World A audit FAIL\n");
        return -1;
    }
    if (delta_b_audit(&ctx->b) != 0) {
        fprintf(stderr, "[delta_ab] World B audit FAIL\n");
        return -1;
    }

    /* ── Step 3  compute merkle_A ──────────────────────────────────── */
    uint8_t root_a[32], root_b[32], root_ab[32];
    if (delta_merkle_compute(&ctx->a, root_a) != 0) return -1;
    if (delta_b_merkle_compute(&ctx->b, root_b) != 0) return -1;

    /* ── Step 4  combined root = SHA256(root_a || root_b) ─────────── */
    uint8_t ab_seed[64];
    memcpy(ab_seed,      root_a, 32);
    memcpy(ab_seed + 32, root_b, 32);
    _sha256_stub(ab_seed, 64, root_ab);

    /* build dual merkle record */
    Delta_DualMerkleRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic  = DELTA_MAGIC;
    rec.epoch  = ctx->a.epoch + 1;
    for (int i = 0; i < LANE_COUNT;   i++) rec.seq_a[i] = ctx->a.lane_seq[i];
    for (int i = 0; i < LANE_B_COUNT; i++) rec.seq_b[i] = ctx->b.lane_seq[i];
    memcpy(rec.root_a,  root_a,  32);
    memcpy(rec.root_b,  root_b,  32);
    memcpy(rec.root_ab, root_ab, 32);
    rec.crc32 = delta_crc32(0, &rec, offsetof(Delta_DualMerkleRecord, crc32));

    /* ── Step 5  write merkle_AB.pending ──────────────────────────── */
    char pogls_root[700];
    const char *base = strrchr(ctx->a.source_path, '/');
    base = base ? base + 1 : ctx->a.source_path;
    char dir_buf[512] = ".";
    const char *slash = strrchr(ctx->a.source_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - ctx->a.source_path);
        if (dlen < sizeof(dir_buf)) {
            strncpy(dir_buf, ctx->a.source_path, dlen);
            dir_buf[dlen] = '\0';
        }
    }
    snprintf(pogls_root, sizeof(pogls_root), "%s/.pogls/%s", dir_buf, base);

    char pend_path[750], final_path[750];
    snprintf(pend_path,  sizeof(pend_path),  "%s/%s", pogls_root, FNAME_MERKLE_AB_PEND);
    snprintf(final_path, sizeof(final_path), "%s/%s", pogls_root, FNAME_MERKLE_AB);

    int mfd = open(pend_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mfd < 0) return -1;
    if (write(mfd, &rec, sizeof(rec)) != sizeof(rec)) { close(mfd); return -1; }
    fsync(mfd);
    close(mfd);

    /* ── Step 6  fsync World A pending lanes ─────────────────────── */
    for (int i = 0; i < LANE_COUNT; i++) {
        if (ctx->a.lane_fd[i] >= 0) fsync(ctx->a.lane_fd[i]);
    }

    /* ── Step 7  fsync World B pending lanes ─────────────────────── */
    for (int i = 0; i < LANE_B_COUNT; i++) {
        if (ctx->b.lane_fd[i] >= 0) fsync(ctx->b.lane_fd[i]);
    }

    /* ── Step 8  rename World A lanes (pending → delta) ──────────── */
    {
        const char *a_pend[] = {"lane_X.pending","lane_nX.pending","lane_Y.pending","lane_nY.pending"};
        const char *a_delt[] = {"lane_X.delta",  "lane_nX.delta",  "lane_Y.delta",  "lane_nY.delta"};
        for (int i = 0; i < LANE_COUNT; i++) {
            char src[700], dst[700];
            snprintf(src, sizeof(src), "%s/%s", pogls_root, a_pend[i]);
            snprintf(dst, sizeof(dst), "%s/%s", pogls_root, a_delt[i]);
            rename(src, dst);
        }
    }

    /* ── Step 9  rename World B lanes ────────────────────────────── */
    {
        char dir_b[750];
        snprintf(dir_b, sizeof(dir_b), "%s/world_b", pogls_root);
        for (int i = 0; i < LANE_B_COUNT; i++) {
            char src[800], dst[800];
            snprintf(src, sizeof(src), "%s/%s", dir_b, B_PENDING_NAMES[i]);
            snprintf(dst, sizeof(dst), "%s/%s", dir_b, B_DELTA_NAMES[i]);
            rename(src, dst);
        }
    }

    /* ── Step 10  rename merkle_AB.pending → snapshot.merkle  ATOMIC ─ */
    if (rename(pend_path, final_path) != 0) {
        fprintf(stderr, "[delta_ab] ATOMIC rename FAIL: %s\n", strerror(errno));
        return -1;
    }

    /* update epochs */
    ctx->a.epoch++;
    ctx->b.epoch = ctx->a.epoch;

    /* re-open pending files สำหรับ batch ถัดไป */
    for (int i = 0; i < LANE_COUNT; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/lane_%s.pending", pogls_root,
                 i==0?"X": i==1?"nX": i==2?"Y":"nY");
        close(ctx->a.lane_fd[i]);
        ctx->a.lane_fd[i] = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    }
    for (int i = 0; i < LANE_B_COUNT; i++) {
        char dir_b[750], path[800];
        snprintf(dir_b, sizeof(dir_b), "%s/world_b", pogls_root);
        snprintf(path,  sizeof(path),  "%s/%s", dir_b, B_PENDING_NAMES[i]);
        close(ctx->b.lane_fd[i]);
        ctx->b.lane_fd[i] = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   DUAL BOOT RECOVERY
   ══════════════════════════════════════════════════════════════════════ */

Delta_DualRecovery delta_ab_recover(const char *source_path) {
    Delta_DualRecovery result = {
        .world_a = DELTA_RECOVERY_NEW,
        .world_b = DELTA_RECOVERY_NEW,
    };

    /* World A — ใช้ delta_recover() เดิม
     * NOTE: delta_recover ถือว่า stat(.pending)==0 = TORN
     * แต่หลัง commit, .pending ถูก re-open ทันที (0 bytes, by design)
     * ถ้า TORN และ .pending ทุกตัวมีขนาด 0 = จริงๆ คือ CLEAN */
    result.world_a = (int)delta_recover(source_path);
    if (result.world_a == DELTA_RECOVERY_TORN) {
        /* re-verify: .pending all 0 bytes = actually CLEAN */
        const char *base2  = strrchr(source_path, '/');
        base2 = base2 ? base2 + 1 : source_path;
        char dir_buf2[512] = ".";
        const char *sl2 = strrchr(source_path, '/');
        if (sl2) {
            size_t dl = (size_t)(sl2 - source_path);
            if (dl < sizeof(dir_buf2)) { strncpy(dir_buf2, source_path, dl); dir_buf2[dl]=0; }
        }
        char pdir2[700];
        snprintf(pdir2, sizeof(pdir2), "%s/.pogls/%s", dir_buf2, base2);
        const char *pnames[] = {
            "lane_X.pending","lane_nX.pending","lane_Y.pending","lane_nY.pending"
        };
        int all_empty = 1;
        for (int i = 0; i < 4; i++) {
            char pp[800]; snprintf(pp, sizeof(pp), "%s/%s", pdir2, pnames[i]);
            struct stat ps;
            if (stat(pp, &ps) == 0 && ps.st_size > 0) { all_empty = 0; break; }
        }
        if (all_empty) result.world_a = DELTA_RECOVERY_CLEAN;
    }

    /* World B — ตรวจ world_b/ subdir */
    const char *base  = strrchr(source_path, '/');
    base = base ? base + 1 : source_path;
    char dir_buf[512] = ".";
    const char *slash = strrchr(source_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - source_path);
        if (dlen < sizeof(dir_buf)) {
            strncpy(dir_buf, source_path, dlen);
            dir_buf[dlen] = '\0';
        }
    }

    char dir_b[700];
    snprintf(dir_b, sizeof(dir_b), "%s/.pogls/%s/world_b", dir_buf, base);

    struct stat st;
    if (stat(dir_b, &st) != 0) {
        result.world_b = DELTA_RECOVERY_NEW;   /* ยังไม่มี world_b dir */
        return result;
    }

    /* ตรวจ pending ค้าง */
    int has_pending = 0;
    for (int i = 0; i < LANE_B_COUNT; i++) {
        char p[800];
        snprintf(p, sizeof(p), "%s/%s", dir_b, B_PENDING_NAMES[i]);
        struct stat ps;
        if (stat(p, &ps) == 0 && ps.st_size > 0) { has_pending = 1; break; }
    }

    /* ตรวจ combined merkle */
    char merkle_path[750];
    char pogls_root[700];
    snprintf(pogls_root,   sizeof(pogls_root),   "%s/.pogls/%s", dir_buf, base);
    snprintf(merkle_path, sizeof(merkle_path), "%s/%s", pogls_root, FNAME_MERKLE_AB);
    int has_merkle = (stat(merkle_path, &st) == 0);

    if (has_merkle && !has_pending)
        result.world_b = DELTA_RECOVERY_CLEAN;
    else if (has_pending)
        result.world_b = DELTA_RECOVERY_TORN;
    else
        result.world_b = DELTA_RECOVERY_NEW;

    return result;
}

/* ══════════════════════════════════════════════════════════════════════
   UTILITY
   ══════════════════════════════════════════════════════════════════════ */

const char *delta_b_lane_name(uint8_t lane_id) {
    switch (lane_id) {
        case LANE_B_X:  return "B_X";
        case LANE_B_NX: return "B_-X";
        case LANE_B_Y:  return "B_Y";
        case LANE_B_NY: return "B_-Y";
        default:        return "B_?";
    }
}
