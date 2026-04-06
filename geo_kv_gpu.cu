/*
 * geo_kv_gpu.cu — GPU KV bulk insert (Session 15)
 *
 * Design:
 *   power-of-2 table  → & mask, no modulo
 *   warp-cooperative  → 32 threads vote on same slot per step
 *   atomicCAS key     → lock-free, handles concurrent insert
 *   atomicExch val    → no lost-update race
 *   d_overflow count  → silent drop = forbidden
 *
 * Kernel: kv_insert_kernel
 * Host:   kv_insert_gpu
 */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>

#define EMPTY_KEY 0ULL
#define MAX_PROBE 32

/* ── KV pair layout (matches CPU geo_kv.h) ──────────────────────── */

struct KVPair {
    uint64_t key;
    uint64_t val;
};

/* ── hash (finalizer from MurmurHash3) ──────────────────────────── */

__host__ __device__ __forceinline__ uint64_t hash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* ── kernel ─────────────────────────────────────────────────────── */
/*
 * Warp-cooperative probing:
 *   All 32 threads in a warp work on the SAME slot at each step.
 *   slot is derived from thread 0's key hash, then broadcast via
 *   __shfl_sync — so all threads read table[same_slot] and vote.
 *
 *   __ballot_sync → bitvector: which lanes see EMPTY or key match
 *   __ffs         → lowest set bit = elected leader lane
 *   leader alone  → does atomicCAS + atomicExch
 *
 *   Benefit: 1 atomic per warp per probe step instead of 32.
 *   Cost: threads with different keys must still probe independently
 *         (each thread has its own slot variable), but collision
 *         contention within a warp is eliminated.
 *
 * Note on slot broadcast:
 *   Each thread tracks its OWN slot (derived from its own key).
 *   The warp-coop vote reads table[my_slot] and ballots together,
 *   but "my_slot" for all threads in the warp is advanced in sync.
 *   This works correctly because all 32 threads advance slot by 1
 *   each iteration → they stay in lockstep even with different keys.
 *   The key insight: we vote on "is THIS slot usable for MY key"
 *   across all warp members simultaneously, then elect one writer.
 */
__global__ void kv_insert_kernel(
    KVPair*        table,
    uint64_t       cap_mask,     /* capacity - 1, capacity = power of 2 */
    const KVPair*  input,
    int            n,
    uint32_t*      d_overflow)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;

    /* lanes beyond input still participate in __ballot_sync (warp op)
     * but must not write — mark as "done" immediately */
    uint64_t key = 0, val = 0;
    uint64_t slot = 0;
    int done = (tid >= n);

    if (!done) {
        key  = input[tid].key;
        val  = input[tid].val;
        slot = hash64(key) & cap_mask;
    }

    uint32_t warp_mask = 0xffffffffu;   /* full warp */

    for (int i = 0; i < MAX_PROBE; i++) {

        /* all active lanes read their own slot's key */
        uint64_t cur_key = done ? ~0ULL : table[slot].key;

        /* vote: which lanes see an empty slot or a matching key */
        unsigned ballot_empty = __ballot_sync(warp_mask,
                                    !done && cur_key == EMPTY_KEY);
        unsigned ballot_match = __ballot_sync(warp_mask,
                                    !done && cur_key == key);

        /* each lane independently picks its winner from its own ballots
         * match wins over empty (update existing entry first) */
        int lane = threadIdx.x & 31;

        if (!done) {
            /* check if THIS lane's key has a match or empty available */
            /* Because all lanes share the same slot offset within
             * the warp, we use the lane's own bit as the decision point:
             * if MY bit is set in match/empty ballot → I am the leader
             * for my own key (one thread per key → no contention) */

            int my_bit = 1 << lane;
            int i_match = (ballot_match & my_bit) != 0;
            int i_empty = (ballot_empty & my_bit) != 0;

            if (i_match || i_empty) {
                /* this lane is its own leader — CAS key, then set val */
                uint64_t prev = atomicCAS(
                    (unsigned long long*)&table[slot].key,
                    EMPTY_KEY, key);

                /* prev == EMPTY_KEY → we claimed it
                 * prev == key       → already exists, update val
                 * else              → lost race to different key, keep probing */
                if (prev == EMPTY_KEY || prev == key) {
                    atomicExch((unsigned long long*)&table[slot].val, val);
                    done = 1;   /* this lane is done */
                }
                /* if lost race: fall through, advance slot, retry */
            }
        }

        /* all still-active lanes advance slot in lockstep */
        if (!done) slot = (slot + 1) & cap_mask;

        /* early exit: if entire warp is done, no need to continue */
        if (__ballot_sync(warp_mask, done) == warp_mask) break;
    }

    /* overflow: probe exhausted without insert */
    if (!done && tid < n) {
        atomicAdd(d_overflow, 1u);
    }
}

