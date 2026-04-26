/*
 * pogls38_hydra_thin.h — Thin Hydra: split + detach for GPU→Federation
 * ══════════════════════════════════════════════════════════════════════
 *
 * Phase: split + detach only (mesh = phase ถัดไป)
 *
 *   split    → batch scaling (54 lanes ÷ 3 = 18/slice, max 2 active)
 *   detached → anomaly quarantine + low-priority retry loop
 *
 * Flow:
 *   GPU batch → hydra_filter(audit)
 *                   ↓ pass          ↓ fail (anomaly)
 *             split_if_needed()   push_detached()
 *                   ↓                   ↓ (retry loop, low-priority)
 *             l38_fed_batch_feed()  l38_fed_batch_feed()
 *
 * Detached retry:
 *   quarantine queue → feed back as low-priority batch
 *   → anomaly learning free (retry count tracked)
 *   → max retries: L38_THIN_DETACH_MAX_RETRY (default 3)
 *
 * Split rule (from pogls_38_slice.h, FROZEN):
 *   54 lanes ÷ 3 = 18 lanes/slice
 *   Slice 0: lanes  0-17
 *   Slice 1: lanes 18-35
 *   Slice 2: lanes 36-53
 *   max 2 active slices at once (split-world phase limit)
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pogls_federation.h"
#include "pogls38_fed_bridge.h"

/* ── constants ─────────────────────────────────────────────────── */
#define L38_THIN_LANES_PER_SLICE  18u   /* 54/3 — FROZEN                */
#define L38_THIN_SLICE_COUNT       3u   /* A/B/C — FROZEN               */
#define L38_THIN_MAX_ACTIVE        2u   /* max active slices (phase)    */
#define L38_THIN_DETACH_MAX_RETRY  3u   /* anomaly retry limit          */
#define L38_THIN_DETACH_QUEUE_MAX  1024u /* quarantine capacity          */

/* ── detached entry ─────────────────────────────────────────────── */
typedef struct {
    uint32_t  hil;
    uint8_t   lane;
    uint8_t   audit;        /* original audit flag                      */
    uint8_t   retry_count;  /* times re-attempted                       */
    uint8_t   slice_id;     /* 0/1/2                                    */
    uint64_t  angular_addr;
    uint64_t  value;
} L38DetachEntry;           /* 24B                                      */

/* ── detach queue (simple ring) ─────────────────────────────────── */
typedef struct {
    L38DetachEntry  buf[L38_THIN_DETACH_QUEUE_MAX];
    uint32_t        head;
    uint32_t        tail;
    uint64_t        total_pushed;
    uint64_t        total_retried;
    uint64_t        total_dropped;  /* exceeded max retry               */
} L38DetachQueue;

static inline void l38_detach_init(L38DetachQueue *q)
{ if (q) memset(q, 0, sizeof(*q)); }

static inline int l38_detach_push(L38DetachQueue *q, const L38DetachEntry *e)
{
    uint32_t next = (q->tail + 1) & (L38_THIN_DETACH_QUEUE_MAX - 1);
    if (next == q->head) return -1;  /* full */
    q->buf[q->tail] = *e;
    q->tail = next;
    q->total_pushed++;
    return 0;
}

static inline int l38_detach_pop(L38DetachQueue *q, L38DetachEntry *out)
{
    if (q->head == q->tail) return 0;
    *out = q->buf[q->head];
    q->head = (q->head + 1) & (L38_THIN_DETACH_QUEUE_MAX - 1);
    return 1;
}

static inline uint32_t l38_detach_depth(const L38DetachQueue *q)
{ return (q->tail - q->head) & (L38_THIN_DETACH_QUEUE_MAX - 1); }

/* ── thin hydra context ─────────────────────────────────────────── */
typedef struct {
    L38DetachQueue  detached;       /* anomaly quarantine queue         */
    uint32_t        active_slices;  /* bitmap: bit0=sliceA, bit1=B      */
    uint64_t        total_splits;
    uint64_t        total_fed;      /* cells routed to fed              */
    uint64_t        total_anomaly;  /* cells pushed to detach           */
} L38HydraThin;

static inline void l38_hydra_thin_init(L38HydraThin *h)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    l38_detach_init(&h->detached);
    h->active_slices = 0x1u;  /* slice A active by default            */
}

/* ── slice_id from lane ─────────────────────────────────────────── */
static inline uint8_t l38_lane_to_slice(uint8_t lane)
{
    return (uint8_t)(lane / L38_THIN_LANES_PER_SLICE);  /* 0,1,2      */
}

/* ── hydra_filter: decide pass or quarantine ────────────────────── */
/*
 * audit==1 → iso fail → quarantine (not outright drop)
 * slice inactive → also quarantine (retry when slice activates)
 * Returns 1 = pass, 0 = quarantine
 */
static inline int l38_hydra_filter(L38HydraThin *h,
                                    uint8_t lane,
                                    uint8_t audit)
{
    if (audit != 0) return 0;                     /* iso fail          */
    uint8_t sid = l38_lane_to_slice(lane);
    if (!((h->active_slices >> sid) & 1u)) return 0; /* slice inactive */
    return 1;
}

/* ── split_if_needed: activate slice B if load warrants ──────────── */
/*
 * Simple heuristic: if detach queue > 25% full → activate next slice.
 * max 2 active (L38_THIN_MAX_ACTIVE).
 */
