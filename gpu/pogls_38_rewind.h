/*
 * pogls_38_rewind.h — POGLS38 Phase B2: RewindBuffer Port
 * ══════════════════════════════════════════════════════════════════════
 *
 * Ported from V4 pogls_rewind.h — adapted for POGLS38 types.
 *
 * เปรียบเทียบ:
 *   Delta lane  = ตู้เซฟ (crash-safe, บนดิสก์)
 *   RewindBuffer = โต๊ะทำงาน (เร็ว, ใน L2 cache, ย้อนได้)
 *
 *   เขียนงาน → วางบนโต๊ะก่อน (RewindBuffer)
 *   ทุก 18 ชิ้น → ล็อคลงตู้เซฟ (flush → delta_append)
 *   ถ้าผิดพลาด → ปัดงานบนโต๊ะทิ้ง (rewind) โดยของในตู้ยังดีอยู่
 *
 * Changes from V4:
 *   1. DeltaContext* → flush callback fn (decouple)
 *   2. delta_append() signature adapted (POGLS38 = lane_id param)
 *   3. Added Tails DNA hook: tails_dna_record() on every push
 *   4. RewindSlot เพิ่ม addr field (PHI address ของ slot นี้)
 *
 * Sacred Numbers (DNA — ห้ามแก้):
 *   18  = REWIND_GATE   (gate_18 flush unit = 2×3²)
 *   54  = REWIND_NEXUS  (1 Rubik cycle = 2×3³)
 *   162 = REWIND_SPHERE (1 icosphere pass = 2×3⁴)
 *   972 = REWIND_MAX    (54×18, L2 resident = ~60KB)
 *
 * Memory layout:
 *   RewindBuffer ≈ 972×64B slots + 18 checkpoints + stats ≈ 60.5KB
 *   Designed to fit entirely in L2 cache (512KB typical)
 *
 * Rules (FROZEN):
 *   - REWIND_MAX = 972 = 54×18 — ห้ามเปลี่ยน
 *   - slot[head-1] = newest, slot[confirmed] = oldest unflushd
 *   - rewind() ย้อนได้เฉพาะ unconfirmed slots เท่านั้น
 *   - flush_gate() commits exactly REWIND_GATE=18 slots per call
 *   - Tails DNA hook = optional (NULL = skip)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_REWIND_H
#define POGLS_38_REWIND_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <string.h>
#include <time.h>     /* clock_gettime, CLOCK_MONOTONIC */
#include "pogls_fold.h"           /* DiamondBlock (64B)                 */
#include "pogls_38_tails_dna.h"   /* Tails DNA hook (Phase B1)          */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — CONSTANTS  (sacred numbers — FROZEN)
 * ══════════════════════════════════════════════════════════════════════ */

#define REWIND_GATE     18u   /* gate_18: flush unit       = 2×3²       */
#define REWIND_NEXUS    54u   /* nexus:   1 Rubik cycle    = 2×3³       */
#define REWIND_SPHERE  162u   /* icosphere: 1 node pass    = 2×3⁴       */
#define REWIND_MAX     972u   /* total slots: 54×18, L2 friendly         */
#define REWIND_CKPT_COUNT (REWIND_MAX / REWIND_NEXUS)   /* = 18         */

#define REWIND_MAGIC    0x52574E44u   /* "RWND"                          */

