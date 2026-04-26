/*
 * test_geo_net_wire.c — S24: geo_net.h compile + basic route test
 */
#include <stdio.h>
#include <assert.h>
#include "core/geo_config.h"
#include "core/geo_cylinder.h"
#include "core/geo_thirdeye.h"
#include "core/geo_net.h"
#include "core/geo_pipeline_wire.h"

int main(void) {
    GeoSeed seed = { .gen2 = 0xDEADBEEFCAFEBABEULL,
                     .gen3 = 0x123456789ABCDEF0ULL };

    /* ── test 1: geo_net basic route ── */
    GeoNet gn;
    geo_net_init(&gn, seed);

    int ok = 0;
    for (uint32_t i = 0; i < CYL_FULL_N; i++) {
        GeoNetAddr a = geo_net_route(&gn, i, 0, 0, seed);
        assert(a.spoke    < 6);
        assert(a.slot     < GEO_SLOTS);
        assert(a.face     < GEO_FACES);
        assert(a.inv_spoke< 6);
        assert(a.inv_spoke == geo_spoke_invert(a.spoke));
        ok++;
    }
    printf("[geo_net]       route %d/%d addrs  ops=%u  state=%s  PASS\n",
           ok, CYL_FULL_N, gn.op_count, geo_net_state_name(&gn));

    /* ── test 2: audit point detection ── */
    int audit_count = 0;
    geo_net_init(&gn, seed);
    for (uint32_t i = 0; i < CYL_FULL_N; i++) {
        GeoNetAddr a = geo_net_route(&gn, i, 0, 0, seed);
        if (geo_net_is_audit_point(&a)) audit_count++;
    }
    printf("[geo_net]       audit_points=%d (expect ~%d)  PASS\n",
           audit_count, CYL_FULL_N / GN_GROUP_SIZE);

    /* ── test 3: geo_pipeline_wire step ── */
    GeoPipeline p;
    geo_pipeline_init(&p, seed);
    int pipe_ok = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        GeoPacketWire pkt;
        uint8_t r = geo_pipeline_step(&p, i, i * 7, 0, &pkt);
        assert(pkt.spoke < 6);
        (void)r;
        pipe_ok++;
    }
    printf("[geo_pipeline]  %d steps  audit_fails=%u  PASS\n",
           pipe_ok, p.audit_fails);

    printf("\nS24: 3/3 PASS\n");
    return 0;
}
