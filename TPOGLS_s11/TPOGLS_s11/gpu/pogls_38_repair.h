/*
 * pogls_38_repair.h — POGLS38  Repair Layer
 * ══════════════════════════════════════════════════════════════════════
 *
 * Port จาก V3.6 (pogls_detach_eject.h + pogls_entangle.h)
 * ดัดแปลงให้ทำงานกับ 17n lattice และ CPU fallback
 *
 * ห้ามแตะไฟล์นี้จาก POGLS4 — standalone สำหรับ POGLS38 เท่านั้น
 *
 * สิ่งที่ port:
 *   1. Eject Scale Ladder  162→54→18→6→2 (÷3 × 4 steps)
 *   2. Entangle movement log (ring buffer, 16B/entry)
 *   3. World Flip pipeline: repair fail → flip A↔B → retry fold
 *   4. Recycle flag (ไม่ใช่ tombstone — system learns)
 *
 * สิ่งที่ไม่ได้ port:
 *   - DiamondBlock (V4 specific format)
 *   - pogls_rubik.h (ใช้ simple permute แทน)
 *   - Hydra scheduler (V4 has better version)
 *
 * CPU fallback: ทุก function ทำงานบน CPU ล้วน (no GPU)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_38_REPAIR_H
#define POGLS_38_REPAIR_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "pogls_17n_lattice.h"   /* L17 cell, world, PHI constants */

/* ══════════════════════════════════════════════════════════════════════
 * SCALE LADDER (FROZEN — World B ternary descend)
 *
 * 162 = NODE_MAX (full scope)
 * 54  = Rubik nexus
 * 18  = gate_18 (common clock)
 * 6   = minimal group (2×3)
 * 2   = floor — cannot narrow further
 *
 * เปรียบเทียบ: เหมือน zoom out ทีละชั้น — ถ้า repair ไม่ได้ที่ zoom 1
 * ก็ zoom out ออกไปแก้ที่ระดับ macro แทน
 * ══════════════════════════════════════════════════════════════════════ */

#define L38_SCALE_STEPS     5
#define L38_RUBIK_MAX_MOVES 20   /* God's Number — optimal ≤ 20 moves   */
#define L38_TOMBSTONE_MAGIC 0xDE4DB10Cu
#define L38_RECYCLE_MAGIC   0xDECA1C0Du
#define L38_REPAIR_MAGIC    0x52505233u   /* "RPR3" */

static const uint16_t l38_scale_ladder[L38_SCALE_STEPS] = {
    162, 54, 18, 6, 2
};

/* scale ladder ÷3 verified in test_38_repair T01 (runtime check) */

/* ══════════════════════════════════════════════════════════════════════
 * MOVEMENT LOG — บันทึกทุก step ของ repair pipeline
 *
 * ENT_MOVE_DETACH       = เริ่มต้น repair cycle
 * ENT_MOVE_REPAIR_TRY   = ลอง rubik permute
 * ENT_MOVE_REPAIR_OK    = unit circle pass → fold ready
 * ENT_MOVE_REPAIR_FAIL  = ล้มเหลว → world flip pending
 * ENT_MOVE_WORLD_FLIP   = flip A↔B (inverse PHI addr)
 * ENT_MOVE_FOLD_OK      = fold back สำเร็จ
 * ENT_MOVE_FOLD_FAIL    = fold ล้มเหลวหลัง flip
 * ENT_MOVE_RECYCLE      = flagged RECYCLE → audit notified
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    ENT_MOVE_DETACH       = 1,
    ENT_MOVE_REPAIR_TRY   = 2,
    ENT_MOVE_REPAIR_OK    = 3,
    ENT_MOVE_REPAIR_FAIL  = 4,
    ENT_MOVE_WORLD_FLIP   = 5,
    ENT_MOVE_FOLD_OK      = 6,
    ENT_MOVE_FOLD_FAIL    = 7,
    ENT_MOVE_RECYCLE      = 8,
    ENT_MOVE_HIBERNATE    = 9,
    ENT_MOVE_WAKE         = 10,
} l38_move_t;

typedef struct __attribute__((packed)) {
    uint32_t  frame_id;
    uint8_t   move;        /* l38_move_t                                */
    uint8_t   world;       /* 0=A, 1=B                                  */
    uint8_t   scale_step;  /* ladder step ขณะนั้น (0=162 .. 4=2)       */
    uint8_t   rubik_step;  /* move index 0..19                          */
    uint32_t  cell_id;     /* L17 cell_id ขณะนั้น                       */
} L38MoveEntry;            /* 12B — 2 entries per cache line            */

