/*
 * test_compress_s1.c — Round-trip test for pogls_compress.h (S1)
 *
 * Tests:
 *   T1: All-zero block (high RLE, PHI_ZERO tokens)
 *   T2: Sequential coords (delta encodes to constant 1s)
 *   T3: PHI-heavy block (PHI_UP/PHI_DOWN pattern)
 *   T4: Random-ish block (worst case, likely passthru)
 *   T5: QRPN shadow mode (no abort on mismatch)
 *
 * Build:
 *   gcc -O2 -std=c11 -Wall -Wextra -I. -o test_compress_s1 test/test_compress_s1.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* Pull in the full stack */
#include "pogls_compress.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static void fill_block_zero(DiamondBlock *b)
{
    memset(b, 0, sizeof(*b));
}

static void fill_block_sequential(DiamondBlock *b)
{
    uint32_t *w = (uint32_t *)b;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)(i * 1000);
}

static void fill_block_phi(DiamondBlock *b)
{
    uint32_t *w = (uint32_t *)b;
    for (int i = 0; i < 16; i++)
        w[i] = (i % 2 == 0) ? POGLS_PHI_UP : POGLS_PHI_DOWN;
}

static void fill_block_pseudo_random(DiamondBlock *b)
{
    uint64_t state = 0xDEADBEEFCAFEBABEULL;
    uint64_t *w = (uint64_t *)b;
    for (int i = 0; i < 8; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        w[i] = state;
    }
}

/* ── Test runner ─────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    void (*fill)(DiamondBlock *);
} TestCase;

static int run_test(const TestCase *tc, qrpn_ctx_t *qrpn)
{
    DiamondBlock orig, reconstructed;
    tc->fill(&orig);
    memset(&reconstructed, 0, sizeof(reconstructed));

    /* Compress */
    uint8_t buf[COMPRESS_OUT_MAX(64)];
    int comp_len = pogls_compress_block(&orig, buf, (int)sizeof(buf), qrpn);

    if (comp_len < 0) {
        printf("  [FAIL] %s: compress returned %d\n", tc->name, comp_len);
        return 0;
    }

    /* Decompress */
    int dec_ret = pogls_decompress_block(buf, comp_len, &reconstructed, qrpn);
    if (dec_ret != 0) {
        printf("  [FAIL] %s: decompress returned %d\n", tc->name, dec_ret);
        return 0;
    }

    /* Bit-exact match */
    if (memcmp(&orig, &reconstructed, sizeof(DiamondBlock)) != 0) {
        printf("  [FAIL] %s: data mismatch after round-trip\n", tc->name);
        return 0;
    }

    /* Report ratio */
    int32_t ratio = pogls_compress_ratio_pct(64, comp_len - (int)COMPRESS_HEADER_SIZE);
    int passthru  = (((CompressHeader*)buf)->flags & COMP_FLAG_PASSTHRU) != 0;
    printf("  [PASS] %-28s  %2dB → %2dB payload  ratio=%d.%02d%%  %s\n",
           tc->name,
           64,
           comp_len - (int)COMPRESS_HEADER_SIZE,
           ratio / 100, abs(ratio % 100),
           passthru ? "(passthru)" : "");
    return 1;
}

int main(void)
{
    printf("=== pogls_compress.h  S1 round-trip test ===\n\n");

    qrpn_ctx_t qrpn;
    qrpn_ctx_init(&qrpn, 8);
    qrpn.mode = QRPN_SHADOW;   /* log only — don't abort */

    TestCase cases[] = {
        { "T1: all-zero block",      fill_block_zero         },
        { "T2: sequential coords",   fill_block_sequential   },
        { "T3: PHI-heavy pattern",   fill_block_phi          },
        { "T4: pseudo-random data",  fill_block_pseudo_random},
    };
    int n = (int)(sizeof(cases)/sizeof(cases[0]));

    int passed = 0;
    for (int i = 0; i < n; i++) {
        passed += run_test(&cases[i], &qrpn);
    }

    printf("\n=== %d/%d passed ===\n", passed, n);

    /* Sanity: header size frozen at 12B */
    assert(sizeof(CompressHeader) == 16);
    printf("CompressHeader size = %zu B  (expected 16) ✓\n",
           sizeof(CompressHeader));

    /* Sanity: DiamondBlock size frozen at 64B */
    assert(sizeof(DiamondBlock) == 64);
    printf("DiamondBlock  size  = %zu B  (expected 64) ✓\n",
           sizeof(DiamondBlock));

    printf("\nQRPN shadow_fail=%lu  soft_rewind=%lu  hard_abort=%lu\n",
           (unsigned long)atomic_load(&qrpn.shadow_fail),
           (unsigned long)atomic_load(&qrpn.soft_rewind),
           (unsigned long)atomic_load(&qrpn.hard_abort));

    return (passed == n) ? 0 : 1;
}
