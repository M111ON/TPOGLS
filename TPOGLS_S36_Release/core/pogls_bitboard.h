/*
 * pogls_bitboard.h — POGLS V3.5 Bitboard Compute + Frontier Propagation
 *
 * Engine 3+4 รวมกัน: bitset operations + graph diffusion via OR
 *
 * Bitboard: 162 nodes → 256-bit mask (4×uint64)
 *   activate:  O(1) bit set
 *   scan:      TZCNT loop — ~1 cycle/node
 *   diffuse:   bitwise OR adjacency — <300ns full graph
 *
 * ใช้ NodeMask จาก pogls_node_soa.h (ไม่ redefine)
 * วางเหนือ Angular Address — ไม่แตะ WAL/Snapshot/Hydra/Ntacle
 * Namespace: bb_* / frontier_*
 */

#ifndef POGLS_BITBOARD_H
#define POGLS_BITBOARD_H

#include <stdint.h>
#include <string.h>
#include "pogls_node_soa.h"   /* NodeMask, NODE_MAX, nodemask_* */

/* ═══════════════════════════════════════════════════════════════════════
   BITBOARD STATE  (wraps NodeMask with named role)
   ═══════════════════════════════════════════════════════════════════════ */

typedef NodeMask Bitboard;      /* 256-bit, 162 nodes */
typedef NodeMask FrontierBB;    /* active frontier  */
typedef NodeMask ContextBB;     /* expanded context */

/* ═══════════════════════════════════════════════════════════════════════
   ADJACENCY TABLE  (precomputed per-node OR mask)
   162 × 32B = 5184B — fits L1 alongside NodeState
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    NodeMask adj[NODE_MAX];    /* adj[n] = bitmask of node n's neighbors */
} BBGraph;

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: BITBOARD PRIMITIVES  (~1 cycle each)
   ═══════════════════════════════════════════════════════════════════════ */

static inline void bb_activate(Bitboard *bb, uint32_t node)
{
    bb->w[node >> 6] |= 1ULL << (node & 63);
}

static inline void bb_deactivate(Bitboard *bb, uint32_t node)
{
    bb->w[node >> 6] &= ~(1ULL << (node & 63));
}

static inline int bb_is_active(const Bitboard *bb, uint32_t node)
{
    return (int)((bb->w[node >> 6] >> (node & 63)) & 1);
}

static inline void bb_clear(Bitboard *bb)
{
    bb->w[0] = bb->w[1] = bb->w[2] = bb->w[3] = 0;
}

static inline Bitboard bb_zero(void)
{
    return nodemask_zero();
}

/* union of two boards */
static inline Bitboard bb_or(Bitboard a, Bitboard b)
{
    a.w[0] |= b.w[0]; a.w[1] |= b.w[1];
    a.w[2] |= b.w[2]; a.w[3] |= b.w[3];
    return a;
}

/* intersection */
static inline Bitboard bb_and(Bitboard a, Bitboard b)
{
    a.w[0] &= b.w[0]; a.w[1] &= b.w[1];
    a.w[2] &= b.w[2]; a.w[3] &= b.w[3];
    return a;
}

/* difference (a AND NOT b) */
static inline Bitboard bb_andnot(Bitboard a, Bitboard b)
{
    a.w[0] &= ~b.w[0]; a.w[1] &= ~b.w[1];
    a.w[2] &= ~b.w[2]; a.w[3] &= ~b.w[3];
    return a;
}

static inline int bb_empty(const Bitboard *bb)
{
    return !(bb->w[0] | bb->w[1] | bb->w[2] | bb->w[3]);
}

/* popcount = number of active nodes */
static inline int bb_popcount(const Bitboard *bb)
{
    return __builtin_popcountll(bb->w[0])
         + __builtin_popcountll(bb->w[1])
         + __builtin_popcountll(bb->w[2])
         + __builtin_popcountll(bb->w[3]);
}

/* ═══════════════════════════════════════════════════════════════════════
   INLINE: BIT SCAN ITERATOR  (TZCNT loop, ~1 cycle/node)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * bb_foreach — iterate active nodes in bb
 * cb(node_id, userdata) called per active bit
 * คือ pattern เดียวกับ ntacle_frontier_iterate แต่ inlineable
 */
