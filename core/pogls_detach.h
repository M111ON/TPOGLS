/*
 * pogls_detach.h — POGLS V3.5 Detach Layer
 *
 * Isolated compute zone (mini-core) สำหรับ quarantine / mutation /
 * recursive bisection โดยไม่แตะ Core, Snapshot, หรือ Hydra engine เดิม
 *
 * Namespace: detach_* / Detach*  (ไม่ชน warp_map, branch_mode_t,
 *            HydraHead, snap_state_t หรือ WAL หลักเลย)
 *
 * ┌─────────────────────────────────────────────────────────────────┐
 * │  Detach lifecycle                                               │
 * │                                                                 │
 * │  detach_create()                                               │
 * │       ↓  DETACH_STATE_ACTIVE                                   │
 * │  detach_write_delta()   ← mutation ทุกอย่างลง delta slab       │
 * │       ↓                                                         │
 * │  detach_dock()          ← เตรียม merge / ตัดสินใจ             │
 * │       ↓ DETACH_STATE_DOCKING                                   │
 * │  detach_fold()          → merge delta กลับ parent  (success)   │
 * │  detach_cut()           → discard delta slab        (failure)  │
 * └─────────────────────────────────────────────────────────────────┘
 *
 * Pointer model:
 *   ptr < bounded_size  →  resolve ใน delta slab (local mutation)
 *   ptr >= bounded_size →  passthrough ไป core_base (shadow read)
 *
 * Dependencies: stdint.h, stdatomic.h, string.h
 *   ห้าม include pogls_hydra.h / pogls_snapshot.h ใน header นี้
 *   เพื่อให้ detach layer เป็น stand-alone module
 */

#ifndef POGLS_DETACH_H
#define POGLS_DETACH_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define DETACH_MAX_DEPTH    8     /* ลึกสุดที่ recursive bisect ได้      */
#define DETACH_MAX_FRAMES   256   /* frame table size                    */
#define DETACH_WAL_LANES    4     /* parallel WAL stripes                */
#define DETACH_WAL_BATCH    64    /* records ก่อน flush                  */

/* ═══════════════════════════════════════════════════════════════════════
   DETACH STATE
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    DETACH_STATE_ACTIVE  = 0,   /* กำลัง compute ใน detached zone       */
    DETACH_STATE_FOLDED  = 1,   /* merge กลับ parent สำเร็จ             */
    DETACH_STATE_CUT     = 2,   /* ตัดทิ้ง delta slab แล้ว             */
    DETACH_STATE_DOCKING = 3,   /* กำลัง verify ก่อน fold/cut           */
} detach_state_t;

/* WAL op codes — แยกจาก WAL_TYPE_* ของ WAL หลัก */
typedef enum {
    DETACH_WAL_OP_CREATE = 1,
    DETACH_WAL_OP_DELTA  = 2,
    DETACH_WAL_OP_FOLD   = 3,
    DETACH_WAL_OP_CUT    = 4,
} detach_wal_op_t;

/* ═══════════════════════════════════════════════════════════════════════
   DETACH FRAME
   represent หนึ่ง detached compute zone (mini-core)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    uint64_t  gate_addr;        /* address บน core ที่แยกออกมา         */
    uint64_t  bounded_size;     /* พื้นที่สูงสุดของ zone นี้            */

    uint32_t  frame_id;         /* UID ของ frame (alloc จาก table)     */
    uint32_t  parent_frame_id;  /* frame แม่ (0 = root)                */

    uint32_t  delta_base;       /* byte offset เริ่มต้นใน slab pool    */
    uint32_t  delta_used;       /* bytes ที่ใช้ไปแล้วใน delta          */

    uint32_t  shadow_passthrough; /* offset สำหรับ ptr >= bounded_size */
    uint16_t  parent_head_id;   /* Hydra head ที่ถูก disable ชั่วคราว */
    uint8_t   depth;            /* ชั้น recursion ปัจจุบัน (0 = top)  */
    uint8_t   state;            /* detach_state_t                      */

    uint64_t  ntacle_mask;      /* Ntacle ที่ bind กับ frame นี้       */

} DetachFrame;

/* ═══════════════════════════════════════════════════════════════════════
   DETACH HEAD  (mini compute head ภายใน detached zone)
   ไม่ใช่ POGLS_HydraHead — เป็น lightweight scheduler เท่านั้น
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    uint32_t  state;            /* 0=idle 1=active 2=done              */
    uint32_t  frame_id;         /* frame ที่ head นี้ดูแล              */

} DetachHead;

/* ═══════════════════════════════════════════════════════════════════════
   DELTA POOL  (flat slab allocator — ไม่มี pointer chain)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    uint8_t          *slabs;       /* contiguous buffer (prealloc)        */
    uint32_t          slab_size;   /* bytes ต่อ 1 slab                    */
    uint32_t          slab_count;  /* จำนวน slab ทั้งหมด                 */
    _Atomic uint32_t  free_head;   /* lock-free free list head            */

} DetachDeltaPool;

/* ═══════════════════════════════════════════════════════════════════════
   DETACH WAL  (แยกจาก WAL_Context หลักโดยสมบูรณ์)
   ═══════════════════════════════════════════════════════════════════════ */

/* 20B per record — fixed size สำหรับ group commit */
typedef struct __attribute__((packed)) {

    uint32_t  frame_id;
    uint32_t  offset;
    uint16_t  size;
    uint8_t   op;               /* detach_wal_op_t                     */
    uint8_t   lane;
    uint32_t  crc32;            /* CRC32 ของ 16B แรก (ไม่รวม crc32)  */

} DetachWALRecord;
/* sizeof = 4+4+2+1+1+4 = 16B → pad ถึง 20B ด้วย __attribute__((packed)) */

