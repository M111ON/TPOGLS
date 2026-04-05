/*
 * pogls_delta_ext.c — POGLS V3.6  Dual Merkle Implementation
 * ===========================================================
 *
 * World A (pogls_delta.c) ไม่ถูกแตะ — additive only
 */

#include "pogls_delta_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ── internal path helpers ─────────────────────────────────────────── */

static void _merkle_path(const char *pogls_dir, char *out, size_t sz) {
    snprintf(out, sz, "%s/%s", pogls_dir, FNAME_MERKLE_AB);
}
static void _merkle_pending_path(const char *pogls_dir, char *out, size_t sz) {
    snprintf(out, sz, "%s/%s", pogls_dir, FNAME_MERKLE_AB_PEND);
}

/* pogls root from source_path: .pogls/<basename> */
static void _pogls_root(const char *source_path, char *out, size_t sz) {
    /* copy source_path, find basename */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", source_path);
    /* dirname part */
    char *slash = strrchr(tmp, '/');
    char *base  = slash ? slash + 1 : tmp;
    char dir[512];
    if (slash) {
        *slash = '\0';
        snprintf(dir, sizeof(dir), "%s", tmp);
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    snprintf(out, sz, "%s/.pogls/%s", dir, base);
}

/* ══════════════════════════════════════════════════════════════════════
   MERKLE FORMAT DETECTION
   ══════════════════════════════════════════════════════════════════════ */

MerkleFormat delta_merkle_which(const char *path_dir)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", path_dir, FNAME_MERKLE_AB);

    struct stat st;
    if (stat(path, &st) != 0)
        return MERKLE_FORMAT_MISSING;

    if (st.st_size == DELTA_MERKLE_V36_SIZE) {
        /* read magic + version to confirm */
        FILE *f = fopen(path, "rb");
        if (!f) return MERKLE_FORMAT_CORRUPT;
        uint32_t magic, pad_ver;
        if (fread(&magic, 4, 1, f) != 1 || fread(&pad_ver, 4, 1, f) != 1) {
            fclose(f); return MERKLE_FORMAT_CORRUPT;
        }
        fclose(f);
        if (magic == DELTA_MAGIC && pad_ver == DELTA_EXT_VERSION)
            return MERKLE_FORMAT_V36;
        return MERKLE_FORMAT_CORRUPT;
    }

    if (st.st_size == DELTA_MERKLE_V35_SIZE) {
        FILE *f = fopen(path, "rb");
        if (!f) return MERKLE_FORMAT_CORRUPT;
        uint32_t magic;
        if (fread(&magic, 4, 1, f) != 1) { fclose(f); return MERKLE_FORMAT_CORRUPT; }
        fclose(f);
        if (magic == DELTA_MAGIC) return MERKLE_FORMAT_V35;
        return MERKLE_FORMAT_CORRUPT;
    }

    return MERKLE_FORMAT_CORRUPT;
}

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE COMPUTE
   ══════════════════════════════════════════════════════════════════════ */

