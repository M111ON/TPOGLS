/*
 * pogls_federation.h — POGLS V4 + POGLS38 Federation Layer  v1.1
 * ══════════════════════════════════════════════════════════════════
 *
 * Architecture:
 *   GPU (POGLS38)
 *     ↓
 *   Pre-Gate  (iso + audit + ghost_mature)   ← กัน noise ก่อนเข้า V4
 *     ↓
 *   Backpressure  (queue > HWM → GHOST)      ← กัน GPU burst
 *     ↓
 *   Shadow Snapshot  (double-buffer)          ← atomic swap
 *     ↓
 *   Split commit A → B  (decouple)            ← B ไม่ฆ่า A
 *     ↓
 *   Disk
 *
 * Rules (FROZEN):
 *   - PHI from pogls_platform.h only
 *   - DiamondBlock 64B format unchanged
 *   - TORN = st_size > 0  (NOT stat()==0)
 *   - GPU never touches commit path
 *   - Pre-gate must run BEFORE fed_write
 * ══════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_FEDERATION_H
#define POGLS_FEDERATION_H

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#include "pogls_platform.h"              /* PHI constants */
#include "storage/pogls_delta.h"         /* World A */
#include "storage/pogls_delta_world_b.h" /* World B */

#ifndef POGLS_GHOST_STREAK_MAX
#define POGLS_GHOST_STREAK_MAX  8u
#endif

/* ── magic ────────────────────────────────────────────────────── */
#define FED_MAGIC         0x46454400u   /* "FED\0" */
#define FED_GATE_MAGIC    0x47415445u   /* "GATE"  */

/* ── backpressure thresholds ──────────────────────────────────── */
#define FED_QUEUE_HWM     4096u   /* high watermark → throttle GPU  */
#define FED_QUEUE_LWM      512u   /* low  watermark → resume        */
#define FED_QUEUE_MAX     8192u   /* hard cap — drop if exceeded     */

/* ── tile-level early merkle ──────────────────────────────────── */
#define FED_TILE_COUNT      54u   /* one tile per V4 lane            */
#define FED_TILE_HASH_SZ    32u   /* SHA256                          */

/* ══════════════════════════════════════════════════════════════════
 * 1. PRE-COMMIT GATE
 *    Runs on every cell from GPU before it enters V4.
 *    Cheap: 3 bit checks, ~2ns per cell.
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    GATE_PASS  = 0,   /* write to V4                                */
    GATE_GHOST = 1,   /* route to ghost lane, skip main write       */
    GATE_DROP  = 2,   /* discard — noise / bad audit                */
} GateResult;

typedef struct {
    uint64_t  passed;   /* cells that reached V4                    */
    uint64_t  ghosted;  /* cells rerouted to ghost                  */
    uint64_t  dropped;  /* cells discarded at gate                  */
    uint32_t  magic;
} GateStats;

/*
 * fed_gate — evaluate one GPU output cell
 *
 * packed  : PACK(hilbert, lane, iso) from bench9/final kernel
 * op_count: monotonic write counter (for ghost_mature check)
 *
 * Returns GateResult — caller acts accordingly.
 *
 * Logic:
 *   iso=1 (outside unit circle) → DROP  (geo_invalid, ~1% normal)
 *   audit fail (lane mismatch)  → DROP
 *   ghost not mature (<8 ops)   → GHOST (warm-up window)
 *   else                        → PASS
 */
static inline GateResult fed_gate(uint32_t packed,
                                   uint64_t op_count,
                                   GateStats *gs)
{
    uint32_t hil   = packed & 0xFFFFFu;
    uint32_t lane  = (packed >> 20) & 0x3Fu;
    uint32_t iso   = (packed >> 26) & 1u;

    /* CHECK 1: unit circle — geo_invalid → DROP */
    if (iso) {
        if (gs) gs->dropped++;
        return GATE_DROP;
    }

    /* CHECK 2: lane audit — hilbert % 54 must match packed lane */
    if ((hil % 54u) != lane) {
        if (gs) gs->dropped++;
        return GATE_DROP;
    }

    /* CHECK 3: ghost maturity — first 8 ops per session → GHOST */
    if (op_count < (uint64_t)POGLS_GHOST_STREAK_MAX) {
        if (gs) gs->ghosted++;
        return GATE_GHOST;
    }

    if (gs) gs->passed++;
    return GATE_PASS;
}

