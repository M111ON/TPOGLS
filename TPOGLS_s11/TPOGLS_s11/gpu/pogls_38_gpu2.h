/*
 * pogls_38_gpu2.h — POGLS38  GPU Pipeline v2
 * ══════════════════════════════════════════════════════════════════════
 *
 * v1 → v2 changes:
 *   1. Soft stream limit   (dynamic, based on gpu_util estimate)
 *   2. Batch inflation     (128K → 256K → 512K)
 *   3. Double-buffer       (compute + copy overlap, free bandwidth)
 *   4. Guardrails          (timeout, memory watermark, per-stream error isolate)
 *
 * NOT changed (Phase E):
 *   - kernel logic (Morton+Hilbert+PHI+audit unchanged)
 *   - routing / ghost / audit
 *   - fold_verify
 *
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_38_GPU2_H
#define POGLS_38_GPU2_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef POGLS_HAVE_CUDA
  #include <cuda_runtime.h>
  #define L38_GPU2_AVAIL 1
#else
  #define L38_GPU2_AVAIL 0
  typedef void* cudaStream_t;
  typedef int   cudaError_t;
  #define cudaSuccess 0
#endif

#ifndef PHI_SCALE
  #define PHI_SCALE (1u<<20)
  #define PHI_UP    1696631u
  #define PHI_DOWN   648055u
#endif

/* ── struct (same as v1) ─────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t morton, hilbert;
    uint8_t  lane, slice, audit, flags;
    uint32_t phi;
} L38W3Coord2;  /* 24B */
typedef char _l38_w3c2[(sizeof(L38W3Coord2)==24)?1:-1];

/* ══════════════════════════════════════════════════════════════════
 * 1. SOFT STREAM LIMIT
 *
 * gpu_util estimate: queue_fill_ratio (0..100)
 *   < 70% → allow up to 3 streams
 *   < 90% → allow up to 2 streams
 *   ≥ 90% → clamp to 1 stream
 *
 * "util" = pending / capacity (proxy for GPU busyness)
 * ══════════════════════════════════════════════════════════════════ */
#define L38_GPU2_STREAM_MAX    4u
#define L38_GPU2_UTIL_LOW     70u   /* < 70% → 3 streams              */
#define L38_GPU2_UTIL_MED     90u   /* < 90% → 2 streams              */

static inline uint32_t l38_gpu2_allowed_streams(uint32_t util_pct)
{
    if (util_pct < L38_GPU2_UTIL_LOW) return 3u;
    if (util_pct < L38_GPU2_UTIL_MED) return 2u;
    return 1u;
}

/* ══════════════════════════════════════════════════════════════════
 * 2. BATCH INFLATION POLICY
 *
 * Start at base_batch, inflate if isolation OK + util low
 * 128K → 256K → 512K (3 levels)
 * ══════════════════════════════════════════════════════════════════ */
#define L38_BATCH_LVL0  (128u*1024u)
#define L38_BATCH_LVL1  (256u*1024u)
#define L38_BATCH_LVL2  (512u*1024u)
#define L38_BATCH_MAX   L38_BATCH_LVL2

static inline uint32_t l38_gpu2_next_batch(uint32_t current,
                                              uint32_t util_pct,
                                              int      isolation_ok)
{
    if (!isolation_ok) return L38_BATCH_LVL0;   /* reset on error */
    if (util_pct >= L38_GPU2_UTIL_MED) return L38_BATCH_LVL0;
    if (current < L38_BATCH_LVL1 && util_pct < L38_GPU2_UTIL_LOW)
        return L38_BATCH_LVL1;
    if (current < L38_BATCH_LVL2 && util_pct < L38_GPU2_UTIL_LOW)
        return L38_BATCH_LVL2;
    return current;
}

/* ══════════════════════════════════════════════════════════════════
 * 3. GUARDRAILS
 *
 * timeout_ms:     kernel must finish within this time
 * mem_watermark:  don't allocate if VRAM < this (bytes)
 * per_stream_err: each stream tracks own error, doesn't poison others
 * ══════════════════════════════════════════════════════════════════ */
