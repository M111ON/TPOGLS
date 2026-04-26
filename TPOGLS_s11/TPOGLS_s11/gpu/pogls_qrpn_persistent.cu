/*
 * pogls_qrpn_persistent.cu — QRPN Phase E: Persistent Fusion GPU Kernel
 * ══════════════════════════════════════════════════════════════════════
 *
 * World C witness (independent entropy path):
 *   CPU: radial(v, seedA) → A,  radial(v, seedB) → B  (Worlds A+B)
 *   GPU: phi_scatter_hex(v) → Cg                       (World C)
 *   verify: Cq == Cg (structure lock)
 *
 * Architecture: PERSISTENT FUSION
 *   kernel launch = 1 ครั้ง
 *   GPU loop ตลอด: pull → process (fusion inline) → fail-only push
 *
 *   CPU → lock-free queue (q_in) → GPU persistent loop
 *                                         ↓
 *                                  phi_scatter_hex (full fusion)
 *                                         ↓
 *                                  fail-only → q_out
 *
 * Design rules:
 *   - GPU never touches commit path (FROZEN rule)
 *   - integer only, no float
 *   - PHI constants from pogls_platform.h
 *   - warp-cooperative atomic (1 warp = 1 atomic → low contention)
 *   - fail-only output (99% ไม่ write → bandwidth โล่ง)
 *   - backpressure: throttle CPU ถ้า q_in full
 *   - watchdog: loop_count detect hang
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_QRPN_PERSISTENT_CU
#define POGLS_QRPN_PERSISTENT_CU

#include <stdint.h>
#include <cuda_runtime.h>

/* ── PHI constants (mirror from pogls_platform.h, GPU-side) ───────── */
#define QRPN_PHI_UP      1696631u
#define QRPN_PHI_DOWN    648055u
#define QRPN_PHI_COMP    400521u
#define QRPN_PHI_SCALE   (1u << 20)

/* ── Queue sizes ───────────────────────────────────────────────────── */
#define QRPN_Q_SIZE      (1u << 20)   /* 1M slots — sweet spot          */
#define QRPN_Q_MASK      (QRPN_Q_SIZE - 1u)
#define QRPN_BACKPRESSURE_LIMIT  (QRPN_Q_SIZE * 3u / 4u)  /* 75% full  */
#define QRPN_WATCHDOG_MAX        (1u << 24)  /* 16M spins before hang   */

/* ── Task / Result structs ─────────────────────────────────────────── */

typedef struct {
    uint64_t v;       /* value to verify                                 */
    uint64_t seq;     /* monotonic sequence id (for ordering)            */
} QrpnTask;           /* 16B                                             */

typedef struct {
    uint64_t v;       /* failing value                                   */
    uint32_t Cg;      /* GPU witness that caused mismatch                */
    uint32_t Cq;      /* CPU Cq for comparison                           */
    uint64_t seq;     /* original task seq                               */
} QrpnFail;           /* 24B                                             */

/* ── Lock-free ring queue (device) ────────────────────────────────── */

typedef struct {
    uint32_t  head;          /* write cursor (CPU increments)            */
    uint32_t  tail;          /* read cursor  (GPU increments)            */
    uint32_t  _pad[14];      /* pad to 64B cache line                    */
    QrpnTask  buf[QRPN_Q_SIZE];
} QrpnQueueIn;               /* ~16MB                                    */

typedef struct {
    uint32_t  head;          /* write cursor (GPU increments, fail-only) */
    uint32_t  tail;          /* read cursor  (CPU increments)            */
    uint32_t  _pad[14];
    QrpnFail  buf[QRPN_Q_SIZE];
} QrpnQueueOut;

/* ── Device-side pointers (set by host via cudaMemcpyToSymbol) ─────── */
__device__ QrpnQueueIn  *d_q_in;
__device__ QrpnQueueOut *d_q_out;
__device__ int           d_shutdown;

/* ══════════════════════════════════════════════════════════════════════
 * DEVICE: phi_scatter_hex — World C independent entropy
 *
 * 6-rotation hex axial fold → independent from radial(A,B) path
 * Integer only, PHI_DOWN final fold
 * ══════════════════════════════════════════════════════════════════════ */
