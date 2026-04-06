/*
 * geo_kv.h — KV_store: truth layer beneath GeoPipeline
 * ══════════════════════════════════════════════════════
 * Session 15
 *
 * Design:
 *   open addressing + linear probe
 *   capacity = 8192 (power-of-2, mask = CAP-1)
 *   load factor ≤ 0.70 → max 5734 live entries
 *   key = uint64_t merkle_root (content-addressed, entropy ok)
 *   val = uint64_t payload
 *   tombstone = special DEAD sentinel (not zero — zero is valid val)
 *
 * Fast-path contract:
 *   kv_get() is only called on gp_read() == 0  (cache+store miss)
 *   Expected hit rate from L1+L2 ≥ 99% → this runs <1% of writes
 *   No over-engineering needed.
 *
 * Caller pattern:
 *   gp_write(gp, key, val);
 *   kv_put(kv, key, val);          // always write truth
 *
 *   if (!gp_read(gp, key, &out))
 *       out = kv_get(kv, key);     // fallback only
 */

#ifndef GEO_KV_H
#define GEO_KV_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ── Constants ──────────────────────────────────────────────────── */

#define KV_CAP       8192U          /* power-of-2 */
#define KV_MASK      (KV_CAP - 1)
#define KV_MAX_LOAD  5734U          /* floor(8192 * 0.70) */

#define KV_KEY_EMPTY 0x0000000000000000ULL   /* never a valid merkle_root */
#define KV_KEY_DEAD  0xDEADDEADDEADDEADULL  /* tombstone after delete */

/* PHI64 already defined in geo_cache.h; guard for standalone use */
#ifndef PHI64
#define PHI64 0x9e3779b97f4a7c15ULL
#endif

/* ── Slot ───────────────────────────────────────────────────────── */

typedef struct {
    uint64_t key;
    uint64_t val;
} KVSlot;

/* ── Table ──────────────────────────────────────────────────────── */

typedef struct {
    KVSlot   slots[KV_CAP];
    uint32_t count;      /* live entries */
    uint32_t tombstones; /* DEAD slots (probe must skip) */
    /* diagnostics */
    uint32_t puts;
    uint32_t gets;
    uint32_t misses;
    uint32_t max_probe;  /* longest probe seen */
} GeoKV;

/* ── Init ───────────────────────────────────────────────────────── */

static inline void kv_init(GeoKV *kv) {
    memset(kv, 0, sizeof(*kv));
    /* slots[i].key == 0 == KV_KEY_EMPTY after memset ✓ */
}

/* ── Hash → starting index ──────────────────────────────────────── */
/*
 * merkle_root already has good entropy → multiply by PHI64 to
 * spread bits, take top bits for index.
 * power-of-2 table: use & mask (not %) → no branch, no div
 */
static inline uint32_t kv_hash(uint64_t key) {
    return (uint32_t)((key * PHI64) >> 32) & KV_MASK;
}

/* ── Put (insert / update) ──────────────────────────────────────── */
/*
 * returns: 1 = stored, 0 = table full (should not happen in normal use)
 *
 * insert strategy: find first EMPTY or DEAD slot after hash(key)
 * update strategy: if key already exists → overwrite val in-place
 *
 * tombstone reuse: if we pass a DEAD slot during probe,
 *   remember it — if key not found ahead, reuse DEAD slot
 *   (avoids inflating tombstone count needlessly)
 */
static inline int kv_put(GeoKV *kv, uint64_t key, uint64_t val) {
    assert(key != KV_KEY_EMPTY && key != KV_KEY_DEAD);
    if (kv->count >= KV_MAX_LOAD) return 0;  /* table full — safe in release */

    uint32_t idx   = kv_hash(key);
    uint32_t probe = 0;
    int32_t  dead_idx = -1;  /* first tombstone seen */

    for (;;) {
        KVSlot *s = &kv->slots[idx];

        if (s->key == KV_KEY_EMPTY) {
            /* end of chain — insert here (or reuse tombstone) */
            if (dead_idx >= 0) {
                /* reuse earlier DEAD slot */
                kv->slots[dead_idx].key = key;
                kv->slots[dead_idx].val = val;
                assert(kv->tombstones > 0);
                kv->tombstones--;
            } else {
                s->key = key;
                s->val = val;
            }
            kv->count++;
            kv->puts++;
            if (probe > kv->max_probe) kv->max_probe = probe;
            return 1;
        }

        if (s->key == KV_KEY_DEAD) {
            if (dead_idx < 0) dead_idx = (int32_t)idx;
        } else if (s->key == key) {
            /* update in-place */
            s->val = val;
            kv->puts++;
            return 1;
        }

        idx = (idx + 1) & KV_MASK;
        probe++;

        if (probe >= KV_CAP) return 0;  /* full — should never reach */
    }
}