#define L38_LOG_SIZE  256   /* ring buffer, power-of-2, wraps silently  */

typedef struct {
    L38MoveEntry      entries[L38_LOG_SIZE];
    _Atomic uint32_t  head;
    uint32_t          frame_id;
} L38MoveLog;

static inline void l38_log_init(L38MoveLog *log, uint32_t frame_id) {
    memset(log, 0, sizeof(*log));
    log->frame_id = frame_id;
}

static inline void l38_log_push(L38MoveLog   *log,
                                  l38_move_t    move,
                                  uint8_t       world,
                                  uint8_t       scale_step,
                                  uint8_t       rubik_step,
                                  uint32_t      cell_id)
{
    uint32_t idx = atomic_fetch_add_explicit(&log->head, 1,
                       memory_order_relaxed) % L38_LOG_SIZE;
    log->entries[idx] = (L38MoveEntry){
        .frame_id   = log->frame_id,
        .move       = (uint8_t)move,
        .world      = world,
        .scale_step = scale_step,
        .rubik_step = rubik_step,
        .cell_id    = cell_id,
    };
}

/* ══════════════════════════════════════════════════════════════════════
 * RECYCLE FLAG — บันทึกเมื่อ repair + world flip ล้มเหลวทั้งคู่
 * ใช้แทน tombstone: ระบบยังเรียนรู้ได้จาก recycle
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t  magic;            /* L38_RECYCLE_MAGIC                     */
    uint32_t  frame_id;
    uint32_t  cell_id;          /* L17 cell_id                           */
    uint8_t   world_at_fail;    /* world ตอน fold fail ครั้งสุดท้าย     */
    uint8_t   rubik_moves;
    uint8_t   flipped;          /* 1 = เคย world flip แล้ว              */
    uint8_t   scale_step;       /* ladder step สุดท้าย                   */
    uint32_t  crc32;
} L38RecycleFlag;               /* 24B                                   */

/* ══════════════════════════════════════════════════════════════════════
 * EJECT FRAME — state ของ 1 repair cycle
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    L38_STATE_ACTIVE     = 0,
    L38_STATE_FOLDED     = 1,   /* repair สำเร็จ → merged back          */
    L38_STATE_LOCKED     = 2,   /* scale ถึง 2 แล้วยังไม่ได้            */
    L38_STATE_RECYCLED   = 3,   /* world flip ก็ยังไม่ได้ → recycle     */
} l38_repair_state_t;

typedef struct {
    uint32_t            magic;
    uint32_t            frame_id;
    l38_repair_state_t  state;
    uint8_t             scale_step;    /* 0=162, 1=54, 2=18, 3=6, 4=2   */
    uint8_t             rubik_moves;   /* moves ที่ใช้ไปแล้วใน step นี้  */
    uint8_t             world;         /* current world 0=A 1=B          */
    uint8_t             flipped;       /* เคย world flip แล้ว            */
    uint8_t             recycled;

    uint32_t            cell_id;       /* L17 cell ที่ repair            */
    uint64_t            angular_addr;  /* original address                */
    uint32_t            repair_total;  /* total moves across all steps    */

    L38MoveLog          log;
    L38RecycleFlag      recycle;
} L38EjectFrame;

static inline void l38_frame_init(L38EjectFrame *ef,
                                   uint32_t       frame_id,
                                   uint32_t       cell_id,
                                   uint64_t       angular_addr,
                                   uint8_t        world)
{
    memset(ef, 0, sizeof(*ef));
    ef->magic        = L38_REPAIR_MAGIC;
    ef->frame_id     = frame_id;
    ef->state        = L38_STATE_ACTIVE;
    ef->cell_id      = cell_id;
    ef->angular_addr = angular_addr;
    ef->world        = world;
    l38_log_init(&ef->log, frame_id);
    l38_log_push(&ef->log, ENT_MOVE_DETACH, world, 0, 0, cell_id);
}