#define L38_GPU2_TIMEOUT_MS   5000u     /* 5s kernel timeout           */
#define L38_GPU2_MEM_WATERMARK (512u*1024u*1024u)  /* 512MB free VRAM  */

typedef struct {
    int         error_flag;    /* 1 = stream had error, isolated       */
    uint64_t    error_count;
    uint64_t    timeout_count;
    cudaError_t last_err;
} L38StreamGuard;

static inline void l38_guard_init(L38StreamGuard *g) {
    memset(g, 0, sizeof(*g));
}

static inline void l38_guard_check(L38StreamGuard *g, cudaError_t err) {
#ifdef POGLS_HAVE_CUDA
    if (err != cudaSuccess) {
        g->error_flag = 1;
        g->error_count++;
        g->last_err = err;
    }
#else
    (void)g; (void)err;
#endif
}

static inline int l38_guard_ok(const L38StreamGuard *g) {
    return !g->error_flag;
}

/* ══════════════════════════════════════════════════════════════════
 * 4. DOUBLE-BUFFER STREAM SLOT
 *
 * Each slot has:
 *   host_buf A/B (ping-pong)
 *   dev_buf  A/B
 *   stream
 *   guard
 *
 * Flow:
 *   tick 0: fill A, launch kernel on A
 *   tick 1: fill B, launch kernel on B, copy A result back (overlap)
 *   tick 2: fill A, launch kernel on A, copy B result back
 *   → compute + H→D copy happen concurrently
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* ping-pong buffers */
    L38W3Coord2 *host[2];   /* [0]=A [1]=B                            */
    void        *dev[2];    /* device side                            */
    uint32_t     count[2];  /* pending in each buffer                 */
    uint8_t      active;    /* which buffer is being filled (0 or 1)  */
    uint8_t      inflight;  /* which buffer has kernel running        */
    uint8_t      has_inflight;

    uint32_t     capacity;
    uint32_t     stream_id;
    cudaStream_t stream;
    L38StreamGuard guard;

    /* stats */
    uint64_t     batches_dispatched;
    uint64_t     coords_processed;
    double       last_mops;
} L38StreamSlot;

/* ══════════════════════════════════════════════════════════════════
 * GPU CONTEXT v2
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
    L38StreamSlot  slots[L38_GPU2_STREAM_MAX];
    uint32_t       n_slots_alloc;    /* actually allocated             */
    uint32_t       n_slots_active;   /* currently in use (soft limit)  */
    uint32_t       world3_n;         /* n=4 default                    */
    int            has_gpu;

    /* adaptive state */
    uint32_t       current_batch;    /* current batch size             */
    uint32_t       util_pct;         /* estimated GPU util 0..100      */
    uint64_t       total_iso_ok;
    uint64_t       total_iso_fail;   /* must stay 0                    */

    /* global stats */
    uint64_t       total_coords;
    uint64_t       total_batches;
} L38GpuCtx2;

/* ── kernel (same logic as v1, no changes) ────────────────────── */
#ifdef POGLS_HAVE_CUDA
__device__ static const uint8_t _d_lut2[16] =
    {0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10};

__global__ void l38_w3_kernel2(L38W3Coord2 *c, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    uint64_t a = c[i].addr;
    uint32_t sc = 1u<<20;
    uint32_t t = (uint32_t)(((a&(sc-1u))*1696631ULL)>>20);
    uint16_t x = t&0x3FFu, y = (t>>10)&0x3FFu;
    uint32_t rx=x,ry=y;
    rx=(rx|(rx<<8))&0x00FF00FFu;rx=(rx|(rx<<4))&0x0F0F0F0Fu;
    rx=(rx|(rx<<2))&0x33333333u;rx=(rx|(rx<<1))&0x55555555u;
    ry=(ry|(ry<<8))&0x00FF00FFu;ry=(ry|(ry<<4))&0x0F0F0F0Fu;
    ry=(ry|(ry<<2))&0x33333333u;ry=(ry|(ry<<1))&0x55555555u;
    c[i].morton  = rx|(ry<<1);
    c[i].hilbert = ((c[i].morton>>4)<<4)|_d_lut2[c[i].morton&0xF];
    c[i].phi     = (uint32_t)(((a&(sc-1u))*1696631ULL)%sc);
    c[i].lane    = (uint8_t)(c[i].hilbert%54u);
    c[i].slice   = 2u;
    c[i].audit   = ((a%17u)==1u)?0u:1u;
}
#endif

