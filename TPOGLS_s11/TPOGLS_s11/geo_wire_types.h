#ifndef GEO_WIRE_TYPES_H
#define GEO_WIRE_TYPES_H

#include <stdint.h>

/* GeoPacket32 (8B canonical — S26+)
 * sig32 = upper 32b of fused isect sig (lower 32b always 0 in phase_mask)
 * flags bit0 = line_ok precomputed on CPU before transfer
 * Shared by geomatrix_kernel.cu and geo_wire_kernel.cu — one definition.
 */
typedef struct __align__(8) {
    uint32_t sig32;
    uint16_t hpos;
    uint8_t  phase;
    uint8_t  flags;
} GeoPacket32;

/* K6 forward declaration — defined in geomatrix_kernel.cu */
__global__ void geo_isect_fused32(
    const uint64_t   * __restrict__ raw,
    const GeoPacket32* __restrict__ pkts32,
    uint8_t          * __restrict__ result,
    int N_bundles);

/* Wire handle + async API — defined in geo_wire_kernel.cu */
typedef struct {
    const uint64_t *d_raw;
    GeoPacket32    *d_pkts32;
    uint8_t        *d_result;
    int             N_bundles;
    cudaStream_t    stream;
} GeoWireHandle;

void geo_wire_launch_async(
    GeoWireHandle     *h,
    const GeoPacket32 *h_pkts32,
    uint8_t           *h_result_out,
    size_t             pkt_bytes,
    int                n_packets);

void geo_wire_launch_multistream(
    const uint64_t    *d_raw,
    GeoPacket32       *d_pkts32,
    uint8_t           *d_result,
    const GeoPacket32 *h_pkts32,
    uint8_t           *h_result_out,
    int                N_packets,
    int                N_STREAMS,
    cudaStream_t      *streams);

#endif /* GEO_WIRE_TYPES_H */
