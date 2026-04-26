/*
 * geo_payload_wired.h — PayloadStore + UnionMask wired
 * ═════════════════════════════════════════════════════
 * Session 14
 *
 * Wraps geo_payload_store.h + geo_union_mask.h into one struct.
 * All pl_write / pl_read / qrpn calls auto-update UnionMask counters.
 *
 * Pattern:
 *   PayloadWired = PayloadStore (data) + UnionMask (observer)
 *   เหมือน sensor ติดข้าง pipeline — ไม่ block, แค่ count
 *
 * Window policy:
 *   pw_eval() — call ทุก PW_WINDOW writes (default 144 = PL_SLOTS)
 *   ผล zone เก็บใน mask.zone[lane] ใช้ได้ทันที
 *
 * Wire points:
 *   pw_write()      → pl_write + um_on_write + compress gate
 *   pw_read()       → pl_read  + um_on_read
 *   pw_read_rewind()→ pl_read_rewind + um_on_read
 *   pw_qrpn()       → um_on_qrpn (call after verify)
 *   pw_eval()       → um_eval + um_reset_all (slide window)
 */

#ifndef GEO_PAYLOAD_WIRED_H
#define GEO_PAYLOAD_WIRED_H

#include "geo_payload_store.h"
#include "geo_union_mask.h"

/* ── Config ─────────────────────────────────────────────────────── */

#define PW_WINDOW   PL_SLOTS    /* eval every 144 writes (= 1 cylinder lap) */

/* ── Wired Store ────────────────────────────────────────────────── */

typedef struct {
    PayloadStore store;         /* data layer          */
    UnionMask    mask;          /* observer layer      */
    uint32_t     write_seq;     /* global write seq    */
    uint32_t     window_tick;   /* writes since eval   */
} PayloadWired;

static inline void pw_init(PayloadWired *pw) {
    pl_init(&pw->store);
    um_init(&pw->mask);
    pw->write_seq    = 0;
    pw->window_tick  = 0;
}

/* ── Auto-eval trigger ──────────────────────────────────────────── */

static inline void pw_eval(PayloadWired *pw) {
    um_eval(&pw->mask);
    um_reset_all(&pw->mask);
    pw->window_tick = 0;
}

static inline void pw_tick_check(PayloadWired *pw) {
    if (pw->window_tick >= PW_WINDOW) pw_eval(pw);
}

/* ── Write (with compress gate) ─────────────────────────────────── */
/*
 * SAFE zone: um_should_write decides 1/4 or 3/4 keep
 * NORMAL/DANGER: always write through
 * returns 1 = written, 0 = dropped (compress)
 */
static inline uint8_t pw_write(PayloadWired *pw,
                                uint64_t addr, uint64_t value) {
    PayloadID id   = pl_id_from_addr(addr);
    uint8_t   zone = pw->mask.zone[id.lane];
    uint8_t   fold = pw->mask.fold[id.lane];

    um_on_write(&pw->mask, id.lane);
    pw->window_tick++;
    pw->write_seq++;

    /* bypass compress until first window completes (no field data yet) */
    if (pw->write_seq > PW_WINDOW &&
        !um_should_write(zone, fold, pw->write_seq)) return 0u;  /* drop */

    pl_write(&pw->store, addr, value);
    pw_tick_check(pw);
    return 1u;
}

/* ── Read ───────────────────────────────────────────────────────── */

static inline PayloadResult pw_read(PayloadWired *pw, uint64_t addr) {
    PayloadID id = pl_id_from_addr(addr);
    um_on_read(&pw->mask, id.lane);
    return pl_read(&pw->store, addr);
}

static inline PayloadResult pw_read_rewind(PayloadWired *pw,
                                            uint64_t addr, uint8_t n_hops) {
    PayloadID id = pl_id_from_addr(addr);
    um_on_read(&pw->mask, id.lane);
    return pl_read_rewind(&pw->store, addr, n_hops);
}

/* ── QRPN feedback ──────────────────────────────────────────────── */
/* call after dodeca_verify: failed=1 if verify rejected            */

static inline void pw_qrpn(PayloadWired *pw, uint64_t addr, uint8_t failed) {
    PayloadID id = pl_id_from_addr(addr);
    um_on_qrpn(&pw->mask, id.lane, failed);
}

/* ── Polarity query ─────────────────────────────────────────────── */

static inline int8_t pw_polarity(PayloadWired *pw,
                                  uint64_t addr, uint8_t hit) {
    PayloadID id = pl_id_from_addr(addr);
    return um_polarity(pw->mask.zone[id.lane], hit);
}

/* ── Stats ──────────────────────────────────────────────────────── */

static inline void pw_print_stats(const PayloadWired *pw) {
    pl_print_stats(&pw->store);
    printf("UnionMask zones: ");
    for (uint8_t i = 0; i < PL_PAIRS; i++) {
        const char *z = pw->mask.zone[i] == UM_ZONE_DANGER ? "DNG" :
                        pw->mask.zone[i] == UM_ZONE_NORMAL ? "NRM" : "SAF";
        printf("[%u]%s(f=%u) ", i, z, pw->mask.fold[i]);
    }
    printf("\n  write_seq=%u  window_tick=%u\n",
           pw->write_seq, pw->window_tick);
}

#endif /* GEO_PAYLOAD_WIRED_H */
