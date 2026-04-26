/* pogls_checksum.h — stub: XOR-fold primitives (impl inline in callers) */
#ifndef POGLS_CHECKSUM_H
#define POGLS_CHECKSUM_H
#include <stdint.h>
#include <stddef.h>
static inline uint32_t pogls_xor_fold32(const uint8_t *b, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i + 3 < n; i += 4)
        v ^= (uint32_t)b[i]|(uint32_t)b[i+1]<<8|(uint32_t)b[i+2]<<16|(uint32_t)b[i+3]<<24;
    return v;
}
#endif
