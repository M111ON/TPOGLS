/*
 * pogls38_fed_bridge.h — POGLS38 GPU → Federation Bridge
 * ══════════════════════════════════════════════════════════════════════
 *
 * Feeds GPU kernel SoA output directly into FederationCtx,
 * bypassing L38WirePipeline entirely.
 *
 * GPU kernel produces (SoA, N elements, host-side after cudaMemcpy):
 *   h_hil[N]   uint32_t  — hilbert spatial address
 *   h_lane[N]  uint8_t   — h_hil % 54 (pre-computed on GPU)
 *   h_audit[N] uint8_t   — 0=pass (addr%17==1), 1=fail (iso)
 *
 * Feed path:
 *   pogls38_phi_scatter<<<>>> → cudaMemcpy to host
 *         ↓
 *   l38_fed_batch_feed(fed, h_hil, h_lane, h_audit, N, stats)
 *         ↓  per cell: audit gate → v38_to_packed → fed_write
 *   FederationCtx handles everything downstream
 *
 * Packed cell layout (must match fed_gate):
 *   hil  = bit[19:0]   → h_hil[i] % 54 (= h_lane[i])
 *   lane = bit[25:20]  → h_lane[i]
 *   iso  = bit[26]     → h_audit[i]
 *   invariant: hil % 54 == lane ✓
 *
 * FROZEN constants (from GPU bench):
 *   L38_GPU_SV   = 34   step vector
 *   L38_GPU_BASE = 1    base addr
 * ══════════════════════════════════════════════════════════════════════
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include "pogls_federation.h"

/* ── constants (FROZEN) ─────────────────────────────────────────── */
#define L38_FED_SV            34u   /* step vector — FROZEN from bench  */
#define L38_FED_BASE          1u    /* base addr   — FROZEN             */
#define L38_FED_BATCH_MAX     (2u * 1024u * 1024u)   /* 2M cells/batch  */

/* ── feed stats ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t  batches;        /* total l38_fed_batch_feed() calls       */
    uint64_t  cells_total;    /* total cells processed                  */
    uint64_t  fed_pass;       /* cells that entered fed_write() → PASS  */
    uint64_t  fed_ghost;      /* cells ghosted (iso fail or warm-up)    */
    uint64_t  fed_drop;       /* cells dropped by fed_gate              */
} L38FedStats;

/* ── packed cell builder ────────────────────────────────────────── */
/*
 * GPU provides h_lane[i] = h_hil[i] % 54 pre-computed.
 * Use h_lane directly — no re-computation needed.
 * hil = h_lane ensures hil%54 == lane invariant.
 */
static inline uint32_t l38_to_packed(uint8_t lane, uint8_t iso)
{
    uint32_t hil = (uint32_t)lane;        /* hil%54 == lane ✓           */
    return  (hil & 0xFFFFFu)              /* bit[19:0] = hil            */
          | ((uint32_t)(lane & 0x3Fu) << 20) /* bit[25:20] = lane       */
          | ((uint32_t)(iso  & 1u)    << 26);/* bit[26]    = iso        */
}

/* ── commit threshold (tunable) ────────────────────────────────── */
#ifndef L38_FED_COMMIT_THRESHOLD
#define L38_FED_COMMIT_THRESHOLD  256u  /* op_count trigger — tune per workload */
#endif