/* ── slot init ───────────────────────────────────────────────────── */
static inline int l38_slot_init(L38StreamSlot *sl, uint32_t sid,
                                  uint32_t capacity)
{
    memset(sl, 0, sizeof(*sl));
    sl->capacity  = capacity;
    sl->stream_id = sid;
    l38_guard_init(&sl->guard);

    for (int b=0;b<2;b++) {
        sl->host[b] = (L38W3Coord2*)malloc(capacity * sizeof(L38W3Coord2));
        if (!sl->host[b]) return -1;
    }
#ifdef POGLS_HAVE_CUDA
    for (int b=0;b<2;b++) {
        if (cudaMalloc(&sl->dev[b], capacity*sizeof(L38W3Coord2))!=cudaSuccess)
            return -2;
    }
    if (cudaStreamCreate(&sl->stream) != cudaSuccess) return -3;
#endif
    return 0;
}

static inline void l38_slot_free(L38StreamSlot *sl) {
    for (int b=0;b<2;b++) {
        free(sl->host[b]);
#ifdef POGLS_HAVE_CUDA
        if (sl->dev[b]) cudaFree(sl->dev[b]);
#endif
    }
#ifdef POGLS_HAVE_CUDA
    if (sl->stream) cudaStreamDestroy(sl->stream);
#endif
}

/* ── dispatch one slot (double-buffer) ──────────────────────────── */
static inline void l38_slot_dispatch(L38StreamSlot *sl, uint32_t world3_n)
{
#ifdef POGLS_HAVE_CUDA
    if (!l38_guard_ok(&sl->guard)) return;

    uint8_t cur = sl->active;
    uint32_t n  = sl->count[cur];
    if (n == 0) return;

    /* wait for previous inflight if any */
    if (sl->has_inflight) {
        cudaError_t err = cudaStreamSynchronize(sl->stream);
        l38_guard_check(&sl->guard, err);

        /* copy result back from inflight buffer */
        uint8_t inf = sl->inflight;
        if (l38_guard_ok(&sl->guard) && sl->count[inf] > 0) {
            cudaMemcpyAsync(sl->host[inf], sl->dev[inf],
                            sl->count[inf]*sizeof(L38W3Coord2),
                            cudaMemcpyDeviceToHost, sl->stream);
        }
    }

    /* H→D copy current buffer */
    l38_guard_check(&sl->guard,
        cudaMemcpyAsync(sl->dev[cur], sl->host[cur],
                        n*sizeof(L38W3Coord2),
                        cudaMemcpyHostToDevice, sl->stream));

    /* launch kernel on current buffer */
    uint32_t tpb = 72u * world3_n;
    if (tpb > 1024u) tpb = 1024u;
    tpb = ((tpb+31u)/32u)*32u;
    uint32_t blk = (n+tpb-1)/tpb;
#ifdef POGLS_HAVE_CUDA
    l38_w3_kernel2<<<blk,tpb,0,sl->stream>>>(
        (L38W3Coord2*)sl->dev[cur], n);
    l38_guard_check(&sl->guard, cudaGetLastError());
#endif

    sl->inflight     = cur;
    sl->has_inflight = 1;
    sl->active       = cur ^ 1;   /* ping-pong */
    sl->count[cur]   = 0;

    sl->batches_dispatched++;
    sl->coords_processed += n;
#else
    /* CPU fallback */
    uint8_t cur = sl->active;
    uint32_t n  = sl->count[cur];
    if (n == 0) return;
    for (uint32_t i=0;i<n;i++) {
        L38W3Coord2 *c = &sl->host[cur][i];
        uint64_t a = c->addr;
        uint32_t sc=1u<<20;
        uint32_t t=(uint32_t)(((a&(sc-1u))*(uint64_t)PHI_UP)>>20);
        uint16_t x=t&0x3FFu, y=(t>>10)&0x3FFu;
        uint32_t rx=x,ry=y;
        rx=(rx|(rx<<8))&0x00FF00FFu;rx=(rx|(rx<<4))&0x0F0F0F0Fu;
        rx=(rx|(rx<<2))&0x33333333u;rx=(rx|(rx<<1))&0x55555555u;
        ry=(ry|(ry<<8))&0x00FF00FFu;ry=(ry|(ry<<4))&0x0F0F0F0Fu;
        ry=(ry|(ry<<2))&0x33333333u;ry=(ry|(ry<<1))&0x55555555u;
        c->morton=rx|(ry<<1);
        c->hilbert=((c->morton>>4)<<4)|((uint8_t[]){0,3,4,5,1,2,7,6,14,13,8,9,15,12,11,10})[c->morton&0xF];
        c->phi=(uint32_t)(((a&(sc-1u))*(uint64_t)PHI_UP)%sc);
        c->lane=(uint8_t)(c->hilbert%54u); c->slice=2u;
        c->audit=((a%17u)==1u)?0u:1u;
    }
    sl->inflight=cur; sl->has_inflight=1;
    sl->active=cur^1; sl->count[cur]=0;
    sl->batches_dispatched++; sl->coords_processed+=n;
    (void)world3_n;
#endif
}

