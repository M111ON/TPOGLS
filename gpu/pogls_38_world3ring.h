/*
 * pogls_38_world3ring.h — POGLS38 Phase A: World 3 Ring
 * ══════════════════════════════════════════════════════════════════════
 *
 * World B (3ⁿ, 153 lanes) ขยายเป็น circular ternary ring
 *
 * โครงสร้าง:
 *   153 = 9 groups × 17 lanes
 *   9 groups = 3² (ternary base)
 *   17 lanes per group = prime bridge (no aliasing)
 *
 * เปรียบเทียบ:
 *   World A = ไม้บรรทัด   (linear, 2ⁿ, start→end)
 *   World B เดิม = ไม้บรรทัดอีกอัน (ยาวกว่า แต่ยังเป็น linear)
 *   World 3 Ring = กำไล  (circular, 9 ปล้อง × 17 ลูก)
 *
 *   เดินไป 17 ลูก = ข้ามปล้อง (gate event)
 *   เดินครบ 9 ปล้อง = กลับจุดเริ่ม (ring_complete event)
 *
 * Addressing:
 *   slot = (n × PHI_DOWN) % 153      ← ternary PHI scatter
 *   group = slot / 17                 ← ปล้อง (0..8)
 *   lane  = slot % 17                 ← ตำแหน่งในปล้อง (0..16)
 *   next  = (slot + 17) % 153         ← ternary step (+1 group)
 *   wrap  = slot % 9 == 0             ← ring_complete check
 *
 * Events:
 *   gate_event     = เดิน 17 ลูก → ข้ามปล้อง (เหมือน gate_18)
 *   ring_complete  = ครบ 9 ปล้อง → กลับจุดเริ่ม (ใหม่)
 *
 * Integration:
 *   l17_write() → ถ้า world == WORLD_B → l38_w3r_write()
 *   ring_complete event → FiftyFourBridge notify (temporal checkpoint)
 *
 * ══════════════════════════════════════════════════════════════════════
 * Rule: ไม่แตะ World A, ไม่แตะ gate_18 global clock
 *       ring_complete เป็น event ใหม่ เพิ่มเติม ไม่แทน
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_WORLD3RING_H
#define POGLS_38_WORLD3RING_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "pogls_17n_lattice.h"

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — CONSTANTS (derived from 17n lattice, FROZEN)
 * ══════════════════════════════════════════════════════════════════════ */

#define W3R_LANES      153u   /* L17_LANES_B = 9 × 17                   */
#define W3R_GROUPS     9u     /* 3² ternary groups                       */
#define W3R_BRIDGE     17u    /* L17_BRIDGE = prime, no aliasing         */
#define W3R_STEP       17u    /* ternary step = +1 group = +17 slots     */
#define W3R_MAGIC      0x57334752u  /* "W3GR"                            */

/* compile-time checks */
typedef char _w3r_lanes [(W3R_GROUPS * W3R_BRIDGE == W3R_LANES)  ? 1 : -1];
typedef char _w3r_groups[(W3R_GROUPS * W3R_GROUPS == 81u)        ? 1 : -1]; /* 3⁴ */
typedef char _w3r_bridge[(W3R_BRIDGE == L17_BRIDGE)              ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — SLOT STRUCT (8B, fits in cache with 2 per line)
 *
 * สถานะของแต่ละ slot ใน ring[153]
 * ══════════════════════════════════════════════════════════════════════ */

#define W3R_FLAG_ACTIVE   0x01u  /* slot has been written                */
#define W3R_FLAG_HOT      0x02u  /* heat >= 6 (ternary threshold: 9×2/3) */
#define W3R_FLAG_WRAP     0x04u  /* this write triggered ring_complete   */
#define W3R_FLAG_GATE     0x08u  /* this write crossed group boundary    */

typedef struct {
    uint32_t last_addr;   /* last angular_addr % PHI_SCALE written here  */
    uint16_t seq;         /* write count for this slot (wraps)           */
    uint8_t  heat;        /* 0..9 (ternary heat: max = W3R_GROUPS)       */
    uint8_t  flags;       /* W3R_FLAG_*                                  */
} W3RSlot;               /* 8B */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — RING CELL (resolved address)
 *
 * เหมือน L17Cell แต่สำหรับ World 3 Ring
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  slot;        /* 0..152 — absolute slot in ring[153]         */
    uint8_t  group;       /* 0..8   — ปล้อง (slot / 17)                  */
    uint8_t  lane;        /* 0..16  — ตำแหน่งในปล้อง (slot % 17)         */
    uint8_t  next_slot;   /* (slot + 17) % 153 — ternary next            */
    uint8_t  prev_slot;   /* (slot + 153 - 17) % 153 — ternary prev      */
    uint8_t  group_pos;   /* group % 3 — ternary digit (0/1/2)           */
    uint8_t  gate_event;  /* 1 = crossed group boundary                  */
    uint8_t  ring_complete; /* 1 = completed full 9-group cycle          */
} W3RCell;               /* 8B */

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — ADDRESSING FUNCTIONS
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * w3r_slot_of — addr → slot (ternary PHI scatter)
 *
 * ใช้ PHI_DOWN เพราะ World B = PHI_DOWN domain
 * scatter → mod 153 (ternary space) — ไม่ใช่ mod 289
 */
