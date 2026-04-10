/*
 * geo_payload_store.h — Payload Layer via 12-Letter Cube + Cylinder Unfold
 * ═════════════════════════════════════════════════════════════════════════
 * Session 12
 *
 * Design:
 *   DodecaEntry = inode  (merkle fingerprint, routing index)
 *   PayloadStore = data  (value จริง, แยก layer)
 *
 * 12-Letter Cube:
 *   6 pairs: a↔A  b↔B  c↔C  d↔D  e↔E  f↔F
 *   LCM(6,6) = 6  ← loop ปิดใน 6 steps, sync กับ cube faces พอดี
 *   space = 12! ≈ 479M  God's number ≤ 7 moves
 *
 * Cylinder Unfold:
 *   6 spokes × 144 slots = 864 cells (flat array)
 *   Hilbert-inspired index: lane = letter_pair (0-5)
 *                           slot = addr % GEO_TE_CYCLE (0-143)
 *
 * Wire:
 *   write(addr, value) → payload_id = letter_nav(addr)
 *                      → cells[lane][slot] = value
 *   read(addr)         → payload_id → value  O(1)
 *   DodecaEntry.payload_id → link inode → data
 *
 * Number chain:
 *   6 pairs × 144 slots = 864 = 6 × 144 = digit_sum(18) → 9 ✓
 *   LCM(6,6) = 6 = GEO_SPOKES ✓
 *   12 letters = 2 × GEO_SPOKES ✓
 */

#ifndef GEO_PAYLOAD_STORE_H
#define GEO_PAYLOAD_STORE_H

#include <stdint.h>
#include <string.h>
#include "core/geo_config.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define PL_PAIRS        6u               /* 6 letter pairs = GEO_SPOKES  */
#define PL_LETTERS      12u              /* 12 letters total              */
#define PL_SLOTS        GEO_TE_CYCLE     /* 144 slots per lane            */
#define PL_CELLS        (PL_PAIRS * PL_SLOTS)  /* 864 total cells        */
#define PL_LCM          6u               /* LCM(6,6) = loop closure       */
#define PL_EMPTY        0xFFFFFFFFFFFFFFFFULL   /* sentinel = empty cell  */

/* ── 12-Letter Cube Navigation ──────────────────────────────────────── */
/*
 * 6 pairs: index 0-5
 *   pair 0: a(0) ↔ A(6)
 *   pair 1: b(1) ↔ B(7)
 *   ...
 *   pair 5: f(5) ↔ F(11)
 *
 * letter_to_pair(letter_id) → 0-5
 * addr → letter_id via addr % PL_LETTERS
 */

static inline uint8_t pl_letter_to_pair(uint8_t letter_id) {
    return (uint8_t)(letter_id % PL_PAIRS);  /* a,A→0  b,B→1 ... f,F→5 */
}

/*
 * Payload ID = flat index into cells[PL_PAIRS][PL_SLOTS]
 * lane = letter pair (0-5)  ← cylinder spoke analog
 * slot = addr % PL_SLOTS    ← dodeca window position
 */
typedef struct {
    uint8_t  lane;       /* 0-5  (letter pair / cylinder spoke) */
    uint8_t  slot;       /* 0-143 (dodeca window position)      */
    uint16_t flat;       /* lane*PL_SLOTS + slot  (flat index)  */
} PayloadID;

static inline PayloadID pl_id_from_addr(uint64_t addr) {
    uint8_t letter_id = (uint8_t)(addr % PL_LETTERS);
    uint8_t lane      = pl_letter_to_pair(letter_id);
    uint8_t slot      = (uint8_t)(addr % PL_SLOTS);
    return (PayloadID){
        .lane = lane,
        .slot = slot,
        .flat = (uint16_t)(lane * PL_SLOTS + slot),
    };
}

/* ── Letter Cube Rewind ─────────────────────────────────────────────── */
/*
 * pair chain navigation: pair → next pair via +1 mod 6
 * LCM(6,6)=6 → loop closes in 6 hops guaranteed
 *
 * rewind(addr, n_hops): walk back n hops in pair chain
 * used for: version history, content-addressable lookup
 */
static inline uint8_t pl_next_pair(uint8_t pair) {
    return (uint8_t)((pair + 1u) % PL_PAIRS);
}

