/*
 * pogls_compute_stack.c — POGLS V3.5 Compute Layer Implementations
 *
 * Implements:
 *   - rubik_init / rubik_selftest
 *   - bb_graph_build
 *   - ew_init / ew_flush / ew_tick / ew_close / ew_stats
 */

#ifdef _WIN32
#  include "pogls_win32_compat.h"
#else
#  define _POSIX_C_SOURCE 200112L
#endif

#include "pogls_rubik.h"
#include "pogls_bitboard.h"
#include "pogls_exec_window.h"
#include "pogls_frontier.h"
#include "pogls_wal_mmap.h"      /* pulls in wal_stripe.h → WSRecord/EWRecord visible */
#include "pogls_compute_lut.h"   /* ComputeLUT, g_clut, clut_init()                  */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* cross-header compile-time guards — both structs visible here */
_Static_assert(sizeof(EWRecord) == sizeof(WSRecord),
    "EWRecord and WSRecord must be same size for ws_replay compatibility");
_Static_assert(sizeof(EWRecord) == 32,
    "EWRecord must be 32B — page alignment broken");

/* ═══════════════════════════════════════════════════════════════════════
   RUBIK ENGINE
   ═══════════════════════════════════════════════════════════════════════ */

/* global LUT tables */
uint8_t rubik_perm_lut[RUBIK_MOVES][RUBIK_COMPACT_STATES];
uint8_t rubik_perm_inv[RUBIK_MOVES][RUBIK_COMPACT_STATES];

/*
 * rubik_init — build permutation tables
 *
 * 18 moves × 256 states — computed from cyclic rotation patterns
 * Each move = cyclic permutation of 4 corner positions
 * State = 8-bit index representing corner arrangement
 *
 * Pattern per move (4-cycle on selected bits):
 *   U  : bits 0,1,2,3 rotate  (b0←b3, b1←b0, b2←b1, b3←b2)
 *   D  : bits 4,5,6,7 rotate
 *   F  : bits 0,2,5,7 rotate
 *   B  : bits 1,3,4,6 rotate
 *   R  : bits 0,1,4,5 rotate
 *   L  : bits 2,3,6,7 rotate
 *   *' = inverse (reverse rotate)
 *   *2 = double (rotate twice)
 */

/* 4-cycle rotate: (a←d, b←a, c←b, d←c) on 8-bit state */
static uint8_t cycle4(uint8_t s, int a, int b, int c, int d)
{
    uint8_t ba = (s >> a) & 1;
    uint8_t bb = (s >> b) & 1;
    uint8_t bc = (s >> c) & 1;
    uint8_t bd = (s >> d) & 1;

    s &= ~((1<<a)|(1<<b)|(1<<c)|(1<<d));
    s |= (bd << a) | (ba << b) | (bb << c) | (bc << d);
    return s;
}

static uint8_t cycle4_inv(uint8_t s, int a, int b, int c, int d)
{
    return cycle4(s, d, c, b, a);  /* reverse direction */
}

/* move definitions: each = {bit positions of the 4-cycle} */
static const int move_bits[6][4] = {
    {0,1,2,3},  /* U: top face corners */
    {4,5,6,7},  /* D: bottom face corners */
    {0,2,5,7},  /* F: front face corners */
    {1,3,4,6},  /* B: back face corners */
    {0,1,4,5},  /* R: right face corners */
    {2,3,6,7},  /* L: left face corners */
};

void rubik_init(void)
{
    for (int face = 0; face < 6; face++) {
        int move    = face * 3;       /* CW */
        int move_p  = face * 3 + 1;  /* CCW */
        int move_2  = face * 3 + 2;  /* 180 */

        int a = move_bits[face][0], b = move_bits[face][1];
        int c = move_bits[face][2], d = move_bits[face][3];

        for (int s = 0; s < RUBIK_COMPACT_STATES; s++) {
            uint8_t cw  = cycle4(    (uint8_t)s, a,b,c,d);
            uint8_t ccw = cycle4_inv((uint8_t)s, a,b,c,d);
            uint8_t dbl = cycle4(cw,              a,b,c,d);

            rubik_perm_lut[move  ][s] = cw;
            rubik_perm_lut[move_p][s] = ccw;
            rubik_perm_lut[move_2][s] = dbl;

            rubik_perm_inv[move  ][s] = ccw;
            rubik_perm_inv[move_p][s] = cw;
            rubik_perm_inv[move_2][s] = dbl;
        }
    }
}

int rubik_selftest(void)
{
    rubik_init();

    /* identity check: 4×CW = identity for each face */
    for (int face = 0; face < 6; face++) {
        for (int s = 0; s < RUBIK_COMPACT_STATES; s++) {
            uint8_t st = (uint8_t)s;
            int move = face * 3;
            st = rubik_perm(st, (uint8_t)move);
            st = rubik_perm(st, (uint8_t)move);
            st = rubik_perm(st, (uint8_t)move);
            st = rubik_perm(st, (uint8_t)move);
            if (st != (uint8_t)s) return -1;  /* 4×CW != identity */
        }
    }

    /* inverse check: CW then CCW = identity */
    for (int face = 0; face < 6; face++) {
        for (int s = 0; s < RUBIK_COMPACT_STATES; s++) {
            uint8_t st  = (uint8_t)s;
            uint8_t cw  = rubik_perm(st, (uint8_t)(face*3));
            uint8_t back = rubik_inv(cw, (uint8_t)(face*3));
            if (back != st) return -2;  /* inv(perm) != identity */
        }
    }

    return 0;  /* PASS */
}