static inline uint8_t w3r_slot_of(uint64_t angular_addr)
{
    uint32_t scattered = (uint32_t)(
        ((angular_addr & (PHI_SCALE - 1u)) * (uint64_t)PHI_DOWN)
        % W3R_LANES
    );
    return (uint8_t)scattered;
}

/*
 * w3r_cell_of — slot → W3RCell (full resolution)
 */
static inline W3RCell w3r_cell_of(uint8_t slot, uint8_t group_counter)
{
    W3RCell c;
    c.slot        = slot % W3R_LANES;
    c.group       = c.slot / W3R_BRIDGE;          /* 0..8  */
    c.lane        = c.slot % W3R_BRIDGE;           /* 0..16 */
    c.next_slot   = (uint8_t)((c.slot + W3R_STEP)  % W3R_LANES);
    c.prev_slot   = (uint8_t)((c.slot + W3R_LANES - W3R_STEP) % W3R_LANES);
    c.group_pos   = c.group % 3u;                  /* ternary digit      */
    c.gate_event  = (c.lane == 0u) ? 1u : 0u;     /* entered new group? */

    /* ring_complete: ternary wrap = group_counter reached W3R_GROUPS    */
    c.ring_complete = (group_counter >= W3R_GROUPS) ? 1u : 0u;
    return c;
}

/*
 * w3r_from_addr — angular_addr → W3RCell (combined)
 */
static inline W3RCell w3r_from_addr(uint64_t angular_addr, uint8_t group_ctr)
{
    return w3r_cell_of(w3r_slot_of(angular_addr), group_ctr);
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — RING CONTEXT
 *
 * ring[153] slots + group counter + stats
 * Size: 153×8B slots + metadata ≈ 1.3KB (L1 friendly)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  magic;
    uint8_t   group_counter;   /* 0..8, resets at ring_complete          */
    uint8_t   _pad[3];

    /* stats */
    uint64_t  total_writes;
    uint64_t  gate_events;     /* group boundary crossings               */
    uint64_t  ring_completes;  /* full 9-group cycles done               */
    uint64_t  hot_slots;       /* slots currently at heat >= 6           */

    /* ring */
    W3RSlot   slots[W3R_LANES];  /* 153 × 8B = 1224B                    */
} W3RContext;                    /* ~1.3KB total                         */

static inline void w3r_init(W3RContext *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->magic = W3R_MAGIC;
    r->group_counter = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 6 — WRITE PATH
 *
 * l38_w3r_write():
 *   1. addr → slot (PHI_DOWN ternary scatter)
 *   2. update slot (heat, seq, flags)
 *   3. gate_event check (lane == 0 = entered new group)
 *   4. group_counter increment → ring_complete check
 *   5. return W3RCell (caller routes to temporal / FiftyFourBridge)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    W3RCell   cell;           /* resolved ring cell                      */
    uint8_t   heat;           /* slot heat after write                   */
    uint8_t   gate_event;     /* 1 = crossed group boundary              */
    uint8_t   ring_complete;  /* 1 = completed full 9-group cycle        */
    uint8_t   _pad;
} W3RWriteResult;

