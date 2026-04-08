/*
 * pogls_sdk_so.h — .so export symbols (Session 17)
 *
 * Build:
 *   gcc -O2 -shared -fPIC -I. -o libpogls.so pogls_sdk_so.c
 *
 * Link:
 *   gcc -O2 -I. -L. -o myapp myapp.c -lpogls -Wl,-rpath,'$ORIGIN'
 *
 * All symbols use POGLS_API visibility.
 * Caller sees only opaque PoglsHandle — no internal types leaked.
 */

#ifndef POGLS_SDK_SO_H
#define POGLS_SDK_SO_H

#include <stdint.h>
#include "pogls_stats_out.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POGLS_API __attribute__((visibility("default")))

typedef void* PoglsHandle;   /* opaque */

POGLS_API PoglsHandle pogls_so_open   (void);
POGLS_API void        pogls_so_close  (PoglsHandle h);
POGLS_API void        pogls_so_write  (PoglsHandle h, uint64_t key, uint64_t val);
POGLS_API uint64_t    pogls_so_read   (PoglsHandle h, uint64_t key);
POGLS_API int         pogls_so_has    (PoglsHandle h, uint64_t key);
POGLS_API void        pogls_so_qrpn   (PoglsHandle h, uint64_t key, uint8_t failed);
POGLS_API void        pogls_so_rewind (PoglsHandle h);
POGLS_API void        pogls_so_stats  (PoglsHandle h);   /* prints to stdout */
POGLS_API void        pogls_so_stats_out(PoglsHandle h, PoglsStatsOut *out); /* machine-readable */

#ifdef __cplusplus
}
#endif

#endif /* POGLS_SDK_SO_H */
