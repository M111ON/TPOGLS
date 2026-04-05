/*
 * pogls_detach_eject.h — POGLS V3.6 Detach Eject Engine
 *
 * 3 Eject modes:
 *
 *   Mode 1 — Emergency Eject (error recovery)
 *     trigger : Checker Beam dirty / XOR fail
 *     scale   : 162 → 54 → 18 → 6 → 2  (÷3 × 4 steps)
 *     repair  : Rubik 20 moves per scale level, XOR verify each step
 *     fail    : LOCKED → audit_emit_anomaly() → Tombstone
 *     silent  : Tails + Ntangle observe throughout
 *
 *   Mode 2 — Resource Hibernation (sleep/wake)
 *     trigger : load high / face idle too long
 *     process : face → SLEEP state, warp_map bridge stays open
 *     wake    : load drops → merge back to core
 *
 *   Mode 3 — Experimental Hydra (extra head)
 *     trigger : manual / need head beyond 16 limit
 *     process : detach face → spawn Hydra head on it
 *     safety  : Tails + Ntangle watch; failure → discard only
 *
 * Dependencies: pogls_fold.h, pogls_rubik.h, pogls_detach.h, pogls_audit.h
 * Namespace: ej_* / EjectFrame
 * ห้าม include pogls_hydra.h ใน header นี้
 */

#ifndef POGLS_DETACH_EJECT_H
#define POGLS_DETACH_EJECT_H

#include <stdint.h>
#include <string.h>
#include "pogls_fold.h"
#include "pogls_rubik.h"

/* ═══════════════════════════════════════════════════════════════════════
   SCALE LADDER  (Mode 1)
   162 → 54 → 18 → 6 → 2  (÷3 each step, 4 steps total)
   ═══════════════════════════════════════════════════════════════════════ */

#define EJ_SCALE_STEPS      5
#define EJ_RUBIK_MAX_MOVES  20   /* God's Number */

static const uint16_t ej_scale_ladder[EJ_SCALE_STEPS] = {
    162, 54, 18, 6, 2
};

/* ═══════════════════════════════════════════════════════════════════════
   EJECT MODE
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    EJ_MODE_EMERGENCY    = 1,   /* error → rubik repair → fold/tombstone  */
    EJ_MODE_HIBERNATE    = 2,   /* load/idle → sleep → warp bridge → wake */
    EJ_MODE_EXPERIMENTAL = 3,   /* manual → extra Hydra head              */
} ej_mode_t;

/* ═══════════════════════════════════════════════════════════════════════
   EJECT STATE
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    EJ_STATE_ACTIVE     = 0,   /* กำลัง repair / hibernate / experiment  */
    EJ_STATE_FOLDED     = 1,   /* repair สำเร็จ → merge กลับ core        */
    EJ_STATE_LOCKED     = 2,   /* repair ล้มเหลว → tombstone pending     */
    EJ_STATE_SLEEPING   = 3,   /* hibernate — warp bridge เปิด          */
    EJ_STATE_WAKING     = 4,   /* กำลัง merge กลับจาก sleep             */
    EJ_STATE_TOMBSTONE  = 5,   /* dead — audit notified, user pending    */
    EJ_STATE_DISCARDED  = 6,   /* experimental ล้มเหลว → cut ทิ้ง       */
} ej_state_t;

/* ═══════════════════════════════════════════════════════════════════════
   TOMBSTONE RECORD  (เก็บไว้ให้ audit + user รู้)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t  magic;            /* 0xDE4DB10C = "DEADBLOC"               */
    uint32_t  frame_id;         /* eject frame ที่สร้าง tombstone นี้   */
    uint64_t  gate_addr;        /* address ที่ตาย                        */
    uint64_t  created_at_ms;    /* timestamp                             */
    uint8_t   rubik_attempts;   /* จำนวน move ที่ลองไปก่อนตาย           */
    uint8_t   scale_reached;    /* scale level สุดท้ายก่อนตาย (0-4)    */
    uint8_t   final_state;      /* core_slot byte ตอนตาย                */
    uint8_t   _pad;
    uint32_t  crc32;            /* CRC32 ของ 24B แรก                    */
} EjectTombstone;
/* sizeof = 4+4+8+8+1+1+1+1+4 = 32B */

#define EJ_TOMBSTONE_MAGIC  0xDE4DB10CU

/* ═══════════════════════════════════════════════════════════════════════
   EJECT FRAME
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    /* identity */
    uint32_t    frame_id;
    uint8_t     mode;           /* ej_mode_t                             */
    uint8_t     state;          /* ej_state_t                            */
    uint8_t     scale_step;     /* ปัจจุบันอยู่ที่ scale ladder step ไหน */
    uint8_t     rubik_moves;    /* moves ที่ใช้ไปแล้ว (0-20)            */

    /* target block */
    uint64_t    gate_addr;      /* address บน core                       */
    DiamondBlock snapshot;      /* สำเนา DiamondBlock ก่อน eject        */

    /* repair state */
    uint8_t     repair_byte_idx; /* byte ใน CoreSlot ที่กำลัง repair    */
    uint8_t     last_rubik_state;/* rubik state ล่าสุด                  */

    /* hibernate */
    uint8_t     warp_open;      /* 1 = warp bridge เปิด (Mode 2)        */

    /* silent observers */
    uint8_t     tails_notified;  /* 1 = Tails รับรู้แล้ว                */
    uint64_t    ntangle_mask;    /* Ntangle ที่ entangle กับ frame นี้   */

    /* tombstone */
    EjectTombstone tombstone;   /* filled เมื่อ state = EJ_STATE_TOMBSTONE */

    /* stats */
    uint64_t    created_at_ms;
    uint32_t    repair_attempts; /* ทั้งหมดข้าม scale levels            */

} EjectFrame;

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 1 — XOR REPAIR ATTEMPT
   ลอง rubik permute CoreSlot แล้ว verify XOR ทันที
   คืน 1 = repair สำเร็จ, 0 = ยังไม่ผ่าน
   ═══════════════════════════════════════════════════════════════════════ */

