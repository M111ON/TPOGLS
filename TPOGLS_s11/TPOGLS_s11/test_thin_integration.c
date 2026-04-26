#include <stdio.h>
#include <assert.h>
#include "./core/pogls_ntangle_graph_thin.h"

int main() {
    NodeState ns;
    init_minimal_graph(&ns);

    FrontierMask current = {0};
    nodemask_set(&current, 0); // เริ่มที่ Node 0

    printf("Starting Thin Integration Test...\n");
    
    // Step 1: Diffuse จาก 0 ไป 1
    FrontierMask next = {0};
    if (current.w[0] & (1ULL << 0)) {
        nodemask_or_thin(&next, &ns.neighbors[0]);
    }

    printf("Step 1 (Node 0 -> ?): Frontier Mask = %lu\n", next.w[0]);
    assert(next.w[0] == (1ULL << 1)); // ต้องเจอ Node 1

    // Step 2: Diffuse จาก 1 ไป 2
    current = next;
    next.w[0] = 0;
    if (current.w[0] & (1ULL << 1)) {
        nodemask_or_thin(&next, &ns.neighbors[1]);
    }

    printf("Step 2 (Node 1 -> ?): Frontier Mask = %lu\n", next.w[0]);
    assert(next.w[0] == (1ULL << 2)); // ต้องเจอ Node 2

    printf("Thin Integration: SUCCESS (Real behavior verified with 3 nodes)\n");
    return 0;
}