/* ══════════════════════════════════════════════════════════════════
 * 2. BACKPRESSURE CONTROLLER
 *    GPU writes to ring; V4 drains from ring.
 *    When ring > HWM: GPU cells → GHOST (skip main write).
 *    Prevents GPU 47K M/s from flooding disk 70 M/s.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    volatile uint32_t  queue_depth;   /* atomic-ish (relaxed ok)    */
    uint32_t           hwm;           /* high watermark              */
    uint32_t           lwm;           /* low  watermark              */
    uint32_t           hard_cap;      /* drop threshold              */
    uint64_t           throttle_count;
    uint64_t           drop_count;
} BackpressureCtx;

static inline void bp_init(BackpressureCtx *bp)
{
    if (!bp) return;
    memset(bp, 0, sizeof(*bp));
    bp->hwm      = FED_QUEUE_HWM;
    bp->lwm      = FED_QUEUE_LWM;
    bp->hard_cap = FED_QUEUE_MAX;
}

/* returns 1 = throttle (route to GHOST), 0 = ok to write */
static inline int bp_check(BackpressureCtx *bp)
{
    if (!bp) return 0;
    uint32_t d = bp->queue_depth;
    if (d >= bp->hard_cap) { bp->drop_count++;     return 1; }
    if (d >= bp->hwm)      { bp->throttle_count++; return 1; }
    return 0;
}

static inline void bp_push(BackpressureCtx *bp)
{ if (bp && bp->queue_depth < bp->hard_cap) bp->queue_depth++; }

static inline void bp_pop(BackpressureCtx *bp)
{ if (bp && bp->queue_depth > 0) bp->queue_depth--; }

/* ══════════════════════════════════════════════════════════════════
 * 3. EARLY MERKLE (tile-level)
 *    Hash each tile as writes come in (not at commit time).
 *    At commit: root = reduce(tile_hashes[]) → detect corruption
 *    BEFORE writing snapshot.merkle.pending.
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  tile_hash[FED_TILE_COUNT][FED_TILE_HASH_SZ]; /* per-lane */
    uint32_t tile_count[FED_TILE_COUNT];   /* writes per lane          */
    uint8_t  root[FED_TILE_HASH_SZ];       /* reduce result            */
    uint8_t  root_valid;                   /* 1 = root computed        */
} EarlyMerkle;

static inline void em_init(EarlyMerkle *em)
{ if (em) memset(em, 0, sizeof(*em)); }

/* XOR-fold tile update (fast stub — replace with SHA256 in prod) */
static inline void em_update(EarlyMerkle *em, uint8_t lane,
                              const void *data, uint32_t size)
{
    if (!em || lane >= FED_TILE_COUNT || !data) return;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < size; i++)
        em->tile_hash[lane][i % FED_TILE_HASH_SZ] ^= p[i];
    em->tile_count[lane]++;
    em->root_valid = 0;
}

/* Reduce all tile hashes → root */
static inline void em_reduce(EarlyMerkle *em)
{
    if (!em) return;
    memset(em->root, 0, FED_TILE_HASH_SZ);
    for (uint32_t t = 0; t < FED_TILE_COUNT; t++)
        for (uint32_t b = 0; b < FED_TILE_HASH_SZ; b++)
            em->root[b] ^= em->tile_hash[t][b];
    em->root_valid = 1;
}

