/*
 * pogls_38_split_world.h — POGLS38 Phase D: Split World
 * ══════════════════════════════════════════════════════════════════════
 *
 * Split World = แยก 1 write stream เป็น 2 heads ที่ทำงานพร้อมกัน
 * แล้ว rejoin กลับเป็น stream เดียวที่ gate_17 boundary
 *
 * เปรียบเทียบ:
 *   ก่อน split = แม่น้ำสายเดียว
 *   หลัง split = แยกเป็น 2 ลำ (A=binary CPU, B=ternary GPU)
 *   rejoin     = ไหลกลับมารวมกัน ณ จุด gate_17
 *   34n+1 mod 17 = 1 เสมอ → สองลำไม่ตัดกัน (no aliasing)
 *
 * Sacred Math (FROZEN):
 *   34  = 2×17 = Fib(9)        ← stride ระหว่าง HEAD_A และ HEAD_B
 *   17  = prime bridge          ← gate period (no aliasing)
 *   34n+1 mod 17 = 1 (always)  ← isolation guarantee
 *
 * Architecture:
 *
 *   L38EngineBridge (Phase C)
 *          │
 *    l38_split_write()
 *          │
 *     ┌────┴────┐
 *     │         │
 *   HEAD_A    HEAD_B          ← 2 heads max (FROZEN)
 *  (World A) (World B)
 *  binary    ternary
 *  CPU       CPU/GPU
 *     │         │
 *     └────┬────┘
 *     gate_17 sync
 *          │
 *    l38_rejoin()
 *          │
 *   single stream again
 *
 * D1: DetachFrame → spawn ≤2 heads
 * D2: World A (CPU binary) ↔ World B (CPU/GPU ternary) run concurrently
 * D3: gate_17 = sync point every 17 writes (34n+1 mod 17 = 1)
 * D4: rejoin = merge HEAD_A + HEAD_B results back to base
 *
 * Rules (FROZEN):
 *   - MAX 2 heads (HEAD_A + HEAD_B) — no more
 *   - HEAD_A addr space: 34n+1          (World A lane, mod17=1)
 *   - HEAD_B addr space: 34n+18 = 34n+1+17  (World B lane, mod17=1)
 *   - gate_17 fires every 17 writes per head
 *   - rejoin requires BOTH heads to reach gate_17 boundary
 *   - Cannot split again before rejoin (no nested splits)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_SPLIT_WORLD_H
#define POGLS_38_SPLIT_WORLD_H

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include "pogls_38_engine_bridge.h"   /* L38EngineBridge, L38BridgeResult */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — CONSTANTS (sacred — FROZEN)
 * ══════════════════════════════════════════════════════════════════════ */

#define SPLIT_GATE       17u    /* gate_17: sync every 17 writes            */
#define SPLIT_STRIDE     34u    /* 2×17 = HEAD_A to HEAD_B addr offset      */
#define SPLIT_PHASE       1u    /* 34n+1: phase offset (mod17 = 1)          */
#define SPLIT_MAX_HEADS   2u    /* FROZEN: max 2 heads per split            */
#define SPLIT_MAGIC  0x53504C54u   /* "SPLT"                                */

/* isolation proof: 34n+1 mod 17 = 1 for all n ≥ 1 */
typedef char _split_isolation[(((34u * 1u + 1u) % 17u) == 1u) ? 1 : -1];
typedef char _split_stride   [(SPLIT_STRIDE == 2u * SPLIT_GATE) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — HEAD STATE
 *
 * Each head is a mini-context: which world it runs, write counter,
 * gate_17 status, and accumulated bridge results since last gate.
 *
 * 8 bytes hot fields first (cache line friendly)
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    HEAD_IDLE    = 0,   /* not spawned                                     */
    HEAD_RUNNING = 1,   /* active, writing                                 */
    HEAD_AT_GATE = 2,   /* reached gate_17 boundary, waiting for rejoin   */
    HEAD_DONE    = 3,   /* rejoin complete                                 */
} head_state_t;