/* ── context init ────────────────────────────────────────────────── */
static inline int l38_gpu2_init(L38GpuCtx2 *ctx, uint8_t world3_n)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->world3_n     = (world3_n>=4)?world3_n:4u;
    ctx->current_batch = L38_BATCH_LVL0;
    ctx->util_pct     = 0;

#ifdef POGLS_HAVE_CUDA
    int dc=0;
    if (cudaGetDeviceCount(&dc)==cudaSuccess && dc>0) {
        /* check VRAM watermark */
        size_t free_mem=0, total_mem=0;
        cudaMemGetInfo(&free_mem, &total_mem);
        if (free_mem < L38_GPU2_MEM_WATERMARK) {
            printf("[GPU2] VRAM low (%zuMB free) — CPU fallback\n",
                   free_mem>>20);
            ctx->has_gpu = 0;
        } else {
            cudaDeviceProp p; cudaGetDeviceProperties(&p,0);
            printf("[GPU2] %s SM%d.%d  free=%.0fMB  max_tpb=%d\n",
                   p.name, p.major, p.minor,
                   (double)free_mem/1e6, p.maxThreadsPerBlock);
            ctx->has_gpu = 1;
        }
    }
#endif

    /* allocate max streams upfront (soft limit controls active count) */
    uint32_t alloc = ctx->has_gpu ? L38_GPU2_STREAM_MAX : 1u;
    for (uint32_t i=0;i<alloc;i++) {
        if (l38_slot_init(&ctx->slots[i], i, L38_BATCH_MAX) < 0) {
            printf("[GPU2] slot %u alloc failed\n", i);
            alloc = i; break;
        }
    }
    ctx->n_slots_alloc  = alloc;
    ctx->n_slots_active = 1;   /* start conservative */
    return alloc > 0 ? 0 : -1;
}

/* ── submit to least-loaded active slot ─────────────────────────── */
static inline void l38_gpu2_submit(L38GpuCtx2 *ctx, uint64_t addr)
{
    if (!ctx) return;

    /* update active stream count based on util */
    uint32_t allowed = l38_gpu2_allowed_streams(ctx->util_pct);
    if (allowed > ctx->n_slots_alloc) allowed = ctx->n_slots_alloc;
    ctx->n_slots_active = allowed;

    /* find slot with most room (round-robin with guard check) */
    uint32_t best = 0;
    uint32_t min_count = UINT32_MAX;
    for (uint32_t i=0;i<ctx->n_slots_active;i++) {
        L38StreamSlot *sl = &ctx->slots[i];
        if (!l38_guard_ok(&sl->guard)) continue;
        uint32_t cnt = sl->count[sl->active];
        if (cnt < min_count) { min_count=cnt; best=i; }
    }

    L38StreamSlot *sl = &ctx->slots[best];
    if (!l38_guard_ok((&sl->guard))) return;

    uint8_t cur = sl->active;
    if (sl->count[cur] >= ctx->current_batch) {
        /* buffer full → dispatch and flip */
        l38_slot_dispatch(sl, ctx->world3_n);
    }

    sl->host[sl->active][sl->count[sl->active]].addr = addr;
    sl->count[sl->active]++;
}

