/*
 * pogls_mesh_entry.h — POGLS V4  MeshEntry + Translation Layer + ReflexBias
 * Version: 1.1  (2026-03)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Changes from v1.0:
 *   [FIX-1] sig: Fibonacci hashing (stronger entropy, fewer collisions)
 *   [FIX-2] delta: explicit int32 cast — safe for future phase scale growth
 *   [FIX-3] decay: lazy O(1) per bucket — no global sweep
 *   [FIX-4] positive reinforcement: SEQ resolved zone gets +1 bias
 *
 * Architecture:
 *   V4 hot path → DetachEntry (frozen, 32B)
 *                      ↓ mesh_translate()
 *               MeshEntry (rich context, 24B)
 *                      ↓ async push via mesh_cb
 *               ReflexBias.update() — lazy decay, pos+neg reinforcement
 *                      ↓
 *               V4 route_final: demote MAIN → GHOST if bias ≤ -4
 *                      ↓ (future)
 *               POGLS38 cluster/temporal (long-term memory)
 *
 * Memory model:
 *   Reflex  = short-term muscle memory (this file)
 *   POGLS38 = long-term pattern memory (future)
 *
 * Rules (FROZEN):
 *   - NEVER modify DetachEntry
 *   - mesh_translate() is the ONLY gateway from V4 → Mesh
 *   - All ops async — never block V4 hot path
 *   - Demotion threshold = -4 (require meaningful signal, not single event)
 * ══════════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_MESH_ENTRY_H
#define POGLS_MESH_ENTRY_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <string.h>

/* forward-compatible reason flags (mirrors pogls_detach_lane.h) */
#ifndef DETACH_REASON_GEO_INVALID
#  define DETACH_REASON_GEO_INVALID  0x01u
#  define DETACH_REASON_GHOST_DRIFT  0x02u
#  define DETACH_REASON_UNIT_CIRCLE  0x04u
#  define DETACH_REASON_OVERFLOW     0x80u
#endif

/* DetachEntry forward declaration (full def in pogls_detach_lane.h) */
#ifndef POGLS_DETACH_LANE_H
typedef struct {
    uint64_t  value;
    uint64_t  angular_addr;
    uint64_t  timestamp_ns;
    uint8_t   reason;
    uint8_t   route_was;
    uint8_t   shell_n;
    uint8_t   phase18;
    uint16_t  phase288;
    uint16_t  phase306;
} DetachEntry;
#endif

/* ══════════════════════════════════════════════════════════════════
 * [FIX-1] Hashing constants for sig
 *
 * Fibonacci hashing (Knuth multiplicative method):
 *   addr  : 64-bit Fibonacci constant 2^64 / φ
 *   value : 32-bit variant (matches PHI_UP family)
 *   phase : shift into upper bits to avoid addr/value overlap
 *
 * Properties:
 *   - avalanche: 1-bit input change → ~32 output bits flip
 *   - uniform:   fills all 32 bits even for sequential inputs
 *   - fast:      2 muls + 2 XORs, ~4 cycles
 * ══════════════════════════════════════════════════════════════════ */
#define MESH_SIG_ADDR_MUL   11400714819323198485ull  /* 2^64 / φ        */
#define MESH_SIG_VALUE_MUL  0x9E3779B1u              /* 2^32 / φ        */
#define MESH_SIG_PHASE_SHIFT 16u

/* ══════════════════════════════════════════════════════════════════
 * MeshEntry type — anomaly character classification
 * Priority (high → low): SEQ > BURST > GHOST > ANOMALY
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
    MESH_TYPE_GHOST   = 0,   /* V4 routed to ghost lane             */
    MESH_TYPE_BURST   = 1,   /* high-frequency anomaly cluster      */
    MESH_TYPE_SEQ     = 2,   /* sequential drift (structured miss)  */
    MESH_TYPE_ANOMALY = 3,   /* geo_invalid / unit circle fail      */
} mesh_entry_type_t;

/* ══════════════════════════════════════════════════════════════════
 * MeshEntry — 24B, language กลางระหว่าง V4 และ POGLS38
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t addr;        /* angular address (PHI space)            */
    uint64_t value;       /* raw data value                         */
    uint32_t sig;         /* [FIX-1] Fibonacci hash fingerprint     */
    uint8_t  type;        /* mesh_entry_type_t                      */
    uint8_t  phase18;     /* gate heartbeat (sacred: gate_18 = 18)  */
    int16_t  delta;       /* [FIX-2] (int32)phase288 - (int32)phase306 */
} MeshEntry;              /* 24B exact                              */

