#include <stdio.h>
#include <stdint.h>

#include "core/pogls_5world_core.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("  OK   %s\n", label); g_pass++; } \
    else { printf("  FAIL %s\n", label); g_fail++; } \
} while (0)

int main(void)
{
    printf("=== POGLS 5-World Core Integrity Test ===\n");

    P5WorldState s = p5world_build(
        20260405u,
        5u,
        0x1122334455667788ULL,
        0x8899AABBCCDDEEFFULL,
        0x0ULL
    );

    P5WorldCheck ok = p5world_verify(&s);

    printf("  route bit (0=icosa,1=dodeca): %u\n", p5world_route_bit(s.addr_icosa));
    printf("  addr_icosa=0x%05x addr_dodeca=0x%05x addr_fibo=0x%05x\n",
           s.addr_icosa, s.addr_dodeca, s.addr_fibo);

    CHECK("required checks pass", ok.all_required_ok == 1u);
    CHECK("optional aggregate xor==0 (when c_fibo==0)", ok.aggregate_ok == 1u);

    P5WorldState agg_non_zero = s;
    agg_non_zero.c_fibo = 0x99BBDDFF11335577ULL;
    agg_non_zero.i_fibo = p5world_not64(agg_non_zero.c_fibo);
    P5WorldCheck agg_non_zero_res = p5world_verify(&agg_non_zero);
    CHECK("optional aggregate can be disabled by payload", agg_non_zero_res.aggregate_ok == 0u);

    P5WorldState bad = s;
    bad.addr_n_dodeca ^= 0x1u;
    bad.i_n_icosa ^= 0x10u;

    P5WorldCheck bad_res = p5world_verify(&bad);
    CHECK("detect broken negative dual mapping", bad_res.neg_dual_ok == 0u);
    CHECK("detect broken invert symmetry", bad_res.invert_cross_ok == 0u);

    printf("\nResult: %d/%d passed\n", g_pass, g_pass + g_fail);
    return (g_fail == 0) ? 0 : 1;
}
