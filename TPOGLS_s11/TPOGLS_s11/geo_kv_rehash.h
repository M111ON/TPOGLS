/*
 * geo_kv_rehash.h — KV in-place rehash (Session 17)
 *
 * Trigger: tombstone/(count+tombstone) > 0.30
 * Strategy: scan all live slots → re-insert into fresh table
 * Cost: O(N) one-shot, rare (only after many deletes)
 */

#ifndef GEO_KV_REHASH_H
#define GEO_KV_REHASH_H

#include "geo_kv.h"
#include <string.h>

#define KVR_TOMBSTONE_THRESH_NUM 3   /* 3/10 = 30% */
#define KVR_TOMBSTONE_THRESH_DEN 10

static inline int kvr_needs_rehash(const GeoKV *kv) {
    uint32_t total = kv->count + kv->tombstones;
    if (total == 0) return 0;
    /* tombstones/total > 3/10  ↔  tombstones*10 > total*3 */
    return (kv->tombstones * KVR_TOMBSTONE_THRESH_DEN >
            total        * KVR_TOMBSTONE_THRESH_NUM);
}

/*
 * kv_rehash() — compact live entries into fresh table in-place.
 * Preserves: count, puts, gets, misses, max_probe.
 * Resets:    tombstones → 0.
 */
static inline void kv_rehash(GeoKV *kv) {
    GeoKV fresh;
    memset(&fresh, 0, sizeof(fresh));
    /* carry stats */
    fresh.puts      = kv->puts;
    fresh.gets      = kv->gets;
    fresh.misses    = kv->misses;
    fresh.max_probe = kv->max_probe;

    for (uint32_t i = 0; i < KV_CAP; i++) {
        KVSlot *s = &kv->slots[i];
        if (s->key == KV_KEY_EMPTY || s->key == KV_KEY_DEAD) continue;
        kv_put(&fresh, s->key, s->val);
    }
    *kv = fresh;
}

/* call after any kv_del() to auto-rehash if needed */
static inline void kv_del_auto(GeoKV *kv, uint64_t key) {
    kv_del(kv, key);
    if (kvr_needs_rehash(kv)) kv_rehash(kv);
}

#endif /* GEO_KV_REHASH_H */