static inline void bb_foreach(const Bitboard *bb,
                               void (*cb)(uint32_t node, void *ud),
                               void *ud)
{
    for (int w = 0; w < 4; w++) {
        uint64_t bits = bb->w[w];
        while (bits) {
            int      b    = __builtin_ctzll(bits);
            bits         &= bits - 1;
            uint32_t node = (uint32_t)((w << 6) | b);
            if (node < NODE_MAX) cb(node, ud);
        }
    }
}

/* pop first active node (returns NODE_MAX if empty) */
static inline uint32_t bb_pop_first(Bitboard *bb)
{
    for (int w = 0; w < 4; w++) {
        if (bb->w[w]) {
            int b       = __builtin_ctzll(bb->w[w]);
            bb->w[w]   &= bb->w[w] - 1;
            uint32_t n  = (uint32_t)((w << 6) | b);
            return (n < NODE_MAX) ? n : NODE_MAX;
        }
    }
    return NODE_MAX;
}

/* ═══════════════════════════════════════════════════════════════════════
   FRONTIER PROPAGATION ENGINE  (<300ns full graph)
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * frontier_diffuse — one expansion step
 *
 * For each active node in frontier, OR its adjacency mask into next.
 * result = all neighbors of frontier nodes
 *
 * latency: O(active_nodes) × 4 OR ops ≈ 40-70ns typical
 */
static inline FrontierBB frontier_diffuse(const FrontierBB *frontier,
                                           const BBGraph    *graph)
{
    FrontierBB next = bb_zero();

    for (int w = 0; w < 3; w++) {   /* word 3 = padding bits, skip */
        uint64_t bits = frontier->w[w];
        while (bits) {
            int      b    = __builtin_ctzll(bits);
            bits         &= bits - 1;
            uint32_t node = (uint32_t)((w << 6) | b);
            if (node >= NODE_MAX) break;
            /* OR all 4 words of this node's adjacency mask */
            next.w[0] |= graph->adj[node].w[0];
            next.w[1] |= graph->adj[node].w[1];
            next.w[2] |= graph->adj[node].w[2];
            next.w[3] |= graph->adj[node].w[3];
        }
    }
    return next;
}

/*
 * frontier_expand — multi-step expansion
 * radius=1: immediate neighbors
 * radius=2: neighbors of neighbors
 * includes original frontier in result
 */
static inline ContextBB frontier_expand(FrontierBB        seed,
                                         const BBGraph    *graph,
                                         int               radius)
{
    ContextBB ctx = seed;
    for (int step = 0; step < radius; step++) {
        FrontierBB next = frontier_diffuse(&ctx, graph);
        ctx = bb_or(ctx, next);
    }
    return ctx;
}

/*
 * frontier_new_only — frontier XOR context (nodes not yet visited)
 * ใช้ใน BFS-style propagation เพื่อหลีก revisit
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
   PARITY AUDIT HOOK  (Ntacle integrity signal)
   ═══════════════════════════════════════════════════════════════════════ */

/* board parity = popcount mod 2 — cheap integrity fingerprint */
static inline uint8_t bb_parity(const Bitboard *bb)
{
    return (uint8_t)(bb_popcount(bb) & 1);
}

/*
 * bb_parity_check — compare frontier parity vs expected
 * เรียกจาก Ntacle audit hook ก่อน WAL window close
 * returns 1 if OK, 0 if mismatch
 */
static inline int bb_parity_check(const Bitboard *bb, uint8_t expected)
{
    return bb_parity(bb) == expected;
}

/* ═══════════════════════════════════════════════════════════════════════
   BBGRAPH BUILD API
   ═══════════════════════════════════════════════════════════════════════ */

/* build BBGraph from flat adjacency table (CSR format)
 * table[offset[n] .. offset[n]+count[n]-1] = neighbor ids
 */
void bb_graph_build(BBGraph        *graph,
                    const uint16_t *adj_table,
                    const uint16_t *adj_offset,
                    const uint8_t  *adj_count);

/* Rubik state → activate specific nodes (permutation routing) */
static inline void bb_rubik_activate(Bitboard    *bb,
                                      uint8_t      rubik_state,
                                      uint32_t     base_node)
{
    /* use parity of rubik_state to select node variant */
    uint32_t node = (base_node + __builtin_popcount(rubik_state)) % NODE_MAX;
    bb_activate(bb, node);
}

#endif /* POGLS_BITBOARD_H */
