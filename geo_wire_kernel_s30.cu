/*
 * geo_wire_kernel.cu — S30 wire integration
 * Exposes geo_wire_launch_async(): K7 fused32_warp behind a clean async API.
 * K6 (geo_isect_fused32) retired — K7 warp-shuffle reduce eliminates __syncthreads().
 *
 * Caller owns all CUDA memory. This file adds no extra allocations.
 *
 * Compile: nvcc -O2 -arch=sm_75 -o geo_wire geo_wire_kernel_s30.cu geomatrix_kernel_s29.cu
 */

#include <stdint.h>
#include <stdio.h>
#include "geo_wire_types_s29.h"   /* GeoPacket32, GeoWireHandle, declarations */

/* ── geo_wire_launch_async ──────────────────────────────────────────
 * Fires H2D + K7 + optional D2H in a single stream.
 * Returns immediately; caller must cudaStreamSynchronize(h->stream).
 *
 * h_pkts32     — pinned host source (cudaMallocHost)
 * h_result_out — pinned host destination (NULL = skip D2H)
 * pkt_bytes    — sizeof(GeoPacket32) × N_packets
 * n_packets    — total packet count
 */
void geo_wire_launch_async(
    GeoWireHandle     *h,
    const GeoPacket32 *h_pkts32,
    uint8_t           *h_result_out,
    size_t             pkt_bytes,
    int                n_packets)
{
    cudaMemcpyAsync(h->d_pkts32, h_pkts32,
                    pkt_bytes, cudaMemcpyHostToDevice, h->stream);

    int grid = (h->N_bundles + 31) / 32;
    geo_isect_fused32_warp<<<grid, 256, 0, h->stream>>>(
        h->d_raw, h->d_pkts32, h->d_result, h->N_bundles);

    if (h_result_out)
        cudaMemcpyAsync(h_result_out, h->d_result,
                        (size_t)n_packets, cudaMemcpyDeviceToHost, h->stream);
    /* caller: cudaStreamSynchronize(h->stream) when results needed */
}

/* ── geo_wire_launch_multistream ────────────────────────────────────
 * Split N_packets across N_STREAMS — each stream: H2D → K7 → D2H.
 * d_raw is shared/readonly across all streams (same bitboard).
 * d_pkts32 / d_result must be contiguous; chunk i at offset i*chunk.
 */
void geo_wire_launch_multistream(
    const uint64_t    *d_raw,
    GeoPacket32       *d_pkts32,
    uint8_t           *d_result,
    const GeoPacket32 *h_pkts32,
    uint8_t           *h_result_out,
    int                N_packets,
    int                N_STREAMS,
    cudaStream_t      *streams)
{
    int    chunk      = N_packets / N_STREAMS;
    int    chunk_bund = chunk / 8;               /* GEO_BUNDLE_WORDS = 8 */
    int    grid       = (chunk_bund + 31) / 32;
    size_t pkt_sz     = sizeof(GeoPacket32);

    for (int s = 0; s < N_STREAMS; s++) {
        size_t off = (size_t)s * chunk;

        cudaMemcpyAsync(d_pkts32 + off, h_pkts32 + off,
                        (size_t)chunk * pkt_sz,
                        cudaMemcpyHostToDevice, streams[s]);

        geo_isect_fused32_warp<<<grid, 256, 0, streams[s]>>>(
            d_raw, d_pkts32 + off, d_result + off, chunk_bund);

        if (h_result_out)
            cudaMemcpyAsync(h_result_out + off, d_result + off,
                            (size_t)chunk,
                            cudaMemcpyDeviceToHost, streams[s]);
    }
}
