/*
 * geo_dodeca.h — Shared Dodeca Layer (Session 13)
 * ================================================
 *
 * Extracted from geo_diamond_field4_fixed.h + geo_diamond_field5.h
 * so both geo_diamond_field.h and geo_diamond_field5.h can include
 * a single source of truth instead of duplicating types.
 *
 * Contains:
 *   1. DiamondFlowCtxV2   — scalar ctx with adaptive drift_threshold
 *   2. diamond_flow_init_v2 / diamond_ctx_branch
 *   3. DodecaEntry / DodecaTable / DodecaResult  — offset world types
 *   4. dodeca_init / dodeca_slot / dodeca_lookup / dodeca_insert
 *   5. DiamondSHA + dodeca_verify + dodeca_insert_sha + dodeca_fill_sha
 *
 * NOT included (stays in parent headers):
 *   - diamond_gate / diamond_gate_v2          (geo_diamond_field*.h)
 *   - diamond_batch_run_v4 / v5               (geo_diamond_field4/5.h)
 *   - DiamondFlowCtx4 / temporal_x4           (geo_diamond_field.h)
 *
 * Include order:
 *   geo_dodeca.h  ← (standalone, needs only stdint.h + string.h)
 *   geo_diamond_field.h   → #include "geo_dodeca.h"
 *   geo_diamond_field5.h  → #include "geo_dodeca.h"
 *   geo_diamond_v5x4.h    → #include "geo_dodeca.h"
 */

#ifndef GEO_DODECA_H
#define GEO_DODECA_H

#include <stdint.h>
#include <string.h>   /* memset */

/* ════════════════════════════════════════════════════════════════════
 * DiamondFlowCtxV2 — scalar ctx with adaptive threshold
 *
 * drift_threshold baked in at init — ไม่ recompute ทุก cell
 * carry ข้าม batch ได้ (streaming GB/TB)
 * ════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t route_addr;      /* accumulated XOR of intersects     */
    uint16_t hop_count;       /* cells passed (not dropped)        */
    uint16_t _pad;
    uint32_t drift_acc;       /* drift accumulator                 */
    uint32_t drift_threshold; /* adaptive — set once at init       */
} DiamondFlowCtxV2;

static inline void diamond_flow_init_v2(DiamondFlowCtxV2 *ctx,
                                         uint64_t baseline)
{
    ctx->route_addr      = 0;
    ctx->hop_count       = 0;
    ctx->_pad            = 0;
    ctx->drift_acc       = 0;
    /* threshold = popcount(baseline) / 2, min 8 */
    uint32_t pc = (uint32_t)__builtin_popcountll(baseline);
    ctx->drift_threshold = (pc >> 1) < 8u ? 8u : (pc >> 1);
}

/* snapshot ณ จุดแยก — drift_threshold inherit ด้วย */
static inline void diamond_ctx_branch(const DiamondFlowCtxV2 *src,
                                       DiamondFlowCtxV2       *dst)
{
    *dst = *src;
}

/* ════════════════════════════════════════════════════════════════════
 * DODECA LANE — offset world lookup
 *
 * L0: offset match → ชี้ตรง ไม่ผ่าน icosa
 * L1: merkle match → DNA เดิม path ต่าง → merge offset
 * miss: ส่งต่อ icosa lane สร้างใหม่
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t merkle_root;   /* geometry fingerprint ของ flow      */
    uint64_t sha256_hi;     /* SHA256 top 8 bytes = integrity      */
    uint64_t sha256_lo;     /* SHA256 bot 8 bytes                  */
    uint8_t  offset;        /* semantic distance จาก baseline      */
    uint16_t hop_count;     /* กี่ hops ใน flow นี้               */
    uint8_t  segment;       /* scroll ที่เท่าไหร่ (0-255)         */
    uint32_t ref_count;     /* กี่ pointer ชี้มา (dedup counter)  */
} DodecaEntry;

#ifndef DODECA_TABLE_SIZE
#  define DODECA_TABLE_SIZE 144u   /* = TE_CYCLE (fibonacci) */
#endif

typedef struct {
    DodecaEntry slots[DODECA_TABLE_SIZE];
    uint32_t    count;       /* entries ที่ใช้งานอยู่  */
    uint32_t    hit_count;   /* L0/L1 hits (dedup)     */
    uint32_t    miss_count;  /* ส่งต่อ icosa            */
} DodecaTable;

static inline void dodeca_init(DodecaTable *t) {
    memset(t, 0, sizeof(*t));
}

/* fibonacci-modulo slot distribution */
static inline uint32_t dodeca_slot(uint64_t merkle_root) {
    return (uint32_t)((merkle_root * 0x9e3779b97f4a7c15ULL) >> 32)
           % DODECA_TABLE_SIZE;
}