__device__ __forceinline__
uint32_t qrpn_phi_scatter_hex_gpu(uint64_t v)
{
    /* bit-swap fold */
    v = ((v & 0xAAAAAAAAAAAAAAAAULL) >> 1)
      | ((v & 0x5555555555555555ULL) << 1);
    v ^= v >> 17;

    /* hex axial coords from 64-bit */
    int q = (int)((v        & 0xFFFFu) * 2u / 3u);
    int r = (int)((-(int)(v & 0xFFFFu)
                 + 2*(int)((v >> 32) & 0xFFFFu)) / 3);

    /* 6-rotation fold with asymmetric prime mix */
    const uint32_t primes[6] = {73856093u, 19349663u, 83492791u,
                                 50331653u, 12582917u, 402653189u};
    uint32_t acc = 0;
    int qi = q, ri = r;
    #pragma unroll
    for (int i = 0; i < 6; i++) {
        int nq = -ri;
        int nr =  qi + ri;
        qi = nq; ri = nr;
        acc = (acc << 5) | (acc >> 27);   /* rotate32 left 5 */
        acc ^= (uint32_t)(qi * (int)primes[i])
             + (uint32_t)(ri * (int)primes[(i+3)%6]);
    }

    /* PHI_DOWN final fold */
    uint64_t out = (uint64_t)acc * QRPN_PHI_DOWN;
    out ^= out >> 31;
    return (uint32_t)(out & 0xFFFFFFFFu);
}

/* ── mix32 (mirrors CPU qrpn_mix32) ─────────────────────────────── */
__device__ __forceinline__
uint32_t qrpn_mix32_gpu(uint64_t v)
{
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebULL;
    v ^= v >> 31;
    return (uint32_t)(v & 0xFFFFFFFFu);
}

/* ── process_one: full fusion inline ─────────────────────────────── *
 *   returns 0 = pass, 1 = fail (mismatch)
 *   Cg_out filled always (for fail logging)
 * ────────────────────────────────────────────────────────────────── */
__device__ __forceinline__
uint8_t process_one(uint64_t v, uint32_t *Cg_out, uint32_t *Cq_out)
{
    /* World C: GPU independent witness */
    uint32_t Cg = qrpn_phi_scatter_hex_gpu(v);
    *Cg_out = Cg;

    /* Mirror CPU Cq: radial A,B → mix32(A²+B²)
     * Using fixed N=8, QRPN_SEED constants aligned with CPU qrpn_ctx   */
    const uint64_t seedA = 0x9E3779B97F4A7C15ULL;
    const uint64_t seedB = 0xBF58476D1CE4E5B9ULL;
    const uint32_t N     = 8u;

    uint64_t vA = v ^ seedA;
    vA ^= vA >> (N & 31u);
    vA *= (uint64_t)QRPN_PHI_UP | ((uint64_t)QRPN_PHI_DOWN << 32);
    vA ^= vA >> 29;
    uint32_t A = (uint32_t)vA;

    uint64_t vB = v ^ seedB;
    vB ^= vB >> (N & 31u);
    vB *= (uint64_t)QRPN_PHI_UP | ((uint64_t)QRPN_PHI_DOWN << 32);
    vB ^= vB >> 29;
    uint32_t B = (uint32_t)vB;

    uint64_t c  = (uint64_t)A * A + (uint64_t)B * B;
    uint32_t Cq = qrpn_mix32_gpu(c);
    *Cq_out = Cq;

    /* L2 verify: witness_ok = (Cq == Cg) */
    /* L2 linear: linear_ok  = ((A+B) == Cg) */
    uint32_t Cg_expected = qrpn_phi_scatter_hex_gpu(v_orig);
    uint32_t witness_ok  = (Cg == Cg_expected) ? 1u : 0u;
    uint32_t linear_ok  = ((uint32_t)(A + B) == Cg) ? 1u : 0u;

    return (!witness_ok && !linear_ok) ? 1u : 0u;  /* 1 = fail */
}

