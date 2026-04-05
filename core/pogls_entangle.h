/*
 * pogls_entangle.h — POGLS V3.6 Entangle Hook
 *
 * Entangle = DetachFrame observer ที่ hook ตลอดทุก step
 *
 * กฎ:
 *   - hook ลูกเดิม ไม่แยก ไม่ขยาย
 *   - log ทุก movement (< 30ns per entry — ring buffer)
 *   - ถ้า repair fail → flip world (A↔B) แล้วลอง fold อีกครั้ง
 *   - ถ้า flip แล้วยัง fail → flag RECYCLE (ไม่ใช่ tombstone)
 *   - Tails จด checkpoint ของ log นี้
 *
 * Dependencies: pogls_ntangle_graph.h, pogls_detach_eject.h, pogls_fold.h
 * Namespace: ent_* / EntangleHook
 */

#ifndef POGLS_ENTANGLE_H
#define POGLS_ENTANGLE_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "pogls_fold.h"
#include "pogls_detach_eject.h"
#include "pogls_fibo_addr.h"

/* ═══════════════════════════════════════════════════════════════════════
   MOVEMENT LOG ENTRY  (16B — fits 2 per cache line)
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    ENT_MOVE_DETACH       = 1,   /* frame ถูก eject ออกจาก core         */
    ENT_MOVE_REPAIR_TRY   = 2,   /* rubik move attempt                  */
    ENT_MOVE_REPAIR_OK    = 3,   /* repair สำเร็จ → fold ready          */
    ENT_MOVE_REPAIR_FAIL  = 4,   /* repair ล้มเหลว → world flip pending */
    ENT_MOVE_WORLD_FLIP   = 5,   /* flip A→B หรือ B→A                  */
    ENT_MOVE_FOLD_OK      = 6,   /* fold กลับ core สำเร็จ              */
    ENT_MOVE_FOLD_FAIL    = 7,   /* fold fail หลัง world flip           */
    ENT_MOVE_RECYCLE      = 8,   /* flag RECYCLE → audit notified       */
    ENT_MOVE_HIBERNATE    = 9,   /* face → sleep                        */
    ENT_MOVE_WAKE         = 10,  /* face → waking                       */
} ent_move_t;

typedef struct __attribute__((packed)) {
    uint32_t  frame_id;   /* EjectFrame ที่เกิดเหตุ                    */
    uint8_t   move;       /* ent_move_t                                 */
    uint8_t   world;      /* 0=A, 1=B — world ขณะนั้น                  */
    uint8_t   rubik_step; /* move ครั้งที่เท่าไหร่ (0 ถ้าไม่เกี่ยว)  */
    uint8_t   flags;      /* reserved                                   */
    uint32_t  addr;       /* vector_pos ขณะนั้น                        */
} EntMoveEntry;
/* sizeof = 4+1+1+1+1+4 = 12B */

/* ═══════════════════════════════════════════════════════════════════════
   MOVEMENT LOG  (ring buffer — no malloc)
   ═══════════════════════════════════════════════════════════════════════ */

#define ENT_LOG_SIZE   256   /* power of 2 — wraps silently             */

typedef struct {
    EntMoveEntry      entries[ENT_LOG_SIZE];
    _Atomic uint32_t  head;   /* write cursor                           */
    uint32_t          frame_id;
} EntangleLog;

static inline void ent_log_init(EntangleLog *log, uint32_t frame_id)
{
    memset(log, 0, sizeof(*log));
    log->frame_id = frame_id;
}

static inline void ent_log_push(EntangleLog *log,
                                 ent_move_t   move,
                                 uint8_t      world,
                                 uint8_t      rubik_step,
                                 uint32_t     addr)
{
    uint32_t idx = atomic_fetch_add_explicit(&log->head, 1,
                       memory_order_relaxed) % ENT_LOG_SIZE;
    log->entries[idx] = (EntMoveEntry){
        .frame_id   = log->frame_id,
        .move       = (uint8_t)move,
        .world      = world,
        .rubik_step = rubik_step,
        .flags      = 0,
        .addr       = addr,
    };
}

