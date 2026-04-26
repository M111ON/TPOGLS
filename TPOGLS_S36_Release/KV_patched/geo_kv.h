/*
 * geo_kv.h — stub for S18 build (mirrors actual S17 interface)
 * Real implementation lives in pogls_sdk.h / geo_kv full header.
 */
#ifndef GEO_KV_H
#define GEO_KV_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define KV_CAP       8192
#define KV_MAX_LOAD  5734
#define KV_KEY_EMPTY 0ULL
#define KV_KEY_DEAD  UINT64_MAX
#define MAX_PROBE    128

typedef struct { uint64_t key, val; } KVSlot;

typedef struct {
    KVSlot   slots[KV_CAP];
    uint32_t count;
    uint32_t tombstones;
    uint64_t puts, gets, misses;
    uint32_t max_probe;
} GeoKV;

static inline void kv_init(GeoKV *kv) { memset(kv, 0, sizeof(*kv)); }

/* Returns 1 = inserted/updated, 0 = probe exhausted (table full) */
static inline int kv_put(GeoKV *kv, uint64_t key, uint64_t val) {
    uint32_t h = (uint32_t)(key & (KV_CAP - 1));
    for (uint32_t i = 0; i < MAX_PROBE; i++) {
        uint32_t idx = (h + i) & (KV_CAP - 1);
        uint64_t k = kv->slots[idx].key;
        if (k == KV_KEY_EMPTY || k == KV_KEY_DEAD || k == key) {
            if (k == KV_KEY_DEAD) kv->tombstones--;
            if (k != key) kv->count++;
            kv->slots[idx] = (KVSlot){key, val};
            kv->puts++;
            if (i > kv->max_probe) kv->max_probe = i;
            return 1;
        }
    }
    return 0;  /* probe exhausted — caller must handle */
}

static inline uint64_t kv_get(GeoKV *kv, uint64_t key) {
    uint32_t h = (uint32_t)(key & (KV_CAP - 1));
    kv->gets++;
    for (uint32_t i = 0; i < MAX_PROBE; i++) {
        uint32_t idx = (h + i) & (KV_CAP - 1);
        uint64_t k = kv->slots[idx].key;
        if (k == key) return kv->slots[idx].val;
        if (k == KV_KEY_EMPTY) { kv->misses++; return 0; }
    }
    kv->misses++;
    return 0;
}

static inline void kv_del(GeoKV *kv, uint64_t key) {
    uint32_t h = (uint32_t)(key & (KV_CAP - 1));
    for (uint32_t i = 0; i < MAX_PROBE; i++) {
        uint32_t idx = (h + i) & (KV_CAP - 1);
        uint64_t k = kv->slots[idx].key;
        if (k == key) {
            kv->slots[idx].key = KV_KEY_DEAD;
            kv->count--;
            kv->tombstones++;
            return;
        }
        if (k == KV_KEY_EMPTY) return;
    }
}

#endif /* GEO_KV_H */