static inline W3RWriteResult l38_w3r_write(W3RContext *r,
                                            uint64_t angular_addr)
{
    W3RWriteResult res = {0};
    if (!r) return res;

    /* ── 1. resolve cell ────────────────────────────────────────── */
    res.cell = w3r_from_addr(angular_addr, r->group_counter);

    /* ── 2. update slot ─────────────────────────────────────────── */
    W3RSlot *s = &r->slots[res.cell.slot];
    s->seq++;
    s->last_addr = (uint32_t)(angular_addr & (PHI_SCALE - 1u));
    s->flags |= W3R_FLAG_ACTIVE;

    /* ternary heat: max = W3R_GROUPS (9), decay every 9 writes     */
    if (s->heat < W3R_GROUPS) s->heat++;
    if ((s->seq % W3R_GROUPS) == 0u)
        s->heat = (uint8_t)(s->heat * 2u / 3u);   /* ×2/3 decay     */
    if (s->heat >= 6u) {
        s->flags |= W3R_FLAG_HOT;
        r->hot_slots++;
    }

    res.heat = s->heat;

    /* ── 3. gate_event (entered new group = lane == 0) ──────────── */
    if (res.cell.gate_event) {
        s->flags |= W3R_FLAG_GATE;
        r->gate_events++;
        r->group_counter++;
        res.gate_event = 1u;
    }

    /* ── 4. ring_complete (group_counter >= 9) ───────────────────── */
    if (r->group_counter >= W3R_GROUPS) {
        r->group_counter = 0;   /* reset → new cycle begins           */
        r->ring_completes++;
        s->flags |= W3R_FLAG_WRAP;
        res.ring_complete = 1u;
        res.cell.ring_complete = 1u;
    }

    /* ── 5. stats ────────────────────────────────────────────────── */
    r->total_writes++;

    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 7 — QUERY HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

/* lookup slot by addr (O(1) via ternary scatter) */
static inline const W3RSlot *w3r_find(const W3RContext *r, uint64_t addr)
{
    if (!r) return NULL;
    uint8_t slot = w3r_slot_of(addr);
    const W3RSlot *s = &r->slots[slot];
    return (s->flags & W3R_FLAG_ACTIVE) ? s : NULL;
}

/* heat of slot */
static inline uint8_t w3r_heat(const W3RContext *r, uint8_t slot)
{
    if (!r || slot >= W3R_LANES) return 0;
    return r->slots[slot].heat;
}

/* ternary next: slot → slot + 17 (mod 153) */
static inline uint8_t w3r_next(uint8_t slot)
{
    return (uint8_t)((slot + W3R_STEP) % W3R_LANES);
}

/* ternary prev: slot → slot - 17 (mod 153) */
static inline uint8_t w3r_prev(uint8_t slot)
{
    return (uint8_t)((slot + W3R_LANES - W3R_STEP) % W3R_LANES);
}

/*
 * w3r_ring_walk — ternary walk: start slot → cb() per step
 *
 * เหมือน "เดินตามกำไล" จากจุดใดก็ได้
 * หยุดเมื่อครบ 9 groups (1 รอบ) หรือ max_steps
 */
static inline uint8_t w3r_ring_walk(
    const W3RContext *r,
    uint8_t start_slot,
    void (*cb)(uint8_t slot, const W3RSlot *s, void *ud),
    void *ud,
    uint8_t max_steps)
{
    if (!r || !cb) return 0;
    uint8_t limit = max_steps ? max_steps : W3R_GROUPS;
    uint8_t cur = start_slot % W3R_LANES;
    for (uint8_t i = 0; i < limit; i++) {
        cb(cur, &r->slots[cur], ud);
        cur = w3r_next(cur);
        if (cur == start_slot) break;  /* full circle */
    }
    return limit;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 8 — L17 INTEGRATION HOOK
 *
 * Drop-in hook สำหรับ l17_write():
 *   ถ้า cell.world == L17_WORLD_B → เรียก l38_w3r_write()
 *   ผล ring_complete → notify FiftyFourBridge (temporal checkpoint)
 *
 * Usage:
 *   L17WriteResult lwr = l17_write(lat, addr, val);
 *   if (lwr.cell.world == L17_WORLD_B)
 *       W3RWriteResult w3r = l38_w3r_hook(&ring, addr, bridge);
 * ══════════════════════════════════════════════════════════════════════ */

/* forward decl — FiftyFourBridge checkpoint (caller provides) */
typedef void (*W3RCheckpointFn)(uint64_t addr, uint64_t ring_seq, void *ud);

typedef struct {
    W3RContext       *ring;
    W3RCheckpointFn   on_ring_complete;  /* NULL = skip                */
    void             *checkpoint_ud;
    uint64_t          ring_seq;          /* monotonic ring_complete seq */
} W3RHook;

static inline void w3r_hook_init(W3RHook *h, W3RContext *ring,
                                  W3RCheckpointFn fn, void *ud)
{
    if (!h) return;
    h->ring             = ring;
    h->on_ring_complete = fn;
    h->checkpoint_ud    = ud;
    h->ring_seq         = 0;
}

static inline W3RWriteResult l38_w3r_hook(W3RHook *h, uint64_t angular_addr)
{
    W3RWriteResult res = {0};
    if (!h || !h->ring) return res;

    res = l38_w3r_write(h->ring, angular_addr);

    if (res.ring_complete && h->on_ring_complete) {
        h->ring_seq++;
        h->on_ring_complete(angular_addr, h->ring_seq, h->checkpoint_ud);
    }

    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 9 — PRINT STATS
 * ══════════════════════════════════════════════════════════════════════ */

static inline void w3r_stats_print(const W3RContext *r)
{
    if (!r) return;
    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  World 3 Ring (Phase A) Stats            ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║ Total writes:    %10llu               ║\n",
           (unsigned long long)r->total_writes);
    printf("║ Gate events:     %10llu               ║\n",
           (unsigned long long)r->gate_events);
    printf("║ Ring completes:  %10llu               ║\n",
           (unsigned long long)r->ring_completes);
    printf("║ Group counter:   %10u                ║\n",
           r->group_counter);
    printf("╚══════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_38_WORLD3RING_H */
