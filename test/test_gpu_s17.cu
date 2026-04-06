/*
 * test_gpu_s17.cu — GPU KV bridge smoke test (Session 17)
 *
 * Build (WSL2, GTX 1050Ti):
 *   nvcc -O2 -arch=sm_61 -I. -o test_gpu_s17 test_gpu_s17.cu
 *
 * Tests:
 *   1. 4K insert → overflow = 0
 *   2. 64K insert → overflow = 0
 *   3. spot-check: 100 random keys verifiable on host
 *   4. duplicate insert (same key twice) → no double-count, val updated
 */

#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

/* pull in GPU kernel */
#include "geo_kv_gpu.cu"   /* defines kv_insert_gpu, hash64, KVPair */

#define CAP    65536U
#define MASK   (CAP - 1)

/* ── host-side linear probe (mirrors GPU logic, uses same hash) ── */
static int host_find(KVPair *table, uint64_t cap, uint64_t key, uint64_t *val_out) {
    uint64_t slot = hash64(key) & (cap - 1);
    for (int p = 0; p < MAX_PROBE; p++) {
        uint64_t s = (slot + p) & (cap - 1);
        if (table[s].key == key) { *val_out = table[s].val; return 1; }
        if (table[s].key == 0)   { return 0; }  /* EMPTY_KEY */
    }
    return 0;
}

/* ── test harness ─────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define TPASS(name) do { printf("  " name " ... PASS\n"); g_pass++; } while(0)
#define TFAIL(name, msg) do { printf("  " name " ... FAIL: " msg "\n"); g_fail++; } while(0)

/* ── helper: alloc, fill, insert, return h_table (caller frees) ─ */
static KVPair *run_batch(int n, uint32_t seed, uint32_t *overflow_out) {
    KVPair *h_in  = (KVPair *)malloc(n * sizeof(KVPair));
    KVPair *h_out = (KVPair *)calloc(CAP, sizeof(KVPair));
    for (int i = 0; i < n; i++) {
        h_in[i].key = (uint64_t)(seed + i + 1) * 0x9e3779b97f4a7c15ULL | 1ULL;
        h_in[i].val = (uint64_t)(seed + i + 1) * 0x1000ULL;
    }

    KVPair   *d_table, *d_input;
    uint32_t *d_ov;
    cudaMalloc(&d_table, CAP * sizeof(KVPair));
    cudaMalloc(&d_input, n   * sizeof(KVPair));
    cudaMalloc(&d_ov,    sizeof(uint32_t));
    cudaMemset(d_table, 0, CAP * sizeof(KVPair));
    cudaMemset(d_ov,    0, sizeof(uint32_t));
    cudaMemcpy(d_input, h_in, n * sizeof(KVPair), cudaMemcpyHostToDevice);

    kv_insert_gpu(d_table, CAP, d_input, n, d_ov);
    cudaDeviceSynchronize();

    cudaMemcpy(overflow_out, d_ov,    sizeof(uint32_t),      cudaMemcpyDeviceToHost);
    cudaMemcpy(h_out,        d_table, CAP * sizeof(KVPair),  cudaMemcpyDeviceToHost);

    cudaFree(d_table); cudaFree(d_input); cudaFree(d_ov);
    free(h_in);
    return h_out;   /* caller frees */
}

/* ── Test 1: 4K insert, overflow=0 ───────────────────────────── */
static void t_insert_4k(void) {
    uint32_t ov = 99;
    KVPair *tbl = run_batch(4096, 0, &ov);
    if (ov != 0) { TFAIL("insert_4k", "overflow != 0"); }
    else         { TPASS("insert_4k"); }
    free(tbl);
}

/* ── Test 2: 64K insert (50% load), overflow=0 ───────────────── */
static void t_insert_64k(void) {
    uint32_t ov = 99;
    KVPair *tbl = run_batch(32768, 1000, &ov);   /* 32768 = 50% of 65536 */
    if (ov != 0) { TFAIL("insert_64k_half", "overflow != 0"); }
    else         { TPASS("insert_64k_half"); }
    free(tbl);
}