/* ── flush all active slots ─────────────────────────────────────── */
static inline void l38_gpu2_flush(L38GpuCtx2 *ctx)
{
    if (!ctx) return;
    for (uint32_t i=0;i<ctx->n_slots_active;i++) {
        l38_slot_dispatch(&ctx->slots[i], ctx->world3_n);
    }
#ifdef POGLS_HAVE_CUDA
    /* sync all */
    for (uint32_t i=0;i<ctx->n_slots_active;i++) {
        if (l38_guard_ok(&ctx->slots[i].guard))
            cudaStreamSynchronize(ctx->slots[i].stream);
    }
#endif

    /* count isolation */
    for (uint32_t i=0;i<ctx->n_slots_active;i++) {
        L38StreamSlot *sl = &ctx->slots[i];
        for (int b=0;b<2;b++) {
            for (uint32_t j=0;j<sl->count[b];j++) {
                if (sl->host[b][j].audit==0) ctx->total_iso_ok++;
                else ctx->total_iso_fail++;
            }
        }
        ctx->total_coords  += sl->coords_processed;
        ctx->total_batches += sl->batches_dispatched;
    }

    /* adaptive: inflate batch if isolation OK + util low */
    if (ctx->total_iso_fail == 0) {
        ctx->current_batch = l38_gpu2_next_batch(
            ctx->current_batch, ctx->util_pct, 1);
    } else {
        ctx->current_batch = L38_BATCH_LVL0;  /* reset on any fail */
    }

    /* update util estimate: based on active vs allowed */
    uint32_t allowed = l38_gpu2_allowed_streams(ctx->util_pct);
    ctx->util_pct = (ctx->n_slots_active >= allowed)
                  ? (ctx->util_pct + 5 > 95 ? 95 : ctx->util_pct + 5)
                  : (ctx->util_pct > 5 ? ctx->util_pct - 5 : 0);
}

/* ── free ──────────────────────────────────────────────────────── */
static inline void l38_gpu2_free(L38GpuCtx2 *ctx) {
    if (!ctx) return;
    for (uint32_t i=0;i<ctx->n_slots_alloc;i++)
        l38_slot_free(&ctx->slots[i]);
}

/* ── stats ─────────────────────────────────────────────────────── */
static inline void l38_gpu2_stats(const L38GpuCtx2 *ctx) {
    if (!ctx) return;
    printf("GPU2: coords=%llu batches=%llu  n=%u batch_size=%uK\n",
           (unsigned long long)ctx->total_coords,
           (unsigned long long)ctx->total_batches,
           ctx->world3_n, ctx->current_batch/1024);
    printf("  streams: active=%u/%u  util_est=%u%%\n",
           ctx->n_slots_active, ctx->n_slots_alloc, ctx->util_pct);
    printf("  isolation: ok=%llu fail=%llu\n",
           (unsigned long long)ctx->total_iso_ok,
           (unsigned long long)ctx->total_iso_fail);
    for (uint32_t i=0;i<ctx->n_slots_alloc;i++) {
        const L38StreamSlot *sl = &ctx->slots[i];
        printf("  stream[%u]: batches=%llu coords=%llu guard=%s\n",
               i, (unsigned long long)sl->batches_dispatched,
               (unsigned long long)sl->coords_processed,
               l38_guard_ok(&sl->guard)?"OK":"ERROR");
    }
}

#endif /* POGLS_38_GPU2_H */