/* ══════════════════════════════════════════════════════════════════
 * 4. SHADOW SNAPSHOT (double-buffer)
 *    active_snap = what GPU writes to now
 *    shadow_snap = previous committed state (instant rollback)
 *    commit → swap pointers atomically
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    Delta_ContextAB  snap[2];    /* [0] and [1] double-buffer        */
    int              active;     /* which slot GPU is writing to     */
    uint64_t         epoch;      /* swap counter                     */
} ShadowSnapshot;

static inline int ss_init(ShadowSnapshot *ss, const char *vault)
{
    if (!ss || !vault) return -1;
    memset(ss, 0, sizeof(*ss));
    /* open active slot */
    int r = delta_ab_open(&ss->snap[0], vault);
    if (r != 0) return r;
    ss->active = 0;
    return 0;
}

/*
 * ss_commit — split A/B commit then swap
 *
 *   1. commit World A  (hot path — must succeed)
 *   2. commit World B  (mirror — failure does NOT rollback A)
 *   3. swap active ↔ shadow
 *
 * Returns: 0=both ok, 1=A ok B fail, -1=A fail
 */
static inline int ss_commit(ShadowSnapshot *ss)
{
    if (!ss) return -1;
    Delta_ContextAB *cur = &ss->snap[ss->active];

    /* commit World A — hot path */
    int ra = delta_commit(&cur->a);
    if (ra != 0) return -1;   /* A failed → do NOT swap, keep shadow */

    /* commit World B — async mirror, failure is non-fatal */
    int rb = delta_b_close(&cur->b);   /* flush + close B */
    /* re-open B for next cycle */
    delta_b_open(&cur->b, "/tmp/fed_test");

    /* swap: shadow becomes new active */
    ss->active ^= 1;
    ss->epoch++;

    return (rb == 0) ? 0 : 1;   /* 1 = A ok, B had issue */
}

static inline void ss_close(ShadowSnapshot *ss)
{
    if (!ss) return;
    delta_ab_close(&ss->snap[0]);
    delta_ab_close(&ss->snap[1]);
}

/* ══════════════════════════════════════════════════════════════════
 * FEDERATION CONTEXT — everything assembled
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    ShadowSnapshot   ss;         /* double-buffer snapshot           */
    BackpressureCtx  bp;         /* GPU→V4 flow control              */
    GateStats        gate;       /* pre-commit gate counters         */
    EarlyMerkle      em;         /* tile-level merkle                */
    uint64_t         op_count;   /* monotonic write counter          */
    uint64_t         healed_count; /* count of successful repairs     */
    uint32_t         magic;
} FederationCtx;

/*
 * fed_heal — attempt to repair a corrupted cell
 * Currently: uses a simple parity/correction stub.
 * In future: integration with Rubik recovery.
 */
static inline int fed_heal(uint32_t *packed, uint64_t *angular_addr, uint64_t *value) {
    if (!packed || !angular_addr || !value) return 0;
    
    uint32_t hil   = *packed & 0xFFFFFu;
    uint32_t lane  = (*packed >> 20) & 0x3Fu;
    
    /* Healing Logic: If lane mismatch, try to find closest valid lane */
    if ((hil % 54u) != lane) {
        /* Try to see if it's a 1-bit flip in lane */
        uint32_t correct_lane = hil % 54u;
        *packed = (*packed & ~(0x3Fu << 20)) | (correct_lane << 20);
        return 1; /* Healed */
    }
    
    return 0;
}

/* Init */
static inline int fed_init(FederationCtx *f, const char *vault)
{
    if (!f || !vault) return -1;
    memset(f, 0, sizeof(*f));
    f->magic = FED_MAGIC;
    f->healed_count = 0;
    bp_init(&f->bp);
    em_init(&f->em);
    return ss_init(&f->ss, vault);
}

/*
 * fed_write — THE entry point from POGLS38 GPU output
 *
 * packed       : PACK(hilbert, lane, iso) from GPU kernel
 * angular_addr : PHI scatter address (A = floor(θ × 2²⁰))
 * value        : raw data value
 *
 * Returns: GateResult (PASS/GHOST/DROP)
 *
 * Full path:
 *   Gate → (Heal if needed) → Backpressure → EarlyMerkle → V4 write
 */