/* ═══════════════════════════════════════════════════════════════════════
   RECYCLE FLAG  (แทน tombstone)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t  magic;       /* 0xDECA1C0D = "RECYCLED"                   */
    uint32_t  frame_id;
    uint64_t  gate_addr;
    uint8_t   world_at_fail; /* world ตอน fold fail ครั้งสุดท้าย       */
    uint8_t   rubik_moves;
    uint8_t   flipped;     /* 1 = เคย world flip แล้ว                  */
    uint8_t   audit_notified; /* 1 = audit รับรู้แล้ว                  */
    uint32_t  crc32;
} EntRecycleFlag;
/* sizeof = 4+4+8+1+1+1+1+4 = 24B */

#define ENT_RECYCLE_MAGIC  0xDECA1C0DU

static inline void ent_build_recycle(EntRecycleFlag *rf,
                                      const EjectFrame *ef,
                                      uint8_t world_at_fail,
                                      uint8_t flipped)
{
    rf->magic          = ENT_RECYCLE_MAGIC;
    rf->frame_id       = ef->frame_id;
    rf->gate_addr      = ef->gate_addr;
    rf->world_at_fail  = world_at_fail;
    rf->rubik_moves    = ef->rubik_moves;
    rf->flipped        = flipped;
    rf->audit_notified = 0;

    /* CRC32 ของ 20B แรก */
    uint32_t crc = 0;
    const uint8_t *p = (const uint8_t *)rf;
    for (int i = 0; i < 20; i++) {
        crc ^= (uint32_t)p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    rf->crc32 = crc;
}

/* ═══════════════════════════════════════════════════════════════════════
   ENTANGLE HOOK  — main struct
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t        frame_id;
    uint8_t         active;       /* 1 = hook ทำงานอยู่                */
    uint8_t         world;        /* current world 0=A 1=B             */
    uint8_t         flipped;      /* เคย world flip แล้ว               */
    uint8_t         recycled;     /* 1 = recycle flag set              */
    EntangleLog     log;
    EntRecycleFlag  recycle;
} EntangleHook;

/* ═══════════════════════════════════════════════════════════════════════
   HOOK LIFECYCLE
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ent_hook_init(EntangleHook    *h,
                                  const EjectFrame *ef,
                                  uint8_t          world)
{
    memset(h, 0, sizeof(*h));
    h->frame_id = ef->frame_id;
    h->active   = 1;
    h->world    = world;
    ent_log_init(&h->log, ef->frame_id);
    ent_log_push(&h->log, ENT_MOVE_DETACH, world, 0,
                 (uint32_t)ef->gate_addr);
}

/* ═══════════════════════════════════════════════════════════════════════
   UNIT CIRCLE CHECK  — in_circle สำหรับ entangle verify
   2a² < PHI_SCALE²  → safe zone
   ═══════════════════════════════════════════════════════════════════════ */

static inline int ent_in_circle(uint32_t addr)
{
    uint64_t a  = (uint64_t)addr;
    uint64_t s2 = (uint64_t)PHI_SCALE * PHI_SCALE;
    return (2 * a * a) < s2;
}

/* ═══════════════════════════════════════════════════════════════════════
   REPAIR PIPELINE  (ทั้งหมด < 1 วิ — ต่อเนื่องไม่หยุด)

   1. ลอง repair (rubik moves)
   2. ถ้า fail → world flip → ลอง fold อีกครั้ง
   3. ถ้า flip แล้วยัง fail → RECYCLE flag → notify audit

   คืน:  1 = fold สำเร็จ
         0 = recycle flagged
   ═══════════════════════════════════════════════════════════════════════ */