/* ── Get ─────────────────────────────────────────────────────────── */
/*
 * returns: val if found, 0 if not found
 * (0 is a valid payload value — use kv_has() if disambiguation needed)
 *
 * probe stops at first EMPTY (not DEAD) — DEAD means chain continues
 */
static inline uint64_t kv_get(GeoKV *kv, uint64_t key) {
    assert(key != KV_KEY_EMPTY && key != KV_KEY_DEAD);

    uint32_t idx   = kv_hash(key);
    uint32_t probe = 0;

    kv->gets++;

    for (;;) {
        KVSlot *s = &kv->slots[idx];

        if (s->key == KV_KEY_EMPTY) {
            kv->misses++;
            return 0;
        }
        if (s->key != KV_KEY_DEAD && s->key == key) {
            return s->val;
        }

        idx = (idx + 1) & KV_MASK;
        if (++probe >= KV_CAP) break;
    }

    kv->misses++;
    return 0;
}

/* ── Has (disambiguate 0-val from miss) ─────────────────────────── */

static inline int kv_has(GeoKV *kv, uint64_t key) {
    assert(key != KV_KEY_EMPTY && key != KV_KEY_DEAD);

    uint32_t idx   = kv_hash(key);
    uint32_t probe = 0;

    for (;;) {
        KVSlot *s = &kv->slots[idx];
        if (s->key == KV_KEY_EMPTY) return 0;
        if (s->key != KV_KEY_DEAD && s->key == key) return 1;
        idx = (idx + 1) & KV_MASK;
        if (++probe >= KV_CAP) break;
    }
    return 0;
}

/* ── Delete ─────────────────────────────────────────────────────── */
/*
 * mark DEAD (tombstone), not EMPTY — preserves probe chains
 * tombstone count tracked for future rehash decision
 */
static inline int kv_del(GeoKV *kv, uint64_t key) {
    assert(key != KV_KEY_EMPTY && key != KV_KEY_DEAD);

    uint32_t idx   = kv_hash(key);
    uint32_t probe = 0;

    for (;;) {
        KVSlot *s = &kv->slots[idx];
        if (s->key == KV_KEY_EMPTY) return 0;  /* not found */
        if (s->key != KV_KEY_DEAD && s->key == key) {
            s->key = KV_KEY_DEAD;
            s->val = 0;
            assert(kv->count > 0);
            kv->count--;
            kv->tombstones++;
            return 1;
        }
        idx = (idx + 1) & KV_MASK;
        if (++probe >= KV_CAP) break;
    }
    return 0;
}

/* ── Load factor check ──────────────────────────────────────────── */

static inline float kv_load(const GeoKV *kv) {
    return (float)(kv->count + kv->tombstones) / KV_CAP;
}

static inline int kv_ok_to_put(const GeoKV *kv) {
    return (kv->count < KV_MAX_LOAD);
}

/* ── Stats ───────────────────────────────────────────────────────── */

static inline void kv_print_stats(const GeoKV *kv) {
    printf("── GeoKV ──\n");
    printf("  count=%u  tombstones=%u  load=%.2f\n",
           kv->count, kv->tombstones, kv_load(kv));
    printf("  puts=%u  gets=%u  misses=%u  max_probe=%u\n",
           kv->puts, kv->gets, kv->misses, kv->max_probe);
}

/* ── Compile-time sanity ─────────────────────────────────────────── */
_Static_assert((KV_CAP & KV_MASK) == 0,   "KV_CAP must be power-of-2");
_Static_assert(KV_CAP >= 8192,             "capacity sanity");

#endif /* GEO_KV_H */
