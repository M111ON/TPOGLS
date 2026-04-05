/*
 * pogls_exec_window.h — POGLS V3.5 Execution Window
 *
 * Deterministic Execution Window: แยก compute world (ns) จาก commit world (ms)
 *
 *   write → append memory log   (ns — no fsync)
 *   write → append memory log
 *   ... (batch 256 ops หรือ 50µs)
 *   window full/timeout → flush WAL ทีเดียว (ms — 1 fsync)
 *
 * Throughput:
 *   1 fsync/write  → ~50-200 ops/s  (disk bound)
 *   batch 256 ops  → ~47,000+ ops/s (~250x)
 *
 * Crash safety:
 *   WAL replay → reconstruct ถึง window boundary
 *   DetachFrame ที่ยัง active → rebuild จาก WAL
 *
 * Namespace: ew_* / ExecWindow
 * ไม่แตะ WAL_Context / ws_* / detach_* โดยตรง
 */

#ifndef POGLS_EXEC_WINDOW_H
#define POGLS_EXEC_WINDOW_H

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200112L
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define EW_MAX_OPS          256          /* count trigger               */
#define EW_MAX_WINDOW_NS    50000ULL     /* time trigger = 50µs         */
#define EW_RECORD_SIZE      32           /* WAL record size in bytes    */
#define EW_BUFFER_BYTES     (EW_MAX_OPS * EW_RECORD_SIZE)  /* 8KB      */
#define WAL_BATCH_BYTES     EW_BUFFER_BYTES   /* alias — team-B compat  */

/* ═══════════════════════════════════════════════════════════════════════
   EW RECORD  (matches WSRecord layout — compatible with ws_replay)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint16_t  op;           /* operation type (ws_op_t)                 */
    uint16_t  lane;         /* stripe lane                              */
    uint32_t  frame_id;     /* DetachFrame (0 if unused)               */
    uint64_t  addr;         /* target address                          */
    uint64_t  value;        /* payload                                 */
    uint32_t  crc32;        /* CRC32C of first 28B                     */
} EWRecord;
/* 2+2+4+8+8+4 = 28B packed */

/* ═══════════════════════════════════════════════════════════════════════
   EXECUTION WINDOW STATE
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    /* in-memory ring buffer */
    uint8_t   buf[EW_BUFFER_BYTES];
    uint32_t  pos;          /* bytes written so far                     */
    uint64_t  op_count;     /* ops accumulated this window             */

    /* timing */
    uint64_t  time_start_ns;
    uint64_t  max_window_ns;
    uint32_t  max_ops;

    /* durability */
    int       wal_fd;       /* fd to flush into                        */
    uint64_t  flush_count;  /* total flushes (stats)                   */
    uint64_t  total_ops;    /* total ops processed (stats)             */

    /* flush signal for io thread */
    _Atomic uint32_t flush_pending;

} ExecWindow;

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: TIMESTAMP  (ns monotonic)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint64_t ew_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: WINDOW CHECK  (should flush?)
   ═══════════════════════════════════════════════════════════════════════ */

static inline int ew_should_flush(const ExecWindow *w)
{
    if (w->op_count >= w->max_ops) return 1;
    if ((ew_now_ns() - w->time_start_ns) >= w->max_window_ns) return 1;
    return 0;
}

/* forward declaration — defined in pogls_exec_window.c (needed by ew_write inline) */
void ew_flush(ExecWindow *w);

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: APPEND RECORD  (hot path — no fsync, ~30ns)
   ═══════════════════════════════════════════════════════════════════════ */

static inline void ew_append(ExecWindow *w, const EWRecord *rec)
{
    if (w->pos + sizeof(EWRecord) > EW_BUFFER_BYTES)
        return;   /* overflow guard — flush should have happened */

    memcpy(w->buf + w->pos, rec, sizeof(EWRecord));
    w->pos      += sizeof(EWRecord);
    w->op_count++;
    w->total_ops++;
}

/*
 * ew_write — compose + append EWRecord in one call
 * hot path for compute layer — return immediately, no disk IO
 */