typedef char _mesh_entry_sz_check[(sizeof(MeshEntry) == 24u) ? 1 : -1];

#define MESH_ENTRY_MAGIC  0x4D454E54u  /* "MENT" */

/* ══════════════════════════════════════════════════════════════════
 * mesh_classify_type — pure logic, no side effects
 * Priority: SEQ > BURST > GHOST > ANOMALY
 * ══════════════════════════════════════════════════════════════════ */
static inline mesh_entry_type_t mesh_classify_type(const DetachEntry *e)
{
    if (!e) return MESH_TYPE_ANOMALY;

    /* SEQ: ghost drift = structured movement missed (highest priority) */
    if (e->reason & DETACH_REASON_GHOST_DRIFT)
        return MESH_TYPE_SEQ;

    /* BURST: geo_invalid at very early gate phase */
    if ((e->reason & DETACH_REASON_GEO_INVALID) && e->phase18 < 3u)
        return MESH_TYPE_BURST;

    /* GHOST: routed as ghost, no geo_invalid */
    if (e->route_was == 1u && !(e->reason & DETACH_REASON_GEO_INVALID))
        return MESH_TYPE_GHOST;

    return MESH_TYPE_ANOMALY;
}

/* ══════════════════════════════════════════════════════════════════
 * mesh_translate — THE gateway (DetachEntry → MeshEntry)
 *
 * [FIX-1] sig: Fibonacci hashing for strong entropy
 * [FIX-2] delta: explicit int32 cast before narrowing to int16
 *
 * Called from Mesh layer drain callback ONLY.
 * NEVER called from V4 hot path.
 * ══════════════════════════════════════════════════════════════════ */
static inline MeshEntry mesh_translate(const DetachEntry *e)
{
    MeshEntry m;
    if (!e) { memset(&m, 0, sizeof(m)); return m; }

    m.addr    = e->angular_addr;
    m.value   = e->value;

    /* [FIX-1] Fibonacci hash: strong avalanche, uniform distribution
     * addr  × Fib64 → upper 32 bits
     * value × Fib32 → mixed in
     * phase << 16   → upper bits to avoid overlap with value low bits */
    m.sig = (uint32_t)((e->angular_addr * MESH_SIG_ADDR_MUL) >> 32)
            ^ (uint32_t)(e->value * MESH_SIG_VALUE_MUL)
            ^ ((uint32_t)e->phase18 << MESH_SIG_PHASE_SHIFT);

    m.type    = (uint8_t)mesh_classify_type(e);
    m.phase18 = e->phase18;

    /* [FIX-2] Explicit int32 subtraction before int16 narrowing
     * Safe for phase288 ∈ [0,287], phase306 ∈ [0,305]
     * Max diff = 305, well within int16 range [-32768, 32767]
     * Cast chain explicit to satisfy strict compilers             */
    m.delta = (int16_t)((int32_t)e->phase288 - (int32_t)e->phase306);

    return m;
}

/* ══════════════════════════════════════════════════════════════════
 * is_mesh_anomaly — guard: send only meaningful signals to Mesh
 * ══════════════════════════════════════════════════════════════════ */
static inline int is_mesh_anomaly(const DetachEntry *e)
{
    if (!e) return 0;
    return (e->reason & DETACH_REASON_GHOST_DRIFT)  ||
           (e->reason & DETACH_REASON_GEO_INVALID)  ||
           (e->reason & DETACH_REASON_UNIT_CIRCLE);
}

/* ══════════════════════════════════════════════════════════════════
 * MeshEntryBuf — SPSC ring 1024 × 24B = 24KB (L2 resident)
 * Producer: Mesh drain callback (off hot path)
 * Consumer: POGLS38 cluster engine
 * ══════════════════════════════════════════════════════════════════ */
#define MESH_ENTRY_BUF_SIZE  1024u
#define MESH_ENTRY_BUF_MASK  (MESH_ENTRY_BUF_SIZE - 1u)

