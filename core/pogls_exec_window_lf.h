/*
 * pogls_exec_window_lf.h — POGLS V3.5 Lock-free ExecWindow
 *
 * แทน ExecWindow แบบ count++ ด้วย atomic ring buffer
 *
 * ข้อดีเหนือ ExecWindow เดิม:
 *   - multi-producer safe (หลาย Hydra head push พร้อมกัน)
 *   - commit worker pop_batch ได้โดยไม่ lock
 *   - ไม่มี flush-before-append race
 *
 * Namespace: ewlf_* / ExecWindowLF
 * ใช้แทน ExecWindow ได้โดยตรง — API compatible ส่วนใหญ่
 */

#ifndef POGLS_EXEC_WINDOW_LF_H
#define POGLS_EXEC_WINDOW_LF_H

#include <stdint.h>
#include <stdatomic.h>

#include "pogls_exec_window.h"   /* EWRecord (32B), EW_MAX_OPS */

/* ═══════════════════════════════════════════════════════════════════════
   CONFIG  — same as ExecWindow for drop-in compatibility
   ═══════════════════════════════════════════════════════════════════════ */

#define EWLF_SIZE    EW_MAX_OPS          /* 1024 — must be power of 2   */
#define EWLF_MASK    (EWLF_SIZE - 1)

_Static_assert((EWLF_SIZE & (EWLF_SIZE-1)) == 0,
    "EWLF_SIZE must be power of 2");

/* ═══════════════════════════════════════════════════════════════════════
   STRUCT  — head/tail on separate cachelines
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    EWRecord         buffer[EWLF_SIZE]; /* 1024 × 32B = 32KB            */

    /* producer side — written by compute/Hydra threads */
    _Atomic uint32_t head;
    uint8_t          _pad_h[60];        /* pad to cacheline             */

    /* consumer side — written by commit worker */
    _Atomic uint32_t tail;
    uint8_t          _pad_t[60];

    /* stats */
    _Atomic uint64_t total_pushed;
    _Atomic uint64_t total_flushed;

} ExecWindowLF;

_Static_assert(sizeof(ExecWindowLF) <= 40960,
    "ExecWindowLF must be < 40KB");

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: INIT
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ewlf_init(ExecWindowLF *ew)
{
    atomic_store(&ew->head,          0);
    atomic_store(&ew->tail,          0);
    atomic_store(&ew->total_pushed,  0);
    atomic_store(&ew->total_flushed, 0);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: PUSH  (producer — MPSC safe via CAS)
   returns 0=ok, -1=full (caller must wait or drop)
   ═══════════════════════════════════════════════════════════════════════ */

static inline int ewlf_push(ExecWindowLF *ew, const EWRecord *rec)
{
    uint32_t head, next;
    do {
        head = atomic_load_explicit(&ew->head, memory_order_relaxed);
        next = (head + 1) & EWLF_MASK;
        if (next == atomic_load_explicit(&ew->tail, memory_order_acquire))
            return -1;  /* full */
    } while (!atomic_compare_exchange_weak_explicit(
                 &ew->head, &head, next,
                 memory_order_release, memory_order_relaxed));

    ew->buffer[head] = *rec;
    atomic_fetch_add_explicit(&ew->total_pushed, 1, memory_order_relaxed);
    return 0;
}

/*
 * ewlf_push_write — compose + push in one call (hot path)
 */
static inline int ewlf_push_write(ExecWindowLF *ew,
                                   uint16_t op,  uint32_t frame_id,
                                   uint64_t addr, uint64_t value)
{
    EWRecord r;
    r.op       = op;
    r.lane     = (uint16_t)(frame_id % 4);
    r.frame_id = frame_id;
    r.addr     = addr;
    r.value    = value;
    r.crc32    = 0;
    r._pad     = 0;
    return ewlf_push(ew, &r);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: POP (single consumer — commit worker)
   ═══════════════════════════════════════════════════════════════════════ */

static inline int ewlf_pop(ExecWindowLF *ew, EWRecord *out)
{
    uint32_t tail = atomic_load_explicit(&ew->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&ew->head, memory_order_acquire))
        return -1;

    *out = ew->buffer[tail];
    atomic_store_explicit(&ew->tail, (tail+1) & EWLF_MASK,
                           memory_order_release);
    return 0;
}

/*
 * ewlf_pop_batch — drain up to max records → dst
 * commit worker calls this, then writes dst[] to mmap WAL
 */
static inline uint32_t ewlf_pop_batch(ExecWindowLF *ew,
                                        EWRecord     *dst,
                                        uint32_t      max)
{
    uint32_t tail  = atomic_load_explicit(&ew->tail, memory_order_relaxed);
    uint32_t head  = atomic_load_explicit(&ew->head, memory_order_acquire);
    uint32_t avail = (head - tail) & EWLF_MASK;
    uint32_t n     = (avail < max) ? avail : max;

    for (uint32_t i = 0; i < n; i++)
        dst[i] = ew->buffer[(tail + i) & EWLF_MASK];

    atomic_store_explicit(&ew->tail, (tail + n) & EWLF_MASK,
                           memory_order_release);
    atomic_fetch_add_explicit(&ew->total_flushed, n, memory_order_relaxed);
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: QUERY + FLUSH CHECK
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t ewlf_size(const ExecWindowLF *ew)
{
    uint32_t h = atomic_load_explicit(&ew->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&ew->tail, memory_order_relaxed);
    return (h - t) & EWLF_MASK;
}

/* commit worker: should flush? */
static inline int ewlf_should_flush(const ExecWindowLF *ew)
{
    return ewlf_size(ew) >= EW_MAX_OPS / 2;  /* flush at half-full     */
}

/* ═══════════════════════════════════════════════════════════════════════
   COMMIT WORKER INTEGRATION
   ═══════════════════════════════════════════════════════════════════════ */

#include "pogls_wal_mmap.h"   /* WALMmap, wal_alloc_record */

/*
 * ewlf_flush_to_wal — pop batch → write to mmap WAL → signal msync
 * commit worker ใช้ function นี้ใน loop หลัก
 *
 * returns records written (0 = nothing to flush)
 */
static inline uint32_t ewlf_flush_to_wal(ExecWindowLF *ew, WALMmap *wm)
{
    EWRecord batch[EW_MAX_OPS];
    uint32_t n = ewlf_pop_batch(ew, batch, EW_MAX_OPS);
    if (n == 0) return 0;

    for (uint32_t i = 0; i < n; i++) {
        WSRecord *r = wal_alloc_record(wm);   /* zero-copy into mmap   */
        if (!r) break;
        /* map EWRecord → WSRecord (same binary layout, direct cast) */
        r->op       = batch[i].op;
        r->lane     = batch[i].lane;
        r->frame_id = batch[i].frame_id;
        r->addr     = batch[i].addr;
        r->value    = batch[i].value;
        r->crc32    = batch[i].crc32;
        r->_pad     = 0;
    }

    wm_signal_flush(wm);   /* wake async flusher thread */
    return n;
}

#endif /* POGLS_EXEC_WINDOW_LF_H */
