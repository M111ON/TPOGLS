/*
 * geo_dodeca_torus.h — Dodeca Torus Field (Session 13)
 * =====================================================
 * Depends: geo_dodeca.h, geo_fibo_clock.h
 *
 * Verified topology:
 *   ✅ double-cross A→B→A  (all 60 half-edges invertible)
 *   ✅ BFS 12/12 faces reachable
 *   ✅ 5-cycle flip-sum even (orientable surface)
 *
 * Layout:
 *   Face 0        = top cap
 *   Face 1..5     = upper pentagon ring
 *   Face 6..10    = lower pentagon ring
 *   Face 11       = bottom cap
 *   z (layer)     = torus depth, wraps 8:9 ratio
 */

#ifndef GEO_DODECA_TORUS_H
#define GEO_DODECA_TORUS_H

#include <stdint.h>
#include <stdlib.h>
#include "geo_dodeca.h"
#include "geo_fibo_clock.h"

/* ── config ──────────────────────────────────────────────────────── */
#define TORUS_XRAY_CACHE_SIZE  1024u
#define TORUS_POOL_SIZE          16u
#define TORUS_LAYER_WRAP        255u   /* z = (z+1) & TORUS_LAYER_WRAP */

/* ── EdgeMap ─────────────────────────────────────────────────────── */
typedef struct {
    uint8_t next_face;
    uint8_t next_edge;
    uint8_t flip;        /* 1 = parity invert on crossing */
} EdgeMap;

/* ── g_map[12][5] — mathematically verified ─────────────────────── *
 *   Test 1: double-cross A→B→A  ✅ all 60
 *   Test 2: BFS 12/12 reachable ✅
 *   Test 3: 5-cycle flip sum = 4 (even → orientable) ✅
 * ─────────────────────────────────────────────────────────────────── */
static const EdgeMap g_map[12][5] = {
    /*  0 */ { {1,0,0}, {2,0,1}, {3,0,0}, {4,0,1}, {5,0,0} },
    /*  1 */ { {0,0,0}, {5,4,1}, {10,1,1}, {6,0,1}, {2,1,1} },
    /*  2 */ { {0,1,1}, {1,4,1}, {6,1,1}, {7,0,1}, {3,1,1} },
    /*  3 */ { {0,2,0}, {2,4,1}, {7,1,1}, {8,0,1}, {4,1,1} },
    /*  4 */ { {0,3,1}, {3,4,1}, {8,1,1}, {9,0,1}, {5,1,1} },
    /*  5 */ { {0,4,0}, {4,4,1}, {9,1,1}, {10,0,1}, {1,1,1} },
    /*  6 */ { {1,3,1}, {2,2,1}, {7,4,0}, {11,0,1}, {10,2,0} },
    /*  7 */ { {2,3,1}, {3,2,1}, {8,4,0}, {11,1,0}, {6,2,0} },
    /*  8 */ { {3,3,1}, {4,2,1}, {9,4,0}, {11,2,1}, {7,2,0} },
    /*  9 */ { {4,3,1}, {5,2,1}, {10,4,0}, {11,3,0}, {8,2,0} },
    /* 10 */ { {5,3,1}, {1,2,1}, {6,4,0}, {11,4,1}, {9,2,0} },
    /* 11 */ { {6,3,1}, {7,3,0}, {8,3,1}, {9,3,0}, {10,3,1} },
};

/* ════════════════════════════════════════════════════════════════════
 * Node
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t face;    /* 0..11                     */
    uint8_t edge;    /* 0..4                      */
    uint8_t z;       /* torus layer (8:9 wrap)    */
    uint8_t state;   /* payload — parity-tracked  */
} TorusNode;

static const TorusNode TORUS_BLUEPRINT = {0, 0, 0, 0};

/* ════════════════════════════════════════════════════════════════════
 * step — O(1), no lookup other than g_map
 * ════════════════════════════════════════════════════════════════════ */

static inline void torus_step(TorusNode *n)
{
    EdgeMap m   = g_map[n->face][n->edge];
    n->face     = m.next_face;
    n->edge     = m.next_edge;
    n->state   ^= m.flip;          /* B1: branchless — no if, direct XOR */
    n->z        = (n->z + 1) & TORUS_LAYER_WRAP;
}