typedef struct {
    MeshEntry         ring[MESH_ENTRY_BUF_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    uint64_t  pushed;
    uint64_t  consumed;
    uint64_t  overflows;
    uint32_t  magic;
} MeshEntryBuf;

static inline void mesh_entry_buf_init(MeshEntryBuf *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->magic = MESH_ENTRY_MAGIC;
}

static inline int mesh_entry_push(MeshEntryBuf *b, const MeshEntry *e)
{
    if (!b || !e) return 0;
    uint32_t h = b->head;
    uint32_t t = __atomic_load_n(&b->tail, __ATOMIC_ACQUIRE);
    if ((uint32_t)(h - t) >= MESH_ENTRY_BUF_SIZE) {
        __atomic_store_n(&b->tail, t + 1u, __ATOMIC_RELEASE);
        b->overflows++;
    }
    b->ring[h & MESH_ENTRY_BUF_MASK] = *e;
    __atomic_store_n(&b->head, h + 1u, __ATOMIC_RELEASE);
    b->pushed++;
    return 1;
}

static inline int mesh_entry_pop(MeshEntryBuf *b, MeshEntry *out)
{
    if (!b || !out) return 0;
    uint32_t t = b->tail;
    uint32_t h = __atomic_load_n(&b->head, __ATOMIC_ACQUIRE);
    if (t == h) return 0;
    *out = b->ring[t & MESH_ENTRY_BUF_MASK];
    __atomic_store_n(&b->tail, t + 1u, __ATOMIC_RELEASE);
    b->consumed++;
    return 1;
}

static inline uint32_t mesh_entry_drain(MeshEntryBuf *b,
                                         MeshEntry *out, uint32_t max)
{
    if (!b || !out || max == 0) return 0;
    uint32_t n = 0;
    while (n < max && mesh_entry_pop(b, &out[n])) n++;
    return n;
}

static inline uint32_t mesh_entry_pending(const MeshEntryBuf *b)
{
    if (!b) return 0;
    return b->head - b->tail;
}

/* ══════════════════════════════════════════════════════════════════
 * ReflexBias v1.1 — instant feedback loop
 *
 * [FIX-3] Lazy decay: O(1) per bucket, not O(N) global sweep
 *         Each bucket tracks its own last_decay epoch.
 *         On access: catch up missed decay cycles before read/write.
 *
 * [FIX-4] Positive reinforcement: SEQ resolved → +1 bias
 *         System can reward stable zones, not only punish bad ones.
 *         Prevents permanent all-negative drift over time.
 *
 * 256 buckets × int8_t = 256B table (fits 4 cache lines)
 * Keyed by addr >> 12 (4KB page granularity)
 * ══════════════════════════════════════════════════════════════════ */
#define REFLEX_BUCKETS           256u
#define REFLEX_BUCKET_MASK       (REFLEX_BUCKETS - 1u)
#define REFLEX_DECAY_SHIFT       3u      /* 7/8 decay — matches FaceState */
#define REFLEX_DECAY_INTERVAL    64u     /* global epoch increment period  */
#define REFLEX_DEMOTE_THRESHOLD  (-4)    /* bias ≤ this → demote MAIN      */
#define REFLEX_MAX               127
#define REFLEX_MIN               (-128)

typedef struct {
    int8_t   bias[REFLEX_BUCKETS];        /* signed bias per 4KB zone       */

    /* [FIX-3] lazy decay: per-bucket epoch, global epoch counter */
    uint8_t  last_decay[REFLEX_BUCKETS];  /* epoch of last decay per bucket */
    uint32_t global_epoch;               /* increments every DECAY_INTERVAL */
    uint32_t push_count;                 /* trigger for epoch advance       */

    /* stats */
    uint64_t reinforcements;   /* negative bias events               */
    uint64_t rewards;          /* [FIX-4] positive bias events       */
    uint64_t lazy_decays;      /* lazy catch-up decay applications   */
    uint64_t decays;           /* epoch advances                     */
} ReflexBias;

static inline void reflex_init(ReflexBias *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

/* ── [FIX-3] lazy catch-up decay for one bucket ──────────────────
 * Called on every read/write of a bucket.
 * Applies all missed decay cycles since last access in O(1).
 * Uses bit-shift exponentiation: bias × (7/8)^missed_epochs
 * ─────────────────────────────────────────────────────────────── */
static inline void _reflex_lazy_decay(ReflexBias *r, uint32_t bucket)
{
    uint8_t current_epoch = (uint8_t)r->global_epoch;
    uint8_t missed = (uint8_t)(current_epoch - r->last_decay[bucket]);
    if (missed == 0) return;

    int8_t bv = r->bias[bucket];
    /* apply missed decay cycles: b = b × (7/8)^missed
     * fast approximation: each shift removes 1/8, apply `missed` times
     * capped at 16 iterations to bound cost (16 × (7/8)^16 ≈ 0.117) */
    uint8_t steps = missed < 16u ? missed : 16u;
    for (uint8_t s = 0; s < steps; s++) {
        bv = (int8_t)((int)bv - ((int)bv >> REFLEX_DECAY_SHIFT));
    }
    r->bias[bucket] = bv;
    r->last_decay[bucket] = current_epoch;
    r->lazy_decays += missed;
}

/* ── advance global epoch (called every DECAY_INTERVAL pushes) ── */
static inline void _reflex_advance_epoch(ReflexBias *r)
{
    r->push_count++;
    if (r->push_count % REFLEX_DECAY_INTERVAL == 0) {
        r->global_epoch++;
        r->decays++;
    }
}

/*
 * reflex_update — called from mesh_cb for each anomaly MeshEntry
 *
 * Penalty table (negative = bad zone):
 *   GHOST   = -1 (mild — routing miss, recoverable)
 *   SEQ     = -1 (mild — structured miss)
 *   BURST   = -3 (hard — high-frequency bad zone)
 *   ANOMALY = -2 (medium — geo/unit circle fail)
 *
 * [FIX-4] Positive reward table:
 *   SEQ on stable zone (+1): system recognises improving routing
 *   Applied when bias is already near zero (recovering zone)
 */
static inline void reflex_update(ReflexBias *r, const MeshEntry *e)
{
    if (!r || !e) return;

    uint32_t bucket = (uint32_t)((e->addr >> 12) & REFLEX_BUCKET_MASK);

    /* [FIX-3] lazy catch-up before write */
    _reflex_lazy_decay(r, bucket);

    int delta = 0;
    switch ((mesh_entry_type_t)e->type) {
    case MESH_TYPE_GHOST:   delta = -1; break;
    case MESH_TYPE_SEQ:     delta = -1; break;
    case MESH_TYPE_BURST:   delta = -3; break;
    case MESH_TYPE_ANOMALY: delta = -2; break;
    default:                delta = -1; break;
    }

    /* [FIX-4] positive reinforcement:
     * SEQ on a zone that is recovering (bias > -2) → small reward
     * Logic: SEQ = structured miss, pattern is learning to avoid it.
     * If zone is not deeply bad → reinforce the "getting better" signal. */
    if ((mesh_entry_type_t)e->type == MESH_TYPE_SEQ &&
        r->bias[bucket] > -2) {
        delta = +1;
        r->rewards++;
    } else {
        r->reinforcements++;
    }

    /* apply + clamp */
    int cur = (int)r->bias[bucket] + delta;
    if (cur > REFLEX_MAX) cur = REFLEX_MAX;
    if (cur < REFLEX_MIN) cur = REFLEX_MIN;
    r->bias[bucket] = (int8_t)cur;

    _reflex_advance_epoch(r);
}

/*
 * reflex_reward — explicit positive reinforcement for stable zone
 * Call when V4 confirms a zone is routing cleanly (future hook)
 */
static inline void reflex_reward(ReflexBias *r, uint64_t addr)
{
    if (!r) return;
    uint32_t bucket = (uint32_t)((addr >> 12) & REFLEX_BUCKET_MASK);
    _reflex_lazy_decay(r, bucket);
    int cur = (int)r->bias[bucket] + 1;
    if (cur > REFLEX_MAX) cur = REFLEX_MAX;
    r->bias[bucket] = (int8_t)cur;
    r->rewards++;
    _reflex_advance_epoch(r);
}

/*
 * reflex_lookup — O(1) read with lazy decay catch-up
 * Returns int8 bias: negative = demote candidate
 */
static inline int8_t reflex_lookup(const ReflexBias *r, uint64_t addr)
{
    if (!r) return 0;
    uint32_t bucket = (uint32_t)((addr >> 12) & REFLEX_BUCKET_MASK);
    /* const cast for lazy decay — decay is logically a read side-effect */
    _reflex_lazy_decay((ReflexBias *)r, bucket);
    return r->bias[bucket];
}

/*
 * reflex_should_demote — decision helper for pipeline route_final
 * Returns 1 if zone has enough anomaly history to demote MAIN → GHOST
 */
static inline int reflex_should_demote(const ReflexBias *r, uint64_t addr)
{
    return reflex_lookup(r, addr) <= (int8_t)REFLEX_DEMOTE_THRESHOLD;
}

#endif /* POGLS_MESH_ENTRY_H */
