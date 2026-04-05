/*
 * pogls_delta.c — Delta Lane Writer + Boot Recovery Scanner
 * POGLS V3.5  Phase 1: 4-Lane Crash Recovery
 *
 * Single Source of Truth: snapshot.merkle
 *   exists + valid  = committed
 *   missing / stale = pending → discard → fallback
 */

#include "pogls_delta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dirent.h>

/* ══════════════════════════════════════════════════════════════════════
   SHA256  (self-contained — no openssl dependency)
   ══════════════════════════════════════════════════════════════════════ */

typedef struct { uint32_t s[8]; uint8_t buf[64]; uint64_t len; uint32_t blen; } SHA256_CTX;

static const uint32_t _K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define RR(v,n) (((v)>>(n))|((v)<<(32-(n))))
#define CH(e,f,g) (((e)&(f))^(~(e)&(g)))
#define MA(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define S0(a) (RR(a,2)^RR(a,13)^RR(a,22))
#define S1(e) (RR(e,6)^RR(e,11)^RR(e,25))
#define G0(x) (RR(x,7)^RR(x,18)^((x)>>3))
#define G1(x) (RR(x,17)^RR(x,19)^((x)>>10))

static void _sha256_transform(SHA256_CTX *c, const uint8_t *d) {
    uint32_t W[64], a,b,e,f,g,h,T1,T2;
    uint32_t cc=c->s[0],cb=c->s[1],ca=c->s[2],cd=c->s[3];
    a=cc;b=cb;uint32_t ccc=ca;uint32_t ccd=cd;
    e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for(int i=0;i<16;i++) W[i]=((uint32_t)d[i*4]<<24)|((uint32_t)d[i*4+1]<<16)|((uint32_t)d[i*4+2]<<8)|(uint32_t)d[i*4+3];
    for(int i=16;i<64;i++) W[i]=G1(W[i-2])+W[i-7]+G0(W[i-15])+W[i-16];
    a=c->s[0];b=c->s[1];uint32_t cv=c->s[2];uint32_t cw=c->s[3];
    e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for(int i=0;i<64;i++){
        T1=h+S1(e)+CH(e,f,g)+_K[i]+W[i];
        T2=S0(a)+MA(a,b,cv);
        h=g;g=f;f=e;e=cw+T1;cw=cv;cv=b;b=a;a=T1+T2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cv;c->s[3]+=cw;
    c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
    (void)cc;(void)cb;(void)ca;(void)cd;(void)ccc;(void)ccd;
}
static void SHA256_Init(SHA256_CTX *c){
    c->s[0]=0x6a09e667;c->s[1]=0xbb67ae85;c->s[2]=0x3c6ef372;c->s[3]=0xa54ff53a;
    c->s[4]=0x510e527f;c->s[5]=0x9b05688c;c->s[6]=0x1f83d9ab;c->s[7]=0x5be0cd19;
    c->len=0;c->blen=0;
}
static void SHA256_Update(SHA256_CTX *c, const void *in, size_t len){
    const uint8_t *p=in;
    while(len>0){
        uint32_t n=64-c->blen; if(n>len)n=(uint32_t)len;
        memcpy(c->buf+c->blen,p,n);c->blen+=n;p+=n;len-=n;
        if(c->blen==64){_sha256_transform(c,c->buf);c->len+=512;c->blen=0;}
    }
}
static void SHA256_Final(uint8_t *out, SHA256_CTX *c){
    c->len+=(uint64_t)c->blen*8;
    c->buf[c->blen++]=0x80;
    if(c->blen>56){while(c->blen<64)c->buf[c->blen++]=0;_sha256_transform(c,c->buf);c->blen=0;}
    while(c->blen<56)c->buf[c->blen++]=0;
    for(int i=7;i>=0;i--){c->buf[56+(7-i)]=(uint8_t)(c->len>>(i*8));}
    _sha256_transform(c,c->buf);
    for(int i=0;i<8;i++){out[i*4]=(uint8_t)(c->s[i]>>24);out[i*4+1]=(uint8_t)(c->s[i]>>16);out[i*4+2]=(uint8_t)(c->s[i]>>8);out[i*4+3]=(uint8_t)c->s[i];}
}
static void SHA256(const uint8_t *d, size_t n, uint8_t *out){
    SHA256_CTX c; SHA256_Init(&c); SHA256_Update(&c,d,n); SHA256_Final(out,&c);
}

/* SHA256 — self-contained, no external dependency */

/* ══════════════════════════════════════════════════════════════════════
   CRC32  (IEEE 802.3)
   ══════════════════════════════════════════════════════════════════════ */

static uint32_t _crc_table[256];
static bool     _crc_ready = false;

static void _crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        _crc_table[i] = c;
    }
    _crc_ready = true;
}