typedef struct {
    /* ── hot: 8B ─────────────────────────────────────────────────── */
    uint32_t  write_count;    /* writes since last gate (0..17)             */
    uint8_t   world;          /* 0=A (binary/CPU), 1=B (ternary/CPU-GPU)   */
    uint8_t   head_id;        /* 0=HEAD_A, 1=HEAD_B                        */
    uint8_t   state;          /* head_state_t                              */
    uint8_t   gate_count;     /* how many gate_17 boundaries crossed       */

    /* ── accumulated stats since spawn ──────────────────────────── */
    uint64_t  total_writes;   /* total writes since spawn                   */
    uint64_t  gate_events;    /* gate_17 boundaries crossed                 */
    uint64_t  addr_base;      /* base angular address for this head         */

    /* ── last gate result snapshot ──────────────────────────────── */
    uint32_t  last_gate_sig;  /* XOR of dna_sig at gate boundary           */
    uint32_t  _pad;
} SplitHead;                  /* 40B */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — SPLIT RESULT
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    L38BridgeResult  head_a;        /* result from HEAD_A write            */
    L38BridgeResult  head_b;        /* result from HEAD_B write (may skip) */
    uint8_t          gate_sync;     /* 1 = both heads at gate_17, synced   */
    uint8_t          rejoin_ready;  /* 1 = can rejoin now                  */
    uint8_t          active_heads;  /* how many heads wrote (1 or 2)       */
    uint8_t          _pad;
    uint32_t         gate_sig_xor;  /* HEAD_A.sig XOR HEAD_B.sig (sync token) */
} SplitWriteResult;

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — REJOIN RESULT
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t  writes_a;        /* total writes from HEAD_A                 */
    uint64_t  writes_b;        /* total writes from HEAD_B                 */
    uint32_t  gate_sig_final;  /* final XOR gate token (integrity check)   */
    uint8_t   balanced;        /* 1 = |writes_a - writes_b| <= SPLIT_GATE */
    uint8_t   _pad[3];
} RejoinResult;

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — SPLIT WORLD CONTEXT
 *
 * Wraps an L38EngineBridge with split-world capability.
 * bridge is a pointer — caller owns and inits it (Phase C).
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t          magic;

    /* ── split state ─────────────────────────────────────────────── */
    uint8_t           is_split;        /* 1 = currently split             */
    uint8_t           _pad[3];
    SplitHead         heads[SPLIT_MAX_HEADS];  /* HEAD_A=[0], HEAD_B=[1]  */

    /* ── base bridge (Phase C — owned by caller) ─────────────────── */
    L38EngineBridge  *bridge;

    /* ── gate_17 sync accumulator ─────────────────────────────────  */
    uint32_t          gate_sig_accum;  /* XOR of all head gate sigs       */
    uint32_t          gates_synced;    /* times both heads synced at gate  */

    /* ── stats ───────────────────────────────────────────────────── */
    uint64_t          total_splits;    /* times l38_split() was called     */
    uint64_t          total_rejoins;   /* times l38_rejoin() was called    */
    uint64_t          total_writes;    /* writes through split path        */
} SplitWorldCtx;

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 6 — INIT
 * ══════════════════════════════════════════════════════════════════════ */

