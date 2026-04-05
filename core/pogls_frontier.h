/*
 * pogls_frontier.h — POGLS V3.5 Frontier Propagation (Merged)
 *
 * รวม API จากทั้งสองทีม:
 *
 *   Team-B style (simple call site):
 *     pogls_frontier_diffuse(node_id, &next)   ← ใช้ global node_adj_map[]
 *
 *   Our style (explicit graph pointer):
 *     frontier_diffuse(&frontier, &bbg)
 *     frontier_expand(seed, &bbg, radius)
 *
 * ทั้งสองใช้ struct เดียวกัน (NodeMask / Bitboard) ไม่มี conversion
 *
 * ไม่แตะ WAL/Snapshot/Hydra/Ntacle
 * Namespace: frontier_* / pogls_frontier_* / bb_*
 */

#ifndef POGLS_FRONTIER_H
#define POGLS_FRONTIER_H

#include "pogls_bitboard.h"          /* Bitboard, FrontierBB, ContextBB  */
#include "pogls_graph_topology.h"    /* adjacency_offset, adjacency_count */

/* ═══════════════════════════════════════════════════════════════════════
   GLOBAL ADJACENCY MAP  (team-B pattern)
   node_adj_map[n] = NodeMask ของ neighbors ทั้งหมดของ node n
   build ครั้งเดียวตอน startup ด้วย frontier_build_adj_map()
   ═══════════════════════════════════════════════════════════════════════ */

extern NodeMask node_adj_map[NODE_MAX];

/*
 * frontier_build_adj_map — สร้าง node_adj_map จาก adjacency table
 * เรียกหลัง topo_build_adjacency() ก่อน engine เริ่ม
 */
void frontier_build_adj_map(const uint16_t *adj_table);

/* ═══════════════════════════════════════════════════════════════════════
   TEAM-B STYLE API  (simple — global adj map, no explicit graph ptr)
   ~30 ns per node
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * pogls_frontier_diffuse — diffuse 1 node → next frontier
 * ใช้ global node_adj_map (ต้อง frontier_build_adj_map() ก่อน)
 *
 * usage:
 *   uint8_t node_id;
 *   Bitboard next = bb_zero();
 *   while (bitboard_next_active(active.w, &node_id))
 *       pogls_frontier_diffuse(node_id, &next);
 */
static inline void pogls_frontier_diffuse(uint8_t       current_node,
                                           FrontierBB   *next_frontier)
{
    if (current_node >= NODE_MAX) return;
    next_frontier->w[0] |= node_adj_map[current_node].w[0];
    next_frontier->w[1] |= node_adj_map[current_node].w[1];
    next_frontier->w[2] |= node_adj_map[current_node].w[2];
    next_frontier->w[3] |= node_adj_map[current_node].w[3];
}

/* ═══════════════════════════════════════════════════════════════════════
   OUR STYLE API  (batch — explicit BBGraph, faster for multi-node)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * frontier_diffuse — one expansion pass over entire frontier mask
 * O(active_nodes) × 4 OR ops ≈ 40-70ns typical
 */
static inline FrontierBB frontier_diffuse(const FrontierBB *frontier,
                                           const BBGraph    *graph)
{
    FrontierBB next = bb_zero();

    for (int w = 0; w < 3; w++) {
        uint64_t bits = frontier->w[w];
        while (bits) {
            int      b    = __builtin_ctzll(bits);
            bits         &= bits - 1;
            uint32_t node = (uint32_t)((w << 6) | b);
            if (node >= NODE_MAX) break;
            next.w[0] |= graph->adj[node].w[0];
            next.w[1] |= graph->adj[node].w[1];
            next.w[2] |= graph->adj[node].w[2];
            next.w[3] |= graph->adj[node].w[3];
        }
    }
    return next;
}

/*
 * frontier_expand — multi-step, accumulates into context
 * radius=1: neighbors | radius=2: neighbors of neighbors
 */
static inline ContextBB frontier_expand(FrontierBB     seed,
                                         const BBGraph *graph,
                                         int            radius)
{
    ContextBB ctx = seed;
    for (int step = 0; step < radius; step++) {
        FrontierBB next = frontier_diffuse(&ctx, graph);
        ctx = bb_or(ctx, next);
    }
    return ctx;
}

/*
 * frontier_new_only — new nodes only (BFS: exclude visited)
 */
static inline FrontierBB frontier_new_only(const FrontierBB *expanded,
                                            const ContextBB  *visited)
{
    FrontierBB fresh;
    fresh.w[0] = expanded->w[0] & ~visited->w[0];
    fresh.w[1] = expanded->w[1] & ~visited->w[1];
    fresh.w[2] = expanded->w[2] & ~visited->w[2];
    fresh.w[3] = expanded->w[3] & ~visited->w[3];
    return fresh;
}

/* ═══════════════════════════════════════════════════════════════════════
   PARITY AUDIT HOOK  (Ntacle integrity)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint8_t frontier_parity(const FrontierBB *f)
{
    return bb_parity(f);
}

static inline int frontier_parity_check(const FrontierBB *f, uint8_t expected)
{
    return bb_parity_check(f, expected);
}

#endif /* POGLS_FRONTIER_H */

/* ═══════════════════════════════════════════════════════════════════════
   UNIFIED STARTUP INIT  (implemented in pogls_compute_stack.c)
 *
 * Call once at process startup BEFORE any compute / WAL operations.
 * Runs: rubik_init + clut_init + frontier_build_adj_map
 * Returns 0=ok, -1=rubik selftest fail, -2=clut selftest fail
 * ═══════════════════════════════════════════════════════════════════════ */

int pogls_compute_stack_init(void);
int pogls_compute_stack_init_full(const uint16_t *adj_table,
                                   const uint8_t  *node_map);