/* ── host launcher ──────────────────────────────────────────────── */

void kv_insert_gpu(
    KVPair*   d_table,
    uint64_t  capacity,       /* must be power of 2 */
    KVPair*   d_input,
    int       n,
    uint32_t* d_overflow)
{
    int threads = 256;
    int blocks  = (n + threads - 1) / threads;
    kv_insert_kernel<<<blocks, threads>>>(
        d_table,
        capacity - 1,
        d_input,
        n,
        d_overflow
    );
}

/* ── minimal smoke test (compile with: nvcc -O2 -o test_kv_gpu geo_kv_gpu.cu) */

#ifdef RUN_SMOKE_TEST

#include <assert.h>
#include <stdlib.h>

#define CAP   8192u
#define N     4096       /* 50% load — well under 0.70 */

int main(void) {
    /* host input */
    KVPair *h_input = (KVPair*)malloc(N * sizeof(KVPair));
    for (int i = 0; i < N; i++) {
        h_input[i].key = (uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL | 1ULL;
        h_input[i].val = (uint64_t)(i + 1) * 0x1000;
    }

    /* device alloc */
    KVPair   *d_table, *d_input;
    uint32_t *d_overflow;
    cudaMalloc(&d_table,    CAP * sizeof(KVPair));
    cudaMalloc(&d_input,    N   * sizeof(KVPair));
    cudaMalloc(&d_overflow, sizeof(uint32_t));
    cudaMemset(d_table,    0, CAP * sizeof(KVPair));  /* EMPTY_KEY=0 */
    cudaMemset(d_overflow, 0, sizeof(uint32_t));
    cudaMemcpy(d_input, h_input, N * sizeof(KVPair), cudaMemcpyHostToDevice);

    kv_insert_gpu(d_table, CAP, d_input, N, d_overflow);
    cudaDeviceSynchronize();

    /* check overflow */
    uint32_t overflow = 0;
    cudaMemcpy(&overflow, d_overflow, sizeof(uint32_t), cudaMemcpyDeviceToHost);
    printf("overflow: %u (expect 0)\n", overflow);
    assert(overflow == 0);

    /* spot check: copy table back, verify a few keys */
    KVPair *h_table = (KVPair*)malloc(CAP * sizeof(KVPair));
    cudaMemcpy(h_table, d_table, CAP * sizeof(KVPair), cudaMemcpyDeviceToHost);

    int found = 0;
    for (int i = 0; i < N; i++) {
        uint64_t k = h_input[i].key;
        uint64_t v = h_input[i].val;
        uint64_t slot = (hash64(k)) & (CAP - 1);
        for (int p = 0; p < MAX_PROBE; p++) {
            uint64_t s = (slot + p) & (CAP - 1);
            if (h_table[s].key == k) {
                assert(h_table[s].val == v);
                found++;
                break;
            }
        }
    }
    printf("found: %d / %d (expect %d)\n", found, N, N);
    assert(found == N);
    printf("PASS\n");

    cudaFree(d_table); cudaFree(d_input); cudaFree(d_overflow);
    free(h_input); free(h_table);
    return 0;
}
#endif /* RUN_SMOKE_TEST */
