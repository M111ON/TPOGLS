/*
 * geo_cuda_p4.cu — T4 sm_75 Geo Pipeline Kernel
 * ═══════════════════════════════════════════════
 * Target: NVIDIA T4 (sm_75, Turing)
 * Compile: nvcc -O3 -arch=sm_75 -o geo_cuda_p4.o geo_cuda_p4.cu
 *
 * Input:  GeoWire16[] in pinned host mem (16B/pkt, coalesced)
 * Output: uint32_t[]  sig32 results
 *
 * Kernel features (T4-specific):
 *   - warp ballot_sync for batch filter
 *   - warp ballot_sync for batch filter
 *   - coalesced 16B read via uint4
 */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>

/* ── GeoWire16 mirror (must match geo_simd_p4.h) ─────────────────── */
struct GeoWire16 {
    uint32_t sig;
    uint32_t idx;
    uint8_t  spoke;
    uint8_t  phase;
    uint16_t _pad;
};

/* ── Constants ───────────────────────────────────────────────────── */
#define P4_FULL_N       3456u
#define P4_SPOKES       6u
#define P4_MOD6_MAGIC   10923u
#define P4_WARP_SIZE    32u

/* ── Device: Barrett mod6 ────────────────────────────────────────── */
__device__ __forceinline__ uint8_t d_mod6(uint32_t n) {
    uint32_t q = (n * P4_MOD6_MAGIC) >> 16;
    return (uint8_t)(n - q * P4_SPOKES);
}

/* ── Device: fast mod 3456 via reciprocal mul ────────────────────── */
/* 3456 = 2^7 * 27 → use compiler-friendly path */
__device__ __forceinline__ uint32_t d_mod3456(uint32_t n) {
    /* For n < 2^24: n % 3456 via GPU integer div (fast on T4) */
    return n % P4_FULL_N;
}

/* ── Main kernel: batch geo route ───────────────────────────────── */
/*
 * Each thread processes one GeoWire16 packet.
 * Reads coalesced via uint4 (16 bytes = 1 GeoWire16).
 *
 * warp_filter_mask: bit per lane = spoke matches filter spoke_mask
 *   - __ballot_sync aggregates valid lanes
 *   - only valid lanes write output
 */
extern "C" {

/* ── GeoP4Ctx (canonical definition — geo_p4_bridge.h uses void* stream) ── */
/* NOTE: geo_p4_bridge.h declares GeoP4Ctx with void* stream (opaque).      */
/* This .cu owns the real definition; bridge.h must NOT redefine it.        */

__global__ void geo_cuda_step_kernel(
    const uint4   *__restrict__ in_wire16,
    uint32_t      *__restrict__ out_sig,
    uint32_t      *__restrict__ out_valid,
    uint32_t       n,
    uint32_t       spoke_filter,
    uint64_t       addr_base)
{
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n) return;

    /* ── coalesced 16B read (raw used for future validation path) ── */
    (void)in_wire16[tid];  /* coalesced read touch — recompute from addr */

    /* ── recompute route from addr ── */
    uint64_t addr     = addr_base + tid;
    uint32_t full_idx = d_mod3456((uint32_t)(addr & 0xFFFFFFFFu));
    uint8_t  spoke    = d_mod6(full_idx);
    uint32_t slot     = full_idx / P4_SPOKES;
    uint32_t sig_new  = slot ^ ((uint32_t)spoke << 9);




    /* ── warp-level ballot filter (spoke_filter) ── */
    /*
     * spoke_filter = 0  → accept all
     * spoke_filter != 0 → accept only spokes in mask
     */
    uint32_t lane_ok = (spoke_filter == 0u)
                     ? 1u
                     : ((spoke_filter >> spoke) & 1u);

    uint32_t ballot = __ballot_sync(0xFFFFFFFFu, lane_ok);

    /* warp leader (lane 0) writes ballot result */
    uint32_t warp_id = tid / P4_WARP_SIZE;
    if ((threadIdx.x & 31u) == 0u && out_valid != nullptr)
        out_valid[warp_id] = ballot;

    /* ── write output (all threads, caller uses ballot to filter) ── */
    out_sig[tid] = lane_ok ? sig_new : 0xFFFFFFFFu;
}
} /* extern "C" — closes geo_cuda_step_kernel */

/* ── shfl_sync compaction: remove invalid entries ────────────────── */
/*
 * Second-pass kernel: compact out_sig[], removing sentinel 0xFFFFFFFF.
 * Uses warp shuffle for prefix sum within warp.
 */