static inline int split_world_init(SplitWorldCtx   *sw,
                                    L38EngineBridge *bridge)
{
    if (!sw || !bridge) return -1;
    memset(sw, 0, sizeof(*sw));
    sw->magic  = SPLIT_MAGIC;
    sw->bridge = bridge;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 7 — SPLIT  (D1: spawn ≤2 heads from base addr)
 *
 * Splits the write stream into HEAD_A (World A) and HEAD_B (World B).
 * HEAD_A operates at:  base_addr + 34n+1         (mod17 = 1)
 * HEAD_B operates at:  base_addr + 34n+1 + 17    (mod17 = 1, different zone)
 *
 * Cannot split if already split (no nested splits).
 * Returns: 0 = success, -1 = already split or invalid
 * ══════════════════════════════════════════════════════════════════════ */

static inline int l38_split(SplitWorldCtx *sw, uint64_t base_addr)
{
    if (!sw || sw->magic != SPLIT_MAGIC) return -1;
    if (sw->is_split) return -1;  /* no nested splits */

    /* HEAD_A: World A, base_addr + SPLIT_PHASE (= 34×1 + 1 for n=1) */
    SplitHead *a = &sw->heads[0];
    memset(a, 0, sizeof(*a));
    a->world       = 0;   /* World A = binary / CPU */
    a->head_id     = 0;
    a->state       = HEAD_RUNNING;
    a->addr_base   = base_addr + SPLIT_PHASE;  /* 34n+1 for n=0 start */

    /* HEAD_B: World B, base_addr + SPLIT_PHASE + SPLIT_GATE */
    SplitHead *b = &sw->heads[1];
    memset(b, 0, sizeof(*b));
    b->world       = 1;   /* World B = ternary / CPU-GPU */
    b->head_id     = 1;
    b->state       = HEAD_RUNNING;
    b->addr_base   = base_addr + SPLIT_PHASE + SPLIT_GATE;  /* +17 offset */

    sw->is_split       = 1;
    sw->gate_sig_accum = 0;
    sw->total_splits++;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 8 — ADDR GENERATOR  (34n+1 pattern)
 *
 * For head h after n writes:
 *   addr = head.addr_base + 34×n
 *
 * This ensures:
 *   (addr) mod 17 = (base + PHASE + 34×n) mod 17
 *                 = (base + 1) mod 17     [since 34n mod 17 = 0]
 *
 * HEAD_A and HEAD_B are in the same residue class mod 17
 * but at different absolute addresses (offset by SPLIT_GATE=17)
 * → no address aliasing between heads
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint64_t split_head_addr(const SplitHead *h)
{
    return h->addr_base + (uint64_t)SPLIT_STRIDE * h->total_writes;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 9 — SPLIT WRITE  (D2: write through both heads)
 *
 * Routes value to HEAD_A and HEAD_B using 34n+1 addressing.
 * After every SPLIT_GATE (17) writes per head → gate_17 fires.
 * When both heads reach gate_17 → gate_sync = 1.
 *
 * D3: gate_17 sync = both heads accumulate gate_sig_xor as integrity token
 * ══════════════════════════════════════════════════════════════════════ */

static inline SplitWriteResult l38_split_write(SplitWorldCtx  *sw,
                                                uint64_t        value,
                                                const uint8_t   rubik_face[6])
{
    SplitWriteResult res = {0};
    if (!sw || sw->magic != SPLIT_MAGIC || !sw->is_split) return res;

    SplitHead *a = &sw->heads[0];
    SplitHead *b = &sw->heads[1];

    /* ── HEAD_A write (World A — binary) ─────────────────────────── */
    if (a->state == HEAD_RUNNING) {
        uint64_t addr_a = split_head_addr(a);
        res.head_a = l38_bridge_write(sw->bridge, addr_a, value,
                                       rubik_face, (uint8_t)(addr_a % 54u));
        a->write_count++;
        a->total_writes++;
        res.active_heads++;

        /* gate_17: every 17 writes → boundary event */
        if (a->write_count >= SPLIT_GATE) {
            a->write_count = 0;
            a->gate_count++;
            a->gate_events++;
            a->last_gate_sig = res.head_a.dna_sig;
            a->state = HEAD_AT_GATE;
        }
    }

    /* ── HEAD_B write (World B — ternary) ────────────────────────── */
    if (b->state == HEAD_RUNNING) {
        uint64_t addr_b = split_head_addr(b);
        res.head_b = l38_bridge_write(sw->bridge, addr_b, value ^ 0xFFFFFFFFu,
                                       rubik_face, (uint8_t)(addr_b % 54u));
        b->write_count++;
        b->total_writes++;
        res.active_heads++;

        if (b->write_count >= SPLIT_GATE) {
            b->write_count = 0;
            b->gate_count++;
            b->gate_events++;
            b->last_gate_sig = res.head_b.dna_sig;
            b->state = HEAD_AT_GATE;
        }
    }

    /* ── D3: gate_17 sync check ───────────────────────────────────── */
    if (a->state == HEAD_AT_GATE && b->state == HEAD_AT_GATE) {
        res.gate_sync    = 1;
        res.rejoin_ready = 1;
        res.gate_sig_xor = a->last_gate_sig ^ b->last_gate_sig;
        sw->gate_sig_accum ^= res.gate_sig_xor;
        sw->gates_synced++;

        /* reset both heads to RUNNING for next gate window */
        a->state = HEAD_RUNNING;
        b->state = HEAD_RUNNING;
    }

    sw->total_writes++;
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 10 — FORCE GATE  (manual gate_17 trigger)
 *
 * Force a gate sync even if write_count < SPLIT_GATE.
 * Used at end of a computation phase to sync before rejoin.
 * ══════════════════════════════════════════════════════════════════════ */

static inline void l38_split_force_gate(SplitWorldCtx *sw)
{
    if (!sw || !sw->is_split) return;
    for (uint8_t i = 0; i < SPLIT_MAX_HEADS; i++) {
        SplitHead *h = &sw->heads[i];
        if (h->state == HEAD_RUNNING && h->write_count > 0) {
            h->gate_count++;
            h->gate_events++;
            h->state = HEAD_AT_GATE;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 11 — REJOIN  (D4: merge split heads back to base)
 *
 * Merges HEAD_A + HEAD_B results back to single stream.
 * Requires both heads to be AT_GATE or RUNNING (forced merge).
 *
 * Balance check: |total_writes_A - total_writes_B| <= SPLIT_GATE
 *   If balanced → orderly rejoin
 *   If unbalanced → skew warning (logged in result)
 *
 * Returns RejoinResult with final integrity token.
 * ══════════════════════════════════════════════════════════════════════ */

static inline RejoinResult l38_rejoin(SplitWorldCtx *sw)
{
    RejoinResult r = {0};
    if (!sw || sw->magic != SPLIT_MAGIC || !sw->is_split) return r;

    SplitHead *a = &sw->heads[0];
    SplitHead *b = &sw->heads[1];

    /* force gate if not already there */
    l38_split_force_gate(sw);

    r.writes_a       = a->total_writes;
    r.writes_b       = b->total_writes;
    r.gate_sig_final = sw->gate_sig_accum ^ a->last_gate_sig ^ b->last_gate_sig;

    /* balance check: skew <= 1 gate window (17 writes) */
    uint64_t diff = (r.writes_a > r.writes_b)
                  ? r.writes_a - r.writes_b
                  : r.writes_b - r.writes_a;
    r.balanced = (diff <= SPLIT_GATE) ? 1u : 0u;

    /* mark both heads DONE */
    a->state = HEAD_DONE;
    b->state = HEAD_DONE;

    /* clear split flag */
    sw->is_split = 0;
    sw->total_rejoins++;

    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 12 — QUERY HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

/* is_split: currently split? */
static inline int split_is_active(const SplitWorldCtx *sw)
{ return sw && sw->is_split; }

/* gate status per head */
static inline int split_head_at_gate(const SplitWorldCtx *sw, uint8_t head_id)
{
    if (!sw || head_id >= SPLIT_MAX_HEADS) return 0;
    return sw->heads[head_id].state == HEAD_AT_GATE;
}

/* isolation check: addr_a and addr_b must not alias */
static inline int split_addr_isolated(uint64_t addr_a, uint64_t addr_b)
{
    /* same residue mod 17, different absolute → no overlap */
    uint32_t r_a = (uint32_t)(addr_a % SPLIT_GATE);
    uint32_t r_b = (uint32_t)(addr_b % SPLIT_GATE);
    return (r_a == r_b) && (addr_a != addr_b);
}

/* validate: 34n+1 mod 17 = 1 for given n */
static inline int split_validate_addr(uint64_t n)
{
    uint64_t addr = 34u * n + SPLIT_PHASE;
    return (addr % SPLIT_GATE) == SPLIT_PHASE;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 13 — VALIDATE
 * ══════════════════════════════════════════════════════════════════════ */

#define SPLIT_OK            0
#define SPLIT_ERR_NULL     -1
#define SPLIT_ERR_MAGIC    -2
#define SPLIT_ERR_BRIDGE   -3
#define SPLIT_ERR_NESTED   -4

static inline int split_world_validate(const SplitWorldCtx *sw)
{
    if (!sw)                        return SPLIT_ERR_NULL;
    if (sw->magic != SPLIT_MAGIC)   return SPLIT_ERR_MAGIC;
    if (!sw->bridge)                return SPLIT_ERR_BRIDGE;
    return SPLIT_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 14 — STATS PRINT
 * ══════════════════════════════════════════════════════════════════════ */

#include <stdio.h>

static inline void split_world_stats_print(const SplitWorldCtx *sw)
{
    if (!sw) return;
    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  Split World (Phase D) Stats             ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║ Total splits:    %10llu               ║\n",
           (unsigned long long)sw->total_splits);
    printf("║ Total rejoins:   %10llu               ║\n",
           (unsigned long long)sw->total_rejoins);
    printf("║ Gates synced:    %10u                ║\n", sw->gates_synced);
    printf("║ Total writes:    %10llu               ║\n",
           (unsigned long long)sw->total_writes);
    printf("║ Currently split: %s                     ║\n",
           sw->is_split ? "YES" : "NO ");
    if (sw->is_split) {
        printf("║ HEAD_A writes:   %10llu               ║\n",
               (unsigned long long)sw->heads[0].total_writes);
        printf("║ HEAD_B writes:   %10llu               ║\n",
               (unsigned long long)sw->heads[1].total_writes);
    }
    printf("╚══════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_38_SPLIT_WORLD_H */
