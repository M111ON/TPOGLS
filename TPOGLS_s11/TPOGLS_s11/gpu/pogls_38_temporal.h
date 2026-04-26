/*
 * pogls_38_temporal.h — POGLS38  Temporal + Negative Shadow Layer
 * ══════════════════════════════════════════════════════════════════════
 *
 * Port จาก V3.4 source (v34_src/):
 *   pogls_temporal.h        FiftyFourBridge ring+hash
 *   pogls_temporal_lane5.h  NegativeShadow + InvertedTimeline
 *   pogls_pressure_bridge.h 16-head pressure-aware dispatch
 *   pogls_world.h           World 4n/5n/6n + XOR guard
 *
 * ══════════════════════════════════════════════════════════════════════
 * "Tails track pointer = negative ของ pointer"
 *
 *   ring (live):    head++ per write — forward timeline
 *   NegativeShadow: เมื่อ evict → บันทึก "รอยเท้า" ใน Lane5
 *                   Tails อ่าน negative pool เป็น emergency key
 *                   ถ้า addr ไม่อยู่ใน ring แต่อยู่ใน neg → เคยผ่านระบบ
 *   InvertedTimeline: head-- audit ring — เดินถอยหลัง
 *                   Tails ใช้อ่าน history โดยไม่ต้อง replay forward
 *
 * เปรียบเทียบ: เหมือน browser history + deleted history
 *   ring = ปัจจุบัน (history อยู่)
 *   NegativeShadow = ถังขยะ (ยังเปิดดูได้ ยังไม่ลบถาวร)
 *   InvertedTimeline = Back button ที่เดินถอยหลัง
 *
 * ══════════════════════════════════════════════════════════════════════
 * XOR guard (World 4n/5n/6n):
 *   xor_guard[0] = XOR ของ payload
 *   xor_guard[1] = xor_guard[0] ^ PHI_DOWN   ← "negative ของ guard"
 *   verify: recalc XOR == xor_guard[0]
 *   ถ้า memory corrupt xor_guard[0] แต่ไม่ corrupt xor_guard[1]
 *   → ตรวจพบได้ทันที (double guard)
 *
 * ══════════════════════════════════════════════════════════════════════
 * Rule: ไม่ include V4 headers เลย — standalone สำหรับ POGLS38
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_TEMPORAL_H
#define POGLS_38_TEMPORAL_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "pogls_17n_lattice.h"   /* PHI constants, L17 cell */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — TEMPORAL ENTRY (16B, 1 cache line)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint64_t  timestamp_ns;   /* CLOCK_MONOTONIC                        */
    uint32_t  data_addr;      /* angular_addr (fibo_addr result)        */
    uint16_t  cycle_count;    /* กี่รอบผ่าน bridge                     */
    uint8_t   world_id;       /* 0=bridge, 4=4n, 5=5n, 6=6n            */
    uint8_t   layer_depth;    /* nested depth 0-255                     */
} L38TemporalEntry;           /* 16B ✓                                  */

typedef char _l38_te_sz[(sizeof(L38TemporalEntry)==16)?1:-1];

/* world IDs */
#define L38_WORLD_BRIDGE   0u
#define L38_WORLD_4N       4u   /* 648B geometry/render                  */
#define L38_WORLD_5N       5u   /* 640B compute/SIMD AVX2                */
#define L38_WORLD_6N       6u   /* 654B AI connect/recall                */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — NEGATIVE SHADOW (16B, same size as entry)
 *
 * "รอยเท้า" ของ entry ที่ถูก evict ออกจาก ring
 * Tails อ่าน neg pool เป็น emergency key:
 *   addr ไม่ใน ring แต่ใน neg → เคยผ่าน (evicted, not lost)
 *   addr ไม่ใน ring ไม่ใน neg → ไม่เคยผ่านระบบ
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_NEG_FLAG_EVICTED  0x01   /* evict จาก hot ring              */
#define L38_NEG_FLAG_SEEN     0x02   /* Tails เคย recall มาแล้ว         */
#define L38_NEG_FLAG_AUDIT    0x04   /* ถูก flag โดย audit              */
#define L38_NEG_FLAG_TAILS    0x08   /* Tails กำลัง track                */

