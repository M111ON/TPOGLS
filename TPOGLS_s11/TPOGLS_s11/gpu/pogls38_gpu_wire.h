/*
 * pogls38_gpu_wire.h — GPU → l38_wire_write() Bridge
 * ══════════════════════════════════════════════════════════════════════
 *
 * Wires pogls38_gpu_final.cu SoA output → l38_wire_write() per cell.
 *
 * GPU kernel produces (SoA, N elements):
 *   hilbert[N]  uint32_t  — spatial address (phi-scatter → morton → hilbert)
 *   lane[N]     uint8_t   — hilbert % 54 (RUBIK_LANES)
 *   audit[N]    uint8_t   — 0=pass (addr%17==1), 1=fail (iso check)
 *
 * Feed path:
 *   pogls38_phi_scatter<<<>>> writes to d_hil, d_lane, d_audit
 *         ↓  (cudaMemcpy to host or pinned memory)
 *   l38_gpu_batch_feed(pipe, h_hil, h_lane, h_audit, N)
 *         ↓  per cell: audit gate → l38_wire_write()
 *   L38WirePipeline (wiring.h) handles everything downstream
 *
 * Audit gate (before wire_write):
 *   audit[i] == 1 → GHOST (iso fail — addr not on 17n lattice)
 *   audit[i] == 0 → PASS  → call l38_wire_write()
 *
 * Stats tracked:
 *   fed_pass    — cells that entered l38_wire_write()
 *   fed_ghost   — cells dropped at iso gate
 *   fed_blocked — cells dropped at pressure block
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS38_GPU_WIRE_H
#define POGLS38_GPU_WIRE_H

#include <stdint.h>
#include <string.h>
#include "pogls_38_wiring.h"

/* ── constants ────────────────────────────────────────────────────── */
#define L38_GPU_BATCH_MAX   (2u * 1024u * 1024u)   /* 2M = GPU kernel N */
#define L38_GPU_SV          34u                    /* step vector (FROZEN from bench) */
#define L38_GPU_BASE        1u                     /* base addr (FROZEN) */

/* ── feed stats ──────────────────────────────────────────────────── */
typedef struct {
    uint64_t  batches;        /* total l38_gpu_batch_feed() calls       */
    uint64_t  cells_total;    /* total cells processed                  */
    uint64_t  fed_pass;       /* cells that entered l38_wire_write()    */
    uint64_t  fed_ghost;      /* cells dropped: iso fail (audit==1)     */
    uint64_t  fed_blocked;    /* cells dropped: pressure BLOCK          */
    uint64_t  fed_anomaly;    /* cells that triggered repair path       */
} L38GpuFeedStats;

/* ── per-cell result (optional, pass NULL to skip) ───────────────── */
typedef struct {
    uint32_t  cell_id;
    uint8_t   head_id;
    uint8_t   quad_ok;
    uint8_t   anomaly;
    uint8_t   result;    /* 0=ghost 1=pass 2=blocked */
} L38GpuCellResult;