/* ══════════════════════════════════════════════════════════════════
 * l38_fed_batch_feed
 *
 * Takes GPU kernel SoA output (host-side after cudaMemcpy)
 * and feeds each element through fed_write().
 *
 * Commit logic (batch-driven, not cycle-driven):
 *   commit fires when EITHER:
 *     a) batch ends AND fed->op_count >= L38_FED_COMMIT_THRESHOLD
 *     b) commit_pending=1 (set externally, e.g. from GPU cycle hint)
 *   → GPU = throughput-driven, NOT time-driven (no TC_CYCLE=720)
 *
 * Parameters:
 *   fed      — initialized FederationCtx
 *   h_hil    — hilbert[N]  uint32_t
 *   h_lane   — lane[N]     uint8_t  (= h_hil%54, pre-computed on GPU)
 *   h_audit  — audit[N]    uint8_t  (0=pass, 1=iso fail)
 *   N        — element count (clamped to L38_FED_BATCH_MAX)
 *   stats    — stats accumulator (must not be NULL)
 *
 * Returns: number of cells that reached GATE_PASS
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t l38_fed_batch_feed(
    FederationCtx      *fed,
    const uint32_t     *h_hil,
    const uint8_t      *h_lane,
    const uint8_t      *h_audit,
    uint32_t            N,
    L38FedStats        *stats)
{
    if (!fed || !h_hil || !h_audit || !stats) return 0;
    if (N > L38_FED_BATCH_MAX) N = L38_FED_BATCH_MAX;

    uint32_t passed = 0;
    stats->batches++;
    stats->cells_total += N;

    for (uint32_t i = 0; i < N; i++) {

        /* lane: use pre-computed h_lane if available, else derive */
        uint8_t lane = h_lane ? h_lane[i] : (uint8_t)(h_hil[i] % 54u);
        uint8_t iso  = h_audit[i] & 1u;

        /* angular_addr: deterministic from batch position (FROZEN formula) */
        uint64_t angular_addr = (uint64_t)L38_FED_BASE
                              + (uint64_t)L38_FED_SV * (uint64_t)i;

        /* value: hilbert packed with lane (reversible) */
        uint64_t value = ((uint64_t)h_hil[i] << 8)
                       | (uint64_t)lane;

        /* packed cell for fed_gate */
        uint32_t packed = l38_to_packed(lane, iso);

        GateResult gr = fed_write(fed, packed, angular_addr, value);

        if (gr == GATE_PASS) {
            stats->fed_pass++;
            passed++;
        } else if (gr == GATE_GHOST) {
            stats->fed_ghost++;
        } else {
            stats->fed_drop++;
        }
    }

    /* ── hybrid commit: batch-end + threshold ───────────────────── */
    /* commit when op_count crosses threshold OR commit_pending set  */
    if (fed->op_count >= L38_FED_COMMIT_THRESHOLD || fed->commit_pending) {
        fed->commit_pending = 0;   /* clear before commit — re-entry safe */
        fed_commit(fed);
    }

    return passed;
}

/* ══════════════════════════════════════════════════════════════════
 * l38_fed_stats_print
 * ══════════════════════════════════════════════════════════════════ */
static inline void l38_fed_stats_print(const L38FedStats *s)
{
    if (!s) return;
    uint64_t t = s->cells_total ? s->cells_total : 1;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  POGLS38 GPU → Federation Feed Stats           ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ Batches:     %10llu                          ║\n",
           (unsigned long long)s->batches);
    printf("║ Total cells: %10llu                          ║\n",
           (unsigned long long)s->cells_total);
    printf("║ Pass:        %10llu (%3llu%%)                  ║\n",
           (unsigned long long)s->fed_pass,
           (unsigned long long)(s->fed_pass * 100u / t));
    printf("║ Ghost:       %10llu (%3llu%%)                  ║\n",
           (unsigned long long)s->fed_ghost,
           (unsigned long long)(s->fed_ghost * 100u / t));
    printf("║ Drop:        %10llu (%3llu%%)                  ║\n",
           (unsigned long long)s->fed_drop,
           (unsigned long long)(s->fed_drop * 100u / t));
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Colab integration (CPU host path — no CUDA headers needed)
 *
 *   // after cudaMemcpy to host:
 *   L38FedStats stats = {0};
 *   uint32_t ok = l38_fed_batch_feed(&fed, h_hil, h_lane, h_audit,
 *                                    N, &stats);
 *   l38_fed_stats_print(&stats);
 *
 *   // commit at cycle boundary (every 720 GPU steps or manually):
 *   fed_commit(&fed);
 * ══════════════════════════════════════════════════════════════════ */