typedef struct __attribute__((packed)) {
    uint32_t  data_addr;      /* addr ที่ถูก evict                       */
    uint64_t  last_ts_ns;     /* timestamp ก่อน evict                    */
    uint16_t  last_cycle;     /* cycle_count ก่อน evict                  */
    uint8_t   last_world;     /* world ขณะ evict                         */
    uint8_t   flags;          /* L38_NEG_FLAG_*                          */
} L38NegShadow;               /* 16B ✓                                   */

typedef char _l38_neg_sz[(sizeof(L38NegShadow)==16)?1:-1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — INVERTED TIMELINE (16B, audit ring)
 *
 * ring ปกติ: head++ (forward write)
 * InvertedTimeline: head-- (backward read)
 * Tails ใช้เดินย้อนหลัง history โดยไม่ต้อง replay
 *
 * event types:
 *   PASS   = ผ่าน bridge ปกติ
 *   EVICT  = ถูก evict ออก ring
 *   RECALL = Tails recall กลับมา
 *   SNAP   = snapshot point
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_IEVENT_PASS    0u
#define L38_IEVENT_EVICT   1u
#define L38_IEVENT_RECALL  2u
#define L38_IEVENT_SNAP    3u

typedef struct __attribute__((packed)) {
    uint64_t  timestamp_ns;
    uint32_t  data_addr;
    uint16_t  seq;           /* global event sequence                    */
    uint8_t   world_id;
    uint8_t   event_type;    /* L38_IEVENT_*                             */
} L38InvertedEntry;          /* 16B ✓                                    */

typedef char _l38_inv_sz[(sizeof(L38InvertedEntry)==16)?1:-1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — FIFTY-FOUR BRIDGE (V3.7 port, adapted for 17n)
 *
 * ring[256] = hot path  (4KB, L1 fit)
 * addr_index[1024] = hash → ring slot  O(1)
 * neg_pool[512] = NegativeShadow  (8KB)
 * inv_ring[256] = InvertedTimeline audit ring
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_TEMPORAL_RING    256u
#define L38_TEMPORAL_HASH   1024u
#define L38_TEMPORAL_NEG     512u   /* negative shadow pool              */
#define L38_TEMPORAL_INV     256u   /* inverted timeline ring            */
#define L38_TEMPORAL_MAGIC   0x46354252u  /* "F54B"                      */
#define L38_HASH_EMPTY       0xFFFFFFFFu  /* sentinel                    */

typedef struct {
    /* hot ring */
    L38TemporalEntry  ring[L38_TEMPORAL_RING];
    _Atomic uint32_t  head;         /* write pointer (forward)           */
    _Atomic uint32_t  tail;         /* oldest entry                      */
    uint64_t          base_ts;

    /* O(1) hash: data_addr → ring slot */
    uint32_t          addr_index[L38_TEMPORAL_HASH];

    /* NegativeShadow pool — Tails tracks pointer */
    L38NegShadow      neg_pool[L38_TEMPORAL_NEG];
    uint32_t          neg_head;     /* write cursor (circular)           */
    uint32_t          neg_count;

    /* InvertedTimeline — head-- audit ring */
    L38InvertedEntry  inv_ring[L38_TEMPORAL_INV];
    uint32_t          inv_head;     /* starts at INV_CAPACITY, decrements*/
    uint16_t          inv_seq;      /* global event counter              */

    /* stats */
    uint64_t          total_passes;
    uint64_t          total_evicts;
    uint64_t          total_recalls;

    uint32_t          magic;
} L38FiftyFourBridge;

static inline uint64_t _l38_now_ns2(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void l38_bridge_init(L38FiftyFourBridge *b)
{
    memset(b, 0, sizeof(*b));
    b->magic    = L38_TEMPORAL_MAGIC;
    b->base_ts  = _l38_now_ns2();
    b->inv_head = L38_TEMPORAL_INV;  /* starts at top, decrements */
    for (uint32_t i = 0; i < L38_TEMPORAL_HASH; i++)
        b->addr_index[i] = L38_HASH_EMPTY;
}

/* ring count */
static inline uint32_t l38_ring_count(const L38FiftyFourBridge *b)
{
    uint32_t h = atomic_load_explicit(&b->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&b->tail, memory_order_relaxed);
    return (h - t) & (L38_TEMPORAL_RING - 1u);
}

static inline int l38_ring_full(const L38FiftyFourBridge *b)
{
    return l38_ring_count(b) >= L38_TEMPORAL_RING - 1u;
}

/* push entry to negative pool (on evict) */
static inline void l38_neg_push(L38FiftyFourBridge *b,
                                  const L38TemporalEntry *e,
                                  uint8_t flags)
{
    uint32_t slot = b->neg_head % L38_TEMPORAL_NEG;
    b->neg_pool[slot] = (L38NegShadow){
        .data_addr  = e->data_addr,
        .last_ts_ns = e->timestamp_ns,
        .last_cycle = e->cycle_count,
        .last_world = e->world_id,
        .flags      = flags | L38_NEG_FLAG_EVICTED,
    };
    b->neg_head = (b->neg_head + 1) % L38_TEMPORAL_NEG;
    if (b->neg_count < L38_TEMPORAL_NEG) b->neg_count++;
    b->total_evicts++;
}

/* lookup negative pool — O(n) but pool is small */
static inline const L38NegShadow *l38_neg_find(const L38FiftyFourBridge *b,
                                                  uint32_t data_addr)
{
    for (uint32_t i = 0; i < b->neg_count && i < L38_TEMPORAL_NEG; i++) {
        uint32_t idx = (b->neg_head + L38_TEMPORAL_NEG - 1u - i)
                     % L38_TEMPORAL_NEG;
        if (b->neg_pool[idx].data_addr == data_addr &&
            (b->neg_pool[idx].flags & L38_NEG_FLAG_EVICTED))
            return &b->neg_pool[idx];
    }
    return NULL;
}

/* push inverted entry (head-- = backward) */
static inline void l38_inv_push(L38FiftyFourBridge *b,
                                  uint32_t data_addr,
                                  uint8_t  event_type,
                                  uint8_t  world_id)
{
    b->inv_head = (b->inv_head - 1u) % L38_TEMPORAL_INV;
    b->inv_ring[b->inv_head] = (L38InvertedEntry){
        .timestamp_ns = _l38_now_ns2(),
        .data_addr    = data_addr,
        .seq          = b->inv_seq++,
        .world_id     = world_id,
        .event_type   = event_type,
    };
}

/* main pass: write entry → ring, evict oldest if full → neg pool */
static inline int l38_bridge_pass(L38FiftyFourBridge *b,
                                    uint32_t data_addr,
                                    uint8_t  world_id,
                                    uint16_t cycle_count)
{
    if (!b) return -1;

    /* evict oldest if full → push to neg pool */
    if (l38_ring_full(b)) {
        uint32_t t   = atomic_load_explicit(&b->tail, memory_order_relaxed);
        uint32_t slot = t & (L38_TEMPORAL_RING - 1u);
        l38_neg_push(b, &b->ring[slot], L38_NEG_FLAG_EVICTED);
        l38_inv_push(b, b->ring[slot].data_addr,
                     L38_IEVENT_EVICT, b->ring[slot].world_id);
        /* clear hash index */
        uint32_t hk = (b->ring[slot].data_addr * PHI_UP) % L38_TEMPORAL_HASH;
        if (b->addr_index[hk] == slot) b->addr_index[hk] = L38_HASH_EMPTY;
        atomic_store_explicit(&b->tail,
                              (t + 1u) & (L38_TEMPORAL_RING - 1u),
                              memory_order_relaxed);
    }

    /* write new entry */
    uint32_t h    = atomic_fetch_add_explicit(&b->head, 1u, memory_order_relaxed)
                    & (L38_TEMPORAL_RING - 1u);
    b->ring[h] = (L38TemporalEntry){
        .timestamp_ns = _l38_now_ns2(),
        .data_addr    = data_addr,
        .cycle_count  = cycle_count,
        .world_id     = world_id,
        .layer_depth  = 0,
    };
    /* update hash index */
    uint32_t hk = (data_addr * PHI_UP) % L38_TEMPORAL_HASH;
    b->addr_index[hk] = h;

    l38_inv_push(b, data_addr, L38_IEVENT_PASS, world_id);
    b->total_passes++;
    return 0;
}

/* O(1) lookup via hash */
static inline const L38TemporalEntry *l38_bridge_find(
    const L38FiftyFourBridge *b, uint32_t data_addr)
{
    uint32_t hk   = (data_addr * PHI_UP) % L38_TEMPORAL_HASH;
    uint32_t slot = b->addr_index[hk];
    if (slot == L38_HASH_EMPTY || slot >= L38_TEMPORAL_RING) return NULL;
    const L38TemporalEntry *e = &b->ring[slot];
    return (e->data_addr == data_addr) ? e : NULL;
}

/* Tails recall: ring → neg pool fallback */
typedef enum {
    L38_RECALL_NOT_FOUND = 0,
    L38_RECALL_HOT       = 1,   /* found in ring                        */
    L38_RECALL_COLD      = 2,   /* found in NegativeShadow              */
} l38_recall_result_t;

static inline l38_recall_result_t l38_bridge_recall(
    L38FiftyFourBridge *b,
    uint32_t            data_addr,
    L38TemporalEntry   *out_entry)
{
    /* 1. hot: O(1) hash */
    const L38TemporalEntry *e = l38_bridge_find(b, data_addr);
    if (e) {
        if (out_entry) *out_entry = *e;
        l38_inv_push(b, data_addr, L38_IEVENT_RECALL, e->world_id);
        b->total_recalls++;
        return L38_RECALL_HOT;
    }

    /* 2. cold: NegativeShadow (Tails emergency key) */
    const L38NegShadow *ns = l38_neg_find(b, data_addr);
    if (ns) {
        if (out_entry) {
            out_entry->timestamp_ns = ns->last_ts_ns;
            out_entry->data_addr    = ns->data_addr;
            out_entry->cycle_count  = ns->last_cycle;
            out_entry->world_id     = ns->last_world;
            out_entry->layer_depth  = 0;
        }
        l38_inv_push(b, data_addr, L38_IEVENT_RECALL, ns->last_world);
        b->total_recalls++;
        return L38_RECALL_COLD;
    }

    return L38_RECALL_NOT_FOUND;
}

/* Tails: read inverted timeline (backward from most recent)
 * cb(entry, userdata) called for each event
 * max_events: max to iterate, 0 = all */
static inline uint32_t l38_tails_read_inverted(
    const L38FiftyFourBridge *b,
    void (*cb)(const L38InvertedEntry *, void *),
    void *ud,
    uint32_t max_events)
{
    uint32_t count   = 0;
    uint32_t limit   = max_events ? max_events : L38_TEMPORAL_INV;
    uint32_t start   = b->inv_head;

    for (uint32_t i = 0; i < limit; i++) {
        uint32_t idx = (start + i) % L38_TEMPORAL_INV;
        const L38InvertedEntry *ie = &b->inv_ring[idx];
        if (ie->timestamp_ns == 0) break;  /* empty slot */
        if (cb) cb(ie, ud);
        count++;
    }
    return count;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — XOR GUARD (World 4n/5n/6n double guard)
 *
 * xor_guard[0] = XOR ของ payload
 * xor_guard[1] = xor_guard[0] ^ PHI_DOWN  ← negative/complement
 * detect single-field corruption via double check
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t  g0;   /* XOR of payload                                   */
    uint64_t  g1;   /* g0 ^ PHI_DOWN = complement guard                 */
} L38XorGuard;

static inline L38XorGuard l38_xor_guard_seal(const uint8_t *data,
                                                uint32_t       len)
{
    uint64_t acc = 0;
    for (uint32_t i = 0; i + 7 < len; i += 8) {
        uint64_t v; memcpy(&v, data + i, 8); acc ^= v;
    }
    return (L38XorGuard){ .g0 = acc, .g1 = acc ^ (uint64_t)PHI_DOWN };
}

static inline int l38_xor_guard_verify(const uint8_t    *data,
                                         uint32_t           len,
                                         const L38XorGuard *guard)
{
    if (!guard->g0 && !guard->g1) return 1;  /* uninitialized = ok */
    L38XorGuard recomputed = l38_xor_guard_seal(data, len);
    return (recomputed.g0 == guard->g0) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 6 — WORLD 4n/5n/6n (simplified for 17n lattice)
 *
 * V3.4 WorldN พร้อม XOR double guard
 * ported: WorldHeader, sizes, seal/verify functions
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_WORLD4N_SIZE  648u   /* 162×4  geometry                     */
#define L38_WORLD5N_SIZE  640u   /* 128×5  compute/SIMD                 */
#define L38_WORLD6N_SIZE  654u   /* 109×6  AI recall                    */

typedef struct __attribute__((packed)) {
    uint32_t  universe_id;
    uint8_t   world_id;     /* 4/5/6                                    */
    uint8_t   engine_id;
    uint8_t   version;
    uint8_t   flags;
} L38WorldHeader;           /* 8B                                       */

#define L38_WORLD_FLAG_DIRTY     0x01u
#define L38_WORLD_FLAG_VALIDATED 0x04u
#define L38_WORLD_FLAG_WORLD_B   0x08u

/* World 4n: 648B  [0-7]=header [8-39]=coord_slots [40-631]=payload
 *           [632-639]=xor_guard[0] [640-647]=xor_guard[1] */
typedef struct __attribute__((packed)) {
    L38WorldHeader  hdr;                /* 8B                           */
    uint32_t        coord_slots[8];     /* 32B fibo anchor coords        */
    uint8_t         payload[592];       /* 592B geometry DNA             */
    L38XorGuard     guard;              /* 16B double guard              */
} L38World4n;                           /* = 648B ✓                      */
typedef char _l38_w4n[(sizeof(L38World4n)==L38_WORLD4N_SIZE)?1:-1];

/* World 6n: 654B — AI recall/connect
 * [0-7]=header [8-637]=payload [638-653]=guard(10B) + HoneycombHook(6B) */
typedef struct __attribute__((packed)) {
    L38WorldHeader  hdr;                /* 8B                           */
    uint8_t         payload[630];       /* 630B AI session data          */
    L38XorGuard     guard;              /* 16B double guard              */
} L38World6n;                           /* = 654B ✓                      */
typedef char _l38_w6n[(sizeof(L38World6n)==L38_WORLD6N_SIZE)?1:-1];

/* seal World 4n XOR guard */
static inline void l38_world4n_seal(L38World4n *w)
{
    w->guard = l38_xor_guard_seal(
        (const uint8_t *)w, offsetof(L38World4n, guard));
}

static inline int l38_world4n_verify(const L38World4n *w) {
    L38XorGuard g; memcpy(&g, &w->guard, sizeof(g));
    return l38_xor_guard_verify(
        (const uint8_t *)w, offsetof(L38World4n, guard), &g);
}

static inline void l38_world6n_seal(L38World6n *w) {
    w->guard = l38_xor_guard_seal(
        (const uint8_t *)w, offsetof(L38World6n, guard));
}

static inline int l38_world6n_verify(const L38World6n *w) {
    L38XorGuard g; memcpy(&g, &w->guard, sizeof(g));
    return l38_xor_guard_verify(
        (const uint8_t *)w, offsetof(L38World6n, guard), &g);
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 7 — PRESSURE BRIDGE (16-head, no shared state)
 *
 * Per-head: FiftyFourBridge + NegativeShadow + pressure counter
 * Backpressure: CAS fails > YIELD_THRESH → sched_yield()
 *              CAS fails > STEAL_THRESH  → redirect to less busy head
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_PB_HEADS         16u
#define L38_PB_MASK          (L38_PB_HEADS - 1u)
#define L38_PB_YIELD_THRESH   4u
#define L38_PB_STEAL_THRESH   8u

typedef struct {
    L38FiftyFourBridge  bridge;       /* temporal ring + neg + inv      */
    _Atomic int         pressure;     /* contention level               */
    uint32_t            head_id;
    uint64_t            ops_own;
    uint64_t            ops_stolen;
    uint64_t            ops_redirected;
    uint64_t            yields;
} L38PressureHead;

typedef struct {
    L38PressureHead heads[L38_PB_HEADS];
} L38PressureBridgeFull;

static inline void l38_pb_init(L38PressureBridgeFull *pb)
{
    memset(pb, 0, sizeof(*pb));
    for (uint32_t i = 0; i < L38_PB_HEADS; i++) {
        pb->heads[i].head_id = i;
        l38_bridge_init(&pb->heads[i].bridge);
    }
}

/* route addr → head_id (mask route) */
static inline uint32_t l38_pb_route(uint32_t data_addr) {
    return data_addr & L38_PB_MASK;
}

/* find least-busy head for work steal */
static inline uint32_t l38_pb_steal_target(const L38PressureBridgeFull *pb)
{
    uint32_t best_id  = 0;
    int      best_p   = atomic_load_explicit(
                            &pb->heads[0].pressure, memory_order_relaxed);
    for (uint32_t i = 1; i < L38_PB_HEADS; i++) {
        int p = atomic_load_explicit(&pb->heads[i].pressure,
                                      memory_order_relaxed);
        if (p < best_p) { best_p = p; best_id = i; }
    }
    return best_id;
}

/* write through pressure bridge */
static inline uint32_t l38_pb_pass(L38PressureBridgeFull *pb,
                                     uint32_t data_addr,
                                     uint8_t  world_id,
                                     uint16_t cycle_count)
{
    uint32_t hid   = l38_pb_route(data_addr);
    L38PressureHead *h = &pb->heads[hid];

    int p = atomic_load_explicit(&h->pressure, memory_order_relaxed);
    if (p >= (int)L38_PB_STEAL_THRESH) {
        /* redirect to less-busy head */
        hid = l38_pb_steal_target(pb);
        h   = &pb->heads[hid];
        pb->heads[l38_pb_route(data_addr)].ops_redirected++;
    }

    atomic_fetch_add_explicit(&h->pressure, 1, memory_order_relaxed);
    l38_bridge_pass(&h->bridge, data_addr, world_id, cycle_count);
    atomic_fetch_sub_explicit(&h->pressure, 1, memory_order_relaxed);

    h->ops_own++;
    return hid;
}

/* Tails recall through all heads */
static inline l38_recall_result_t l38_pb_recall(
    L38PressureBridgeFull *pb, uint32_t data_addr,
    L38TemporalEntry *out)
{
    /* 1. try direct head */
    uint32_t hid = l38_pb_route(data_addr);
    l38_recall_result_t r = l38_bridge_recall(&pb->heads[hid].bridge,
                                               data_addr, out);
    if (r != L38_RECALL_NOT_FOUND) return r;

    /* 2. scan all heads (addr may have been redirected) */
    for (uint32_t i = 0; i < L38_PB_HEADS; i++) {
        if (i == hid) continue;
        r = l38_bridge_recall(&pb->heads[i].bridge, data_addr, out);
        if (r != L38_RECALL_NOT_FOUND) return r;
    }
    return L38_RECALL_NOT_FOUND;
}

#endif /* POGLS_38_TEMPORAL_H */