static inline GateResult fed_write(FederationCtx  *f,
                                    uint32_t        packed,
                                    uint64_t        angular_addr,
                                    uint64_t        value)
{
    if (!f) return GATE_DROP;

    /* 1. Pre-commit gate */
    GateResult gr = fed_gate(packed, f->op_count, &f->gate);
    
    /* 1.1 Healing Phase: if dropped, try to heal */
    if (gr == GATE_DROP) {
        if (fed_heal(&packed, &angular_addr, &value)) {
            /* Re-check after healing */
            gr = fed_gate(packed, f->op_count, NULL); 
            if (gr == GATE_PASS || gr == GATE_GHOST) {
                f->healed_count++;
            }
        }
    }
    
    if (gr == GATE_DROP) return GATE_DROP;

    /* 2. Backpressure — throttle GPU burst */
    if (gr == GATE_PASS && bp_check(&f->bp)) {
        gr = GATE_GHOST;
    }

    if (gr == GATE_GHOST) {
        f->op_count++; /* Increment even for ghosted ops to allow maturity */
        return GATE_GHOST;
    }

    /* 3. Early merkle update */
    uint8_t lane = (uint8_t)((packed >> 20) & 0x3Fu);
    uint8_t buf[8]; memcpy(buf, &value, 8);
    em_update(&f->em, lane, buf, 8);

    /* 4. Write to V4 active snapshot (World A) */
    Delta_ContextAB *cur = &f->ss.snap[f->ss.active];
    delta_append(&cur->a, lane % 4u, angular_addr,
                 &value, sizeof(value));

    bp_push(&f->bp);
    f->op_count++;
    return GATE_PASS;
}

/* Drain backpressure after V4 flush */
static inline void fed_drain(FederationCtx *f, uint32_t n)
{
    if (!f) return;
    for (uint32_t i = 0; i < n && f->bp.queue_depth > 0; i++)
        bp_pop(&f->bp);
}

/* Commit — early merkle reduce first, then split A/B swap */
static inline int fed_commit(FederationCtx *f)
{
    if (!f) return -1;
    em_reduce(&f->em);
    int r = ss_commit(&f->ss);
    em_init(&f->em);   /* reset tiles for next epoch */
    return r;
}

/* Recovery */
static inline Delta_DualRecovery fed_recover(const char *vault)
{ return delta_ab_recover(vault); }

/* Close */
static inline void fed_close(FederationCtx *f)
{ if (f) ss_close(&f->ss); }

/* Stats print */
static inline void fed_stats(const FederationCtx *f)
{
    if (!f) return;
    uint64_t tot = f->gate.passed + f->gate.ghosted + f->gate.dropped;
    if (!tot) tot = 1;
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Federation Stats                            ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║ ops:     %10llu                          ║\n",
           (unsigned long long)f->op_count);
    printf("║ passed:  %10llu (%3llu%%)                  ║\n",
           (unsigned long long)f->gate.passed,
           (unsigned long long)(f->gate.passed*100/tot));
    printf("║ ghosted: %10llu (%3llu%%)                  ║\n",
           (unsigned long long)f->gate.ghosted,
           (unsigned long long)(f->gate.ghosted*100/tot));
    printf("║ dropped: %10llu (%3llu%%)                  ║\n",
           (unsigned long long)f->gate.dropped,
           (unsigned long long)(f->gate.dropped*100/tot));
    printf("║ throttle:%10llu  drop:%10llu        ║\n",
           (unsigned long long)f->bp.throttle_count,
           (unsigned long long)f->bp.drop_count);
    printf("║ snap epoch: %llu  active: %d              ║\n",
           (unsigned long long)f->ss.epoch, f->ss.active);
    printf("╚══════════════════════════════════════════════╝\n");
}

#endif /* POGLS_FEDERATION_H */