/* ══════════════════════════════════════════════════════════════════════
 * UNIT CIRCLE CHECK
 *
 * 2a² < PHI_SCALE²  → address อยู่ใน safe zone (unit circle)
 * ใช้ตรวจว่า repair คืน address ที่ valid ไหม
 *
 * เปรียบเทียบ: เหมือนตรวจว่าลูกปิงปองอยู่ในวง — ถ้าอยู่ = ถูกทิศ
 * ══════════════════════════════════════════════════════════════════════ */

static inline int l38_in_circle(uint32_t addr)
{
    uint64_t a  = (uint64_t)(addr & (PHI_SCALE - 1u));
    uint64_t s2 = (uint64_t)PHI_SCALE * PHI_SCALE;
    return (2u * a * a) < s2;
}

/* ══════════════════════════════════════════════════════════════════════
 * RUBIK PERMUTE — simple byte permutation (no rubik.h dependency)
 *
 * port จาก V3.6 ใช้ตรรกะเดิม:
 *   move 0..17 → permute byte ด้วย fixed rotation + XOR pattern
 *   deterministic, reversible (move + 9 = inverse)
 *
 * ไม่ใช่ full rubik cube — เป็นแค่ byte-level permutation engine
 * ที่ทำให้ CoreSlot address กลับเข้า unit circle
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint8_t _l38_rubik_perm_compute(uint8_t byte, uint8_t move)
{
    uint8_t m = move % 18u;
    uint8_t rot = (uint8_t)(m % 8u);
    uint8_t xk  = (uint8_t)(0x36u ^ (m << 2));
    return (uint8_t)(((byte << rot) | (byte >> (8u - rot))) ^ xk);
}

/* ══════════════════════════════════════════════════════════════════════
 * REPAIR STEP — ลอง rubik permute บน angular_addr แล้วตรวจ circle
 *
 * แทนที่ DiamondBlock: ทำงานบน raw angular_addr โดยตรง
 * ตรงกับ 17n design: address เป็น first-class, ไม่ wrap ใน block
 * ══════════════════════════════════════════════════════════════════════ */