typedef enum {
    DODECA_HIT_EXACT  = 0,
    DODECA_HIT_MERKLE = 1,
    DODECA_MISS       = 2,
} DodecaResult;
#define DODECA_HIT DODECA_HIT_EXACT  /* alias: exact match = hit */

static inline DodecaResult dodeca_lookup(DodecaTable        *t,
                                          uint64_t            merkle_root,
                                          uint8_t             offset,
                                          DodecaEntry       **out_entry)
{
    uint32_t idx = dodeca_slot(merkle_root);
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t s = (idx + i) % DODECA_TABLE_SIZE;
        DodecaEntry *e = &t->slots[s];
        if (e->ref_count == 0) break;
        if (e->merkle_root == merkle_root) {
            e->ref_count++;
            *out_entry = e;
            t->hit_count++;
            if (e->offset == offset) return DODECA_HIT_EXACT;
            return DODECA_HIT_MERKLE;
        }
    }
    t->miss_count++;
    *out_entry = NULL;
    return DODECA_MISS;
}

static inline DodecaEntry *dodeca_insert(DodecaTable *t,
                                          uint64_t     merkle_root,
                                          uint64_t     sha256_hi,
                                          uint64_t     sha256_lo,
                                          uint8_t      offset,
                                          uint16_t     hop_count,
                                          uint8_t      segment)
{
    uint32_t idx = dodeca_slot(merkle_root);
    DodecaEntry *target = NULL;
    uint32_t min_ref = UINT32_MAX;

    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t s = (idx + i) % DODECA_TABLE_SIZE;
        DodecaEntry *e = &t->slots[s];
        if (e->ref_count == 0) { target = e; break; }
        if (e->ref_count < min_ref) { min_ref = e->ref_count; target = e; }
    }
    if (!target) return NULL;

    target->merkle_root = merkle_root;
    target->sha256_hi   = sha256_hi;
    target->sha256_lo   = sha256_lo;
    target->offset      = offset;
    target->hop_count   = hop_count;
    target->segment     = segment;
    target->ref_count   = 1;

    if (t->count < DODECA_TABLE_SIZE) t->count++;
    return target;
}

/* ════════════════════════════════════════════════════════════════════
 * DiamondSHA — SHA256 integrity layer (opt-in, NULL = skip)
 * ════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t hi;   /* SHA256 bytes [0..7]  */
    uint64_t lo;   /* SHA256 bytes [8..15] */
} DiamondSHA;

static inline DiamondSHA diamond_sha_from_raw(const uint8_t sha256_raw[32])
{
    DiamondSHA s;
    s.hi = ((uint64_t)sha256_raw[0]  << 56) | ((uint64_t)sha256_raw[1]  << 48)
         | ((uint64_t)sha256_raw[2]  << 40) | ((uint64_t)sha256_raw[3]  << 32)
         | ((uint64_t)sha256_raw[4]  << 24) | ((uint64_t)sha256_raw[5]  << 16)
         | ((uint64_t)sha256_raw[6]  <<  8) |  (uint64_t)sha256_raw[7];
    s.lo = ((uint64_t)sha256_raw[8]  << 56) | ((uint64_t)sha256_raw[9]  << 48)
         | ((uint64_t)sha256_raw[10] << 40) | ((uint64_t)sha256_raw[11] << 32)
         | ((uint64_t)sha256_raw[12] << 24) | ((uint64_t)sha256_raw[13] << 16)
         | ((uint64_t)sha256_raw[14] <<  8) |  (uint64_t)sha256_raw[15];
    return s;
}

static inline int diamond_sha_is_zero(const DiamondSHA *s)
{
    return (s->hi == 0) && (s->lo == 0);
}

/* return: 0=no SHA stored, 1=match OK, -1=mismatch (corrupt) */
static inline int dodeca_verify(const DodecaEntry  *e,
                                 const DiamondSHA   *sha)
{
    if (!sha) return 0;
    if (e->sha256_hi == 0 && e->sha256_lo == 0) return 0;
    if (e->sha256_hi == sha->hi && e->sha256_lo == sha->lo) return  1;
    return -1;
}

/* thin wrapper: insert with DiamondSHA* (NULL = defer SHA to later) */
static inline DodecaEntry *dodeca_insert_sha(DodecaTable      *t,
                                               uint64_t          merkle_root,
                                               const DiamondSHA *sha,
                                               uint8_t           offset,
                                               uint16_t          hop_count,
                                               uint8_t           segment)
{
    uint64_t hi = sha ? sha->hi : 0;
    uint64_t lo = sha ? sha->lo : 0;
    return dodeca_insert(t, merkle_root, hi, lo, offset, hop_count, segment);
}

/* backfill SHA after deferred insert */
static inline void dodeca_fill_sha(DodecaEntry    *e,
                                    const DiamondSHA *sha)
{
    if (!e || !sha) return;
    e->sha256_hi = sha->hi;
    e->sha256_lo = sha->lo;
}

#endif /* GEO_DODECA_H */