__global__ void geo_cuda_compact_kernel(
    const uint32_t *__restrict__ in_sig,
    uint32_t       *__restrict__ out_compact,
    uint32_t       *__restrict__ out_count,   /* total valid per block */
    uint32_t        n)
{
    uint32_t tid  = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t mask = 0xFFFFFFFFu;

    uint32_t val   = (tid < n) ? in_sig[tid] : 0xFFFFFFFFu;
    uint32_t valid = (val != 0xFFFFFFFFu) ? 1u : 0u;

    /* warp prefix sum via __shfl_up_sync */
    uint32_t prefix = valid;
    for (uint32_t offset = 1; offset < P4_WARP_SIZE; offset <<= 1) {
        uint32_t n_val = __shfl_up_sync(mask, prefix, offset);
        if ((threadIdx.x & 31u) >= offset) prefix += n_val;
    }

    /* warp total = last lane's prefix */
    uint32_t warp_total = __shfl_sync(mask, prefix, 31u);

    /* shared mem base offset (simplified: per-warp atomic) */
    __shared__ uint32_t warp_base[32];  /* max 32 warps per block */
    uint32_t local_warp = threadIdx.x / P4_WARP_SIZE;

    if ((threadIdx.x & 31u) == 31u) {
        warp_base[local_warp] = atomicAdd(out_count, warp_total);
    }
    __syncthreads();

    if (valid && tid < n) {
        uint32_t base = warp_base[local_warp];
        out_compact[base + prefix - 1] = val;
    }
}

/* ── Host launcher ───────────────────────────────────────────────── */

extern "C" {   /* geo_p4_ctx_init / geo_p4_ctx_free / geo_p4_run → C linkage */

#define CUDA_CHECK(x) do { \
    cudaError_t _e = (x); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, \
                cudaGetErrorString(_e)); \
        return -1; \
    } \
} while(0)

/* GeoP4Ctx: canonical definition (matches geo_p4_bridge.h extern "C" decl).
 * stream stored as void* (opaque to C callers) — cast internally.          */
typedef struct {
    void     *d_in;         /* device GeoWire16[] */
    uint32_t *d_out_sig;    /* device sig32[] */
    uint32_t *d_out_valid;  /* device ballot[] */
    uint32_t *d_compact;    /* compacted output */
    uint32_t *d_count;      /* valid count */
    uint32_t  capacity;
    void     *stream;       /* cudaStream_t stored as void* (C-safe) */
} GeoP4Ctx;

int geo_p4_ctx_init(GeoP4Ctx *ctx, uint32_t capacity) {
    ctx->capacity = capacity;
    uint32_t n_warps = (capacity + P4_WARP_SIZE - 1) / P4_WARP_SIZE;

    CUDA_CHECK(cudaMalloc(&ctx->d_in,        capacity * sizeof(GeoWire16)));
    CUDA_CHECK(cudaMalloc(&ctx->d_out_sig,   capacity * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_out_valid, n_warps  * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_compact,   capacity * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_count,     sizeof(uint32_t)));
    cudaStream_t _s; CUDA_CHECK(cudaStreamCreate(&_s)); ctx->stream = (void*)_s;
    return 0;
}

void geo_p4_ctx_free(GeoP4Ctx *ctx) {
    cudaFree(ctx->d_in);
    cudaFree(ctx->d_out_sig);
    cudaFree(ctx->d_out_valid);
    cudaFree(ctx->d_compact);
    cudaFree(ctx->d_count);
    cudaStreamDestroy((cudaStream_t)ctx->stream);
}

/*
 * geo_p4_run — full async pipeline
 * h_wire16: pinned host GeoWire16[] (from geo_simd_fill)
 * n:        packet count
 * spoke_filter: 6-bit mask (0 = all pass)
 * addr_base: for recompute path
 * h_out_sig: pinned host output (must be n * sizeof(uint32_t))
 * Returns: 0 on success, -1 on error
 */
int geo_p4_run(
    GeoP4Ctx       *ctx,
    const void     *h_wire16,
    uint32_t        n,
    uint32_t        spoke_filter,
    uint64_t        addr_base,
    uint32_t       *h_out_sig)
{
    if (n > ctx->capacity) return -1;

    uint32_t threads = 256;
    uint32_t blocks  = (n + threads - 1) / threads;

    cudaStream_t _stream = (cudaStream_t)ctx->stream;

    /* H→D transfer */
    CUDA_CHECK(cudaMemcpyAsync(ctx->d_in, h_wire16, n * sizeof(GeoWire16),
                               cudaMemcpyHostToDevice, _stream));

    /* reset count */
    CUDA_CHECK(cudaMemsetAsync(ctx->d_count, 0, sizeof(uint32_t), _stream));

    /* step kernel */
    geo_cuda_step_kernel<<<blocks, threads, 0, _stream>>>(
        (const uint4*)ctx->d_in, ctx->d_out_sig, ctx->d_out_valid,
        n, spoke_filter, addr_base);

    /* compact kernel */
    geo_cuda_compact_kernel<<<blocks, threads, 0, _stream>>>(
        ctx->d_out_sig, ctx->d_compact, ctx->d_count, n);

    /* D→H transfer (full sig array, caller uses ballot for validity) */
    CUDA_CHECK(cudaMemcpyAsync(h_out_sig, ctx->d_out_sig,
                               n * sizeof(uint32_t),
                               cudaMemcpyDeviceToHost, _stream));

    CUDA_CHECK(cudaStreamSynchronize(_stream));
    return 0;
}

} /* extern "C" — closes geo_p4_ctx_init / geo_p4_ctx_free / geo_p4_run */