uint32_t delta_crc32(uint32_t crc, const void *data, size_t len) {
    if (!_crc_ready) _crc_init();
    const uint8_t *p = data;
    crc ^= 0xFFFFFFFFu;
    while (len--) crc = _crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ══════════════════════════════════════════════════════════════════════
   UTILITY
   ══════════════════════════════════════════════════════════════════════ */

const char *delta_lane_name(uint8_t lane_id) {
    switch (lane_id) {
        case LANE_X:  return "X";
        case LANE_NX: return "-X";
        case LANE_Y:  return "Y";
        case LANE_NY: return "-Y";
        default:      return "?";
    }
}

const char *delta_recovery_str(Delta_RecoveryResult r) {
    switch (r) {
        case DELTA_RECOVERY_CLEAN: return "CLEAN";
        case DELTA_RECOVERY_TORN:  return "TORN→fallback";
        case DELTA_RECOVERY_NEW:   return "NEW";
        default:                   return "ERROR";
    }
}

static const char *_pending_name(uint8_t lane_id) {
    switch (lane_id) {
        case LANE_X:  return FNAME_PENDING_X;
        case LANE_NX: return FNAME_PENDING_NX;
        case LANE_Y:  return FNAME_PENDING_Y;
        case LANE_NY: return FNAME_PENDING_NY;
        default:      return NULL;
    }
}

static const char *_delta_name(uint8_t lane_id) {
    switch (lane_id) {
        case LANE_X:  return FNAME_LANE_X;
        case LANE_NX: return FNAME_LANE_NX;
        case LANE_Y:  return FNAME_LANE_Y;
        case LANE_NY: return FNAME_LANE_NY;
        default:      return NULL;
    }
}

/* write_all — loop write */
static int _write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* read_all — loop read, return 1 on EOF, -1 on error */
static int _read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return 1;
        if (r <  0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* mkdir -p สำหรับ single level */
static int _mkdir_p(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;
    return mkdir(path, 0755);
}

/* build path: base/name → dst */
static void _join(char *dst, size_t dsz,
                  const char *base, const char *name) {
    snprintf(dst, dsz, "%s/%s", base, name);
}

/* ══════════════════════════════════════════════════════════════════════
   MERKLE
   ══════════════════════════════════════════════════════════════════════ */

int delta_merkle_compute(Delta_Context *ctx, uint8_t root_out[32]) {
    /*
     * Merkle root = SHA256(SHA256(lane_X) || SHA256(lane_nX) ||
     *                      SHA256(lane_Y) || SHA256(lane_nY))
     * ใช้ .pending files ที่ยังเปิดอยู่
     */
    SHA256_CTX outer;
    SHA256_Init(&outer);

    for (int i = 0; i < LANE_COUNT; i++) {
        int fd = ctx->lane_fd[i];
        if (fd < 0) {
            /* lane ว่าง — hash ของ empty string */
            uint8_t empty_hash[32];
            SHA256((const uint8_t *)"", 0, empty_hash);
            SHA256_Update(&outer, empty_hash, 32);
            continue;
        }

        /* seek to beginning, hash ทุก byte */
        if (lseek(fd, 0, SEEK_SET) < 0) return -1;

        SHA256_CTX inner;
        SHA256_Init(&inner);

        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            SHA256_Update(&inner, buf, (size_t)n);
        if (n < 0) return -1;

        uint8_t lane_hash[32];
        SHA256_Final(lane_hash, &inner);
        SHA256_Update(&outer, lane_hash, 32);

        /* seek back to end for continued appending */
        lseek(fd, 0, SEEK_END);
    }

    SHA256_Final(root_out, &outer);
    return 0;
}

/* write snapshot.merkle.pending */
static int _write_merkle_pending(Delta_Context *ctx) {
    char path[1024];
    _join(path, sizeof(path), ctx->pogls_dir, FNAME_MERKLE_PENDING);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    Delta_MerkleRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = DELTA_MAGIC;
    rec.epoch = ctx->epoch + 1;   /* next epoch */
    for (int i = 0; i < LANE_COUNT; i++)
        rec.seq[i] = ctx->lane_seq[i];

    if (delta_merkle_compute(ctx, rec.root) != 0) {
        close(fd); return -1;
    }

    /* CRC covers bytes 0..sizeof-4 */
    rec.crc32 = delta_crc32(0, &rec,
                             offsetof(Delta_MerkleRecord, crc32));

    int r = _write_all(fd, &rec, sizeof(rec));
    close(fd);
    return r;
}

/* read + verify snapshot.merkle */
static int _read_merkle(const char *pogls_dir,
                         Delta_MerkleRecord *out) {
    char path[1024];
    _join(path, sizeof(path), pogls_dir, FNAME_MERKLE);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    Delta_MerkleRecord rec;
    int r = _read_all(fd, &rec, sizeof(rec));
    close(fd);
    if (r != 0) return -1;

    if (rec.magic != DELTA_MAGIC) return -1;

    /* verify CRC */
    uint32_t stored = rec.crc32;
    rec.crc32 = 0;
    uint32_t computed = delta_crc32(0, &rec,
                                     offsetof(Delta_MerkleRecord, crc32));
    rec.crc32 = stored;
    if (computed != stored) return -1;

    if (out) *out = rec;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   AUDIT
   ══════════════════════════════════════════════════════════════════════ */

int delta_audit(const Delta_Context *ctx) {
    /*
     * Invariant (integer exact):
     *   lane_X.seq  - lane_nX.seq == 0
     *   lane_Y.seq  - lane_nY.seq == 0
     */
    int64_t diff_x = (int64_t)ctx->lane_seq[LANE_X]
                   - (int64_t)ctx->lane_seq[LANE_NX];
    int64_t diff_y = (int64_t)ctx->lane_seq[LANE_Y]
                   - (int64_t)ctx->lane_seq[LANE_NY];

    if (diff_x != 0) {
        fprintf(stderr, "delta_audit: X+(-X) = %lld ≠ 0\n",
                (long long)diff_x);
        return -1;
    }
    if (diff_y != 0) {
        fprintf(stderr, "delta_audit: Y+(-Y) = %lld ≠ 0\n",
                (long long)diff_y);
        return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   OPEN
   ══════════════════════════════════════════════════════════════════════ */

int delta_open(Delta_Context *ctx, const char *source_path) {
    if (!ctx || !source_path) return -1;
    memset(ctx, 0, sizeof(*ctx));

    strncpy(ctx->source_path, source_path, sizeof(ctx->source_path) - 1);

    /* ── สร้าง .pogls/ directory ──────────────────────────────────── */
    /* หา parent directory ของ source file */
    char parent[512];
    strncpy(parent, source_path, sizeof(parent) - 1);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = '\0';
    else       strncpy(parent, ".", sizeof(parent) - 1);

    char pogls_root[600];
    snprintf(pogls_root, sizeof(pogls_root), "%s/%s", parent, POGLS_DIR);
    if (_mkdir_p(pogls_root) != 0 && errno != EEXIST) return -1;

    /* .pogls/<filename>/ */
    const char *basename = strrchr(source_path, '/');
    basename = basename ? basename + 1 : source_path;
    snprintf(ctx->pogls_dir, sizeof(ctx->pogls_dir),
             "%s/%s", pogls_root, basename);
    if (_mkdir_p(ctx->pogls_dir) != 0 && errno != EEXIST) return -1;

    /* ── restore epoch จาก merkle ────────────────────────────────── */
    Delta_MerkleRecord rec;
    if (_read_merkle(ctx->pogls_dir, &rec) == 0) {
        ctx->epoch = rec.epoch;
        for (int i = 0; i < LANE_COUNT; i++)
            ctx->lane_seq[i] = rec.seq[i];
    }

    /* ── เปิด .pending files ─────────────────────────────────────── */
    for (int i = 0; i < LANE_COUNT; i++) {
        char path[1024];
        _join(path, sizeof(path), ctx->pogls_dir, _pending_name(i));
        int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            /* cleanup */
            for (int j = 0; j < i; j++)
                if (ctx->lane_fd[j] >= 0) close(ctx->lane_fd[j]);
            return -1;
        }
        ctx->lane_fd[i] = fd;
    }

    ctx->is_open = true;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   APPEND
   ══════════════════════════════════════════════════════════════════════ */

int delta_append(Delta_Context *ctx, uint8_t lane_id,
                 uint64_t addr, const void *data, uint32_t size) {
    if (!ctx || !ctx->is_open) return -1;
    if (lane_id >= LANE_COUNT)  return -1;
    if (!data || size == 0 || size > DELTA_MAX_PAYLOAD) return -1;

    int fd = ctx->lane_fd[lane_id];
    if (fd < 0) return -1;

    /* build header */
    Delta_BlockHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic        = DELTA_MAGIC;
    hdr.lane_id      = lane_id;
    hdr.version      = DELTA_VERSION;
    hdr.seq          = ++ctx->lane_seq[lane_id];
    hdr.addr         = addr;
    hdr.payload_size = size;
    hdr.crc32        = 0;

    /* CRC = header(excl crc32) + payload */
    uint32_t crc = delta_crc32(0, &hdr,
                                offsetof(Delta_BlockHeader, crc32));
    crc = delta_crc32(crc, data, size);
    hdr.crc32 = crc;

    /* append header + payload — O_APPEND ทำ atomic position update */
    if (_write_all(fd, &hdr, sizeof(hdr)) != 0)  return -1;
    if (_write_all(fd, data, size)         != 0)  return -1;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   COMMIT  (ลำดับห้ามสลับ — ตาม protocol)
   ══════════════════════════════════════════════════════════════════════ */

int delta_commit(Delta_Context *ctx) {
    if (!ctx || !ctx->is_open) return -1;

    /* Step 3  audit invariant */
    if (delta_audit(ctx) != 0) return -1;

    /* Step 5  write merkle.pending */
    if (_write_merkle_pending(ctx) != 0) return -1;

    /* Step 6-7  fsync ทุก lane + merkle.pending */
    for (int i = 0; i < LANE_COUNT; i++) {
        if (ctx->lane_fd[i] >= 0) {
            /* flush buffered writes ก่อน fsync */
            if (fsync(ctx->lane_fd[i]) != 0) {
                fprintf(stderr, "delta_commit: fsync lane %d failed: %s\n",
                        i, strerror(errno));
                return -1;
            }
        }
    }
    /* fsync merkle.pending */
    {
        char path[1024];
        _join(path, sizeof(path), ctx->pogls_dir, FNAME_MERKLE_PENDING);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) { fsync(fd); close(fd); }
    }

    /* Step 8  rename lane_*.pending → lane_*.delta */
    for (int i = 0; i < LANE_COUNT; i++) {
        char src[1024], dst[1024];
        _join(src, sizeof(src), ctx->pogls_dir, _pending_name(i));
        _join(dst, sizeof(dst), ctx->pogls_dir, _delta_name(i));
        if (rename(src, dst) != 0) return -1;
        close(ctx->lane_fd[i]);
        ctx->lane_fd[i] = -1;
    }

    /* Step 9  rename merkle.pending → snapshot.merkle  ← ATOMIC FINAL */
    {
        char src[1024], dst[1024];
        _join(src, sizeof(src), ctx->pogls_dir, FNAME_MERKLE_PENDING);
        _join(dst, sizeof(dst), ctx->pogls_dir, FNAME_MERKLE);
        if (rename(src, dst) != 0) return -1;
    }

    /* Step 10  update epoch */
    ctx->epoch++;

    fprintf(stderr, "delta_commit: epoch=%llu  seqs=[%llu,%llu,%llu,%llu]\n",
            (unsigned long long)ctx->epoch,
            (unsigned long long)ctx->lane_seq[0],
            (unsigned long long)ctx->lane_seq[1],
            (unsigned long long)ctx->lane_seq[2],
            (unsigned long long)ctx->lane_seq[3]);

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   CLOSE
   ══════════════════════════════════════════════════════════════════════ */

int delta_close(Delta_Context *ctx) {
    if (!ctx) return -1;
    for (int i = 0; i < LANE_COUNT; i++) {
        if (ctx->lane_fd[i] >= 0) {
            close(ctx->lane_fd[i]);
            ctx->lane_fd[i] = -1;
        }
    }
    ctx->is_open = false;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   BOOT RECOVERY SCANNER
   ══════════════════════════════════════════════════════════════════════ */

Delta_RecoveryResult delta_recover(const char *source_path) {
    if (!source_path) return DELTA_RECOVERY_ERROR;

    /* สร้าง pogls_dir path */
    char parent[512];
    strncpy(parent, source_path, sizeof(parent) - 1);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = '\0';
    else       strncpy(parent, ".", sizeof(parent) - 1);

    const char *basename = strrchr(source_path, '/');
    basename = basename ? basename + 1 : source_path;

    char pogls_dir[700];
    snprintf(pogls_dir, sizeof(pogls_dir),
             "%s/%s/%s", parent, POGLS_DIR, basename);

    /* ── กรณี C — ไม่มี .pogls/ dir ─────────────────────────────── */
    struct stat st;
    if (stat(pogls_dir, &st) != 0)
        return DELTA_RECOVERY_NEW;

    /* ── อ่าน snapshot.merkle ────────────────────────────────────── */
    Delta_MerkleRecord merkle;
    int has_merkle = (_read_merkle(pogls_dir, &merkle) == 0);

    /* ── ตรวจ .pending files ─────────────────────────────────────── */
    bool has_pending = false;
    const char *pending_names[LANE_COUNT] = {
        FNAME_PENDING_X, FNAME_PENDING_NX,
        FNAME_PENDING_Y, FNAME_PENDING_NY
    };
    for (int i = 0; i < LANE_COUNT; i++) {
        char path[1024];
        _join(path, sizeof(path), pogls_dir, pending_names[i]);
        if (stat(path, &st) == 0) { has_pending = true; break; }
    }

    /* ── กรณี A — CLEAN ──────────────────────────────────────────── */
    if (has_merkle && !has_pending) {
        /* verify merkle seq matches .delta last seq */
        /* (quick check: ถ้า .delta ไม่มี = torn) */
        bool all_delta = true;
        const char *delta_names[LANE_COUNT] = {
            FNAME_LANE_X, FNAME_LANE_NX,
            FNAME_LANE_Y, FNAME_LANE_NY
        };
        for (int i = 0; i < LANE_COUNT; i++) {
            char path[1024];
            _join(path, sizeof(path), pogls_dir, delta_names[i]);
            if (stat(path, &st) != 0) { all_delta = false; break; }
        }
        if (all_delta) {
            fprintf(stderr, "delta_recover [%s]: CLEAN  epoch=%llu\n",
                    basename,
                    (unsigned long long)merkle.epoch);
            return DELTA_RECOVERY_CLEAN;
        }
    }

    /* ── กรณี B — TORN ───────────────────────────────────────────── */
    fprintf(stderr,
            "delta_recover [%s]: TORN — discarding .pending, fallback N-1\n",
            basename);

    /* discard .pending */
    for (int i = 0; i < LANE_COUNT; i++) {
        char path[1024];
        _join(path, sizeof(path), pogls_dir, pending_names[i]);
        unlink(path);   /* ไม่มีก็ไม่เป็นไร */
    }
    /* discard merkle.pending */
    {
        char path[1024];
        _join(path, sizeof(path), pogls_dir, FNAME_MERKLE_PENDING);
        unlink(path);
    }

    return DELTA_RECOVERY_TORN;
}

/* ── scan vault directory ────────────────────────────────────────────── */

int delta_scan_vault(const char *vault_path,
                     void (*callback)(const char *file,
                                      Delta_RecoveryResult result,
                                      void *userdata),
                     void *userdata) {
    if (!vault_path || !callback) return -1;

    DIR *d = opendir(vault_path);
    if (!d) return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* skip hidden files + . .. */
        if (ent->d_name[0] == '.') continue;

        char full[600];
        snprintf(full, sizeof(full), "%s/%s", vault_path, ent->d_name);

        /* skip directories */
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) continue;

        Delta_RecoveryResult r = delta_recover(full);
        callback(full, r, userdata);
        count++;
    }
    closedir(d);
    return count;
}
