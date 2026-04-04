/*
 * geo_route.h — Geometry Routing Layer (Session 18)
 *
 * Role: ThetaCoord → TorusNode (ใช้งานกับ diamond_flow_step)
 *
 * Layer map:
 *   theta_map   → ThetaCoord (face, edge, z)
 *   geo_route   → TorusNode  (face, edge, z, state=0)
 *   diamond_flow→ route_addr accumulate → DNA write
 *
 * TorusNode.state reset to 0 เสมอ — state เป็น runtime ของ torus ไม่ใช่ input
 */

#ifndef GEO_ROUTE_H
#define GEO_ROUTE_H

#include "theta_map.h"
#include "geo_dodeca_torus.h"   /* TorusNode, torus_step, g_map */

/* ── direct map ─────────────────────────────────────────────────── */
/*
 * geo_route_init — ThetaCoord → TorusNode เริ่มต้น
 *   face/edge/z ถ่ายตรง, state=0
 *   O(1), no branch
 */
static inline TorusNode geo_route_init(ThetaCoord tc) {
    TorusNode n;
    n.face  = tc.face;
    n.edge  = tc.edge;
    n.z     = tc.z;
    n.state = 0;
    return n;
}

/* ── fast path: raw → TorusNode ─────────────────────────────────── */
static inline TorusNode geo_route_from_raw(uint64_t raw) {
    return geo_route_init(theta_map(raw));
}

/*
 * geo_route_advance — เดิน n steps บน torus จาก initial node
 *   ใช้เมื่อต้องการ "warm up" ก่อนเข้า flow จริง
 *   steps=0 → ไม่เดิน (ส่งกลับ node เดิม)
 */
static inline TorusNode geo_route_advance(TorusNode n, int steps) {
    for (int i = 0; i < steps; i++)
        torus_step(&n);
    return n;
}

/*
 * geo_route_to_baseline_input — แปลง TorusNode → uint64 สำหรับใช้เป็น
 *   baseline hint ใน diamond_flow (optional, ถ้าต้องการ seed ตาม geometry)
 *
 *   pack: [63:56]=face [55:48]=edge [47:40]=z [39:0]=0
 *   ไม่ใช่ route_addr — เป็นแค่ geometry descriptor
 */
static inline uint64_t geo_route_pack(const TorusNode *n) {
    return ((uint64_t)n->face  << 56)
         | ((uint64_t)n->edge  << 48)
         | ((uint64_t)n->z     << 40);
}

/*
 * geo_route_batch — N raw inputs → N TorusNodes
 *   out[] ต้องมีขนาด >= n
 */
static inline void geo_route_batch(const uint64_t *in, TorusNode *out, int n) {
    for (int i = 0; i < n; i++)
        out[i] = geo_route_from_raw(in[i]);
}

#endif /* GEO_ROUTE_H */