static inline int ej_try_rubik_repair(DiamondBlock *b, uint8_t move)
{
    /* permute แต่ละ byte ของ CoreSlot ด้วย rubik move */
    uint8_t raw[8];
    uint64_t core = b->core.raw;
    memcpy(raw, &core, 8);

    for (int i = 0; i < 8; i++)
        raw[i] = rubik_perm(raw[i], move);

    uint64_t new_core;
    memcpy(&new_core, raw, 8);
    b->core.raw = new_core;
    b->invert   = ~new_core;   /* rebuild invert */

    /* L1 XOR verify — ~0.3ns */
    return fold_xor_audit(b);
}

/* ═══════════════════════════════════════════════════════════════════════
   MODE 1 — EMERGENCY EJECT
   เรียกเมื่อ Checker Beam / XOR fail
   คืน 1 = repaired (fold ready), 0 = locked (tombstone needed)
   ═══════════════════════════════════════════════════════════════════════ */

static inline int ej_emergency_repair(EjectFrame *ef, DiamondBlock *b)
{
    /* ลอง rubik repair ทีละ move ไม่เกิน 20 */
    for (uint8_t m = 0; m < EJ_RUBIK_MAX_MOVES; m++) {

        uint8_t move = (uint8_t)(m % RUBIK_MOVES);
        ef->rubik_moves++;
        ef->repair_attempts++;

        if (ej_try_rubik_repair(b, move)) {
            /* L2 Fibo verify เพิ่มเติม */
            if (!fold_fibo_needs_merkle(b)) {
                ef->state = EJ_STATE_FOLDED;
                return 1;   /* repair สำเร็จ */
            }
        }
    }

    /* ครบ 20 moves — scale down แล้วลองใหม่ */
    if (ef->scale_step < EJ_SCALE_STEPS - 1) {
        ef->scale_step++;
        ef->rubik_moves = 0;
        /* caller จะ narrow scope แล้วเรียก ej_emergency_repair ใหม่ */
        return 0;
    }

    /* ถึง scale 2 แล้วยังไม่ได้ → LOCKED */
    ef->state = EJ_STATE_LOCKED;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   TOMBSTONE BUILD
   เรียกหลัง ej_emergency_repair คืน 0 และ state = LOCKED
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ej_build_tombstone(EjectFrame *ef, uint64_t now_ms)
{
    EjectTombstone *t = &ef->tombstone;
    t->magic           = EJ_TOMBSTONE_MAGIC;
    t->frame_id        = ef->frame_id;
    t->gate_addr       = ef->gate_addr;
    t->created_at_ms   = now_ms;
    t->rubik_attempts  = ef->rubik_moves;
    t->scale_reached   = ef->scale_step;
    t->final_state     = (uint8_t)(ef->snapshot.core.raw & 0xFF);
    t->_pad            = 0;

    /* CRC32 simple (ใช้ได้กับ header — ไม่ต้องการ cryptographic strength) */
    uint32_t crc = 0;
    const uint8_t *p = (const uint8_t *)t;
    for (int i = 0; i < 28; i++) {
        crc ^= (uint32_t)p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    t->crc32 = crc;

    ef->state = EJ_STATE_TOMBSTONE;
}

/* ═══════════════════════════════════════════════════════════════════════
   MODE 2 — HIBERNATE
   ดีด face ออก → SLEEP → warp bridge เปิด
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ej_hibernate(EjectFrame *ef)
{
    ef->state      = EJ_STATE_SLEEPING;
    ef->warp_open  = 1;   /* warp_map bridge เปิด → Hydra read/write ผ่านได้ */
}

static inline void ej_wake(EjectFrame *ef)
{
    if (ef->state != EJ_STATE_SLEEPING) return;
    ef->state     = EJ_STATE_WAKING;
    ef->warp_open = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   MODE 3 — EXPERIMENTAL HYDRA
   ดีด face ออก → spawn extra head บน face นั้น
   caller ต้องดูแล head lifecycle เอง
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ej_experimental_discard(EjectFrame *ef)
{
    /* ล้มเหลว → ตัดทิ้ง ไม่แตะ core */
    ef->state     = EJ_STATE_DISCARDED;
    ef->warp_open = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   FRAME INIT
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ej_frame_init(EjectFrame     *ef,
                                  uint32_t        frame_id,
                                  ej_mode_t       mode,
                                  uint64_t        gate_addr,
                                  const DiamondBlock *block,
                                  uint64_t        ntangle_mask,
                                  uint64_t        now_ms)
{
    memset(ef, 0, sizeof(*ef));
    ef->frame_id      = frame_id;
    ef->mode          = (uint8_t)mode;
    ef->state         = EJ_STATE_ACTIVE;
    ef->gate_addr     = gate_addr;
    ef->ntangle_mask  = ntangle_mask;
    ef->created_at_ms = now_ms;
    if (block)
        ef->snapshot = *block;   /* สำเนา block ก่อน repair */
}

/* ═══════════════════════════════════════════════════════════════════════
   SCALE INFO HELPER
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint16_t ej_current_scale(const EjectFrame *ef)
{
    uint8_t s = ef->scale_step;
    if (s >= EJ_SCALE_STEPS) s = EJ_SCALE_STEPS - 1;
    return ej_scale_ladder[s];
}

#endif /* POGLS_DETACH_EJECT_H */