typedef struct {

    _Atomic uint32_t  pos;      /* atomic write index (0..BATCH-1)    */
    DetachWALRecord   buffer[DETACH_WAL_BATCH];

} DetachWALLane;

typedef struct {

    DetachWALLane  lane[DETACH_WAL_LANES];
    int            fd;          /* file descriptor สำหรับ flush        */
    /* flush policy: lane flushed เมื่อ pos == DETACH_WAL_BATCH-1
     * หรือ detach_wal_flush_all() ตอน shutdown / checkpoint          */

} DetachWAL;

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: POINTER RESOLVE
   hot path — เรียกทุก read/write ใน detached zone
   ═══════════════════════════════════════════════════════════════════════ */

static inline void *
detach_resolve_ptr(DetachFrame    *frame,
                   DetachDeltaPool *pool,
                   void           *core_base,
                   uint64_t        ptr)
{
    if (ptr < frame->bounded_size)
        return pool->slabs + frame->delta_base + ptr;

    return (uint8_t *)core_base + (ptr - frame->shadow_passthrough);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: DEPTH GUARD
   ═══════════════════════════════════════════════════════════════════════ */

static inline int
detach_depth_ok(uint8_t depth)
{
    return depth < DETACH_MAX_DEPTH;
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: MINI HEAD COUNT
   scale จำนวน head ตามขนาด zone (bounded_size >> 16 = MB rough estimate)
   ═══════════════════════════════════════════════════════════════════════ */

#define DETACH_MINI_HEAD_MAX 8

static inline uint8_t
detach_head_count(uint64_t bounded_size)
{
    uint8_t h = (uint8_t)(bounded_size >> 16);
    if (h > DETACH_MINI_HEAD_MAX) h = DETACH_MINI_HEAD_MAX;
    if (h == 0)                   h = 1;
    return h;
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: WAL LANE SELECT
   frame_id กระจาย lane อัตโนมัติ
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t
detach_wal_lane(uint32_t frame_id)
{
    return frame_id % DETACH_WAL_LANES;
}

/* ═══════════════════════════════════════════════════════════════════════
   DELTA POOL API
   ═══════════════════════════════════════════════════════════════════════ */

/* init pool จาก prealloc buffer — ไม่ malloc ใน runtime */
int      detach_pool_init(DetachDeltaPool *pool,
                          void            *mem,
                          uint32_t         slab_size,
                          uint32_t         slab_count);

/* alloc slab — คืน slab index หรือ UINT32_MAX ถ้าเต็ม */
uint32_t detach_pool_alloc(DetachDeltaPool *pool);

/* free slab กลับ pool */
void     detach_pool_free(DetachDeltaPool *pool, uint32_t slab_id);

/* ═══════════════════════════════════════════════════════════════════════
   LIFECYCLE API
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * detach_create — สร้าง DetachFrame ใหม่ใน table
 *
 * table        : array[DETACH_MAX_FRAMES] ที่ caller จัดสรร
 * parent_frame : frame_id ของ parent (UINT32_MAX = root)
 * parent_head  : head_id ของ POGLS_HydraHead ที่จะ disable
 * gate         : address บน core ที่แยกออก
 * size         : bounded_size ของ zone ใหม่
 *
 * คืน frame_id ใหม่  หรือ UINT32_MAX ถ้า depth เกิน / table เต็ม
 */
uint32_t detach_create(DetachFrame *table,
                       uint32_t     parent_frame,
                       uint16_t     parent_head,
                       uint64_t     gate,
                       uint64_t     size);

/*
 * detach_write_delta — เขียน mutation ลง delta slab
 * ไม่แตะ core_base โดยตรง
 */
int detach_write_delta(DetachFrame    *frame,
                       DetachDeltaPool *pool,
                       uint32_t        offset,
                       const void     *src,
                       uint32_t        size);

/*
 * detach_dock — เปลี่ยน state เป็น DOCKING
 * เตรียมให้ Tail verify ก่อนตัดสินใจ fold/cut
 */
void detach_dock(DetachFrame *frame);

/*
 * detach_fold — merge delta กลับ parent core region
 * core_base + gate_addr คือ destination
 */
void detach_fold(DetachFrame    *frame,
                 DetachDeltaPool *pool,
                 void           *core_base);

/*
 * detach_cut — ทิ้ง delta slab โดยไม่ merge
 * ใช้เมื่อ zone มีปัญหาแก้ไม่ได้
 */
void detach_cut(DetachFrame    *frame,
                DetachDeltaPool *pool);

/* ═══════════════════════════════════════════════════════════════════════
   WAL API
   ═══════════════════════════════════════════════════════════════════════ */

int  detach_wal_init(DetachWAL *wal, int fd);

/* append record ลง lane ที่เหมาะสม — thread-safe ผ่าน atomic pos */
void detach_wal_append(DetachWAL       *wal,
                       DetachWALRecord  rec);

/* flush lane ที่ระบุ (เรียกจาก io thread) */
void detach_wal_flush_lane(DetachWAL *wal, uint32_t lane_id);

/* flush ทุก lane — เรียกตอน shutdown หรือ checkpoint */
void detach_wal_flush_all(DetachWAL *wal);

/* WAL replay หลัง crash */
int  detach_wal_replay(int fd, DetachFrame *table, DetachDeltaPool *pool);

#endif /* POGLS_DETACH_H */
