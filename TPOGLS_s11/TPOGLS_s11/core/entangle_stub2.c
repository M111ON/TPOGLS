#include <stdint.h>
#include "pogls_ntangle_graph.h"

void nodemask_or(FrontierMask *dst, const FrontierMask *src) {
    for(int i=0; i<NODE_MASK_WORDS; i++) dst->w[i] |= src->w[i];
}

int entangle_task_pop(EntangleTaskQueue *q, EntangleTask *t) { return -1; }

void node_update(NodeState *ns, uint32_t nid, uint64_t ms, FrontierMask *fr) {}

void entangle_diffuse(NodeState *ns, const FrontierMask *f, FrontierMask *next) {
    for (int w = 0; w < NODE_MASK_WORDS; w++) {
        uint64_t bits = f->w[w];
        while (bits) {
            int bit  = __builtin_ctzll(bits);
            int node = (w << 6) | bit;
            if (node < NODE_MAX) nodemask_or(next, &ns->neighbors[node]);
            bits &= bits - 1;
        }
    }
}

void entangle_worker(NodeState *ns, EntangleTaskQueue *q, uint64_t ms, FrontierMask *fr) {
    EntangleTask t;
    while (entangle_task_pop(q, &t) == 0) node_update(ns, t.node_id, ms, fr);
}