/* ═══════════════════════════════════════════════════════════════════════
   FRONTIER — global adjacency map  (team-B pattern: extern in frontier.h)
   ═══════════════════════════════════════════════════════════════════════ */

NodeMask node_adj_map[NODE_MAX];

void frontier_build_adj_map(const uint16_t *adj_table)
{
    for (int n = 0; n < NODE_MAX; n++) {
        NodeMask m   = nodemask_zero();
        uint16_t off = adjacency_offset[n];
        uint8_t  cnt = adjacency_count[n];
        for (uint8_t k = 0; k < cnt; k++) {
            uint16_t nb = adj_table[off + k];
            if (nb < NODE_MAX)
                nodemask_set(&m, nb);
        }
        node_adj_map[n] = m;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   BITBOARD: BB GRAPH BUILD
   ═══════════════════════════════════════════════════════════════════════ */

/* global adjacency map — shared across all workers */
NodeMask node_adj_map[NODE_MAX];

void bb_graph_build(BBGraph        *graph,
                    const uint16_t *adj_table,
                    const uint16_t *adj_offset,
                    const uint8_t  *adj_count)
{
    for (int n = 0; n < NODE_MAX; n++) {
        NodeMask m = nodemask_zero();
        uint16_t off = adj_offset[n];
        uint8_t  cnt = adj_count[n];
        for (uint8_t k = 0; k < cnt; k++) {
            uint16_t nb = adj_table[off + k];
            if (nb < NODE_MAX)
                nodemask_set(&m, nb);
        }
        graph->adj[n] = m;
    }
}


/* ═══════════════════════════════════════════════════════════════════════
   EXECUTION WINDOW
   ═══════════════════════════════════════════════════════════════════════ */

void ew_init(ExecWindow *w, uint32_t max_ops,
             uint64_t max_ns, int wal_fd)
{
    memset(w, 0, sizeof(*w));
    w->max_ops        = (max_ops > 0) ? max_ops : EW_MAX_OPS;
    w->max_window_ns  = (max_ns  > 0) ? max_ns  : EW_MAX_WINDOW_NS;
    w->wal_fd         = wal_fd;
    w->time_start_ns  = ew_now_ns();
    atomic_store(&w->flush_pending, 0);
}

void ew_flush(ExecWindow *w)
{
    if (w->pos == 0) goto reset;

    if (w->wal_fd >= 0) {
        ssize_t written = write(w->wal_fd, w->buf, w->pos);
        (void)written;
        fdatasync(w->wal_fd);
    }

    w->flush_count++;

reset:
    w->pos           = 0;
    w->op_count      = 0;
    w->time_start_ns = ew_now_ns();
    atomic_store(&w->flush_pending, 0);
}

int ew_tick(ExecWindow *w)
{
    if (!ew_should_flush(w)) return 0;
    ew_flush(w);
    return 1;
}

void ew_close(ExecWindow *w)
{
    ew_flush(w);   /* drain remaining */
}

void ew_stats(const ExecWindow *w)
{
    uint64_t ops    = w->total_ops;
    uint64_t flushes = w->flush_count;
    double   avg_batch = flushes > 0 ? (double)ops / flushes : 0;

    printf("ExecWindow stats:\n");
    printf("  total_ops    = %llu\n",  (unsigned long long)ops);
    printf("  total_flushes= %llu\n",  (unsigned long long)flushes);
    printf("  avg batch    = %.1f ops/flush\n", avg_batch);
    printf("  throughput   ≈ %.0f ops/s (assume 5ms/fsync)\n",
           avg_batch / 0.005);
}

/* ═══════════════════════════════════════════════════════════════════════
   pogls_compute_stack_init — single entry point for startup
 *
 * Call once at process startup BEFORE any compute/WAL operations.
 *
 * Initialises:
 *   1. rubik_init()             — build 18-move permutation tables
 *   2. clut_init(&g_clut)       — build L1-packed ComputeLUT
 *   3. frontier_build_adj_map() — build node_adj_map[NODE_MAX]
 *
 * Returns 0 on success, -1 if any selftest fails.
 * ═══════════════════════════════════════════════════════════════════════ */

int pogls_compute_stack_init(void)
{
    /* 1. Rubik perm/inv tables */
    rubik_init();
    if (rubik_selftest() != 0) return -1;

    /* 2. L1-packed ComputeLUT (Rubik + Morton + NodeLUT) */
    clut_init(&g_clut);
    if (clut_selftest(&g_clut) != 0) return -2;

    /* 3. Frontier adjacency map:
     *    Caller is responsible for calling:
     *        topo_build_adjacency(topo_adjacency_table);
     *        frontier_build_adj_map(topo_adjacency_table);
     *    before using frontier functions.
     *    This init only covers compute LUT + rubik (no topology dependency). */

    return 0;   /* PASS */
}

/*
 * pogls_compute_stack_init_full — extended init with custom adj table
 *
 * adj_table  : uint16_t[NODE_MAX × K] adjacency pairs (NULL = default)
 * node_map   : uint8_t[256] addr[19:12]→node_id override (NULL = default)
 */
int pogls_compute_stack_init_full(const uint16_t *adj_table,
                                   const uint8_t  *node_map)
{
    rubik_init();
    if (rubik_selftest() != 0) return -1;

    clut_init(&g_clut);
    if (node_map)
        clut_node_build(&g_clut, node_map, 256);
    if (clut_selftest(&g_clut) != 0) return -2;

    frontier_build_adj_map(adj_table);
    return 0;
}