static inline void ew_write(ExecWindow *w,
                             uint16_t    op,
                             uint32_t    frame_id,
                             uint64_t    addr,
                             uint64_t    value)
{
    /* flush-before-append (team-B pattern) — ไม่ drop record */
    if (w->pos + sizeof(EWRecord) > EW_BUFFER_BYTES)
        ew_flush(w);

    EWRecord *r = (EWRecord *)(w->buf + w->pos);
    r->op       = op;
    r->lane     = (uint16_t)(frame_id % 4);
    r->frame_id = frame_id;
    r->addr     = addr;
    r->value    = value;
    r->crc32    = 0;

    w->pos      += sizeof(EWRecord);
    w->op_count++;
    w->total_ops++;
}

/* sign last appended record's crc32 field (optional — call after ew_write) */
static inline void ew_sign_last(ExecWindow *w)
{
    if (w->pos < sizeof(EWRecord)) return;
    EWRecord *r = (EWRecord *)(w->buf + w->pos - sizeof(EWRecord));
    /* FNV-1a fast — caller can swap for CRC32C if needed */
    uint32_t h = 0x811C9DC5u;
    const uint8_t *p = (const uint8_t *)r;
    for (uint32_t i = 0; i < sizeof(EWRecord) - 4; i++)
        h = (h ^ p[i]) * 0x01000193u;
    r->crc32 = h;
}

/* ═══════════════════════════════════════════════════════════════════════
   API (implemented in pogls_exec_window.c)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * ew_init — initialize window
 * max_ops      : count trigger  (recommend 256)
 * max_ns       : time trigger   (recommend 50000 = 50µs)
 * wal_fd       : writable fd (or -1 for io-thread mode)
 */
void ew_init(ExecWindow *w, uint32_t max_ops,
             uint64_t max_ns, int wal_fd);

/*
 * ew_tick — call from compute loop to check and trigger flush
 * returns 1 if flush happened
 */
int ew_tick(ExecWindow *w);

/*
 * ew_close — force flush remaining records (shutdown path)
 */
void ew_close(ExecWindow *w);

/*
 * ew_stats — print throughput stats to stdout
 */
void ew_stats(const ExecWindow *w);

/* ═══════════════════════════════════════════════════════════════════════
   INTEGRATION HELPERS
   ═══════════════════════════════════════════════════════════════════════ */

/* write pipeline: compute + append to window (no WAL direct) */
static inline void ew_node_write(ExecWindow *w,
                                  uint64_t    addr,
                                  uint64_t    value)
{
    ew_write(w, 1 /*WS_OP_WRITE*/, 0, addr, value);
}

static inline void ew_detach_event(ExecWindow *w,
                                    uint32_t    frame_id,
                                    uint64_t    gate_addr)
{
    ew_write(w, 2 /*WS_OP_DETACH*/, frame_id, gate_addr, 0);
}

static inline void ew_ntacle_event(ExecWindow *w,
                                    uint32_t    frame_id,
                                    uint64_t    node_mask)
{
    ew_write(w, 5 /*WS_OP_EVENT*/, frame_id, 0, node_mask);
}

/* ═══════════════════════════════════════════════════════════════════════
   TEAM-B COMPATIBLE API
   pogls_window_append / pogls_window_flush / get_time_ns
   WAL_BATCH_BYTES — maps to EW_BUFFER_BYTES
   ═══════════════════════════════════════════════════════════════════════ */

#define WAL_BATCH_BYTES EW_BUFFER_BYTES   /* alias สำหรับ team-B code    */

/* get_time_ns — compat symbol ที่ team_exec_window.c ต้องการ */
static inline uint64_t get_time_ns(void) { return ew_now_ns(); }

/*
 * pogls_window_append — team-B style generic append
 * flush-before-append ถ้า buffer เต็ม (ไม่ drop)
 */
static inline void pogls_window_append(ExecWindow *w,
                                        void       *record,
                                        size_t      size)
{
    if (w->pos + size > EW_BUFFER_BYTES)
        ew_flush(w);

    if (size <= EW_BUFFER_BYTES) {
        memcpy(w->buf + w->pos, record, size);
        w->pos      += (uint32_t)size;
        w->op_count++;
        w->total_ops++;
        if (w->op_count >= w->max_ops)
            ew_flush(w);
    }
}

/* pogls_window_flush — team-B style alias */
static inline void pogls_window_flush(ExecWindow *w) { ew_flush(w); }

#endif /* POGLS_EXEC_WINDOW_H */