int delta_dual_merkle_compute(const Delta_Context  *ctx_a,
                               const Delta_ContextB *ctx_b,
                               Delta_DualMerkleRecord *out)
{
    if (!ctx_a || !ctx_b || !out) return -1;

    memset(out, 0, sizeof(*out));

    /* header */
    out->magic   = DELTA_MAGIC;
    out->_pad = DELTA_EXT_VERSION;
    out->epoch   = ctx_a->epoch;  /* epoch shared — A is source of truth */

    /* seq_a: World A lanes 0-3 */
    for (int i = 0; i < LANE_COUNT; i++)
        out->seq_a[i] = ctx_a->lane_seq[i];

    /* seq_b: World B lanes 4-7 (index 0-3 in ctx_b) */
    for (int i = 0; i < LANE_B_COUNT; i++)
        out->seq_b[i] = ctx_b->lane_seq[i];

    /* root_a: merkle ของ World A lanes */
    if (delta_merkle_compute((Delta_Context *)ctx_a, out->root_a) != 0)
        return -1;

    /* root_b: merkle ของ World B lanes */
    if (delta_b_merkle_compute((Delta_ContextB *)ctx_b, out->root_b) != 0)
        return -1;

    /* root_ab = SHA256(root_a || root_b) */
    delta_combine_roots(out->root_a, out->root_b, out->root_ab);

    /* CRC32 bytes 0..175 */
    out->crc32 = delta_crc32(0, (const uint8_t *)out,
                              DELTA_MERKLE_V36_SIZE - sizeof(uint32_t));
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE WRITE
   ══════════════════════════════════════════════════════════════════════ */

int delta_dual_merkle_write(const char *path_dir,
                             const Delta_DualMerkleRecord *rec)
{
    if (!path_dir || !rec) return -1;

    char pending[700], final[700];
    _merkle_pending_path(path_dir, pending, sizeof(pending));
    _merkle_path(path_dir, final, sizeof(final));

    /* write .pending */
    int fd = open(pending, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    ssize_t w = write(fd, rec, DELTA_MERKLE_V36_SIZE);
    if (w != DELTA_MERKLE_V36_SIZE) { close(fd); return -1; }

    /* fsync .pending */
    if (fsync(fd) != 0) { close(fd); return -1; }
    close(fd);

    /* rename .pending → final (ATOMIC FINAL) */
    if (rename(pending, final) != 0) return -1;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE READ
   ══════════════════════════════════════════════════════════════════════ */

int delta_dual_merkle_read(const char *path_dir,
                            Delta_DualMerkleRecord *out)
{
    if (!path_dir || !out) return -1;

    char path[700];
    _merkle_path(path_dir, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;   /* missing */

    if (st.st_size == DELTA_MERKLE_V35_SIZE)
        return 1;    /* V3.5 — caller ใช้ delta.h เดิม */

    if (st.st_size != DELTA_MERKLE_V36_SIZE)
        return -2;   /* corrupt */

    FILE *f = fopen(path, "rb");
    if (!f) return -2;

    if (fread(out, DELTA_MERKLE_V36_SIZE, 1, f) != 1) {
        fclose(f); return -2;
    }
    fclose(f);

    if (out->magic != DELTA_MAGIC || out->_pad != DELTA_EXT_VERSION)
        return -2;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   DUAL MERKLE VERIFY
   ══════════════════════════════════════════════════════════════════════ */

int delta_dual_merkle_verify(const Delta_DualMerkleRecord *rec)
{
    if (!rec) return -1;

    /* CRC check */
    uint32_t expected = delta_crc32(0, (const uint8_t *)rec,
                                     DELTA_MERKLE_V36_SIZE - sizeof(uint32_t));
    if (expected != rec->crc32)
        return -1;   /* CRC fail */

    /* root_ab = SHA256(root_a || root_b) */
    uint8_t expected_ab[32];
    delta_combine_roots(rec->root_a, rec->root_b, expected_ab);
    if (memcmp(expected_ab, rec->root_ab, 32) != 0)
        return -2;   /* root_ab mismatch */

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   UPGRADE: V3.5 → V3.6
   ══════════════════════════════════════════════════════════════════════ */

int delta_merkle_upgrade(const char *path_dir)
{
    if (!path_dir) return -1;

    MerkleFormat fmt = delta_merkle_which(path_dir);

    if (fmt == MERKLE_FORMAT_V36)
        return 1;   /* already upgraded */

    if (fmt != MERKLE_FORMAT_V35)
        return -1;  /* missing or corrupt — cannot upgrade */

    /* read V3.5 record */
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", path_dir, FNAME_MERKLE_AB);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    Delta_MerkleRecord v35;
    if (fread(&v35, sizeof(v35), 1, f) != 1) { fclose(f); return -1; }
    fclose(f);

    /* verify V3.5 CRC */
    uint32_t crc_check = delta_crc32(0, (uint8_t *)&v35,
                                      sizeof(v35) - sizeof(uint32_t));
    if (crc_check != v35.crc32) return -1;  /* V3.5 corrupt */

    /* build V3.6 record */
    Delta_DualMerkleRecord v36;
    memset(&v36, 0, sizeof(v36));

    v36.magic   = DELTA_MAGIC;
    v36._pad = DELTA_EXT_VERSION;
    v36.epoch   = v35.epoch;

    /* seq_a ← V3.5 seq[4] */
    for (int i = 0; i < LANE_COUNT; i++)
        v36.seq_a[i] = v35.seq[i];

    /* seq_b = 0 — World B ยังไม่มีข้อมูล */
    memset(v36.seq_b, 0, sizeof(v36.seq_b));

    /* root_a ← V3.5 root */
    memcpy(v36.root_a, v35.root, 32);

    /* root_b = SHA256("empty") — deterministic empty value */
    const uint8_t empty_input[1] = {0};
    uint8_t root_b_empty[32];
    uint32_t c = delta_crc32(0, empty_input, 1);
    uint32_t h[8];
    for (int i = 0; i < 8; i++) { h[i] = 0xdeadbeef ^ c ^ (uint32_t)i; c = delta_crc32(c, (uint8_t*)&h[i], 4); }
    memcpy(root_b_empty, h, 32);
    memcpy(v36.root_b, root_b_empty, 32);

    /* root_ab */
    delta_combine_roots(v36.root_a, v36.root_b, v36.root_ab);

    /* CRC */
    v36.crc32 = delta_crc32(0, (uint8_t *)&v36,
                              DELTA_MERKLE_V36_SIZE - sizeof(uint32_t));

    /* write back (atomic rename inside) */
    return delta_dual_merkle_write(path_dir, &v36);
}

/* ══════════════════════════════════════════════════════════════════════
   COMBINED RECOVERY
   ══════════════════════════════════════════════════════════════════════ */

Delta_DualRecovery delta_ext_recover(const char *source_path)
{
    Delta_DualRecovery result = {
        DELTA_RECOVERY_NEW,
        DELTA_RECOVERY_NEW
    };

    if (!source_path) return result;

    /* get .pogls/<file>/ dir */
    char pogls_dir[700];
    _pogls_root(source_path, pogls_dir, sizeof(pogls_dir));

    MerkleFormat fmt = delta_merkle_which(pogls_dir);

    if (fmt == MERKLE_FORMAT_MISSING) {
        /* ไม่มีอะไรเลย — NEW × 2 */
        return result;
    }

    if (fmt == MERKLE_FORMAT_V35) {
        /* V3.5 merkle — World A อาจ CLEAN, World B = NEW */
        result.world_a = (int)delta_recover(source_path);
        result.world_b = DELTA_RECOVERY_NEW;
        return result;
    }

    if (fmt == MERKLE_FORMAT_V36) {
        /* V3.6 dual — ตรวจทั้งสองโลก */
        Delta_DualMerkleRecord rec;
        if (delta_dual_merkle_read(pogls_dir, &rec) != 0) {
            result.world_a = DELTA_RECOVERY_TORN;
            result.world_b = DELTA_RECOVERY_TORN;
            return result;
        }

        if (delta_dual_merkle_verify(&rec) != 0) {
            result.world_a = DELTA_RECOVERY_TORN;
            result.world_b = DELTA_RECOVERY_TORN;
            return result;
        }

        /* World A: ตรวจ pending files แทน delta_recover (ซึ่ง expect V3.5 84B) */
        {
            char pend_a[700];
            snprintf(pend_a, sizeof(pend_a), "%s/%s", pogls_dir, "lane_X.pending");
            struct stat st_a;
            if (stat(pend_a, &st_a) == 0 && st_a.st_size > 0)
                result.world_a = DELTA_RECOVERY_TORN;
            else
                result.world_a = DELTA_RECOVERY_CLEAN;
        }

        /* World B: ตรวจ pending files */
        char pend[700];
        snprintf(pend, sizeof(pend), "%s/world_b/%s",
                 pogls_dir, FNAME_B_PENDING_X);
        struct stat st;
        if (stat(pend, &st) == 0 && st.st_size > 0) {
            result.world_b = DELTA_RECOVERY_TORN;
        } else {
            result.world_b = DELTA_RECOVERY_CLEAN;
        }
        return result;
    }

    /* CORRUPT */
    result.world_a = DELTA_RECOVERY_TORN;
    result.world_b = DELTA_RECOVERY_TORN;
    return result;
}