static inline uint8_t pl_prev_pair(uint8_t pair) {
    return (uint8_t)((pair + PL_PAIRS - 1u) % PL_PAIRS);
}

/* rewind n steps from addr → ancestor addr (same slot, n pairs back) */
static inline PayloadID pl_rewind(uint64_t addr, uint8_t n_hops) {
    PayloadID id = pl_id_from_addr(addr);
    uint8_t lane = id.lane;
    for (uint8_t i = 0; i < (n_hops % PL_LCM); i++)
        lane = pl_prev_pair(lane);
    return (PayloadID){
        .lane = lane,
        .slot = id.slot,
        .flat = (uint16_t)(lane * PL_SLOTS + id.slot),
    };
}

/* ── PayloadStore ───────────────────────────────────────────────────── */
/*
 * Flat layout: cells[PL_PAIRS][PL_SLOTS]
 * = cylinder unfolded → 6 lanes × 144 slots
 * = 864 × 8 bytes = 6.75 KB (cache-friendly, zero pointer)
 */
typedef struct {
    uint64_t cells[PL_PAIRS][PL_SLOTS];  /* 6 × 144 = 864 values  */
    uint32_t write_count;                /* total writes           */
    uint32_t hit_count;                  /* reads that found value */
    uint32_t miss_count;                 /* reads on empty cell    */
} PayloadStore;

static inline void pl_init(PayloadStore *ps) {
    for (uint32_t i = 0; i < PL_PAIRS; i++)
        for (uint32_t j = 0; j < PL_SLOTS; j++)
            ps->cells[i][j] = PL_EMPTY;
    ps->write_count = 0;
    ps->hit_count   = 0;
    ps->miss_count  = 0;
}

/* ── Write / Read ───────────────────────────────────────────────────── */

static inline PayloadID pl_write(PayloadStore *ps,
                                  uint64_t addr, uint64_t value) {
    PayloadID id = pl_id_from_addr(addr);
    ps->cells[id.lane][id.slot] = value;
    ps->write_count++;
    return id;
}

typedef struct {
    uint64_t  value;
    uint8_t   found;    /* 1 = hit, 0 = miss (empty cell) */
    PayloadID id;
} PayloadResult;

static inline PayloadResult pl_read(PayloadStore *ps, uint64_t addr) {
    PayloadID id  = pl_id_from_addr(addr);
    uint64_t  val = ps->cells[id.lane][id.slot];
    uint8_t   hit = (val != PL_EMPTY) ? 1u : 0u;
    if (hit) ps->hit_count++; else ps->miss_count++;
    return (PayloadResult){ .value = val, .found = hit, .id = id };
}

/* rewind read: walk back n_hops in pair chain, same slot */
static inline PayloadResult pl_read_rewind(PayloadStore *ps,
                                            uint64_t addr, uint8_t n_hops) {
    PayloadID id  = pl_rewind(addr, n_hops);
    uint64_t  val = ps->cells[id.lane][id.slot];
    uint8_t   hit = (val != PL_EMPTY) ? 1u : 0u;
    if (hit) ps->hit_count++; else ps->miss_count++;
    return (PayloadResult){ .value = val, .found = hit, .id = id };
}

/* ── Stats ──────────────────────────────────────────────────────────── */

static inline void pl_print_stats(const PayloadStore *ps) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < PL_PAIRS; i++)
        for (uint32_t j = 0; j < PL_SLOTS; j++)
            if (ps->cells[i][j] != PL_EMPTY) used++;
    printf("PayloadStore: %u/%u cells used  writes=%u  hits=%u  miss=%u\n",
           used, PL_CELLS, ps->write_count, ps->hit_count, ps->miss_count);
    printf("  lanes: ");
    for (uint32_t i = 0; i < PL_PAIRS; i++) {
        uint32_t cnt = 0;
        for (uint32_t j = 0; j < PL_SLOTS; j++)
            if (ps->cells[i][j] != PL_EMPTY) cnt++;
        printf("[%u]=%u ", i, cnt);
    }
    printf("\n");
}

/* ── Compile-time verify ────────────────────────────────────────────── */
#if (PL_PAIRS != GEO_SPOKES)
#  error "PL_PAIRS must equal GEO_SPOKES (6)"
#endif
#if (PL_CELLS != 864u)
#  error "PL_CELLS must be 864 (6×144)"
#endif

#endif /* GEO_PAYLOAD_STORE_H */
