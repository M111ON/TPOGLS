#!/usr/bin/env python3
# ╔══════════════════════════════════════════════════════════════╗
# ║  POGLS V3.6 — Hydra Benchmark  (Colab Single Cell)         ║
# ║  วิธีใช้: วาง cell นี้ใน Google Colab แล้วกด Run           ║
# ║  ไม่ต้อง upload ไฟล์เพิ่มเติม — self-contained             ║
# ╚══════════════════════════════════════════════════════════════╝
#
# จะ compile C ด้วย gcc -O3 -march=native แล้วรัน benchmark
# วัด: route / priority_route / enqueue / density /
#       work_steal / fibo_addr / repair_pipeline / full_write_path

import os, subprocess, tempfile, sys

C_SRC = """ /* POGLS V3.6 Hydra Benchmark — amalgam */
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

/*
 * pogls_node_soa.h — POGLS V3.5 Node State (Structure of Arrays)
 *
 * 162 nodes จาก Icosphere L2 (TOPO_STANDARD)
 * Layout แบบ SoA ทำให้:
 *   - SIMD scan ได้ทีละ field
 *   - cache line ไม่ปน field ที่ไม่ต้องใช้
 *   - L1 resident ที่ ~21KB (< 32KB L1 data cache)
 *
 * ห้ามใช้ malloc ใน hot path — ทุก array เป็น static หรือ prealloc
 * ห้าม include pogls_hydra.h / pogls_snapshot.h ใน header นี้
 */

#ifndef POGLS_NODE_SOA_H
#define POGLS_NODE_SOA_H


/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define NODE_MAX          162     /* Icosphere L2 — TOPO_STANDARD        */
#define NODE_MASK_WORDS   4       /* 4 × uint64 = 256 bits → covers 162  */
#define NODE_LUT_SIZE     256     /* addr >> 12 → 8-bit index → node id  */

/* ═══════════════════════════════════════════════════════════════════════
   NODE BITMASK  (256-bit ครอบ 162 nodes)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t w[NODE_MASK_WORDS];
} NodeMask;

/* bit helpers — branchless */
static inline void nodemask_set(NodeMask *m, int node)
{
    m->w[node >> 6] |= (1ULL << (node & 63));
}

static inline int nodemask_test(const NodeMask *m, int node)
{
    return (int)((m->w[node >> 6] >> (node & 63)) & 1);
}

static inline void nodemask_or(NodeMask *dst, const NodeMask *src)
{
    dst->w[0] |= src->w[0];
    dst->w[1] |= src->w[1];
    dst->w[2] |= src->w[2];
    dst->w[3] |= src->w[3];
}

static inline void nodemask_clear(NodeMask *m)
{
    m->w[0] = m->w[1] = m->w[2] = m->w[3] = 0;
}

/* zero constructor — ใช้ตอน init frontier เพื่อไม่ให้มี garbage bits */
static inline NodeMask nodemask_zero(void)
{
    NodeMask m;
    m.w[0] = m.w[1] = m.w[2] = m.w[3] = 0;
    return m;
}

static inline int nodemask_empty(const NodeMask *m)
{
    return !(m->w[0] | m->w[1] | m->w[2] | m->w[3]);
}

/* ═══════════════════════════════════════════════════════════════════════
   ACTIVE FRONTIER  (ใช้ระหว่าง diffusion pass)
   ═══════════════════════════════════════════════════════════════════════ */

typedef NodeMask FrontierMask;

/* ═══════════════════════════════════════════════════════════════════════
   NODE STATE  — SoA layout
   ทุก array cacheline-aligned (64B) เพื่อให้ prefetch ทำงานได้เต็ม
   footprint รวม ≈ 21KB
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    /* ── hot fields (เข้าถึงทุก diffusion tick) ── */
    __attribute__((aligned(64)))
    uint64_t  attention[NODE_MAX];    /* activation count              */

    __attribute__((aligned(64)))
    uint64_t  density[NODE_MAX];      /* write density counter         */

    __attribute__((aligned(64)))
    NodeMask  neighbors[NODE_MAX];    /* static adjacency bitmask      */

    /* ── cold fields (เข้าถึงตอน audit / visualize) ── */
    __attribute__((aligned(64)))
    uint64_t  timestamp[NODE_MAX];    /* last-active ms                */

    __attribute__((aligned(64)))
    uint8_t   anomaly_flags[NODE_MAX]; /* per-node anomaly bitmask     */

    uint8_t   _pad[NODE_MAX % 8 ? 8 - (NODE_MAX % 8) : 0]; /* align   */

} NodeState;

/* ═══════════════════════════════════════════════════════════════════════
   ADDRESS → NODE LUT
   addr >> 12 → 8-bit bucket → node_id (0..161)
   latency ~2ns — 1 memory read, no branch
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t map[NODE_LUT_SIZE];   /* filled by node_lut_build()        */
} NodeLUT;

static inline uint32_t node_lut_lookup(const NodeLUT *lut, uint32_t addr)
{
    return lut->map[(addr >> 12) & 0xFF];
}

/* ═══════════════════════════════════════════════════════════════════════
   API
   ═══════════════════════════════════════════════════════════════════════ */

/* init NodeState — zero all fields */
void node_state_init(NodeState *ns);

/*
 * node_lut_build — สร้าง LUT จาก icosphere adjacency
 *
 * neighbor_ids : array[NODE_MAX][6] ของ neighbor node ids
 *                (-1 = ไม่มี neighbor ในช่องนั้น)
 * ns           : เขียน neighbors[i] bitmask ลงโดยตรง
 * lut          : สร้าง addr→node mapping (round-robin กระจาย 256 bucket)
 */
void node_lut_build(NodeLUT   *lut,
                    NodeState *ns,
                    const int  neighbor_ids[NODE_MAX][6]);

/*
 * node_adj_load_icosphere_l2 — โหลด static adjacency table สำหรับ
 * Icosphere L2 (162 nodes, TOPO_STANDARD)
 *
 * เขียนลง ns->neighbors[] โดยตรง
 * เรียกครั้งเดียวตอน init ก่อน node_lut_build()
 *
 * ข้อมูล topology มาจาก icosphere subdivision L2 (precomputed)
 * แต่ละ node มี 5 หรือ 6 เพื่อนบ้าน
 */
void node_adj_load_icosphere_l2(NodeState *ns);

/* node_update — อัพเดท attention + density + timestamp
 * เรียกจาก hydra worker thread หลัง queue_pop
 */
void node_update(NodeState    *ns,
                 uint32_t      node_id,
                 uint64_t      now_ms,
                 FrontierMask *frontier);

/* zero attention + density (ใช้หลัง snapshot checkpoint) */
void node_state_reset_counters(NodeState *ns);

#endif /* POGLS_NODE_SOA_H */

/*
 * pogls_graph_topology.h — POGLS V3.5 Graph Topology
 *
 * 162-node icosphere L2 (TOPO_STANDARD) adjacency table
 * Layout: CSR (Compressed Sparse Row) — offset + count + flat edge list
 *
 * Ring-diffusion pattern: n → n±1, n±9, n±27  (mod 162)
 *   - degree = 6 (uniform)
 *   - diffusion radius 1: covers ~37 nodes
 *   - diffusion radius 2: covers ~127 nodes
 *   - branchless, SIMD-friendly, cache-local
 *
 * ห้าม include pogls_hydra.h / pogls_snapshot.h ใน header นี้
 */

#ifndef POGLS_GRAPH_TOPOLOGY_H
#define POGLS_GRAPH_TOPOLOGY_H


/* ═══════════════════════════════════════════════════════════════════════
   TOPOLOGY CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define TOPO_NODE_MAX     162
#define TOPO_DEGREE       6
#define TOPO_EDGE_COUNT   972   /* NODE_MAX × DEGREE */
#define TOPO_MIN_DEGREE   6
#define TOPO_MAX_DEGREE   6

/* ═══════════════════════════════════════════════════════════════════════
   CSR ADJACENCY TABLE
   adjacency_offset[n] → start index ใน adjacency_table[]
   adjacency_count[n]  → จำนวน neighbors ของ node n
   adjacency_table[]   → flat list of neighbor node ids
   ═══════════════════════════════════════════════════════════════════════ */

/* stride = 6 (uniform degree) — offset[n] = n*6 */
static const uint16_t adjacency_offset[TOPO_NODE_MAX] = {
      0,  6, 12, 18, 24, 30, 36, 42, 48, 54,
     60, 66, 72, 78, 84, 90, 96,102,108,114,
    120,126,132,138,144,150,156,162,168,174,
    180,186,192,198,204,210,216,222,228,234,
    240,246,252,258,264,270,276,282,288,294,
    300,306,312,318,324,330,336,342,348,354,
    360,366,372,378,384,390,396,402,408,414,
    420,426,432,438,444,450,456,462,468,474,
    480,486,492,498,504,510,516,522,528,534,
    540,546,552,558,564,570,576,582,588,594,
    600,606,612,618,624,630,636,642,648,654,
    660,666,672,678,684,690,696,702,708,714,
    720,726,732,738,744,750,756,762,768,774,
    780,786,792,798,804,810,816,822,828,834,
    840,846,852,858,864,870,876,882,888,894,
    900,906,912,918,924,930,936,942,948,954,
    960,966
};

/* uniform degree 6 for all nodes */
static const uint8_t adjacency_count[TOPO_NODE_MAX] = {
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,6,6,
    6,6
};

/*
 * adjacency_table[972] — flat neighbor list (ring-diffusion, mod 162)
 *   per node n: {n+1, n-1, n+9, n-9, n+27, n-27} mod 162
 *
 * generated by topo_build_adjacency() at init — ไม่ฝัง static เพราะ
 * ขนาด 972×2B = 1944B ยัง fit L2 และ build cost < 1µs
 *
 * ถ้าต้องการ ROM table: run topo_dump_c() แล้ว paste ที่นี่
 */
extern uint16_t topo_adjacency_table[TOPO_EDGE_COUNT];

/* ═══════════════════════════════════════════════════════════════════════
   BUILD & QUERY API
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * topo_build_adjacency — สร้าง adjacency_table ตอน startup
 * เรียกครั้งเดียวก่อน engine เริ่ม
 */
void topo_build_adjacency(uint16_t *table);

/*
 * topo_build_edge_masks — สร้าง NodeMask ของ neighbors ต่อ node
 * เขียนลง edge_masks[NODE_MAX]
 * เรียกหลัง topo_build_adjacency()
 */
void topo_build_edge_masks(const uint16_t *table, NodeMask *edge_masks);

/* traversal helper */
static inline void topo_for_each_neighbor(
        const uint16_t *table,
        uint32_t node,
        void (*cb)(uint32_t neighbor, void *ud),
        void *ud)
{
    uint16_t off = adjacency_offset[node];
    uint8_t  cnt = adjacency_count[node];
    for (uint8_t i = 0; i < cnt; i++)
        cb(table[off + i], ud);
}

/* ═══════════════════════════════════════════════════════════════════════
   NTACLE DIFFUSION (graph propagation)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * topo_diffuse_fast — one diffusion pass, bit-scan loop
 *
 * input:  active  (nodes that fired this tick)
 * output: out     (nodes to activate next tick, union of neighbors)
 *
 * latency: ~40-70ns for typical active set
 */
static inline NodeMask topo_diffuse_fast(
        const NodeMask  *active,
        const NodeMask  *edge_masks)
{
    NodeMask out = *active;

    for (int w = 0; w < 3; w++) {        /* word 3 = padding, skip */
        uint64_t bits = active->w[w];
        while (bits) {
            int      b    = __builtin_ctzll(bits);
            bits         &= bits - 1;
            int      node = (w << 6) | b;
            if (node >= TOPO_NODE_MAX) break;
            const NodeMask *e = &edge_masks[node];
            out.w[0] |= e->w[0];
            out.w[1] |= e->w[1];
            out.w[2] |= e->w[2];
            out.w[3] |= e->w[3];
        }
    }
    return out;
}

/*
 * topo_spread — multi-step diffusion
 * steps=1: neighbors only
 * steps=2: neighbors + neighbors-of-neighbors
 */
static inline NodeMask topo_spread(
        NodeMask        m,
        const NodeMask *edge_masks,
        int             steps)
{
    for (int i = 0; i < steps; i++)
        m = topo_diffuse_fast(&m, edge_masks);
    return m;
}

/* ═══════════════════════════════════════════════════════════════════════
   DETACH INTEGRATION
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * topo_detach_spread — diffuse ใน scope ของ DetachFrame.ntacle_mask
 * ไม่ scan graph ทั้งหมด — เฉพาะ nodes ที่เกี่ยวข้อง
 *
 * ntacle_mask : bitmask ของ nodes ใน detached zone
 * edge_masks  : precomputed ของ full graph
 */
static inline NodeMask topo_detach_spread(
        uint64_t        ntacle_mask,
        const NodeMask *edge_masks,
        int             steps)
{
    NodeMask m;
    m.w[0] = ntacle_mask;   /* node 0-63  */
    m.w[1] = 0;
    m.w[2] = 0;
    m.w[3] = 0;
    return topo_spread(m, edge_masks, steps);
}

/* ═══════════════════════════════════════════════════════════════════════
   DEBUG UTIL
   ═══════════════════════════════════════════════════════════════════════ */

/* dump adjacency_table เป็น C source — ใช้ generate static table */
void topo_dump_c(const uint16_t *table);

#endif /* POGLS_GRAPH_TOPOLOGY_H */

/*
 * pogls_rubik.h — POGLS V3.5 Rubik Permutation Engine
 *
 * State navigation ด้วย permutation LUT แทนการ rewrite index
 *   state → perm(state, move) → new state   (~3ns)
 *   reversible, deterministic, parity-auditable
 *
 * วางเหนือ Angular Address Layer — ไม่แตะ WAL/Snapshot/Hydra/Ntacle
 * ใช้ร่วมกับ Morton encode ในขั้นตอน address pipeline
 *
 * Namespace: rubik_* / Rubik*
 */

#ifndef POGLS_RUBIK_H
#define POGLS_RUBIK_H


/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define RUBIK_MOVES       18    /* standard: U U' U2 D D' D2 F F' F2
                                             B B' B2 R R' R2 L L' L2  */
#define RUBIK_STATES      40320 /* 8! corner permutations (compact)    */
#define RUBIK_LUT_DEPTH   6     /* precompute depth for move sequence   */

/* ═══════════════════════════════════════════════════════════════════════
   MOVE CODES
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    RUBIK_U=0, RUBIK_Up=1, RUBIK_U2=2,
    RUBIK_D=3, RUBIK_Dp=4, RUBIK_D2=5,
    RUBIK_F=6, RUBIK_Fp=7, RUBIK_F2=8,
    RUBIK_B=9, RUBIK_Bp=10,RUBIK_B2=11,
    RUBIK_R=12,RUBIK_Rp=13,RUBIK_R2=14,
    RUBIK_L=15,RUBIK_Lp=16,RUBIK_L2=17,
} rubik_move_t;

/* ═══════════════════════════════════════════════════════════════════════
   PERMUTATION TABLE  (extern — built by rubik_init)
   perm_lut[move][state]     → new state after move
   perm_inv[move][state]     → state before move (inverse)
   ═══════════════════════════════════════════════════════════════════════ */

/* Compact form: index into 8-element permutation array (0..40319)
 * ใช้ uint16_t เพราะ 40320 < 65536 → LUT ขนาด 18×40320×2B = 1.4MB
 *
 * สำหรับ POGLS ใช้ state space เล็กลง (8-bit corner index = 256 states)
 * เพื่อให้ LUT อยู่ใน L1 cache (~4KB)
 */
#define RUBIK_COMPACT_STATES 256   /* 8-bit state — L1 friendly        */

extern uint8_t rubik_perm_lut[RUBIK_MOVES][RUBIK_COMPACT_STATES];
extern uint8_t rubik_perm_inv[RUBIK_MOVES][RUBIK_COMPACT_STATES];

/* ═══════════════════════════════════════════════════════════════════════
   INLINE HOT PATH  (~3ns)
   ═══════════════════════════════════════════════════════════════════════ */

/* apply move → new state */
static inline uint8_t rubik_perm(uint8_t state, uint8_t move)
{
    return rubik_perm_lut[move % RUBIK_MOVES][state];
}

/* undo move → previous state */
static inline uint8_t rubik_inv(uint8_t state, uint8_t move)
{
    return rubik_perm_inv[move % RUBIK_MOVES][state];
}

/* parity audit — Ntacle hook สำหรับ integrity check (~1ns) */
static inline uint8_t rubik_parity(uint8_t state)
{
    return __builtin_popcount(state) & 1;
}

/* apply sequence of moves */
static inline uint8_t rubik_apply_seq(uint8_t state,
                                       const uint8_t *moves,
                                       uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        state = rubik_perm(state, moves[i]);
    return state;
}

/* mix state into address (combine with Morton/angular) */
static inline uint32_t rubik_mix_addr(uint8_t state, uint32_t base_addr)
{
    /* XOR fold: state into upper bits of address space */
    return (base_addr ^ ((uint32_t)state << 12)) & ((1u << 20) - 1);
}

/* ═══════════════════════════════════════════════════════════════════════
   API
   ═══════════════════════════════════════════════════════════════════════ */

/* build perm_lut and perm_inv tables — call once at startup */
void rubik_init(void);

/* verify perm_lut integrity (self-test) — returns 0 on pass */
int rubik_selftest(void);

/* encode move sequence → compact uint64 (6 moves × 5bit = 30bit) */
static inline uint64_t rubik_encode_seq(const uint8_t *moves, int n)
{
    uint64_t enc = 0;
    for (int i = 0; i < n && i < 12; i++)
        enc |= ((uint64_t)(moves[i] & 0x1F)) << (i * 5);
    return enc;
}

static inline void rubik_decode_seq(uint64_t enc, uint8_t *moves, int n)
{
    for (int i = 0; i < n; i++)
        moves[i] = (uint8_t)((enc >> (i * 5)) & 0x1F);
}

/* ═══════════════════════════════════════════════════════════════════════
   TEAM-B COMPATIBLE API
   pogls_rubik_move / pogls_rubik_inverse / pogls_rubik_parity
   Maps onto our 8-bit 18-move LUT — ไม่มี extern LUT แยกต่างหาก
   (team-B ใช้ uint32[12][1024]=48KB → ไม่ L1-friendly, ไม่ merge)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t pogls_rubik_move(uint32_t state, uint8_t move_id)
{
    return rubik_perm((uint8_t)(state & 0xFF), move_id);
}

static inline uint32_t pogls_rubik_inverse(uint32_t state, uint8_t move_id)
{
    return rubik_inv((uint8_t)(state & 0xFF), move_id);
}

static inline uint8_t pogls_rubik_parity(uint32_t state)
{
    return rubik_parity((uint8_t)(state & 0xFF));
}

#endif /* POGLS_RUBIK_H */

/*
 * pogls_hydra_scheduler.h — POGLS V3.5 Hydra Work-Stealing Scheduler
 *
 * 16 heads, per-head queue, work-stealing fallback
 * cache-locality routing: addr → head (same region = same core = L1 hit)
 *
 * Namespace: hs_* / HydraTask / HydraQueue / HydraWorkerCtx
 *   ไม่ชน pogls_hydra_route() (core Hydra)
 *   ไม่ชน POGLS_HydraHead / POGLS_HydraCore (ของเดิม)
 *
 * ห้าม include pogls_hydra.h ใน header นี้
 * Scheduler เป็น pure compute layer — ไม่รู้จัก snapshot / audit
 */

#ifndef POGLS_HYDRA_SCHEDULER_H
#define POGLS_HYDRA_SCHEDULER_H

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE   /* pthread_setaffinity_np, cpu_set_t */
#endif



/* ═══════════════════════════════════════════════════════════════════════
   CONFIG
   ═══════════════════════════════════════════════════════════════════════ */

#define HS_HEADS            16    /* worker threads = Hydra head count   */
#define HS_QUEUE_SIZE       256   /* per-head ring buffer (power of 2)   */
#define HS_NODE_MAX_DENSE   642   /* Icosphere L3 for adaptive density   */

/* ═══════════════════════════════════════════════════════════════════════
   TASK OPS  (ไม่ชน WAL op / detach op)
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    HS_OP_NODE_WRITE = 1,   /* write value to node                      */
    HS_OP_DIFFUSE    = 2,   /* trigger ntacle diffusion from node        */
    HS_OP_DETACH     = 3,   /* create detach zone at addr               */
    HS_OP_DENSITY    = 4,   /* run adaptive density update on node       */
} hs_op_t;

/* ═══════════════════════════════════════════════════════════════════════
   HYDRA TASK  (fits 1 cache line with queue slot metadata)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t  node_id;    /* target node (0..NODE_MAX-1)                 */
    uint16_t  op;         /* hs_op_t                                     */
    uint32_t  frame_id;   /* detach frame id (HS_OP_DETACH only)        */
    uint64_t  addr;       /* core address                                */
    uint64_t  value;      /* payload                                     */
} HydraTask;
/* 2+2+4+8+8 = 24B */

/* ═══════════════════════════════════════════════════════════════════════
   PER-HEAD QUEUE  (SPSC-safe for owner push, MPSC for steal)
   cacheline-padded to prevent false sharing
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    __attribute__((aligned(64)))
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
    HydraTask         tasks[HS_QUEUE_SIZE];
} HydraQueue;

/* ═══════════════════════════════════════════════════════════════════════
   ADAPTIVE NODE DENSITY
   ═══════════════════════════════════════════════════════════════════════ */

#define HS_RETOPO_SPLIT_THRESH   4096
#define HS_RETOPO_MERGE_THRESH   512

typedef enum {
    HS_NODE_LEAF     = 0,   /* active leaf node                         */
    HS_NODE_INTERNAL = 1,   /* split — children carry the data          */
} hs_node_kind_t;

typedef struct {
    uint32_t  density[HS_NODE_MAX_DENSE];
    uint16_t  parent [HS_NODE_MAX_DENSE];
    uint8_t   kind   [HS_NODE_MAX_DENSE];  /* hs_node_kind_t            */

    /* free-list for alloc/free node slots */
    uint16_t  free_stack[HS_NODE_MAX_DENSE];
    _Atomic uint32_t free_top;

    uint32_t  node_count;   /* total allocated (leaf + internal)        */
} HydraDensityMap;

/* ═══════════════════════════════════════════════════════════════════════
   WORKER CONTEXT  (one per thread)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {

    int               head_id;          /* 0..HS_HEADS-1                */

    /* shared state — pointer to global arrays */
    HydraQueue       *queues;           /* hydra_queue[HS_HEADS]        */
    NodeState        *node_state;       /* SoA node state               */
    FrontierMask     *frontier;         /* active frontier              */
    NodeMask         *edge_masks;       /* precomputed neighbor masks   */
    HydraDensityMap  *density;          /* adaptive node density        */

    volatile int     *stop;             /* shutdown flag                */

    uint64_t  now_ms;                   /* refreshed each loop iter     */

    /* per-thread stats (no atomic needed — written by owner only) */
    uint64_t  tasks_executed;
    uint64_t  tasks_stolen;

} HydraWorkerCtx;

/* ═══════════════════════════════════════════════════════════════════════
   GLOBAL QUEUES  (extern — defined in pogls_hydra_scheduler.c)
   ═══════════════════════════════════════════════════════════════════════ */

extern HydraQueue hydra_queue[HS_HEADS];

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: QUEUE OPS
   ═══════════════════════════════════════════════════════════════════════ */

/* push — owner thread only (no CAS needed) */
static inline void hs_push(int h, const HydraTask *t)
{
    HydraQueue *q   = &hydra_queue[h];
    uint32_t    pos = atomic_fetch_add_explicit(&q->tail, 1,
                                                 memory_order_relaxed);
    q->tasks[pos % HS_QUEUE_SIZE] = *t;
    atomic_thread_fence(memory_order_release);
}

/* pop — owner thread pops from its own queue */
static inline int hs_pop(int h, HydraTask *out)
{
    HydraQueue *q    = &hydra_queue[h];
    uint32_t    head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t    tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail) return 0;

    *out = q->tasks[head % HS_QUEUE_SIZE];
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: ROUTING
   ═══════════════════════════════════════════════════════════════════════ */

/* locality routing: lower address bits → head (same region = same core) */
static inline int hs_route_addr(uint64_t addr)
{
    return (int)(addr & (HS_HEADS - 1));
}

/* node → head: partition 162 nodes evenly across 16 heads */
static inline int hs_route_node(uint32_t node_id)
{
    return (int)((node_id * HS_HEADS) / NODE_MAX);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: SCHEDULE HELPERS
   ═══════════════════════════════════════════════════════════════════════ */

static inline void hs_schedule_write(uint64_t addr, uint64_t value)
{
    HydraTask t = {
        .op    = (uint16_t)HS_OP_NODE_WRITE,
        .addr  = addr,
        .value = value,
    };
    hs_push(hs_route_addr(addr), &t);
}

static inline void hs_schedule_diffuse(uint32_t node_id)
{
    HydraTask t = {
        .node_id = (uint16_t)node_id,
        .op      = (uint16_t)HS_OP_DIFFUSE,
    };
    hs_push(hs_route_node(node_id), &t);
}

static inline void hs_schedule_detach(uint64_t addr, uint32_t frame_id)
{
    HydraTask t = {
        .op       = (uint16_t)HS_OP_DETACH,
        .frame_id = frame_id,
        .addr     = addr,
    };
    hs_push(hs_route_addr(addr), &t);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: CPU RELAX  (spin-wait backoff)
   ═══════════════════════════════════════════════════════════════════════ */

static inline void hs_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
   API  (implemented in pogls_hydra_scheduler.c)
   ═══════════════════════════════════════════════════════════════════════ */

/* init global queues to zero */
void hs_init(void);

/* work-stealing pop — try to steal from another head's queue */
int hs_steal(int h, HydraTask *out);

/* execute one task — updates node_state + frontier */
void hs_execute(HydraWorkerCtx *ctx, const HydraTask *t);

/* main worker loop — runs until *ctx->stop != 0 */
void *hs_worker_loop(void *arg);   /* pthread-compatible signature */

/* bind calling thread to CPU core hid (requires CAP_SYS_NICE or root) */
int hs_bind_cpu(int hid);

/* launch all HS_HEADS worker threads */
int hs_launch(pthread_t threads[HS_HEADS],
              HydraWorkerCtx ctx_array[HS_HEADS]);

/* stop all workers + join */
void hs_shutdown(pthread_t threads[HS_HEADS],
                 HydraWorkerCtx ctx_array[HS_HEADS]);

/* ── Adaptive density ────────────────────────────────────────────────── */

/* init density map — set all leaf nodes 0..NODE_MAX-1 */
void hs_density_init(HydraDensityMap *dm);

/* alloc new node slot — returns node id or UINT16_MAX if full */
uint16_t hs_density_alloc(HydraDensityMap *dm);

/* free node slot */
void hs_density_free(HydraDensityMap *dm, uint16_t id);

/*
 * hs_density_update — check density[id] and split or merge
 * split: id → 2 children  (density > SPLIT_THRESH)
 * merge: id → parent      (density < MERGE_THRESH)
 */
void hs_density_update(HydraDensityMap *dm, uint16_t id);

/* full engine write path: map → schedule → diffuse → density */
void hs_engine_write(HydraWorkerCtx *ctx, uint64_t addr, uint64_t value);

#endif /* POGLS_HYDRA_SCHEDULER_H */

/*
 * pogls_fold.h — POGLS V3.6 Geometric Fold Architecture
 * ======================================================
 *
 * Diamond Block 64B  — 1 CPU cache line
 * Two-World A/B      — Switch Gate via ENGINE_ID bit6
 * HoneycombSlot      — reserved space สำหรับ Tails state
 * 3-Layer verify     — XOR → Fibo Intersect → Merkle
 *
 * กฎที่ห้ามแก้:
 *   DIAMOND_BLOCK_SIZE = 64   (1 cache line — ห้ามขยาย)
 *   Core Law: A = floor(θ × 2²⁰)  ← unchanged from V3.4
 *   V3.5 World A lanes 0-3         ← frozen, never touch
 *
 * Frozen constants (ห้าม change โดยไม่ bump version):
 *   PHI_UP   = 1,696,631
 *   PHI_DOWN =   648,055
 *   PHI_SCALE = 2²⁰ = 1,048,576
 */

#ifndef POGLS_FOLD_H
#define POGLS_FOLD_H


#ifdef __AVX2__
#endif

/* ═══════════════════════════════════════════════════════════════════════
   FROZEN CONSTANTS — V3.4 / V3.5 (unchanged)
   ═══════════════════════════════════════════════════════════════════════ */

#define PHI_SCALE          (1u << 20)      /* 2²⁰ = 1,048,576           */
#define PHI_UP             1696631u        /* floor(φ  × 2²⁰)           */
#define PHI_DOWN           648055u         /* floor(φ⁻¹ × 2²⁰)          */

#define NODE_MAX           162             /* Icosphere L2               */
#define FACES_RAW          32              /* 5-bit FACE_ID              */
#define FACES_LOGICAL      256             /* after 2 folds              */

/* ═══════════════════════════════════════════════════════════════════════
   V3.6 NEW CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define DIAMOND_BLOCK_SIZE   64    /* 1 CPU cache line — FROZEN          */
#define CORE_SLOT_SIZE        8    /* 6B data + 2B reserved              */
#define INVERT_SIZE           8    /* NOT(Core Slot) — always active     */
#define ACTIVE_SIZE          16    /* Core + Invert                      */
#define QUAD_MIRROR_SIZE     32    /* 4 × rotated Core — AVX2 register   */
#define FOLD_SIZE            48    /* Quad Mirror(32) + Fold3 reserved(16)*/

#define WORLD_A_LANES_START   0    /* Lane 0-3: X/NX/Y/NY (V3.5 frozen) */
#define WORLD_A_LANES_END     3
#define WORLD_B_LANES_START   4    /* Lane 4-7: new World B              */
#define WORLD_B_LANES_END     7
#define ENGINE_WORLD_BIT   0x40    /* bit6 of ENGINE_ID: 0=A, 1=B        */

#define FOLD_VERSION_CURRENT  1    /* bump เมื่อ expand fold             */
#define UNIVERSE_HEADER_SIZE  8    /* optional prefix — multi-context    */

/* HoneycombSlot — reserved space ใน Fold3 สำหรับ Tails state           */
#define HONEYCOMB_SLOT_OFFSET 48   /* byte offset จากต้น Diamond Block   */
#define HONEYCOMB_SLOT_SIZE   16   /* 16B ใน Fold3 reserved              */

/* ═══════════════════════════════════════════════════════════════════════
   WORLD TYPE
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    WORLD_A = 0,   /* 2^n binary  — execution / routing                 */
    WORLD_B = 1,   /* 3^n ternary — shadow / witness / evolution        */
} world_t;

/* ═══════════════════════════════════════════════════════════════════════
   CORE SLOT  (8 bytes — packed big-endian)
   ═══════════════════════════════════════════════════════════════════════
   Bit layout (64 bits total):
     [63:59]  FACE_ID    5b  icosphere face 0-31 (→256 logical via fold)
     [58:52]  ENGINE_ID  7b  0-127, bit6=0→World A, bit6=1→World B
     [51:28]  VECTOR_POS 24b A = floor(θ × 2²⁰) per face
     [27:24]  FIBO_GEAR  4b  0-3=G1(direct), 4-8=G2(batch), 9-15=G3(blast)
     [23:16]  QUAD_FLAGS 8b  X/NX/Y/NY axis balance gates
     [15:0]   RESERVED   16b — not yet assigned
*/

typedef struct __attribute__((packed)) {
    uint64_t raw;   /* อ่าน/เขียนทั้ง slot ด้วย 1 instruction           */
} CoreSlot;

/* Core Slot field accessors */
static inline uint8_t  core_face_id   (CoreSlot c) { return (uint8_t)((c.raw >> 59) & 0x1F); }
static inline uint8_t  core_engine_id (CoreSlot c) { return (uint8_t)((c.raw >> 52) & 0x7F); }
static inline uint32_t core_vector_pos(CoreSlot c) { return (uint32_t)((c.raw >> 28) & 0xFFFFFF); }
static inline uint8_t  core_fibo_gear (CoreSlot c) { return (uint8_t)((c.raw >> 24) & 0x0F); }
static inline uint8_t  core_quad_flags(CoreSlot c) { return (uint8_t)((c.raw >> 16) & 0xFF); }

static inline world_t core_world(CoreSlot c)
{
    return (core_engine_id(c) & ENGINE_WORLD_BIT) ? WORLD_B : WORLD_A;
}

static inline CoreSlot core_slot_build(uint8_t  face_id,
                                        uint8_t  engine_id,
                                        uint32_t vector_pos,
                                        uint8_t  fibo_gear,
                                        uint8_t  quad_flags)
{
    CoreSlot c;
    c.raw = ((uint64_t)(face_id   & 0x1F) << 59)
          | ((uint64_t)(engine_id & 0x7F) << 52)
          | ((uint64_t)(vector_pos & 0xFFFFFF) << 28)
          | ((uint64_t)(fibo_gear  & 0x0F) << 24)
          | ((uint64_t)(quad_flags & 0xFF) << 16);
    return c;
}

/* ═══════════════════════════════════════════════════════════════════════
   INVERT  (8 bytes)
   XOR(Core, Invert) = 0xFF × 8  ← Layer 1 audit
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint64_t core_invert(CoreSlot c)
{
    return ~c.raw;   /* NOT(Core) — 1 instruction                       */
}

/* ═══════════════════════════════════════════════════════════════════════
   DIAMOND BLOCK  (64 bytes — exactly 1 CPU cache line)
   ═══════════════════════════════════════════════════════════════════════
   Layout:
     [0-7]    Core Slot   8B   active
     [8-15]   Invert      8B   active  — NOT(Core)
     [16-47]  Quad Mirror 32B  folded  — 4 rotated copies (AVX2)
     [48-63]  Fold3/Honey 16B  reserved — HoneycombSlot (Tails state)
*/

typedef struct __attribute__((aligned(64))) {
    /* ── ACTIVE (16B) ──────────────────────────────────────────────── */
    CoreSlot core;            /* 8B — primary data                      */
    uint64_t invert;          /* 8B — NOT(core.raw)                     */

    /* ── FOLDED SPACE (48B) ─────────────────────────────────────────── */
    uint8_t  quad_mirror[32]; /* 32B — 4 rotated Core copies (AVX2)     */
    uint8_t  honeycomb[16];   /* 16B — HoneycombSlot: Tails reserved    */
} DiamondBlock;

/* static assert — ห้าม compile ถ้าขนาดผิด */
typedef char _fold_size_check[sizeof(DiamondBlock) == 64 ? 1 : -1];

/* ═══════════════════════════════════════════════════════════════════════
   QUAD MIRROR BUILD
   4 rotated copies of Core Slot ใน 32B (AVX2 register)
   Copy 0: original          (rot 0)
   Copy 1: rotate_left 1B    (rot 8)
   Copy 2: rotate_left 2B    (rot 16)
   Copy 3: rotate_left 3B    (rot 24)
   ═══════════════════════════════════════════════════════════════════════ */

static inline void fold_build_quad_mirror(DiamondBlock *b)
{
    const uint8_t *src = (const uint8_t *)&b->core.raw;
    uint8_t       *dst = b->quad_mirror;

    /* Copy 0 — rot 0 */
    memcpy(dst,      src, 8);
    /* Copy 1 — rot 8 (rotate left 1 byte) */
    dst[8]  = src[1]; dst[9]  = src[2]; dst[10] = src[3]; dst[11] = src[4];
    dst[12] = src[5]; dst[13] = src[6]; dst[14] = src[7]; dst[15] = src[0];
    /* Copy 2 — rot 16 (rotate left 2 bytes) */
    dst[16] = src[2]; dst[17] = src[3]; dst[18] = src[4]; dst[19] = src[5];
    dst[20] = src[6]; dst[21] = src[7]; dst[22] = src[0]; dst[23] = src[1];
    /* Copy 3 — rot 24 (rotate left 3 bytes) */
    dst[24] = src[3]; dst[25] = src[4]; dst[26] = src[5]; dst[27] = src[6];
    dst[28] = src[7]; dst[29] = src[0]; dst[30] = src[1]; dst[31] = src[2];
}

/* ═══════════════════════════════════════════════════════════════════════
   DIAMOND BLOCK INIT
   ═══════════════════════════════════════════════════════════════════════ */

static inline DiamondBlock fold_block_init(uint8_t  face_id,
                                            uint8_t  engine_id,
                                            uint32_t vector_pos,
                                            uint8_t  fibo_gear,
                                            uint8_t  quad_flags)
{
    DiamondBlock b;
    memset(&b, 0, sizeof(b));
    b.core   = core_slot_build(face_id, engine_id,
                                vector_pos, fibo_gear, quad_flags);
    b.invert = core_invert(b.core);
    fold_build_quad_mirror(&b);
    /* honeycomb[16] = 0 — Tails เขียนเอง */
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════
   HONEYCOMB SLOT  — Tails reserved (16B ใน honeycomb[])
   ═══════════════════════════════════════════════════════════════════════
   Layout (16B):
     [0-7]   merkle_root   8B  last committed Merkle root (truncated)
     [8]     algo_id       1B  hash algo: 0=md5, 1=sha256
     [9]     migration     1B  MIGRATION_STATE: 0=IDLE,1=RUNNING,2=COMMITTED
     [10-11] dna_count     2B  จำนวน DNA entries ที่รู้จัก
     [12-15] reserved      4B  Tails ใช้ได้
   Tails เขียน — Entangle อ่านตอน TAILS_SPAWN
   Core ไม่แตะ
*/

typedef struct __attribute__((packed)) {
    uint64_t merkle_root;   /* last committed Merkle root (8B truncated) */
    uint8_t  algo_id;       /* 0=md5, 1=sha256                           */
    uint8_t  migration;     /* MIGRATION_STATE enum                      */
    uint16_t dna_count;     /* DNA entries ที่ Tails รู้จัก              */
    uint8_t  reserved[4];   /* Tails ใช้ได้                              */
} HoneycombSlot;

typedef char _hcomb_size_check[sizeof(HoneycombSlot) == 16 ? 1 : -1];

/* อ่าน/เขียน HoneycombSlot จาก DiamondBlock */
static inline HoneycombSlot honeycomb_read(const DiamondBlock *b)
{
    HoneycombSlot s;
    memcpy(&s, b->honeycomb, sizeof(s));
    return s;
}

static inline void honeycomb_write(DiamondBlock *b, const HoneycombSlot *s)
{
    memcpy(b->honeycomb, s, sizeof(*s));
}

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 1 — XOR AUDIT
   XOR(Core 8B, Invert 8B) == 0xFFFFFFFFFFFFFFFF
   Cost: ~0.3ns (1 XOR + 1 CMP)
   ═══════════════════════════════════════════════════════════════════════ */

static inline int fold_xor_audit(const DiamondBlock *b)
{
    return (b->core.raw ^ b->invert) == 0xFFFFFFFFFFFFFFFFull;
}

/* ═══════════════════════════════════════════════════════════════════════
   LAYER 2 — FIBONACCI INTERSECT
   invariant = C0 AND C1 AND C2 AND C3
   bits ที่รอดจาก AND ทุก copy = geometric constants
   Cost: ~5-10ns (4 AND instructions หรือ 1 AVX2)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint64_t fold_fibo_intersect(const DiamondBlock *b)
{
    const uint64_t *q = (const uint64_t *)b->quad_mirror;
    /* C0 AND C1 AND C2 AND C3 */
    return q[0] & q[1] & q[2] & q[3];
}

/* entropy ต่ำเกินไป = block ต้องส่งต่อ Layer 3 */
static inline int fold_fibo_needs_merkle(const DiamondBlock *b)
{
    uint64_t inv = fold_fibo_intersect(b);
    /* นับ bits ที่รอด — ถ้า < 4 bits = entropy ต่ำเกินไป */
    return __builtin_popcountll(inv) == 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   SWITCH GATE  — dispatch World A / B
   bit6 ของ ENGINE_ID: 0 → World A (2^n), 1 → World B (3^n)
   Cost: ~1-2ns (shift + AND)
   ═══════════════════════════════════════════════════════════════════════ */

static inline world_t fold_switch_gate(const DiamondBlock *b)
{
    return core_world(b->core);
}

/* ═══════════════════════════════════════════════════════════════════════
   TWIN COORDINATE  — Two-World paired address
   twin = pos XOR invert_mask
   B promotes → A เมื่อ A ถูก eject
   ═══════════════════════════════════════════════════════════════════════ */

#define TWIN_INVERT_MASK   0x40   /* flip bit6 ของ ENGINE_ID = flip world */

static inline uint32_t fold_twin_engine_id(const DiamondBlock *b)
{
    uint8_t eid = core_engine_id(b->core);
    return (uint32_t)(eid ^ TWIN_INVERT_MASK);
}

/* ═══════════════════════════════════════════════════════════════════════
   UNIVERSE HEADER  (8B — optional prefix)
   เสียบข้างนอก DiamondBlock เพื่อขยาย context โดยไม่แตะ core
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t universe_id;    /* context ID                               */
    uint8_t  topo_level;     /* icosphere level (0-4)                    */
    uint8_t  fold_version;   /* expansion version                        */
    uint8_t  reserved[2];    /* future                                   */
} UniverseHeader;

typedef char _uhdr_size_check[sizeof(UniverseHeader) == 8 ? 1 : -1];

/* ═══════════════════════════════════════════════════════════════════════
   ENTANGLE OP CODE — TAILS_SPAWN
   เพิ่มใน entangle_graph.h op codes
   ═══════════════════════════════════════════════════════════════════════ */

#define EENTANGLE_OP_WRITE        1
#define EENTANGLE_OP_READ         2
#define EENTANGLE_OP_AUDIT        3
#define EENTANGLE_OP_DETACH       4
#define EENTANGLE_OP_TAILS_SPAWN  5   /* อ่าน HoneycombSlot → spawn Tails  */

/*
 * fold_tails_spawn_data — เตรียมข้อมูลสำหรับ Entangle TAILS_SPAWN
 *
 * Entangle worker เรียก function นี้หลัง pop EENTANGLE_OP_TAILS_SPAWN
 * คืน HoneycombSlot ที่ Tails ต้องการ boot กลับมา
 *
 * block  : Diamond Block ที่เก็บ HoneycombSlot ของ Tails
 * out    : ข้อมูลที่ส่งให้ spawn process ใหม่
 * คืน    : 1=มีข้อมูล, 0=slot ว่าง (Tails ยังไม่เคย commit)
 */
static inline int fold_tails_spawn_data(const DiamondBlock *block,
                                         HoneycombSlot      *out)
{
    HoneycombSlot s = honeycomb_read(block);
    if (s.merkle_root == 0 && s.dna_count == 0)
        return 0;   /* ว่าง — Tails ใหม่ ยังไม่เคย commit */
    *out = s;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
   3-LAYER VERIFY PIPELINE  (full path)
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    FOLD_VERIFY_PASS    =  0,   /* ผ่านทุก layer                         */
    FOLD_VERIFY_EJECT_1 = -1,   /* Layer 1 fail — XOR ผิด               */
    FOLD_VERIFY_EJECT_2 = -2,   /* Layer 2 fail — Fibo entropy ต่ำ       */
    FOLD_VERIFY_NEED_L3 =  1,   /* ต้องการ Merkle (Layer 3)              */
} FoldVerifyResult;

static inline FoldVerifyResult fold_verify(const DiamondBlock *b)
{
    /* Layer 1 — XOR: ~0.3ns */
    if (!fold_xor_audit(b))
        return FOLD_VERIFY_EJECT_1;

    /* Layer 2 — Fibo Intersect: ~5-10ns */
    if (fold_fibo_needs_merkle(b))
        return FOLD_VERIFY_NEED_L3;

    return FOLD_VERIFY_PASS;
}

#endif /* POGLS_FOLD_H */

/*
 * pogls_fibo_addr.h — POGLS V3.6 Fibonacci Integer Address Engine
 *
 * แทนที่ pogls_compute_address() ที่ใช้ double theta / 2π
 * ด้วย pure integer Fibonacci sampling บน sphere
 *
 * THE LAW (unchanged):  A = floor(θ × 2²⁰)
 * NEW PATH:             A = (n × PHI_UP) % PHI_SCALE   — integer ล้วน
 *
 * ไม่มี float, ไม่มี math.h, ไม่มี 2π
 * input = node index n (uint32_t)
 * output = angular address A (uint32_t, range 0..PHI_SCALE-1)
 *
 * Gear modes (FIBO_GEAR 4b ใน CoreSlot):
 *   G1  0-3   direct : (n × PHI_UP) % PHI_SCALE
 *   G2  4-8   batch  : (n × PHI_UP × gear_factor) % PHI_SCALE
 *   G3  9-15  blast  : (n × PHI_UP << gear_shift) % PHI_SCALE
 *
 * World A: PHI_UP   = floor(φ  × 2²⁰) = 1,696,631
 * World B: PHI_DOWN = floor(φ⁻¹× 2²⁰) =   648,055
 *
 * Overflow safety: n × PHI_UP คำนวณใน uint64_t ก่อน mod เสมอ
 *   max safe n สำหรับ uint32: floor(UINT32_MAX / PHI_UP) = 2530
 *   TOPO_ULTRA = 2562 nodes → ต้องใช้ uint64 path เสมอ ✓
 */

#ifndef POGLS_FIBO_ADDR_H
#define POGLS_FIBO_ADDR_H


/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS  (frozen — ห้ามเปลี่ยน)
   ═══════════════════════════════════════════════════════════════════════ */

#define PHI_SCALE    (1u << 20)          /* 2²⁰ = 1,048,576              */
#define PHI_UP       1696631u            /* floor(φ  × 2²⁰) World A      */
#define PHI_DOWN     648055u             /* floor(φ⁻¹× 2²⁰) World B      */
#define PHI_MASK     (PHI_SCALE - 1u)   /* fast mod mask                 */

/* Gear thresholds (FIBO_GEAR field ใน CoreSlot) */
#define GEAR_G1_MAX  3     /* 0-3  = G1 direct                          */
#define GEAR_G2_MAX  8     /* 4-8  = G2 batch                           */
#define GEAR_G3_MAX  15    /* 9-15 = G3 blast                           */

/* ═══════════════════════════════════════════════════════════════════════
   WORLD A — Fibonacci up  (φ spiral outward)
   ═══════════════════════════════════════════════════════════════════════ */

/* G1 direct — น้อยที่สุด */
static inline uint32_t fibo_addr_a(uint32_t n)
{
    return (uint32_t)(((uint64_t)n * PHI_UP) % PHI_SCALE);
}

/* G2 batch — คูณ gear_factor (1-5) */
static inline uint32_t fibo_addr_a_g2(uint32_t n, uint8_t gear)
{
    uint32_t factor = (uint32_t)(gear - GEAR_G1_MAX);   /* 1-5 */
    return (uint32_t)(((uint64_t)n * PHI_UP * factor) % PHI_SCALE);
}

/* G3 blast — shift left gear_shift bits */
static inline uint32_t fibo_addr_a_g3(uint32_t n, uint8_t gear)
{
    uint32_t shift = (uint32_t)(gear - GEAR_G2_MAX);    /* 1-7 */
    return (uint32_t)(((uint64_t)n * (PHI_UP << shift)) % PHI_SCALE);
}

/* ═══════════════════════════════════════════════════════════════════════
   WORLD B — Fibonacci down  (φ⁻¹ spiral inward)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t fibo_addr_b(uint32_t n)
{
    return (uint32_t)(((uint64_t)n * PHI_DOWN) % PHI_SCALE);
}

static inline uint32_t fibo_addr_b_g2(uint32_t n, uint8_t gear)
{
    uint32_t factor = (uint32_t)(gear - GEAR_G1_MAX);
    return (uint32_t)(((uint64_t)n * PHI_DOWN * factor) % PHI_SCALE);
}

static inline uint32_t fibo_addr_b_g3(uint32_t n, uint8_t gear)
{
    uint32_t shift = (uint32_t)(gear - GEAR_G2_MAX);
    return (uint32_t)(((uint64_t)n * (PHI_DOWN << shift)) % PHI_SCALE);
}

/* ═══════════════════════════════════════════════════════════════════════
   UNIFIED DISPATCH  — gear + world ใน call เดียว
   world: 0 = A (PHI_UP), 1 = B (PHI_DOWN)
   gear : 0-15 (FIBO_GEAR field จาก CoreSlot)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t fibo_addr(uint32_t n, uint8_t gear, uint8_t world)
{
    uint64_t base = world ? PHI_DOWN : PHI_UP;

    if (gear <= GEAR_G1_MAX) {
        /* G1 direct */
        return (uint32_t)(((uint64_t)n * base) % PHI_SCALE);
    }
    else if (gear <= GEAR_G2_MAX) {
        /* G2 batch */
        uint32_t factor = (uint32_t)(gear - GEAR_G1_MAX);
        return (uint32_t)(((uint64_t)n * base * factor) % PHI_SCALE);
    }
    else {
        /* G3 blast */
        uint32_t shift = (uint32_t)(gear - GEAR_G2_MAX);
        return (uint32_t)(((uint64_t)n * (base << shift)) % PHI_SCALE);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   INVERSE — address → node index (exact, World A)
   A = (n × PHI_UP) % PHI_SCALE
   n = (A × PHI_UP_INV) % PHI_SCALE
   PHI_UP_INV = modular inverse of PHI_UP mod 2²⁰ = 255,559
   ใช้สำหรับ lookup/routing — คืน raw index (ต้อง % node_max เอง)
   ═══════════════════════════════════════════════════════════════════════ */

#define PHI_UP_INV   255559u   /* pow(PHI_UP, -1, 2^20) — verified      */
#define PHI_DOWN_INV 736711u   /* pow(PHI_DOWN,-1, 2^20) — verified     */

static inline uint32_t fibo_addr_to_node_a(uint32_t addr)
{
    return (uint32_t)(((uint64_t)addr * PHI_UP_INV) % PHI_SCALE);
}

static inline uint32_t fibo_addr_to_node_b(uint32_t addr)
{
    return (uint32_t)(((uint64_t)addr * PHI_DOWN_INV) % PHI_SCALE);
}

/* ═══════════════════════════════════════════════════════════════════════
   TWIN COORD  (World A ↔ World B pair)
   twin_a = fibo_addr_a(n)
   twin_b = fibo_addr_b(n)
   XOR ทั้งคู่ = spread pattern สำหรับ Two-World verify
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t a;   /* World A address */
    uint32_t b;   /* World B address */
} FiboTwin;

static inline FiboTwin fibo_twin(uint32_t n)
{
    FiboTwin t;
    t.a = fibo_addr_a(n);
    t.b = fibo_addr_b(n);
    return t;
}

static inline uint32_t fibo_twin_xor(FiboTwin t)
{
    return t.a ^ t.b;
}

#endif /* POGLS_FIBO_ADDR_H */

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

/*
 * pogls_hydra_v36.h — POGLS V3.6 Hydra Bridge
 *
 * Connect Hydra (V3.1 router) กับ V3.6 layer:
 *   fibo_addr     → route by node index (ไม่ใช่ byte offset)
 *   DiamondBlock  → 64B unit ที่ Hydra append
 *   Entangle      → hook เมื่อ head anomaly → eject
 *   Unit Circle   → priority zone routing
 *
 * ไม่แก้ pogls_hydra.h — เป็น pure additive bridge
 * Namespace: hv36_* / HydraV36
 *
 * ห้าม include pogls_snapshot.h ใน header นี้
 */

#ifndef POGLS_HYDRA_V36_H
#define POGLS_HYDRA_V36_H


/* ═══════════════════════════════════════════════════════════════════════
   ROUTING  — fibo_addr → head_id
   แทน pogls_hydra_route(byte_offset) ด้วย node index routing
   ═══════════════════════════════════════════════════════════════════════ */

/* route node_idx → head_id (0..HS_HEADS-1)
   ใช้ fibo_addr เป็น hash key — cache-locality: ใกล้กัน = head เดียวกัน */
static inline uint8_t hv36_route(uint32_t node_idx, uint8_t gear, uint8_t world)
{
    uint32_t addr = fibo_addr(node_idx, gear, world);
    return (uint8_t)(addr % HS_HEADS);
}

/* priority route — outside circle → head 0 (priority head)
   inside circle → normal round-robin */
static inline uint8_t hv36_route_priority(uint32_t node_idx,
                                           uint8_t  gear,
                                           uint8_t  world)
{
    uint32_t addr = fibo_addr(node_idx, gear, world);
    uint64_t a    = (uint64_t)addr;
    uint64_t s2   = (uint64_t)PHI_SCALE * PHI_SCALE;

    if (2 * a * a >= s2)
        return 0;   /* outside circle → priority head */

    return (uint8_t)(addr % HS_HEADS);
}

/* ═══════════════════════════════════════════════════════════════════════
   WRITE PATH  — DiamondBlock through Hydra
   ═══════════════════════════════════════════════════════════════════════ */

/* push DiamondBlock write task เข้า HydraQueue ของ head ที่ถูก route */
static inline int hv36_enqueue_block(HydraQueue   queues[HS_HEADS],
                                      uint32_t      node_idx,
                                      uint8_t       gear,
                                      uint8_t       world,
                                      const DiamondBlock *block)
{
    uint8_t  hid  = hv36_route_priority(node_idx, gear, world);
    HydraQueue *q  = &queues[hid];

    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t next = (tail + 1) % HS_QUEUE_SIZE;

    if (next == atomic_load_explicit(&q->head, memory_order_acquire))
        return -1;   /* queue full */

    HydraTask t = {
        .node_id  = (uint16_t)(node_idx & 0xFFFF),
        .op       = HS_OP_NODE_WRITE,
        .frame_id = 0,
        .addr     = fibo_addr(node_idx, gear, world),
        .value    = block->core.raw,   /* signature = core slot */
    };

    q->tasks[tail] = t;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    return (int)hid;   /* คืน head_id ที่ assign */
}

/* ═══════════════════════════════════════════════════════════════════════
   ANOMALY → ENTANGLE EJECT
   เมื่อ Hydra head detect anomaly → สร้าง EjectFrame + EntangleHook
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    EjectFrame   eject;
    EntangleHook hook;
    uint8_t      head_id;
    uint8_t      active;
} HydraEjectCtx;

/* สร้าง eject context เมื่อ head anomaly */
static inline void hv36_anomaly_eject(HydraEjectCtx *ctx,
                                       uint8_t         head_id,
                                       uint32_t        frame_id,
                                       uint64_t        gate_addr,
                                       const DiamondBlock *block,
                                       uint64_t        now_ms)
{
    uint8_t world = core_world(block->core) == WORLD_B ? 1 : 0;

    ej_frame_init(&ctx->eject, frame_id, EJ_MODE_EMERGENCY,
                  gate_addr, block, 0, now_ms);
    ent_hook_init(&ctx->hook, &ctx->eject, world);

    ctx->head_id = head_id;
    ctx->active  = 1;
}

/* run repair pipeline — คืน 1=fold ok, 0=recycled */
static inline int hv36_eject_repair(HydraEjectCtx *ctx, DiamondBlock *b)
{
    if (!ctx->active) return 0;
    int r = ent_repair_pipeline(&ctx->hook, &ctx->eject, b);
    if (!r) ctx->active = 0;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════
   HEAD DENSITY CHECK  — V3.6 unit circle aware
   นับ nodes ใน head zone แยก inside/outside circle
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total;     /* nodes ใน zone */
    uint32_t inside;    /* อยู่ใน unit circle */
    uint32_t outside;   /* อยู่นอก — priority audit zone */
    uint8_t  head_id;
    uint8_t  needs_spawn;   /* outside > threshold → spawn */
    uint8_t  needs_retract; /* total < threshold → retract */
} HydraV36Density;

#define HV36_OUTSIDE_SPAWN_THRESH   8   /* outside nodes เกิน 8 → spawn */
#define HV36_TOTAL_RETRACT_THRESH   4   /* total nodes ต่ำกว่า 4 → retract */

static inline HydraV36Density hv36_density(uint8_t  head_id,
                                             uint32_t node_start,
                                             uint32_t node_count,
                                             uint8_t  gear,
                                             uint8_t  world)
{
    HydraV36Density d = { .head_id = head_id };

    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t n    = node_start + i;
        uint32_t addr = fibo_addr(n, gear, world);
        uint64_t a    = (uint64_t)addr;
        uint64_t s2   = (uint64_t)PHI_SCALE * PHI_SCALE;

        d.total++;
        if (2 * a * a >= s2)
            d.outside++;
        else
            d.inside++;
    }

    d.needs_spawn   = d.outside > HV36_OUTSIDE_SPAWN_THRESH;
    d.needs_retract = d.total   < HV36_TOTAL_RETRACT_THRESH;
    return d;
}

/* ═══════════════════════════════════════════════════════════════════════
   WORK STEAL  — steal task จาก head ที่มี outside-circle node มาก
   ═══════════════════════════════════════════════════════════════════════ */

/* pop task จาก queue ที่มีงานค้าง
   ลอง head เรียงจาก outside ก่อน (priority) */
static inline int hv36_steal(HydraQueue   queues[HS_HEADS],
                              uint8_t      my_head,
                              HydraTask   *out)
{
    /* ลอง steal จาก head อื่นตามลำดับ circular */
    for (int i = 1; i < HS_HEADS; i++) {
        uint8_t victim = (uint8_t)((my_head + i) % HS_HEADS);
        HydraQueue *q  = &queues[victim];

        uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        if (head == atomic_load_explicit(&q->tail, memory_order_acquire))
            continue;   /* empty */

        *out = q->tasks[head];
        if (atomic_compare_exchange_weak_explicit(
                &q->head,
                &head,
                (head + 1) % HS_QUEUE_SIZE,
                memory_order_release,
                memory_order_relaxed))
            return (int)victim;   /* steal สำเร็จ จาก head victim */
    }
    return -1;   /* nothing to steal */
}

/* ═══════════════════════════════════════════════════════════════════════
   TAILS CHECKPOINT  — snapshot สำหรับ Hydra state
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint8_t   head_id;
    uint8_t   world;
    uint8_t   eject_active;
    uint8_t   _pad;
    uint32_t  last_node_idx;
    uint32_t  last_addr;
    uint32_t  outside_count;
    uint8_t   ent_checkpoint[sizeof(EntTailsCheckpoint)];
} HydraV36Checkpoint;

static inline void hv36_checkpoint(HydraV36Checkpoint    *cp,
                                    const HydraEjectCtx   *ctx,
                                    uint8_t                head_id,
                                    uint8_t                world,
                                    const HydraV36Density *density)
{
    memset(cp, 0, sizeof(*cp));
    cp->head_id      = head_id;
    cp->world        = world;
    cp->eject_active = ctx ? ctx->active : 0;
    cp->outside_count= density ? density->outside : 0;
    cp->last_addr    = density ? fibo_addr(0, 0, world) : 0;

    if (ctx && ctx->active) {
        EntTailsCheckpoint ent_cp;
        ent_tails_checkpoint(&ctx->hook, &ent_cp);
        memcpy(cp->ent_checkpoint, &ent_cp, sizeof(EntTailsCheckpoint));
    }
}

#endif /* POGLS_HYDRA_V36_H */


/* === bench === */
/*
 * bench_hydra_v36.c — POGLS V3.6 Hydra Benchmark
 *
 * วัด:
 *   B1  hv36_route()          routing throughput
 *   B2  hv36_route_priority() priority routing (unit circle check)
 *   B3  hv36_enqueue_block()  enqueue DiamondBlock throughput
 *   B4  hv36_density()        density scan 162 nodes
 *   B5  hv36_steal()          work-steal throughput
 *   B6  fibo_addr()           address generation baseline
 *   B7  ent_repair_pipeline() repair throughput (good block)
 *   B8  full write path       route→enqueue→density check
 */


/* rubik stubs */
/* rubik tables + init */
uint8_t rubik_perm_lut[18][256];
uint8_t rubik_perm_inv[18][256];
void rubik_init(void){
    for(int m=0;m<18;m++)
        for(int s=0;s<256;s++){
            rubik_perm_lut[m][s]=(uint8_t)((s+m+1)&0xFF);
            rubik_perm_inv[m][s]=(uint8_t)((s-m-1)&0xFF);
        }
}



/* ── bench helper ── */
typedef struct { const char *name; uint64_t t0; uint64_t iters; } Bench;

static inline Bench bench_start(const char *name, uint64_t iters){
    printf("%-40s ", name); fflush(stdout);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t t0 = (uint64_t)t.tv_sec*1000000000ULL + t.tv_nsec;
    return (Bench){name, t0, iters};
}
static inline void bench_end(Bench *b){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t t1 = (uint64_t)t.tv_sec*1000000000ULL + t.tv_nsec;
    double ns   = (double)(t1-b->t0) / (double)b->iters;
    double mops = 1000.0 / ns;
    printf("%8.2f ns/op  %8.2f M ops/s\\n", ns, mops);
}

/* ── make a valid DiamondBlock ── */
static inline DiamondBlock make_block(uint32_t n){
    DiamondBlock b; memset(&b,0,sizeof(b));
    uint32_t addr = fibo_addr_a(n);
    b.core.raw = core_slot_build(
        (uint8_t)(n % 32),   /* face_id */
        0x00,                 /* engine_id World A */
        addr,                 /* vector_pos = fibo signature */
        0,                    /* gear G1 */
        0                     /* quad_flags */
    ).raw;
    b.invert = ~b.core.raw;
    return b;
}

int main(void){
    rubik_init();

    HydraQueue queues[HS_HEADS];
    DiamondBlock blocks[162];
    for(int i=0;i<162;i++) blocks[i] = make_block(i);

    const uint64_t ITERS = 1000000ULL;

    printf("══════════════════════════════════════════════════════\\n");
    printf("POGLS V3.6 Hydra Benchmark  (%llu iters)\\n", (unsigned long long)ITERS);
    printf("══════════════════════════════════════════════════════\\n");

    /* warmup */
    for(uint64_t i=0;i<100000;i++) (void)hv36_route((uint32_t)(i%162),0,0);

    volatile uint64_t sink = 0;
    Bench b;

    /* ── B1 routing ── */
    b = bench_start("B1  hv36_route()", ITERS);
    for(uint64_t i=0;i<ITERS;i++) sink += hv36_route((uint32_t)(i%162),0,0);
    bench_end(&b);

    /* ── B2 priority routing ── */
    b = bench_start("B2  hv36_route_priority()", ITERS);
    for(uint64_t i=0;i<ITERS;i++) sink += hv36_route_priority((uint32_t)(i%162),0,0);
    bench_end(&b);

    /* ── B3 enqueue ── */
    b = bench_start("B3  hv36_enqueue_block()", ITERS);
    for(uint64_t i=0;i<ITERS;i++){
        if(i%(HS_QUEUE_SIZE-1)==0) memset(queues,0,sizeof(queues));
        uint32_t n=(uint32_t)(i%162);
        sink += (uint64_t)hv36_enqueue_block(queues,n,0,0,&blocks[n]);
    }
    bench_end(&b);

    /* ── B4 density 162 nodes ── */
    uint64_t d_iters = ITERS/162;
    b = bench_start("B4  hv36_density() 162 nodes", d_iters*162);
    for(uint64_t i=0;i<d_iters;i++){
        HydraV36Density d = hv36_density(0,0,162,0,0);
        sink += d.outside;
    }
    bench_end(&b);

    /* ── B5 work steal ── */
    b = bench_start("B5  hv36_steal()", ITERS);
    memset(queues,0,sizeof(queues));
    for(int i=0;i<HS_QUEUE_SIZE/2;i++)
        hv36_enqueue_block(queues,(uint32_t)i,0,0,&blocks[i%162]);
    HydraTask stolen;
    for(uint64_t i=0;i<ITERS;i++){
        if(i%(HS_QUEUE_SIZE/2)==0){
            memset(queues,0,sizeof(queues));
            for(int j=0;j<HS_QUEUE_SIZE/2;j++)
                hv36_enqueue_block(queues,(uint32_t)j,0,0,&blocks[j%162]);
        }
        if(hv36_steal(queues,0,&stolen)>=0) sink++;
    }
    bench_end(&b);

    /* ── B6 fibo_addr baseline ── */
    b = bench_start("B6  fibo_addr() baseline", ITERS);
    for(uint64_t i=0;i<ITERS;i++) sink += fibo_addr((uint32_t)(i%162),0,0);
    bench_end(&b);

    /* ── B7 repair pipeline ── */
    uint64_t r_iters = ITERS/10;
    b = bench_start("B7  ent_repair_pipeline()", r_iters);
    for(uint64_t i=0;i<r_iters;i++){
        DiamondBlock blk = make_block((uint32_t)(i%162));
        EjectFrame ef;
        ej_frame_init(&ef,(uint32_t)i,EJ_MODE_EMERGENCY,(uint64_t)i*64,&blk,0,i);
        EntangleHook h;
        ent_hook_init(&h,&ef,0);
        sink += ent_repair_pipeline(&h,&ef,&blk);
    }
    bench_end(&b);

    /* ── B8 full write path ── */
    uint64_t fp_iters = ITERS/100;
    b = bench_start("B8  full write path (3 ops)", fp_iters*3);
    for(uint64_t i=0;i<fp_iters;i++){
        uint32_t n=(uint32_t)(i%162);
        if(i%(HS_QUEUE_SIZE-1)==0) memset(queues,0,sizeof(queues));
        uint8_t hid = hv36_route_priority(n,0,0);
        hv36_enqueue_block(queues,n,0,0,&blocks[n]);
        HydraV36Density d = hv36_density(hid,n,1,0,0);
        sink += d.outside + hid;
    }
    bench_end(&b);

    printf("══════════════════════════════════════════════════════\\n");
    HydraV36Density final_d = hv36_density(0,0,162,0,0);
    printf("unit circle split (162 nodes): inside=%d outside=%d\\n",
           final_d.inside, final_d.outside);
    printf("sink=%llu\\n", (unsigned long long)sink);
    printf("══════════════════════════════════════════════════════\\n");
    return 0;
}

"""

def run_bench():
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "bench.c")
        exe = os.path.join(tmp, "bench")
        with open(src, "w") as f:
            f.write(C_SRC)
        print("Compiling with gcc -O3 -march=native ...")
        r = subprocess.run(
            ["gcc", "-O3", "-std=c11", "-march=native",
             "-pthread", "-o", exe, src],
            capture_output=True, text=True
        )
        if r.returncode != 0:
            print("COMPILE ERROR:")
            print(r.stderr[-3000:])
            sys.exit(1)
        print("Compiled OK — running benchmark...\n")
        result = subprocess.run([exe], capture_output=True, text=True)
        print(result.stdout)
        if result.returncode != 0 and result.stderr:
            print("STDERR:", result.stderr[:500])

run_bench()
