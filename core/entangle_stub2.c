#include "pogls_ntangle_graph.h"

void entangle_diffuse(NodeState *ns, const FrontierMask *f, FrontierMask *next) {
    for (int w = 0; w < NODE_MASK_WORDS; w++) {
        uint64_t bits = f->w[w];
        while (bits) {
            int bit  = __builtin_ctzll(bits);
            int node = (w << 6) | bit;
            if (node < NODE_MAX)
                nodemask_or(next, &ns->neighbors[node]);
            bits &= bits - 1;
        }
    }
}

void entangle_worker(NodeState *ns, EntangleTaskQueue *q,
                   FrontierMask *fr, uint64_t ms) {
    EntangleTask t;
    while (entangle_task_pop(q, &t) == 0)
        node_update(ns, t.node_id, ms, fr);
}

void entangle_frontier_iterate(const FrontierMask *f,
                              void (*cb)(uint32_t, void*), void *ud) {
    for (int w = 0; w < NODE_MASK_WORDS; w++) {
        uint64_t bits = f->w[w];
        while (bits) {
            int bit  = __builtin_ctzll(bits);
            uint32_t node = (uint32_t)((w << 6) | bit);
            if (node < NODE_MAX) cb(node, ud);
            bits &= bits - 1;
        }
    }
}