static inline int ent_repair_pipeline(EntangleHook *h,
                                       EjectFrame   *ef,
                                       DiamondBlock *b)
{
    /* validity = fibo address signature ยังอยู่ใน safe zone
       repair goal: หา rubik move ที่ทำให้ vector_pos กลับเข้า circle
       ไม่ใช่แค่ XOR (rebuild invert pass เสมอ) */

    uint32_t orig_addr = core_vector_pos(b->core);

    /* ── step 1: repair attempts ── */
    for (uint8_t m = 0; m < EJ_RUBIK_MAX_MOVES; m++) {
        ent_log_push(&h->log, ENT_MOVE_REPAIR_TRY, h->world, m,
                     orig_addr);

        /* permute core bytes */
        uint8_t raw[8];
        uint64_t core = b->core.raw;
        memcpy(raw, &core, 8);
        for (int i = 0; i < 8; i++)
            raw[i] = rubik_perm(raw[i], m);
        uint64_t new_core;
        memcpy(&new_core, raw, 8);

        /* check: vector_pos ของ core ใหม่อยู่ใน unit circle ไหม */
        uint32_t new_addr = (uint32_t)((new_core >> 32) & 0xFFFFF);
        if (ent_in_circle(new_addr)) {
            b->core.raw = new_core;
            b->invert   = ~new_core;
            ent_log_push(&h->log, ENT_MOVE_REPAIR_OK, h->world, m,
                         new_addr);
            ef->state = EJ_STATE_FOLDED;
            ent_log_push(&h->log, ENT_MOVE_FOLD_OK, h->world, m,
                         new_addr);
            return 1;
        }
    }

    /* ── step 2: repair fail → world flip ── */
    ent_log_push(&h->log, ENT_MOVE_REPAIR_FAIL, h->world, 0,
                 orig_addr);

    if (!h->flipped) {
        h->world   ^= 1;
        h->flipped  = 1;
        ent_log_push(&h->log, ENT_MOVE_WORLD_FLIP, h->world, 0,
                     orig_addr);

        /* world flip: inverse addr — ถ้า A→B addr ยังอยู่ใน circle = fold ok */
        uint32_t flip_addr = h->world
            ? fibo_addr_b(fibo_addr_to_node_a(orig_addr))
            : fibo_addr_a(fibo_addr_to_node_b(orig_addr));

        if (ent_in_circle(flip_addr)) {
            b->invert = ~b->core.raw;
            ef->state = EJ_STATE_FOLDED;
            ent_log_push(&h->log, ENT_MOVE_FOLD_OK, h->world, 0,
                         flip_addr);
            return 1;
        }
    }

    /* ── step 3: flip แล้วยัง fail → RECYCLE ── */
    ent_log_push(&h->log, ENT_MOVE_FOLD_FAIL, h->world, 0, orig_addr);
    ent_log_push(&h->log, ENT_MOVE_RECYCLE,   h->world, 0, orig_addr);

    ent_build_recycle(&h->recycle, ef, h->world, h->flipped);
    h->recycled = 1;
    h->active   = 0;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   HIBERNATE / WAKE  (Mode 2)
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ent_hibernate(EntangleHook *h, EjectFrame *ef)
{
    ej_hibernate(ef);
    ent_log_push(&h->log, ENT_MOVE_HIBERNATE, h->world, 0,
                 (uint32_t)ef->gate_addr);
}

static inline void ent_wake(EntangleHook *h, EjectFrame *ef)
{
    ej_wake(ef);
    ent_log_push(&h->log, ENT_MOVE_WAKE, h->world, 0,
                 (uint32_t)ef->gate_addr);
}

/* ═══════════════════════════════════════════════════════════════════════
   TAILS CHECKPOINT  (Tails จด log summary)
   caller ส่ง buffer 32B ไป — Tails เก็บไว้ใน HoneycombSlot
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t  frame_id;
    uint32_t  move_count;   /* total moves logged                       */
    uint8_t   last_move;    /* ent_move_t ล่าสุด                       */
    uint8_t   world_final;  /* world ตอนสุดท้าย                        */
    uint8_t   flipped;
    uint8_t   recycled;
    uint32_t  last_addr;
    uint8_t   _pad[12];     /* pad to 32B                               */
} EntTailsCheckpoint;
/* sizeof = 4+4+1+1+1+1+4+12 = 28... pad to 32 */

static inline void ent_tails_checkpoint(const EntangleHook *h,
                                         EntTailsCheckpoint *cp)
{
    memset(cp, 0, sizeof(*cp));
    cp->frame_id   = h->frame_id;
    cp->move_count = atomic_load_explicit(&h->log.head,
                         memory_order_relaxed);

    /* last entry */
    uint32_t last_idx = (cp->move_count - 1) % ENT_LOG_SIZE;
    if (cp->move_count > 0) {
        cp->last_move = h->log.entries[last_idx].move;
        cp->last_addr = h->log.entries[last_idx].addr;
    }

    cp->world_final = h->world;
    cp->flipped     = h->flipped;
    cp->recycled    = h->recycled;
}

#endif /* POGLS_ENTANGLE_H */
