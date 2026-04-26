#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_api.h"

static int run(const char *label, const uint8_t *src, size_t len)
{
    size_t bound = pogls_encode_bound(len);
    uint8_t *blob = malloc(bound);
    uint8_t *out  = malloc(len + 64);
    size_t blob_len = bound, out_len = len + 64;

    int r = pogls_encode(src, len, blob, &blob_len);
    if (r != POGLS_OK) {
        printf("  [FAIL] %s encode r=%d\n", label, r); goto done;
    }

    /* peek orig_size */
    size_t peek = pogls_decode_orig_size(blob, blob_len);
    if (peek != len) {
        printf("  [FAIL] %s peek=%zu != %zu\n", label, peek, len); goto done;
    }

    r = pogls_decode(blob, blob_len, out, &out_len);
    if (r != POGLS_OK) {
        printf("  [FAIL] %s decode r=%d\n", label, r); goto done;
    }
    if (out_len != len || (len > 0 && memcmp(out, src, len) != 0)) {
        printf("  [FAIL] %s mismatch\n", label); goto done;
    }
    printf("  [PASS] %s: %zu→blob %zu→orig %zu (%.1f%% ratio)\n",
           label, len, blob_len, out_len,
           len > 0 ? (1.0 - (double)blob_len/len)*100.0 : 0.0);
    free(blob); free(out); return 1;
done:
    free(blob); free(out); return 0;
}

/* wrong magic → POGLS_ERR_MAGIC */
static int test_bad_magic(void)
{
    uint8_t src[64]; memset(src, 0xAB, 64);
    size_t bound = pogls_encode_bound(64);
    uint8_t *blob = malloc(bound); size_t blen = bound;
    pogls_encode(src, 64, blob, &blen);
    blob[0] ^= 0xFF;   /* corrupt magic */
    uint8_t out[64]; size_t olen = 64;
    int r = pogls_decode(blob, blen, out, &olen);
    free(blob);
    if (r == POGLS_ERR_MAGIC) { puts("  [PASS] bad-magic: detected"); return 1; }
    printf("  [FAIL] bad-magic: got=%d\n", r); return 0;
}

/* wrong layout_sig → POGLS_ERR_LAYOUT */
static int test_layout_drift(void)
{
    uint8_t src[64]; memset(src, 0x55, 64);
    size_t bound = pogls_encode_bound(64);
    uint8_t *blob = malloc(bound); size_t blen = bound;
    pogls_encode(src, 64, blob, &blen);
    /* corrupt layout_sig (offset 28 in BlobHeader) */
    uint32_t bad = 0xDEAD;
    memcpy(blob + 28, &bad, 4);
    uint8_t out[64]; size_t olen = 64;
    int r = pogls_decode(blob, blen, out, &olen);
    free(blob);
    if (r == POGLS_ERR_LAYOUT) { puts("  [PASS] layout-drift: detected"); return 1; }
    printf("  [FAIL] layout-drift: got=%d\n", r); return 0;
}

int main(void)
{
    puts("═══════════════════════════════════════");
    puts("  POGLS API Test (pogls_encode/decode)");
    puts("═══════════════════════════════════════");

    /* build test patterns */
    uint8_t small[63], exact[64], multi[512], large[9216];
    for (int i = 0; i < 63;   i++) small[i] = (uint8_t)(i * 3);
    for (int i = 0; i < 64;   i++) exact[i] = (uint8_t)(i * 7);
    for (int i = 0; i < 512;  i++) multi[i] = (uint8_t)(i & 0xFF);
    uint32_t v = 0x9E3779B9u;
    for (int i = 0; i < 9216; i += 4) { v = v*0x9E3779B9u+1; memcpy(large+i,&v,4); }

    int pass = 0, total = 0;
    #define RUN(expr) do { total++; pass += (expr); } while(0)

    RUN(run("small-63B",  small, 63));
    RUN(run("exact-64B",  exact, 64));
    RUN(run("multi-512B", multi, 512));
    RUN(run("large-9216B",large, 9216));
    RUN(test_bad_magic());
    RUN(test_layout_drift());

    puts("═══════════════════════════════════════");
    printf("  Result: %d / %d PASS\n", pass, total);
    puts("═══════════════════════════════════════");
    return (pass == total) ? 0 : 1;
}
