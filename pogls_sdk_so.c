/*
 * pogls_sdk_so.c — libpogls.so implementation
 *
 * Build:
 *   gcc -O2 -shared -fPIC -I. -o libpogls.so pogls_sdk_so.c -lm
 */

#include "pogls_sdk.h"
#include "pogls_sdk_so.h"
#include "pogls_stats_out.h"

PoglsHandle pogls_so_open(void)                              { return pogls_open(); }
void        pogls_so_close(PoglsHandle h)                    { pogls_close(h); }
void        pogls_so_write(PoglsHandle h, uint64_t k, uint64_t v) { pogls_write(h, k, v); }
uint64_t    pogls_so_read (PoglsHandle h, uint64_t k)        { return pogls_read(h, k); }
int         pogls_so_has  (PoglsHandle h, uint64_t k)        { return pogls_has(h, k); }
void        pogls_so_qrpn (PoglsHandle h, uint64_t k, uint8_t f){ pogls_qrpn(h, k, f); }
void        pogls_so_rewind(PoglsHandle h)                   { pogls_rewind(h); }
void        pogls_so_stats (PoglsHandle h)                   { pogls_print_stats(h); }
void        pogls_so_stats_out(PoglsHandle h, PoglsStatsOut *out) {
    PoglsCtx *ctx = (PoglsCtx *)h;
    if (!ctx || !out) return;
    pogls_stats_fill(ctx, out);
}