/* ══════════════════════════════════════════════════════════════════
 * l38_gpu_batch_feed
 *
 * Takes GPU kernel SoA output (host-side after cudaMemcpy)
 * and feeds each element through l38_wire_write().
 *
 * Parameters:
 *   pipe     — fully-initialized L38WirePipeline (from wiring.h)
 *   h_hil    — host array: hilbert[N]   (uint32_t)
 *   h_lane   — host array: lane[N]      (uint8_t)  — informational only
 *   h_audit  — host array: audit[N]     (uint8_t)  — 0=pass, 1=fail
 *   N        — element count (must be <= L38_GPU_BATCH_MAX)
 *   stats    — stats accumulator (must not be NULL)
 *   results  — optional per-cell results (pass NULL to skip)
 *
 * Returns: number of cells that passed into l38_wire_write()
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t l38_gpu_batch_feed(
    L38WirePipeline    *pipe,
    const uint32_t     *h_hil,
    const uint8_t      *h_lane,
    const uint8_t      *h_audit,
    uint32_t            N,
    L38GpuFeedStats    *stats,
    L38GpuCellResult   *results)   /* optional, may be NULL */
{
    if (!pipe || !h_hil || !h_audit || !stats) return 0;
    if (N > L38_GPU_BATCH_MAX) N = L38_GPU_BATCH_MAX;

    uint32_t passed = 0;
    stats->batches++;
    stats->cells_total += N;

    for (uint32_t i = 0; i < N; i++) {

        /* ── Audit gate ──────────────────────────────────────────── */
        if (h_audit[i] != 0) {
            /* iso fail: addr % 17 != 1  → not on 17n lattice → GHOST */
            stats->fed_ghost++;
            if (results) {
                results[i].result  = 0; /* ghost */
                results[i].cell_id = 0;
                results[i].head_id = 0;
                results[i].quad_ok = 0;
                results[i].anomaly = 0;
            }
            continue;
        }

        /* ── Build angular_addr from hilbert index ───────────────── */
        /* hilbert is the spatial address in PHI space.
         * Reconstruct the original addr: base + sv * i
         * (same formula used in GPU kernel to generate addresses)
         * This is exact — no information loss.                        */
        uint64_t angular_addr = (uint64_t)L38_GPU_BASE +
                                (uint64_t)L38_GPU_SV * (uint64_t)i;

        /* ── Value: pack hilbert + lane into 64-bit value ───────── */
        /* Encoding: hilbert in [31:8], lane in [7:0]
         * Downstream can unpack if needed.                           */
        uint64_t value = ((uint64_t)h_hil[i] << 8) |
                         (uint64_t)(h_lane ? h_lane[i] : (h_hil[i] % 54u));

        /* ── Wire write ──────────────────────────────────────────── */
        L38WriteResult wr = l38_wire_write(pipe, angular_addr, value);

        /* ── Track stats ─────────────────────────────────────────── */
        if (wr.pressure_level == L38_PRESSURE_BLOCK) {
            stats->fed_blocked++;
            if (results) {
                results[i].result  = 2; /* blocked */
                results[i].cell_id = wr.cell_id;
                results[i].head_id = wr.head_id;
                results[i].quad_ok = wr.quad_ok;
                results[i].anomaly = wr.anomaly;
            }
        } else {
            stats->fed_pass++;
            passed++;
            if (wr.anomaly) stats->fed_anomaly++;
            if (results) {
                results[i].result  = 1; /* pass */
                results[i].cell_id = wr.cell_id;
                results[i].head_id = wr.head_id;
                results[i].quad_ok = wr.quad_ok;
                results[i].anomaly = wr.anomaly;
            }
        }
    }

    return passed;
}

/* ══════════════════════════════════════════════════════════════════
 * l38_gpu_stats_print — print feed stats
 * ══════════════════════════════════════════════════════════════════ */
static inline void l38_gpu_stats_print(const L38GpuFeedStats *s)
{
    if (!s) return;
    uint64_t t = s->cells_total ? s->cells_total : 1;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  GPU → Wire Feed Stats                          ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ Batches:     %10llu                          ║\n",
           (unsigned long long)s->batches);
    printf("║ Total cells: %10llu                          ║\n",
           (unsigned long long)s->cells_total);
    printf("║ Pass:        %10llu (%3llu%%)                  ║\n",
           (unsigned long long)s->fed_pass,
           (unsigned long long)(s->fed_pass * 100u / t));
    printf("║ Ghost(iso):  %10llu (%3llu%%)                  ║\n",
           (unsigned long long)s->fed_ghost,
           (unsigned long long)(s->fed_ghost * 100u / t));
    printf("║ Blocked(BP): %10llu (%3llu%%)                  ║\n",
           (unsigned long long)s->fed_blocked,
           (unsigned long long)(s->fed_blocked * 100u / t));
    printf("║ Anomaly:     %10llu (%3llu%%)                  ║\n",
           (unsigned long long)s->fed_anomaly,
           (unsigned long long)(s->fed_anomaly * 100u / t));
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Colab integration stub
 *
 * In Colab notebook, after running GPU kernel:
 *
 *   // 1. Allocate host buffers
 *   uint32_t *h_hil  = (uint32_t*)malloc(N*4);
 *   uint8_t  *h_lane = (uint8_t*) malloc(N);
 *   uint8_t  *h_aud  = (uint8_t*) malloc(N);
 *
 *   // 2. Run GPU kernel
 *   pogls38_phi_scatter<<<BLK,TPB>>>(d_hil, d_lane, d_aud, BASE, SV, N, 1);
 *   cudaDeviceSynchronize();
 *
 *   // 3. Copy to host
 *   cudaMemcpy(h_hil,  d_hil,  N*4, cudaMemcpyDeviceToHost);
 *   cudaMemcpy(h_lane, d_lane, N,   cudaMemcpyDeviceToHost);
 *   cudaMemcpy(h_aud,  d_aud,  N,   cudaMemcpyDeviceToHost);
 *
 *   // 4. Feed to POGLS38 (THE WIRE — 1 line per batch)
 *   uint32_t ok = l38_gpu_batch_feed(&pipe, h_hil, h_lane, h_aud,
 *                                    N, &stats, NULL);
 *
 *   // 5. Print stats
 *   l38_gpu_stats_print(&stats);
 *
 * ══════════════════════════════════════════════════════════════════ */

#endif /* POGLS38_GPU_WIRE_H */
