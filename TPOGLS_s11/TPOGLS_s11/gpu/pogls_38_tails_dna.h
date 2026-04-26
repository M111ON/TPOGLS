/*
 * pogls_38_tails_dna.h — POGLS38 Phase B1: Tails DNA
 * ══════════════════════════════════════════════════════════════════════
 *
 * Tails DNA = lineage tracking สำหรับ World 3 Ring (W3R)
 *
 * ปัญหาที่แก้:
 *   W3R ปัจจุบันรู้แค่ว่า slot นี้ถูก write กี่ครั้ง (seq) และ hot แค่ไหน
 *   แต่ไม่รู้ว่า "write ครั้งนี้เชื่อมกับ slot ไหนก่อนหน้า"
 *   ทำให้ FiftyFourBridge temporal recall ไม่สามารถ reconstruct chain ได้
 *
 * เปรียบเทียบ:
 *   W3RSlot  = ป้ายชื่อบน slot (บอกว่ามีใครอยู่)
 *   Tails DNA = สมุดบันทึก (บอกว่าคนนี้มาจากไหน ไปไหนต่อ มีลายมือแบบไหน)
 *
 * โครงสร้าง DNA entry (24B per slot):
 *   prev_slot   (1B)  ← slot ก่อนหน้าใน ternary write chain
 *   next_slot   (1B)  ← slot ถัดไปที่ถูก write หลังนี้ (0xFF = unknown)
 *   cycle_id    (2B)  ← ring_complete ครั้งที่เท่าไหร่
 *   sig_fast    (4B)  ← CRC32(last_addr ^ slot ^ cycle_id) = recall_tag
 *   rubik_state (6B)  ← Rubik permutation snapshot (6 face states × 1B)
 *   write_tick  (4B)  ← monotonic tick ที่ write นี้เกิดขึ้น
 *   _pad        (6B)  ← align to 24B
 *
 * Memory: 153 × 24B = 3,672B ≈ 3.6KB (L2 friendly)
 * Total W3RContext + DNA context ≈ 5KB
 *
 * Rules (FROZEN):
 *   - ไม่แตะ W3RSlot, W3RContext, l38_w3r_write()
 *   - DNA เป็น parallel array แยก — additive only
 *   - sig_fast ต้อง deterministic: input เดิม → output เดิมเสมอ
 *   - prev/next chain ใช้ 0xFF = NULL (ยังไม่รู้)
 *
 * Integration (2 บรรทัด):
 *   W3RWriteResult wr = l38_w3r_write(ring, addr);
 *   tails_dna_record(dna, &wr, addr, prev_slot, tick);
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_TAILS_DNA_H
#define POGLS_38_TAILS_DNA_H

#include <stdint.h>
#include <string.h>
#include "pogls_38_world3ring.h"

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1 — CONSTANTS
 * ══════════════════════════════════════════════════════════════════════ */

#define DNA_MAGIC       0x444E4100u   /* "DNA\0" */
#define DNA_NULL_SLOT   0xFFu         /* prev/next unknown = NULL */
#define DNA_CHAIN_DEPTH 9u            /* max chain depth = W3R_GROUPS (sacred) */

/* sig_fast seed — XOR with PHI_DOWN for ternary domain */
#define DNA_SIG_SEED    (PHI_DOWN & 0xFFFFFFFFu)

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2 — DNA ENTRY (24B — 2.67 per cache line = ~3 per 64B)
 *
 * เปรียบเทียบ:
 *   slot  = บ้านเลขที่
 *   entry = สมุดทะเบียนบ้าน ว่าคนนี้มาจากบ้านไหน ย้ายไปไหน ลายนิ้วมือคืออะไร
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    /* lineage chain */
    uint8_t  prev_slot;     /* slot ก่อนหน้าที่ write ก่อนนี้ (DNA_NULL_SLOT = first) */
    uint8_t  next_slot;     /* slot ถัดไปที่ถูก write หลังนี้ (DNA_NULL_SLOT = unknown) */
    uint16_t cycle_id;      /* ring_complete ครั้งที่เท่าไหร่ (wraps at 65535) */

    /* identity */
    uint32_t sig_fast;      /* CRC32(last_addr ^ slot ^ cycle_id) = recall tag */

    /* rubik snapshot — 6 face states ณ เวลา write นี้ */
    uint8_t  rubik_face[6]; /* face 0..5 → permutation index 0..255 */

    /* timing */
    uint32_t write_tick;    /* monotonic tick counter ของ w3r write นี้ */

    /* padding to 24B */
    uint8_t  _pad[6];
} TailsDNAEntry;            /* 1+1+2+4+6+4+6 = 24B */