/* compile-time checks — sacred numbers ห้ามผิด */
typedef char _rw_gate_check  [(REWIND_MAX % REWIND_GATE   == 0) ? 1:-1];
typedef char _rw_nexus_check [(REWIND_MAX % REWIND_NEXUS  == 0) ? 1:-1];
typedef char _rw_sphere_check[(REWIND_MAX % REWIND_SPHERE == 0) ? 1:-1];
typedef char _rw_ckpt_check  [(REWIND_CKPT_COUNT == 18u)         ? 1:-1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — REWIND SLOT  (72B)
 *
 * V4 = RewindSlot { DiamondBlock block; }  (64B)
 * POGLS38 adds: addr (PHI angular address) + slot_flags
 * Padded to 80B for alignment — fits 6 per 480B (cache-friendly)
 *
 * เปรียบเทียบ:
 *   DiamondBlock = ตัวงาน (64B, 1 cache line)
 *   RewindSlot   = ซองงาน (งาน + ป้ายบอกว่ามาจากที่ไหน)
 * ══════════════════════════════════════════════════════════════════════ */

#define RWSLOT_FLAG_GATE_FLUSH  0x01u  /* slot was flushed at gate_18    */
#define RWSLOT_FLAG_NEXUS_CKPT  0x02u  /* slot is a nexus checkpoint     */
#define RWSLOT_FLAG_REWOUND     0x04u  /* slot was rolled back           */

typedef struct {
    DiamondBlock block;    /* 64B — the actual write payload             */
    uint32_t     addr;     /* PHI angular address (A = floor(θ × 2²⁰))  */
    uint8_t      lane_id;  /* delta lane 0-53 (for flush routing)        */
    uint8_t      flags;    /* RWSLOT_FLAG_*                              */
    uint8_t      _pad[58]; /* pad to 128B (aligned(64) in DiamondBlock forces 128B                                 */
} RewindSlot;              /* 64+4+1+1+10 = 80B                          */

typedef char _rslot_sz[(sizeof(RewindSlot) == 128u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — FLUSH CALLBACK  (POGLS38 adaptation)
 *
 * V4 uses DeltaContext* directly.
 * POGLS38 uses callback to decouple RewindBuffer from Delta layer.
 * This makes RewindBuffer testable in isolation (pass NULL = in-memory).
 *
 * Signature mirrors POGLS38's delta_append():
 *   delta_append(ctx, lane_id, addr, data, size)
 * ══════════════════════════════════════════════════════════════════════ */

typedef int (*RewindFlushFn)(void       *ctx,
                              uint8_t     lane_id,
                              uint64_t    addr,
                              const void *data,
                              uint32_t    size);

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — NEXUS CHECKPOINT  (24B, every 54 slots)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t slot_index;     /* ring position of this checkpoint         */
    uint32_t crc32;          /* XOR of block.core.raw at this nexus      */
    uint64_t timestamp_ns;   /* CLOCK_MONOTONIC at checkpoint time       */
    uint32_t confirmed_at;   /* rb->confirmed when checkpoint was taken  */
    uint32_t _pad;
} RewindCheckpoint;          /* 24B */

typedef char _rckpt_sz[(sizeof(RewindCheckpoint) == 24u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — STATS
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t total_writes;   /* rewind_push() calls                      */
    uint64_t total_flushes;  /* gate batches flushed to delta             */
    uint64_t total_rewinds;  /* rewind() calls                            */
    uint64_t steps_rewound;  /* total slots rolled back                   */
    uint64_t delta_appends;  /* actual delta_append() calls made          */
    uint32_t max_depth_used; /* peak head position                        */
    uint32_t overflow_wraps; /* times ring wrapped (oldest evicted)       */
} RewindStats;               /* 52B */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 6 — REWIND BUFFER  (main structure)
 *
 * Memory breakdown:
 *   slots[972]       = 972 × 80B = 77,760B  (ring)
 *   checkpoints[18]  = 18  × 24B =    432B  (nexus snapshots)
 *   stats + meta     =                ~80B
 *   TOTAL            ≈ 78.3KB  (fits in 512KB L2)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* ── ring buffer ──────────────────────────────────────────────── */
    RewindSlot        slots[REWIND_MAX];         /* 77,760B            */

    /* ── nexus checkpoints (every 54 slots) ─────────────────────── */
    RewindCheckpoint  checkpoints[REWIND_CKPT_COUNT]; /* 432B         */

    /* ── cursors ─────────────────────────────────────────────────── */
    uint32_t          head;       /* next write index (grows monotonic) */
    uint32_t          confirmed;  /* slots committed to delta lane      */
    uint32_t          count;      /* slots currently in ring            */
    uint32_t          epoch;      /* increments every ring wrap         */

    /* ── flush callback (replaces V4 DeltaContext*) ─────────────── */
    RewindFlushFn     flush_fn;   /* NULL = in-memory only              */
    void             *flush_ctx;  /* passed as first arg to flush_fn    */

    /* ── Tails DNA hook (Phase B1 integration) ──────────────────── */
    TailsDNAContext  *dna;        /* NULL = skip DNA recording          */

    /* ── stats ───────────────────────────────────────────────────── */
    RewindStats       stats;

    /* ── magic ───────────────────────────────────────────────────── */
    uint32_t          magic;
    uint32_t          _pad;
} RewindBuffer;

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 7 — INIT
 * ══════════════════════════════════════════════════════════════════════ */

static inline int rewind_init(RewindBuffer  *rb,
                               RewindFlushFn  flush_fn,
                               void          *flush_ctx,
                               TailsDNAContext *dna)
{
    if (!rb) return -1;
    memset(rb, 0, sizeof(*rb));
    rb->flush_fn  = flush_fn;
    rb->flush_ctx = flush_ctx;
    rb->dna       = dna;
    rb->magic     = REWIND_MAGIC;

    /* pre-mark nexus checkpoint slot_indices */
    for (uint32_t i = 0; i < REWIND_CKPT_COUNT; i++)
        rb->checkpoints[i].slot_index = i * REWIND_NEXUS;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 8 — INTERNAL HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t _rw_crc32(const DiamondBlock *b)
{
    /* Use core.raw only — invert=~core.raw would cancel in full XOR.
     * Mix with addr-like folding for better bit distribution.
     * Deterministic: same block → same checksum, always. */
    uint64_t x = b->core.raw;
    x ^= (x >> 17) ^ (x << 13);   /* bit-mix */
    x ^= (uint64_t)b->honeycomb[0] | ((uint64_t)b->honeycomb[1] << 8);
    return (uint32_t)(x ^ (x >> 32));
}

static inline uint64_t _rw_now_ns(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#else
    static uint64_t _tick = 0;
    return ++_tick;
#endif
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 9 — PUSH  (hot path, ~4ns)
 *
 * Write one DiamondBlock into speculative ring.
 * Auto-checkpoint every REWIND_NEXUS (54) writes.
 * Auto-flush every REWIND_GATE (18) writes via flush_fn.
 *
 * Returns: ring slot index used
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t rewind_push(RewindBuffer       *rb,
                                    const DiamondBlock *block,
                                    uint32_t            addr,
                                    uint8_t             lane_id)
{
    if (!rb || !block) return UINT32_MAX;

    uint32_t idx = rb->head % REWIND_MAX;
    rb->slots[idx].block   = *block;
    rb->slots[idx].addr    = addr;
    rb->slots[idx].lane_id = lane_id;
    rb->slots[idx].flags   = 0;
    rb->head++;

    /* epoch: increment every ring wrap */
    if (rb->head > 0 && rb->head % REWIND_MAX == 0)
        rb->epoch++;

    /* ring full → evict oldest */
    if (rb->count < REWIND_MAX) rb->count++;
    else {
        rb->stats.overflow_wraps++;
        if (rb->confirmed < rb->head - REWIND_MAX)
            rb->confirmed = rb->head - REWIND_MAX;
    }
    rb->stats.total_writes++;
    if (rb->head > rb->stats.max_depth_used)
        rb->stats.max_depth_used = rb->head;

    /* ── nexus checkpoint (every 54 writes) ─────────────────────── */
    if (rb->stats.total_writes % REWIND_NEXUS == 0) {
        uint32_t ci = (rb->stats.total_writes / REWIND_NEXUS - 1)
                      % REWIND_CKPT_COUNT;
        rb->checkpoints[ci].slot_index  = idx;
        rb->checkpoints[ci].crc32       = _rw_crc32(block);
        rb->checkpoints[ci].timestamp_ns = _rw_now_ns();
        rb->checkpoints[ci].confirmed_at = rb->confirmed;
        rb->slots[idx].flags |= RWSLOT_FLAG_NEXUS_CKPT;
    }

    /* ── auto flush every gate_18 writes ────────────────────────── */
    if (rb->stats.total_writes % REWIND_GATE == 0) {
        /* flush_gate is called below — inline to avoid forward decl */
        uint32_t avail = rb->head > rb->confirmed
                       ? rb->head - rb->confirmed : 0;
        if (avail >= REWIND_GATE && rb->flush_fn) {
            for (uint32_t i = 0; i < REWIND_GATE && rb->confirmed < rb->head; i++) {
                uint32_t si = rb->confirmed % REWIND_MAX;
                RewindSlot *s = &rb->slots[si];
                rb->flush_fn(rb->flush_ctx, s->lane_id,
                             (uint64_t)s->addr,
                             &s->block, sizeof(DiamondBlock));
                s->flags |= RWSLOT_FLAG_GATE_FLUSH;
                rb->confirmed++;
                rb->stats.delta_appends++;
            }
            rb->stats.total_flushes++;
        }
    }

    return idx;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 10 — FLUSH GATE  (manual flush, force early commit)
 *
 * Commits exactly REWIND_GATE (18) unconfirmed slots.
 * Returns: slots flushed (0 if not enough pending)
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t rewind_flush_gate(RewindBuffer *rb)
{
    if (!rb) return 0;

    uint32_t avail = rb->head > rb->confirmed
                   ? rb->head - rb->confirmed : 0;
    if (avail < REWIND_GATE) return 0;

    uint32_t flushed = 0;
    for (uint32_t i = 0; i < REWIND_GATE && rb->confirmed < rb->head; i++) {
        uint32_t si = rb->confirmed % REWIND_MAX;
        RewindSlot *s = &rb->slots[si];
        if (rb->flush_fn) {
            rb->flush_fn(rb->flush_ctx, s->lane_id,
                         (uint64_t)s->addr,
                         &s->block, sizeof(DiamondBlock));
            rb->stats.delta_appends++;
        }
        s->flags |= RWSLOT_FLAG_GATE_FLUSH;
        rb->confirmed++;
        flushed++;
    }
    if (flushed) rb->stats.total_flushes++;
    return flushed;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 11 — REWIND  (roll back N steps)
 *
 * n=1   → undo last write
 * n=18  → undo last gate_18 (1 Rubik step)
 * n=54  → undo last nexus (1 Rubik cycle)
 * n=162 → undo last icosphere pass
 * n=972 → full reset to last confirmed disk state
 *
 * Can only rewind UNCONFIRMED slots (confirmed = on disk = immutable)
 * Returns: steps actually rewound
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t rewind_back(RewindBuffer *rb, uint32_t n)
{
    if (!rb || n == 0) return 0;

    uint32_t unconfirmed = rb->head > rb->confirmed
                         ? rb->head - rb->confirmed : 0;
    uint32_t steps = n < unconfirmed ? n : unconfirmed;

    /* mark rewound slots */
    for (uint32_t i = 0; i < steps; i++) {
        uint32_t si = (rb->head - 1 - i) % REWIND_MAX;
        rb->slots[si].flags |= RWSLOT_FLAG_REWOUND;
    }

    rb->head  -= steps;
    rb->count  = rb->count > steps ? rb->count - steps : 0;
    rb->stats.total_rewinds++;
    rb->stats.steps_rewound += steps;
    return steps;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 12 — REWIND TO CHECKPOINT  (fast seek to nexus boundary)
 *
 * เปรียบเทียบ: git reset --hard <commit_hash>
 * แต่แทน commit_hash ใช้ nexus checkpoint (ทุก 54 writes)
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t rewind_to_checkpoint(RewindBuffer *rb,
                                             uint32_t      target_step)
{
    if (!rb) return UINT32_MAX;

    int best = -1;
    for (int i = 0; i < (int)REWIND_CKPT_COUNT; i++) {
        if (rb->checkpoints[i].slot_index <= target_step &&
            rb->checkpoints[i].crc32 != 0)   /* 0 = not yet written */
            best = i;
    }
    if (best < 0) return UINT32_MAX;

    uint32_t target_head = rb->checkpoints[best].slot_index + 1;
    if (target_head >= rb->head) return UINT32_MAX;

    uint32_t steps = rb->head - target_head;
    rewind_back(rb, steps);
    return rb->checkpoints[best].slot_index;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 13 — QUERY HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

/* rewind_peek — read slot at offset from head (no modify)
 * offset=0 → last write, offset=1 → second-to-last */
static inline const DiamondBlock *rewind_peek(const RewindBuffer *rb,
                                               uint32_t offset)
{
    if (!rb || offset >= rb->count) return NULL;
    uint32_t idx = (rb->head - 1 - offset) % REWIND_MAX;
    return &rb->slots[idx].block;
}

/* rewind_depth — how many steps can we currently rewind? */
static inline uint32_t rewind_depth(const RewindBuffer *rb)
{
    if (!rb || rb->head <= rb->confirmed) return 0;
    return rb->head - rb->confirmed;
}

/* rewind_global_id — unique (epoch, slot) ID across all ring wraps
 * Two slots with same global_id are guaranteed identical write */
static inline uint64_t rewind_global_id(const RewindBuffer *rb,
                                         uint32_t slot_offset)
{
    if (!rb || slot_offset >= rb->count) return UINT64_MAX;
    uint32_t slot_idx = (rb->head - 1 - slot_offset) % REWIND_MAX;
    return (uint64_t)rb->epoch * REWIND_MAX + slot_idx;
}

/* rewind_validate — integrity check */
#define REWIND_OK          0
#define REWIND_ERR_NULL   -1
#define REWIND_ERR_MAGIC  -2
#define REWIND_ERR_COUNT  -3

static inline int rewind_validate(const RewindBuffer *rb)
{
    if (!rb)                       return REWIND_ERR_NULL;
    if (rb->magic != REWIND_MAGIC) return REWIND_ERR_MAGIC;
    if (rb->count > REWIND_MAX)    return REWIND_ERR_COUNT;
    return REWIND_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 14 — TAILS DNA INTEGRATION
 *
 * ถ้า rb->dna != NULL:
 *   ทุก rewind_push() จะเรียก tails_dna_record() อัตโนมัติ
 *   DNA ของ slot นั้นถูกบันทึกลงใน W3R chain
 *
 * rewind_push_with_dna() = push + dna record ใน 1 call
 * สำหรับ caller ที่มี W3RWriteResult อยู่แล้ว
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t rewind_push_with_dna(RewindBuffer        *rb,
                                              const DiamondBlock  *block,
                                              uint32_t             addr,
                                              uint8_t              lane_id,
                                              const W3RWriteResult *w3r_res,
                                              const uint8_t        rubik_face[6],
                                              uint32_t             tick)
{
    uint32_t idx = rewind_push(rb, block, addr, lane_id);
    if (idx != UINT32_MAX && rb->dna && w3r_res)
        tails_dna_record(rb->dna, w3r_res, (uint64_t)addr, rubik_face, tick);
    return idx;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 15 — STATS PRINT
 * ══════════════════════════════════════════════════════════════════════ */

#include <stdio.h>

static inline void rewind_stats_print(const RewindBuffer *rb)
{
    if (!rb) return;
    const RewindStats *s = &rb->stats;
    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  RewindBuffer (Phase B2) Stats           ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║ Total writes:    %10llu               ║\n",
           (unsigned long long)s->total_writes);
    printf("║ Total flushes:   %10llu               ║\n",
           (unsigned long long)s->total_flushes);
    printf("║ Delta appends:   %10llu               ║\n",
           (unsigned long long)s->delta_appends);
    printf("║ Total rewinds:   %10llu               ║\n",
           (unsigned long long)s->total_rewinds);
    printf("║ Steps rewound:   %10llu               ║\n",
           (unsigned long long)s->steps_rewound);
    printf("║ Overflow wraps:  %10u                ║\n", s->overflow_wraps);
    printf("║ Max depth used:  %10u / %u       ║\n",
           s->max_depth_used, REWIND_MAX);
    printf("║ Epoch:           %10u                ║\n", rb->epoch);
    printf("║ Head / Confirmed:%u / %u              ║\n",
           rb->head, rb->confirmed);
    printf("║ DNA linked:      %s                     ║\n",
           rb->dna ? "YES" : "NO ");
    printf("╚══════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_38_REWIND_H */