/* ── Test 3: spot-check 100 keys on host ─────────────────────── */
static void t_spot_check(void) {
    int n = 4096;
    KVPair *h_in = (KVPair *)malloc(n * sizeof(KVPair));
    for (int i = 0; i < n; i++) {
        h_in[i].key = (uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL | 1ULL;
        h_in[i].val = (uint64_t)(i + 1) * 0x1000ULL;
    }

    KVPair *d_table, *d_input; uint32_t *d_ov, ov;
    cudaMalloc(&d_table, CAP * sizeof(KVPair));
    cudaMalloc(&d_input, n   * sizeof(KVPair));
    cudaMalloc(&d_ov,    sizeof(uint32_t));
    cudaMemset(d_table, 0, CAP * sizeof(KVPair));
    cudaMemset(d_ov,    0, sizeof(uint32_t));
    cudaMemcpy(d_input, h_in, n * sizeof(KVPair), cudaMemcpyHostToDevice);
    kv_insert_gpu(d_table, CAP, d_input, n, d_ov);
    cudaDeviceSynchronize();

    KVPair *h_tbl = (KVPair *)malloc(CAP * sizeof(KVPair));
    cudaMemcpy(h_tbl, d_table, CAP * sizeof(KVPair), cudaMemcpyDeviceToHost);
    cudaMemcpy(&ov,   d_ov,    sizeof(uint32_t),     cudaMemcpyDeviceToHost);

    int miss = 0;
    for (int i = 0; i < 100; i++) {
        uint64_t got;
        if (!host_find(h_tbl, CAP, h_in[i].key, &got) || got != h_in[i].val)
            miss++;
    }

    cudaFree(d_table); cudaFree(d_input); cudaFree(d_ov);
    free(h_in); free(h_tbl);

    if (miss) { TFAIL("spot_check_100", "key not found or wrong val"); }
    else      { TPASS("spot_check_100"); }
}

/* ── Test 4: duplicate key → val updated, no inflate ─────────── */
static void t_duplicate_key(void) {
    /* insert key once with val=111, then again with val=222 */
    uint64_t KEY = 0xDEADBEEF00000001ULL;
    KVPair pairs[2] = {{KEY, 111}, {KEY, 222}};

    KVPair *d_table, *d_input; uint32_t *d_ov, ov;
    cudaMalloc(&d_table, CAP * sizeof(KVPair));
    cudaMalloc(&d_input, 2   * sizeof(KVPair));
    cudaMalloc(&d_ov,    sizeof(uint32_t));
    cudaMemset(d_table, 0, CAP * sizeof(KVPair));
    cudaMemset(d_ov,    0, sizeof(uint32_t));
    cudaMemcpy(d_input, pairs, 2 * sizeof(KVPair), cudaMemcpyHostToDevice);
    kv_insert_gpu(d_table, CAP, d_input, 2, d_ov);
    cudaDeviceSynchronize();

    KVPair h_tbl[CAP]; uint64_t got;
    cudaMemcpy(h_tbl, d_table, CAP * sizeof(KVPair), cudaMemcpyDeviceToHost);
    cudaMemcpy(&ov,   d_ov,    sizeof(uint32_t),     cudaMemcpyDeviceToHost);

    cudaFree(d_table); cudaFree(d_input); cudaFree(d_ov);

    /* count how many slots have KEY */
    int count = 0;
    for (uint32_t i = 0; i < CAP; i++)
        if (h_tbl[i].key == KEY) count++;

    int found = host_find(h_tbl, CAP, KEY, &got);

    if (ov != 0 || count != 1 || !found || got != 222)
        TFAIL("duplicate_key", "key duplicated or wrong val");
    else
        TPASS("duplicate_key");
}

/* ── main ─────────────────────────────────────────────────────── */
int main(void) {
    int dev; cudaGetDevice(&dev);
    cudaDeviceProp p; cudaGetDeviceProperties(&p, dev);
    printf("=== GPU KV S17 [%s sm_%d%d] ===\n",
           p.name, p.major, p.minor);

    t_insert_4k();
    t_insert_64k();
    t_spot_check();
    t_duplicate_key();

    printf("─────────────────────────────────\n");
    printf("%d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
