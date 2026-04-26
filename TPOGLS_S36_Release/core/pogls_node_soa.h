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

#include <stdint.h>
#include <string.h>

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