/* compile-time size check */
typedef char _dna_entry_size_check[(sizeof(TailsDNAEntry) == 24u) ? 1 : -1];

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 3 — DNA CONTEXT
 *
 * parallel array ขนาด 153 ตาม W3R ring[153]
 * total = 24B × 153 + header ≈ 3.7KB
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t      magic;
    uint32_t      total_records;    /* จำนวน write ที่ record ไปแล้ว */
    uint16_t      current_cycle;    /* cycle_id ปัจจุบัน (sync กับ ring_completes) */
    uint8_t       last_slot;        /* slot ที่ถูก write ล่าสุด (prev สำหรับ write ต่อไป) */
    uint8_t       _pad;

    TailsDNAEntry entries[W3R_LANES];  /* 153 × 24B = 3,672B */
} TailsDNAContext;                     /* ~3,684B ≈ 3.6KB */

static inline void tails_dna_init(TailsDNAContext *d)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->magic      = DNA_MAGIC;
    d->last_slot  = DNA_NULL_SLOT;
    d->current_cycle = 0;
    /* mark all prev/next as unknown */
    for (uint8_t i = 0; i < W3R_LANES; i++) {
        d->entries[i].prev_slot = DNA_NULL_SLOT;
        d->entries[i].next_slot = DNA_NULL_SLOT;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 4 — CRC32 (self-contained, no dependency on delta.c)
 *
 * Deterministic — same input → same sig_fast always
 * Polynomial: 0xEDB88320 (IEEE 802.3, standard)
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t _dna_crc32(uint32_t crc, uint32_t val)
{
    /* สูตรตรง: CRC32 ของ 4 bytes ด้วย polynomial IEEE */
    static const uint32_t _t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    crc = ~crc;
    for (int b = 0; b < 4; b++) {
        uint8_t byte = (uint8_t)(val >> (b * 8));
        crc = (crc >> 4) ^ _t[(crc ^ byte)       & 0xFu];
        crc = (crc >> 4) ^ _t[(crc ^ (byte >> 4)) & 0xFu];
    }
    return ~crc;
}

/* ── sig_fast: deterministic recall tag ─────────────────────────────
 * input:  last_addr (32-bit), slot (8-bit), cycle_id (16-bit)
 * output: uint32_t CRC32 — same input → same output, always
 */
static inline uint32_t dna_sig_fast(uint32_t last_addr,
                                     uint8_t  slot,
                                     uint16_t cycle_id)
{
    uint32_t seed = DNA_SIG_SEED ^ (uint32_t)slot ^ ((uint32_t)cycle_id << 8);
    return _dna_crc32(seed, last_addr);
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 5 — RECORD FUNCTION (hot path)
 *
 * เรียกทันทีหลัง l38_w3r_write()
 * อัพเดท DNA entry ของ slot นี้ และ patch next_slot ของ prev entry
 *
 * ~4ns per call (1 CRC32 + 2 struct writes + 1 patch)
 * ══════════════════════════════════════════════════════════════════════ */

static inline void tails_dna_record(TailsDNAContext     *d,
                                     const W3RWriteResult *wr,
                                     uint64_t             angular_addr,
                                     const uint8_t        rubik_face[6],
                                     uint32_t             tick)
{
    if (!d || !wr) return;

    uint8_t  slot     = wr->cell.slot;
    uint16_t cycle    = d->current_cycle;
    uint32_t last_a32 = (uint32_t)(angular_addr & 0xFFFFFFFFu);

    /* ── update cycle if ring completed ─────────────────────────── */
    if (wr->ring_complete) {
        d->current_cycle++;            /* wraps at 65535 — intended */
        cycle = d->current_cycle;
    }

    /* ── patch prev entry's next_slot ───────────────────────────── */
    if (d->last_slot != DNA_NULL_SLOT && d->last_slot < W3R_LANES) {
        d->entries[d->last_slot].next_slot = slot;
    }

    /* ── write DNA entry for this slot ──────────────────────────── */
    TailsDNAEntry *e = &d->entries[slot];
    e->prev_slot  = d->last_slot;
    e->next_slot  = DNA_NULL_SLOT;      /* unknown until next write  */
    e->cycle_id   = cycle;
    e->sig_fast   = dna_sig_fast(last_a32, slot, cycle);
    e->write_tick = tick;

    /* rubik face snapshot — copy 6 bytes if provided */
    if (rubik_face)
        memcpy(e->rubik_face, rubik_face, 6);
    else
        memset(e->rubik_face, 0, 6);

    /* ── advance last_slot ───────────────────────────────────────── */
    d->last_slot = slot;
    d->total_records++;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 6 — CHAIN WALK (read-only, O(depth))
 *
 * เดิน lineage chain ย้อนหลัง: slot → prev → prev → ...
 * เหมือน "ตามรอยเท้าย้อนกลับ" ตาม DNA chain
 *
 * max_depth = DNA_CHAIN_DEPTH (9) = W3R_GROUPS = sacred number
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  chain[DNA_CHAIN_DEPTH];  /* slot ids, [0] = start, [n] = oldest */
    uint8_t  depth;                   /* actual chain length found            */
    uint32_t sig_xor;                 /* XOR of all sig_fast in chain         */
} TailsDNAChain;

static inline TailsDNAChain tails_dna_walk_back(const TailsDNAContext *d,
                                                  uint8_t start_slot)
{
    TailsDNAChain chain = {{0}, 0, 0};
    if (!d || start_slot >= W3R_LANES) return chain;

    uint8_t cur = start_slot;
    for (uint8_t i = 0; i < DNA_CHAIN_DEPTH; i++) {
        if (cur == DNA_NULL_SLOT || cur >= W3R_LANES) break;
        chain.chain[i]  = cur;
        chain.sig_xor  ^= d->entries[cur].sig_fast;
        chain.depth++;
        cur = d->entries[cur].prev_slot;
        if (cur == start_slot) break;  /* loop guard */
    }
    return chain;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 7 — RECALL LOOKUP (O(153) linear scan on sig_fast)
 *
 * ค้นหา slot ที่มี sig_fast ตรงกับ target
 * ใช้สำหรับ FiftyFourBridge recall: "หา slot ที่มี pattern นี้"
 *
 * ถ้า want O(1): เพิ่ม hash table ใน Phase B2 (ไม่ทำตอนนี้)
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint8_t tails_dna_recall(const TailsDNAContext *d,
                                        uint32_t target_sig)
{
    if (!d || target_sig == 0) return DNA_NULL_SLOT;
    for (uint8_t i = 0; i < W3R_LANES; i++) {
        /* sig_fast == 0 means never written (init state) */
        if (d->entries[i].sig_fast == target_sig)
            return i;
    }
    return DNA_NULL_SLOT;  /* not found */
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 8 — HOOK (drop-in integration with W3RHook)
 *
 * TailsDNAHook ห่อ W3RHook + TailsDNAContext ไว้ด้วยกัน
 * ใช้แค่ l38_tails_hook() แทน l38_w3r_hook() เพื่อได้ทั้ง write + DNA
 *
 * Usage:
 *   TailsDNAHook th;
 *   tails_hook_init(&th, &ring, on_complete_fn, ud);
 *   W3RWriteResult wr = l38_tails_hook(&th, addr, rubik_face, tick);
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    W3RHook         w3r;    /* must be first — cast-compatible */
    TailsDNAContext dna;
    uint32_t        tick;   /* auto-increment per call */
} TailsDNAHook;

static inline void tails_hook_init(TailsDNAHook    *th,
                                    W3RContext      *ring,
                                    W3RCheckpointFn  on_complete,
                                    void            *ud)
{
    if (!th) return;
    w3r_hook_init(&th->w3r, ring, on_complete, ud);
    tails_dna_init(&th->dna);
    th->tick = 0;
}

static inline W3RWriteResult l38_tails_hook(TailsDNAHook    *th,
                                              uint64_t         angular_addr,
                                              const uint8_t    rubik_face[6])
{
    W3RWriteResult res = {0};
    if (!th) return res;

    res = l38_w3r_hook(&th->w3r, angular_addr);
    tails_dna_record(&th->dna, &res, angular_addr, rubik_face, th->tick++);

    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 9 — STATS PRINT
 * ══════════════════════════════════════════════════════════════════════ */

#include <stdio.h>

static inline void tails_dna_stats_print(const TailsDNAContext *d)
{
    if (!d) return;
    uint32_t written = 0;
    for (uint8_t i = 0; i < W3R_LANES; i++)
        if (d->entries[i].prev_slot != DNA_NULL_SLOT ||
            d->entries[i].cycle_id  != 0)
            written++;

    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  Tails DNA (Phase B1) Stats              ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║ Total records:   %10u               ║\n", d->total_records);
    printf("║ Slots with DNA:  %10u / 153         ║\n", written);
    printf("║ Current cycle:   %10u               ║\n", d->current_cycle);
    printf("║ Last slot:        %3u (0xFF=none)        ║\n", d->last_slot);
    printf("╚══════════════════════════════════════════╝\n\n");
}

#endif /* POGLS_38_TAILS_DNA_H */