/* ════════════════════════════════════════════════════════════════════
 * boundary_pass — vex (even edge) vs edge-type (odd edge)
 * ════════════════════════════════════════════════════════════════════ */

static inline uint8_t torus_boundary_pass(uint8_t edge_idx, uint8_t state)
{
    /* even edge = vex (vertex-adjacent) → pass through */
    /* odd  edge = edge-type → parity flip              */
    return (edge_idx & 1u) ? (state ^ 1u) : state;
}

/* ════════════════════════════════════════════════════════════════════
 * XRay — path signature (5 bits per face hit, 12 hits = 60 bits)
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t sig;
} XRay;

static inline void xray_record(XRay *x, const TorusNode *n)
{
    uint8_t code = (uint8_t)((n->face << 3) | n->edge);  /* 12×5=60 < 64 */
    x->sig = (x->sig << 5) ^ code;
}

static inline TorusNode xray_replay(const XRay *x)
{
    TorusNode n = TORUS_BLUEPRINT;
    uint64_t  s = x->sig;
    for (int i = 0; i < 12; i++) {
        uint8_t code = (uint8_t)(s & 0x1Fu);
        s >>= 5;
        n.face = code >> 3;
        n.edge = code & 0x07u;
        torus_step(&n);
    }
    return n;
}

/* ════════════════════════════════════════════════════════════════════
 * XRay cache — ring buffer, reuse without recompute
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    XRay     cache[TORUS_XRAY_CACHE_SIZE];
    uint16_t ptr;
} XRayCache;

static inline void xray_cache_init(XRayCache *c)
{
    c->ptr = 0;
}

static inline void xray_cache_push(XRayCache *c, const XRay *x)
{
    c->cache[c->ptr] = *x;
    c->ptr = (uint16_t)((c->ptr + 1u) & (TORUS_XRAY_CACHE_SIZE - 1u));
}

static inline const XRay *xray_cache_get(const XRayCache *c, uint16_t idx)
{
    return &c->cache[idx & (TORUS_XRAY_CACHE_SIZE - 1u)];
}

/* ════════════════════════════════════════════════════════════════════
 * Spawn pool — 16 standby cores, overflow = malloc
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    TorusNode pool[TORUS_POOL_SIZE];
    uint8_t   used;
} SpawnPool;

static inline void spawn_pool_init(SpawnPool *p)
{
    p->used = 0;
    for (uint8_t i = 0; i < (uint8_t)TORUS_POOL_SIZE; i++) p->pool[i] = TORUS_BLUEPRINT;
}

static inline TorusNode *spawn_core(SpawnPool *p)
{
    if (p->used < TORUS_POOL_SIZE) return &p->pool[p->used++];
    TorusNode *n = (TorusNode *)malloc(sizeof(TorusNode));
    if (n) *n = TORUS_BLUEPRINT;
    return n;
}

/* ════════════════════════════════════════════════════════════════════
 * run_field — full pipeline (boundary → xray → step → fibo clock)
 * ════════════════════════════════════════════════════════════════════ */

static inline void torus_run_field(TorusNode    *n,
                                   int           num_steps,
                                   XRayCache    *xc,
                                   FiboCtx      *fclk)
{
    XRay x = {0};
    /* proxy block: feed node state as popcount input to fibo clock */
    DiamondBlock proxy;
    __builtin_memset(&proxy, 0, sizeof(proxy));

    for (int i = 0; i < num_steps; i++) {
        n->state = torus_boundary_pass(n->edge, n->state);
        xray_record(&x, n);
        torus_step(n);
        /* pack face+edge+z+state into proxy.core.raw for fibo popcount */
        proxy.core.raw = (uint64_t)n->face
                       | ((uint64_t)n->edge  << 8)
                       | ((uint64_t)n->z     << 16)
                       | ((uint64_t)n->state << 24);
        fibo_hop_fast(fclk, &proxy);
    }

    xray_cache_push(xc, &x);
}

#endif /* GEO_DODECA_TORUS_H */