static inline void l38_split_if_needed(L38HydraThin *h)
{
    uint32_t active = __builtin_popcount(h->active_slices);
    if (active >= L38_THIN_MAX_ACTIVE) return;

    uint32_t depth = l38_detach_depth(&h->detached);
    if (depth > L38_THIN_DETACH_QUEUE_MAX / 4) {
        /* activate next slice */
        for (uint32_t s = 0; s < L38_THIN_SLICE_COUNT; s++) {
            if (!((h->active_slices >> s) & 1u)) {
                h->active_slices |= (1u << s);
                h->total_splits++;
                break;
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * l38_hydra_batch_feed — main entry (replaces l38_fed_batch_feed)
 *
 * Thin Hydra wrapper:
 *   1. filter each cell (audit + slice active)
 *   2. pass  → accumulate into pass sub-batch → l38_fed_batch_feed
 *   3. fail  → push_detached (quarantine)
 *   4. after batch: split_if_needed + drain detach queue (retry)
 * ══════════════════════════════════════════════════════════════════ */
static inline uint32_t l38_hydra_batch_feed(
    L38HydraThin       *h,
    FederationCtx      *fed,
    const uint32_t     *h_hil,
    const uint8_t      *h_lane,
    const uint8_t      *h_audit,
    uint32_t            N,
    L38FedStats        *stats)
{
    if (!h || !fed || !h_hil || !h_audit || !stats) return 0;
    if (N > L38_FED_BATCH_MAX) N = L38_FED_BATCH_MAX;

    /* ── pass path: feed directly ─────────────────────────────── */
    /* build pass sub-arrays on stack (max 4096 per chunk) */
#define _CHUNK 4096u
    uint32_t pass_hil [_CHUNK];
    uint8_t  pass_lane[_CHUNK];
    uint8_t  pass_aud [_CHUNK];
    uint32_t chunk_n = 0;
    uint32_t total_passed = 0;

    for (uint32_t i = 0; i < N; i++) {
        uint8_t lane = h_lane ? h_lane[i] : (uint8_t)(h_hil[i] % 54u);
        uint8_t audit = h_audit[i];

        if (l38_hydra_filter(h, lane, audit)) {
            pass_hil [chunk_n] = h_hil[i];
            pass_lane[chunk_n] = lane;
            pass_aud [chunk_n] = 0u;       /* already filtered: iso=0  */
            chunk_n++;
            if (chunk_n == _CHUNK) {
                total_passed += l38_fed_batch_feed(fed, pass_hil,
                                                   pass_lane, pass_aud,
                                                   chunk_n, stats);
                chunk_n = 0;
            }
        } else {
            /* quarantine */
            L38DetachEntry e;
            e.hil         = h_hil[i];
            e.lane        = lane;
            e.audit       = audit;
            e.retry_count = 0;
            e.slice_id    = l38_lane_to_slice(lane);
            e.angular_addr = (uint64_t)L38_FED_BASE
                           + (uint64_t)L38_FED_SV * (uint64_t)i;
            e.value        = ((uint64_t)h_hil[i] << 8) | lane;
            l38_detach_push(&h->detached, &e);
            h->total_anomaly++;
        }
    }
    /* flush remaining */
    if (chunk_n > 0) {
        total_passed += l38_fed_batch_feed(fed, pass_hil,
                                           pass_lane, pass_aud,
                                           chunk_n, stats);
    }
#undef _CHUNK

    h->total_fed += total_passed;

    /* ── split_if_needed ─────────────────────────────────────── */
    l38_split_if_needed(h);

    /* ── retry detach queue (low-priority) ───────────────────── */
    uint32_t retry_limit = l38_detach_depth(&h->detached);
    for (uint32_t r = 0; r < retry_limit; r++) {
        L38DetachEntry e;
        if (!l38_detach_pop(&h->detached, &e)) break;

        e.retry_count++;
        uint8_t slice_now_active = (h->active_slices >> e.slice_id) & 1u;

        if (e.retry_count > L38_THIN_DETACH_MAX_RETRY || e.audit != 0) {
            /* exceeded retries or iso fail — drop */
            h->detached.total_dropped++;
            continue;
        }

        if (!slice_now_active) {
            /* slice still inactive — re-queue */
            l38_detach_push(&h->detached, &e);
            h->detached.total_retried++;
            continue;
        }

        /* retry: re-feed as single-cell batch */
        uint32_t rh = e.hil;
        uint8_t  rl = e.lane, ra = 0u;
        total_passed += l38_fed_batch_feed(fed, &rh, &rl, &ra, 1, stats);
        h->detached.total_retried++;
        h->total_fed++;
    }

    return total_passed;
}

/* ── stats print ────────────────────────────────────────────────── */
static inline void l38_hydra_thin_stats(const L38HydraThin *h)
{
    if (!h) return;
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  Hydra Thin Stats                               ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ active_slices: 0x%02x  total_splits: %-8llu    ║\n",
           h->active_slices,
           (unsigned long long)h->total_splits);
    printf("║ total_fed:     %-10llu                       ║\n",
           (unsigned long long)h->total_fed);
    printf("║ total_anomaly: %-10llu                       ║\n",
           (unsigned long long)h->total_anomaly);
    printf("║ detach_depth:  %-10u                        ║\n",
           l38_detach_depth(&h->detached));
    printf("║ retried:       %-10llu  dropped: %-8llu  ║\n",
           (unsigned long long)h->detached.total_retried,
           (unsigned long long)h->detached.total_dropped);
    printf("╚══════════════════════════════════════════════════╝\n\n");
}