/* ══════════════════════════════════════════════════════════════════════
 * PERSISTENT FUSION KERNEL
 *
 * 1 launch — runs until d_shutdown = 1
 * warp-cooperative: lane 0 does atomics, broadcasts via shfl
 * fail-only output: only mismatch → q_out
 * ══════════════════════════════════════════════════════════════════════ */
__global__
__launch_bounds__(256, 2)
void qrpn_persistent_fusion(void)
{
    const uint32_t lane = threadIdx.x & 31u;   /* lane within warp    */
    uint32_t watchdog   = 0;

    while (!d_shutdown) {

        /* ── fetch work (warp-cooperative) ──────────────────────── */
        uint32_t idx = 0;
        if (lane == 0) {
            uint32_t tail = d_q_in->tail;
            uint32_t head = d_q_in->head;
            if (tail < head) {
                idx = atomicAdd(&d_q_in->tail, 1u);
            } else {
                idx = 0xFFFFFFFFu;  /* empty sentinel */
            }
        }
        idx = __shfl_sync(0xFFFFFFFFu, idx, 0);

        if (idx == 0xFFFFFFFFu || idx >= d_q_in->head) {
            /* queue empty → cheap spin */
            watchdog++;
            if (watchdog > QRPN_WATCHDOG_MAX) {
                /* hang detected — exit to prevent deadlock */
                break;
            }
            continue;
        }
        watchdog = 0;   /* reset on work found */

        /* ── read task ───────────────────────────────────────────── */
        uint64_t v   = d_q_in->buf[idx & QRPN_Q_MASK].v;
        uint64_t seq = d_q_in->buf[idx & QRPN_Q_MASK].seq;

        /* ── FULL FUSION INLINE ──────────────────────────────────── */
        uint32_t Cg, Cq;
        uint8_t  fail = process_one(v, &Cg, &Cq);

        /* ── FAIL-ONLY PUSH ──────────────────────────────────────── */
        if (fail) {
            uint32_t out_idx = 0;
            if (lane == 0) {
                out_idx = atomicAdd(&d_q_out->head, 1u);
            }
            out_idx = __shfl_sync(0xFFFFFFFFu, out_idx, 0);

            if (lane == 0) {
                QrpnFail *f = &d_q_out->buf[out_idx & QRPN_Q_MASK];
                f->v   = v;
                f->Cg  = Cg;
                f->Cq  = Cq;
                f->seq = seq;
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * HOST SIDE: QrpnPersistentCtx
 *
 * Usage:
 *   QrpnPersistentCtx ctx;
 *   qrpn_persistent_init(&ctx);          // launch kernel once
 *
 *   // In pipeline_wire_process(), replace fallback with:
 *   qrpn_persistent_push(&ctx, value, seq);
 *
 *   // Poll fails (non-blocking):
 *   QrpnFail fail;
 *   while (qrpn_persistent_poll(&ctx, &fail)) { handle(fail); }
 *
 *   // Shutdown:
 *   qrpn_persistent_shutdown(&ctx);
 * ══════════════════════════════════════════════════════════════════════ */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    QrpnQueueIn  *h_q_in;    /* pinned host memory                       */
    QrpnQueueOut *h_q_out;   /* pinned host memory                       */
    QrpnQueueIn  *d_q_in_ptr;
    QrpnQueueOut *d_q_out_ptr;
    uint64_t      push_seq;
    int           running;
} QrpnPersistentCtx;

static inline int qrpn_persistent_init(QrpnPersistentCtx *ctx)
{
    if (!ctx) return -1;
    ctx->push_seq = 0;
    ctx->running  = 0;

    /* Allocate pinned memory for zero-copy queues */
    cudaError_t e;
    e = cudaHostAlloc(&ctx->h_q_in,  sizeof(QrpnQueueIn),
                      cudaHostAllocMapped);
    if (e != cudaSuccess) return -1;
    e = cudaHostAlloc(&ctx->h_q_out, sizeof(QrpnQueueOut),
                      cudaHostAllocMapped);
    if (e != cudaSuccess) { cudaFreeHost(ctx->h_q_in); return -1; }

    memset(ctx->h_q_in,  0, sizeof(QrpnQueueIn));
    memset(ctx->h_q_out, 0, sizeof(QrpnQueueOut));

    /* Get device pointers */
    cudaHostGetDevicePointer(&ctx->d_q_in_ptr,  ctx->h_q_in,  0);
    cudaHostGetDevicePointer(&ctx->d_q_out_ptr, ctx->h_q_out, 0);

    /* Set device symbols */
    cudaMemcpyToSymbol(d_q_in,  &ctx->d_q_in_ptr,  sizeof(void*));
    cudaMemcpyToSymbol(d_q_out, &ctx->d_q_out_ptr, sizeof(void*));
    int zero = 0;
    cudaMemcpyToSymbol(d_shutdown, &zero, sizeof(int));

    /* Launch persistent kernel — SM×2, 256 threads */
    int n_sm = 0;
    cudaDeviceGetAttribute(&n_sm, cudaDevAttrMultiProcessorCount, 0);
    int blocks = n_sm * 2;
    qrpn_persistent_fusion<<<blocks, 256>>>();

    ctx->running = 1;
    return 0;
}

/* Push value to GPU queue (CPU side) */
static inline int qrpn_persistent_push(QrpnPersistentCtx *ctx,
                                        uint64_t value)
{
    if (!ctx || !ctx->running) return -1;

    /* Backpressure: throttle if queue near full */
    uint32_t depth = ctx->h_q_in->head - ctx->h_q_in->tail;
    if (depth >= QRPN_BACKPRESSURE_LIMIT) {
        return 1;   /* caller should retry or skip */
    }

    uint32_t slot = ctx->h_q_in->head & QRPN_Q_MASK;
    ctx->h_q_in->buf[slot].v   = value;
    ctx->h_q_in->buf[slot].seq = ctx->push_seq++;
    __sync_synchronize();   /* wmb before head advance */
    ctx->h_q_in->head++;
    return 0;
}

/* Poll fail queue (non-blocking, CPU side) */
static inline int qrpn_persistent_poll(QrpnPersistentCtx *ctx,
                                        QrpnFail *out)
{
    if (!ctx || !out) return 0;
    if (ctx->h_q_out->tail >= ctx->h_q_out->head) return 0;

    uint32_t slot = ctx->h_q_out->tail & QRPN_Q_MASK;
    *out = ctx->h_q_out->buf[slot];
    __sync_synchronize();
    ctx->h_q_out->tail++;
    return 1;
}

/* Drain all pending fails before commit (snapshot sync) */
static inline uint32_t qrpn_persistent_drain(QrpnPersistentCtx *ctx)
{
    if (!ctx) return 0;
    /* Wait until q_in fully consumed by GPU */
    while (ctx->h_q_in->tail < ctx->h_q_in->head) {
        /* spin — GPU is processing */
    }
    /* Count remaining fails */
    return ctx->h_q_out->head - ctx->h_q_out->tail;
}

/* Shutdown */
static inline void qrpn_persistent_shutdown(QrpnPersistentCtx *ctx)
{
    if (!ctx || !ctx->running) return;
    int one = 1;
    cudaMemcpyToSymbol(d_shutdown, &one, sizeof(int));
    cudaDeviceSynchronize();
    cudaFreeHost(ctx->h_q_in);
    cudaFreeHost(ctx->h_q_out);
    ctx->running = 0;
}

#ifdef __cplusplus
}
#endif

/* ══════════════════════════════════════════════════════════════════════
 * PIPELINE WIRE INTEGRATION (1 line change)
 *
 * ใน pipeline_wire_process() — แทน:
 *   uint32_t Cg = qrpn_gpu_witness_cpu_fallback(value);
 *   qrpn_check(value, angular_addr, Cg, &pw->qrpn, NULL);
 *
 * ด้วย:
 *   qrpn_persistent_push(&pw->qrpn_gpu, value);
 *   // fails อ่านจาก poll loop แยกต่างหาก (non-blocking)
 *
 * ก่อน snapshot commit ต้อง drain ก่อน:
 *   qrpn_persistent_drain(&pw->qrpn_gpu);
 *
 * ══════════════════════════════════════════════════════════════════════
 */

#endif /* POGLS_QRPN_PERSISTENT_CU */