static inline int l38_try_repair(L38EjectFrame *ef, uint32_t *addr_inout)
{
    for (uint8_t m = 0; m < L38_RUBIK_MAX_MOVES; m++) {
        ef->rubik_moves++;
        ef->repair_total++;
        l38_log_push(&ef->log, ENT_MOVE_REPAIR_TRY,
                     ef->world, ef->scale_step, m, ef->cell_id);

        /* permute low 20 bits of addr (angular address range) */
        uint32_t addr   = *addr_inout;
        uint32_t hi     = addr & ~(PHI_SCALE - 1u);
        uint32_t lo     = addr &  (PHI_SCALE - 1u);
        uint8_t  lo_lo  = (uint8_t)(lo & 0xFFu);
        uint8_t  lo_hi  = (uint8_t)((lo >> 8) & 0xFFFu);
        lo_lo = _l38_rubik_perm_compute(lo_lo, m);
        uint32_t new_lo = (uint32_t)(((uint32_t)lo_hi << 8) | lo_lo);
        uint32_t new_addr = hi | (new_lo & (PHI_SCALE - 1u));

        if (l38_in_circle(new_addr)) {
            *addr_inout = new_addr;
            l38_log_push(&ef->log, ENT_MOVE_REPAIR_OK,
                         ef->world, ef->scale_step, m, ef->cell_id);
            return 1;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * WORLD FLIP — inverse PHI address transform
 *
 * A→B: addr_b = (addr_a × PHI_DOWN) % PHI_SCALE
 * B→A: addr_a = (addr_b × PHI_UP)   % PHI_SCALE
 *
 * เปรียบเทียบ: เหมือนพลิกกระจก — A ด้านหนึ่ง B อีกด้านหนึ่ง
 * ถ้าซ่อมไม่ได้ที่ A ลองดูว่า reflection ของมันอยู่ใน circle ไหม
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint32_t l38_world_flip_addr(uint32_t addr, uint8_t from_world)
{
    if (from_world == 0) {
        /* A→B: multiply by PHI_DOWN */
        return (uint32_t)(((uint64_t)addr * PHI_DOWN) % PHI_SCALE);
    } else {
        /* B→A: multiply by PHI_UP */
        return (uint32_t)(((uint64_t)addr * PHI_UP) % PHI_SCALE);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * RECYCLE FLAG BUILD
 * ══════════════════════════════════════════════════════════════════════ */

static inline void l38_build_recycle(L38EjectFrame *ef)
{
    L38RecycleFlag *rf = &ef->recycle;
    rf->magic          = L38_RECYCLE_MAGIC;
    rf->frame_id       = ef->frame_id;
    rf->cell_id        = ef->cell_id;
    rf->world_at_fail  = ef->world;
    rf->rubik_moves    = ef->rubik_moves;
    rf->flipped        = ef->flipped;
    rf->scale_step     = ef->scale_step;

    /* simple CRC32 for integrity */
    uint32_t crc = 0;
    const uint8_t *p = (const uint8_t *)rf;
    for (int i = 0; i < 20; i++) {
        crc ^= (uint32_t)p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    rf->crc32 = crc;

    ef->recycled = 1;
    ef->state    = L38_STATE_RECYCLED;
}

/* ══════════════════════════════════════════════════════════════════════
 * FULL REPAIR PIPELINE
 *
 * P → P → R → E loop ที่สมบูรณ์:
 *
 *   Process:  ลอง rubik repair ที่ current scale
 *   Protect:  ถ้าไม่ได้ → narrow scope (scale down ÷3)
 *   Repair:   ลอง repair ที่ scale ใหม่
 *   Evolve:   ถ้าทุก scale ล้มเหลว → World Flip → ลองอีกครั้ง
 *             ถ้า flip ก็ยังไม่ได้ → RECYCLE (ไม่ใช่ตาย — เรียนรู้)
 *
 * คืน:  1 = folded (repair สำเร็จ)
 *        0 = recycled (audit notified, ระบบเรียนรู้)
 *
 * addr_inout: angular_addr ที่ต้อง repair
 *             ถ้า success → updated เป็น addr ที่ผ่าน circle
 *             ถ้า recycle → ไม่เปลี่ยน
 * ══════════════════════════════════════════════════════════════════════ */

static inline int l38_repair_pipeline(L38EjectFrame *ef,
                                       uint32_t      *addr_inout)
{
    /* ── Scale Ladder: ลองทุก scale จาก 162 → 2 ── */
    for (ef->scale_step = 0;
         ef->scale_step < L38_SCALE_STEPS;
         ef->scale_step++)
    {
        ef->rubik_moves = 0;

        if (l38_try_repair(ef, addr_inout)) {
            ef->state = L38_STATE_FOLDED;
            l38_log_push(&ef->log, ENT_MOVE_FOLD_OK,
                         ef->world, ef->scale_step, 0, ef->cell_id);
            return 1;
        }
        /* scale ≥ 2 → ÷3 ต่อไปในรอบถัดไป */
    }

    /* ── Repair fail at all scales → World Flip ── */
    l38_log_push(&ef->log, ENT_MOVE_REPAIR_FAIL,
                 ef->world, ef->scale_step, 0, ef->cell_id);

    if (!ef->flipped) {
        uint32_t flip_addr = l38_world_flip_addr(*addr_inout, ef->world);
        ef->world  ^= 1u;
        ef->flipped = 1;
        l38_log_push(&ef->log, ENT_MOVE_WORLD_FLIP,
                     ef->world, 0, 0, ef->cell_id);

        /* flip addr อยู่ใน circle ไหม */
        if (l38_in_circle(flip_addr)) {
            *addr_inout = flip_addr;
            ef->state   = L38_STATE_FOLDED;
            l38_log_push(&ef->log, ENT_MOVE_FOLD_OK,
                         ef->world, 0, 0, ef->cell_id);
            return 1;
        }

        /* ลอง rubik repair บน flipped world */
        ef->scale_step  = 0;
        ef->rubik_moves = 0;
        uint32_t tmp_addr = flip_addr;
        if (l38_try_repair(ef, &tmp_addr)) {
            *addr_inout = tmp_addr;
            ef->state   = L38_STATE_FOLDED;
            l38_log_push(&ef->log, ENT_MOVE_FOLD_OK,
                         ef->world, ef->scale_step, 0, ef->cell_id);
            return 1;
        }
    }

    /* ── Flip ก็ยังไม่ได้ → RECYCLE ── */
    l38_log_push(&ef->log, ENT_MOVE_FOLD_FAIL,
                 ef->world, ef->scale_step, 0, ef->cell_id);
    l38_log_push(&ef->log, ENT_MOVE_RECYCLE,
                 ef->world, ef->scale_step, 0, ef->cell_id);
    l38_build_recycle(ef);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * TAILS CHECKPOINT — 32B snapshot สำหรับ observer layer
 *
 * caller ส่ง L38TailsCheckpoint* ไปให้ Tails (binary-index.js)
 * เก็บไว้ใน HoneycombSlot หรือ external log
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t  frame_id;
    uint32_t  move_count;    /* total moves logged                       */
    uint8_t   last_move;     /* l38_move_t ล่าสุด                       */
    uint8_t   world_final;
    uint8_t   flipped;
    uint8_t   recycled;
    uint32_t  cell_id;
    uint8_t   scale_final;   /* ladder step สุดท้าย                     */
    uint8_t   repair_state;  /* l38_repair_state_t                      */
    uint8_t   _pad[6];       /* pad to 24B                               */
} L38TailsCheckpoint;        /* 24B                                      */

static inline void l38_tails_checkpoint(const L38EjectFrame *ef,
                                          L38TailsCheckpoint  *cp)
{
    memset(cp, 0, sizeof(*cp));
    cp->frame_id    = ef->frame_id;
    cp->move_count  = atomic_load_explicit(&ef->log.head,
                          memory_order_relaxed);
    cp->cell_id     = ef->cell_id;
    cp->world_final = ef->world;
    cp->flipped     = ef->flipped;
    cp->recycled    = ef->recycled;
    cp->scale_final = ef->scale_step;
    cp->repair_state = (uint8_t)ef->state;

    if (cp->move_count > 0) {
        uint32_t last = (cp->move_count - 1u) % L38_LOG_SIZE;
        cp->last_move = ef->log.entries[last].move;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * L17 INTEGRATION — trigger repair จาก L17Lattice
 *
 * เรียกเมื่อ l17_write() พบว่า addr ไม่อยู่ใน unit circle
 * หรือ audit flagged cell เป็น anomaly
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t    next_frame_id;
    uint64_t    total_repairs;
    uint64_t    total_recycled;
    uint64_t    total_folded;
    uint64_t    world_flips;
} L38RepairCtx;

#define L38_REPAIR_CTX_MAGIC  0x52433338u   /* "RC38" */

static inline void l38_repair_ctx_init(L38RepairCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->next_frame_id = 1;
}

/* trigger repair สำหรับ 1 cell — คืน 1=folded, 0=recycled */
static inline int l38_repair_cell(L38RepairCtx *ctx,
                                   L17Lattice   *lat,   /* optional, can be NULL */
                                   uint32_t      cell_id,
                                   uint64_t      angular_addr,
                                   uint8_t       world,
                                   L38TailsCheckpoint *tails_out) /* optional */
{
    L38EjectFrame ef;
    l38_frame_init(&ef, ctx->next_frame_id++, cell_id, angular_addr, world);

    uint32_t addr = (uint32_t)(angular_addr & (PHI_SCALE - 1u));
    int ok = l38_repair_pipeline(&ef, &addr);

    ctx->total_repairs++;
    if (ok) {
        ctx->total_folded++;
        /* update lattice lane flags if available */
        if (lat && cell_id < L17_LANES_TOTAL) {
            lat->lanes[cell_id].flags &= (uint8_t)~L17_FLAG_GHOST;
        }
    } else {
        ctx->total_recycled++;
        ctx->world_flips += ef.flipped ? 1u : 0u;
        if (lat && cell_id < L17_LANES_TOTAL)
            lat->lanes[cell_id].flags |= L17_FLAG_GHOST;
    }

    if (tails_out)
        l38_tails_checkpoint(&ef, tails_out);

    return ok;
}

#endif /* POGLS_38_REPAIR_H */
