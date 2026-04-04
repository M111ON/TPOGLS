/*
 * bench_s24.c — Session 24 Benchmark
 * ====================================
 * Tests: scalar / auto-AVX2 / x8-accumulator / x8+B1(branchless route)
 *
 * Compile:
 *   gcc -O2 -mavx2 -o bench_s24 bench_s24.c -lm
 *
 * No external deps — self-contained
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── constants ─────────────────────────────────────────────────────── */
#define CYL_FULL_N    3072u
#define BENCH_ROUNDS  2000u
#define WARMUP_ROUNDS   50u
#define GEO_BUNDLE_WORDS 8u

/* ── theta_mix64 ───────────────────────────────────────────────────── */
static inline uint64_t theta_mix64(uint64_t x) {
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33; x *= UINT64_C(0xc4ceb9fe1a85ec53);
    return x ^ (x >> 33);
}

/* ── geo_route_addr_guard (branchless) ─────────────────────────────── */
#define GEO_SEED_GUARD UINT64_C(0x9E3779B97F4A7C15)
#define TOPO_MASK      UINT64_C(0x0000FFFFFFFFFFFF)

static inline uint64_t geo_route_addr_guard(uint64_t route_addr, uint64_t isect) {
    /* branchless version: mask selects PHI-mix or passthrough */
    uint64_t degenerate = ((route_addr & TOPO_MASK) == 0) ? UINT64_MAX : 0ULL;
    return route_addr ^ (degenerate & (GEO_SEED_GUARD ^ isect));
}

/* ── timer ─────────────────────────────────────────────────────────── */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── build core_raw (shared setup, not timed) ──────────────────────── */
static void build_core_raw(const uint64_t *raw, uint64_t *core, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint64_t h    = theta_mix64(raw[i]);
        uint8_t  face = (uint8_t)((((uint64_t)(uint32_t)(h>>32) * 12u)) >> 32);
        uint8_t  edge = (uint8_t)((((uint64_t)(uint32_t)(h    ) *  5u)) >> 32);
        core[i] = ((uint64_t)face << 59) | ((uint64_t)edge << 52)
                | (raw[i] & UINT64_C(0x000FFFFFFFFFFFFF));
    }
}

/* ── S23 baseline: scalar no-vec ───────────────────────────────────── */
__attribute__((noinline, target("no-avx,no-avx2,no-sse4.2")))
static uint64_t bench_scalar(const uint64_t *core, uint32_t n) {
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t r = core[i];
        uint64_t v = r & ((r>>8)|(r<<56)) & ((r>>16)|(r<<48)) & ((r>>24)|(r<<40));
        acc ^= v;
    }
    return acc;
}

/* ── S23 baseline: scalar + auto-AVX2 ─────────────────────────────── */
__attribute__((noinline, target("avx2")))
static uint64_t bench_scalar_avx2(const uint64_t *core, uint32_t n) {
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t r = core[i];
        uint64_t v = r & ((r>>8)|(r<<56)) & ((r>>16)|(r<<48)) & ((r>>24)|(r<<40));
        acc ^= v;
    }
    return acc;
}

/* ── S24: x8 multi-accumulator + branchless guard ──────────────────── *
 *
 * หลักการ:
 *   8 lanes อิสระ → ไม่มี dependency chain → compiler auto-vectorize เต็ม
 *   ไม่มี multiply → latency หาย
 *   branch guard ออกนอก loop → pipeline ไม่สะดุด
 *   seed walk ด้วย shift+add → entropy ดี ไม่มี imul
 */
__attribute__((noinline, target("avx2")))
static uint64_t bench_x8(const uint64_t *raw, uint32_t n) {
    uint64_t r0=0, r1=0, r2=0, r3=0, r4=0, r5=0, r6=0, r7=0;
    uint64_t seed = theta_mix64(raw[0]);

    #define STEP(idx, acc) do { \
        uint64_t x = raw[i+(idx)] ^ seed; \
        seed += UINT64_C(0x9E3779B97F4A7C15); \
        seed = (seed << 13) | (seed >> 51); \
        uint64_t isect = x \
            & ((x << 8)  | (x >> 56)) \
            & ((x << 16) | (x >> 48)) \
            & ((x << 24) | (x >> 40)); \
        uint64_t m = isect; \
        m ^= m << 13; \
        m ^= m >> 7; \
        m ^= m << 17; \
        (acc) ^= m; \
    } while(0)

    #pragma GCC ivdep
    for (uint32_t i = 0; i + 8u <= n; i += 8u) {
        STEP(0, r0); STEP(1, r1); STEP(2, r2); STEP(3, r3);
        STEP(4, r4); STEP(5, r5); STEP(6, r6); STEP(7, r7);
    }
    #undef STEP

    /* tree reduce — 3 levels */
    uint64_t r = (r0^r1) ^ (r2^r3) ^ (r4^r5) ^ (r6^r7);

    /* B1: guard ครั้งเดียวนอก loop — branchless */
    r = geo_route_addr_guard(r, r);
    return r;
}

/* ── x8 ทำงานกับ core_raw (pre-packed) แทน raw ─────────────────────── */
__attribute__((noinline, target("avx2")))
static uint64_t bench_x8_core(const uint64_t *core, uint32_t n) {
    uint64_t r0=0, r1=0, r2=0, r3=0, r4=0, r5=0, r6=0, r7=0;

    #define STEPC(idx, acc) do { \
        uint64_t x = core[i+(idx)]; \
        uint64_t isect = x \
            & ((x << 8)  | (x >> 56)) \
            & ((x << 16) | (x >> 48)) \
            & ((x << 24) | (x >> 40)); \
        uint64_t m = isect; m ^= m<<13; m ^= m>>7; m ^= m<<17; \
        (acc) ^= m; \
    } while(0)

    #pragma GCC ivdep
    for (uint32_t i = 0; i + 8u <= n; i += 8u) {
        STEPC(0,r0); STEPC(1,r1); STEPC(2,r2); STEPC(3,r3);
        STEPC(4,r4); STEPC(5,r5); STEPC(6,r6); STEPC(7,r7);
    }
    #undef STEPC

    uint64_t r = (r0^r1) ^ (r2^r3) ^ (r4^r5) ^ (r6^r7);
    r = geo_route_addr_guard(r, r);
    return r;
}

/* ── Option X: ตัด xorshift ออก — isect ตรงๆ ไม่มี m chain ──────── */
__attribute__((noinline, target("avx2")))
static uint64_t bench_x8_optX(const uint64_t *core, uint32_t n) {
    uint64_t r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0;

    #define STEPX(idx, acc) do { \
        uint64_t x = core[i+(idx)]; \
        (acc) ^= x \
            & ((x<<8) |(x>>56)) \
            & ((x<<16)|(x>>48)) \
            & ((x<<24)|(x>>40)); \
    } while(0)

    #pragma GCC ivdep
    for (uint32_t i = 0; i+8u <= n; i += 8u) {
        STEPX(0,r0); STEPX(1,r1); STEPX(2,r2); STEPX(3,r3);
        STEPX(4,r4); STEPX(5,r5); STEPX(6,r6); STEPX(7,r7);
    }
    #undef STEPX

    uint64_t r = (r0^r1)^(r2^r3)^(r4^r5)^(r6^r7);
    return geo_route_addr_guard(r, r);
}

/* ── Option Y: 8 seed อิสระ — แต่ละ lane เดิน seed ของตัวเอง ──────── */
__attribute__((noinline, target("avx2")))
static uint64_t bench_x8_optY(const uint64_t *raw, uint32_t n) {
    uint64_t r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0;

    /* seed แยก 8 ตัว — init ต่างกัน ไม่แชร์ state */
    uint64_t s0 = theta_mix64(raw[0] ^ UINT64_C(0x0000000000000001));
    uint64_t s1 = theta_mix64(raw[0] ^ UINT64_C(0x0000000100000000));
    uint64_t s2 = theta_mix64(raw[0] ^ UINT64_C(0x0000000200000000));
    uint64_t s3 = theta_mix64(raw[0] ^ UINT64_C(0x0000000300000000));
    uint64_t s4 = theta_mix64(raw[0] ^ UINT64_C(0x0000000400000000));
    uint64_t s5 = theta_mix64(raw[0] ^ UINT64_C(0x0000000500000000));
    uint64_t s6 = theta_mix64(raw[0] ^ UINT64_C(0x0000000600000000));
    uint64_t s7 = theta_mix64(raw[0] ^ UINT64_C(0x0000000700000000));

    #define STEPY(idx, acc, seed) do { \
        uint64_t x = raw[i+(idx)] ^ (seed); \
        (seed) = ((seed)<<13)|((seed)>>51); \
        uint64_t isect = x \
            & ((x<<8) |(x>>56)) \
            & ((x<<16)|(x>>48)) \
            & ((x<<24)|(x>>40)); \
        (acc) ^= isect; \
    } while(0)

    #pragma GCC ivdep
    for (uint32_t i = 0; i+8u <= n; i += 8u) {
        STEPY(0,r0,s0); STEPY(1,r1,s1); STEPY(2,r2,s2); STEPY(3,r3,s3);
        STEPY(4,r4,s4); STEPY(5,r5,s5); STEPY(6,r6,s6); STEPY(7,r7,s7);
    }
    #undef STEPY

    uint64_t r = (r0^r1)^(r2^r3)^(r4^r5)^(r6^r7);
    return geo_route_addr_guard(r, r);
}

/* ── C1: 1 load → 4 rotated streams — compute more per byte ────────── *
 *
 * 1 load x → derive v0..v3 via rotl (pure register, zero extra load)
 * isect ของแต่ละ rotation → pattern ต่างกัน → 4 independent results
 * 4 acc อิสระ → compiler vectorize เต็ม
 */
__attribute__((noinline, target("avx2")))
static uint64_t bench_x8_c1(const uint64_t *core, uint32_t n) {
    uint64_t r0=0, r1=0, r2=0, r3=0;

    #define ROTL(x, k) (((x)<<(k))|((x)>>(64-(k))))
    #define ISECT(x) ((x) \
        & (((x)<<8) |((x)>>56)) \
        & (((x)<<16)|((x)>>48)) \
        & (((x)<<24)|((x)>>40)))

    #pragma GCC ivdep
    for (uint32_t i = 0; i < n; i++) {
        uint64_t x  = core[i];          /* 1 load */
        uint64_t v0 = x;
        uint64_t v1 = ROTL(x,  7);     /* shift angle 1 */
        uint64_t v2 = ROTL(x, 13);     /* shift angle 2 */
        uint64_t v3 = ROTL(x, 29);     /* shift angle 3 — prime gap */
        r0 ^= ISECT(v0);
        r1 ^= ISECT(v1);
        r2 ^= ISECT(v2);
        r3 ^= ISECT(v3);
    }
    #undef ROTL
    #undef ISECT

    uint64_t r = (r0^r1)^(r2^r3);
    return geo_route_addr_guard(r, r);
}

/* ── C1 x8: C1 pattern + 8 unrolled lanes ──────────────────────────── */
__attribute__((noinline, target("avx2")))
static uint64_t bench_x8_c1_unroll(const uint64_t *core, uint32_t n) {
    uint64_t r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0;

    #define ROTL(x,k)  (((x)<<(k))|((x)>>(64-(k))))
    #define ISECT(x)   ((x)&(ROTL(x,8)|ROTL(x,56))&(ROTL(x,16)|ROTL(x,48))&(ROTL(x,24)|ROTL(x,40)))
    #define STEP4(xi, ra, rb, rc, rd) do { \
        uint64_t _x = (xi); \
        (ra) ^= ISECT(_x); \
        (rb) ^= ISECT(ROTL(_x, 7)); \
        (rc) ^= ISECT(ROTL(_x,13)); \
        (rd) ^= ISECT(ROTL(_x,29)); \
    } while(0)

    #pragma GCC ivdep
    for (uint32_t i = 0; i+2u <= n; i += 2u) {
        STEP4(core[i+0], r0, r1, r2, r3);
        STEP4(core[i+1], r4, r5, r6, r7);
    }
    #undef STEP4
    #undef ISECT
    #undef ROTL

    uint64_t r = (r0^r1)^(r2^r3)^(r4^r5)^(r6^r7);
    return geo_route_addr_guard(r, r);
}

/* ── Option X16: unroll 16 lanes — push past AVX2 ceiling ──────────── */
__attribute__((noinline, target("avx2")))
static uint64_t bench_x16_optX(const uint64_t *core, uint32_t n) {
    uint64_t r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0;
    uint64_t r8=0,r9=0,ra=0,rb=0,rc=0,rd=0,re=0,rf=0;

    #define STEPX16(idx, acc) do { \
        uint64_t x = core[i+(idx)]; \
        (acc) ^= x \
            & ((x<<8) |(x>>56)) \
            & ((x<<16)|(x>>48)) \
            & ((x<<24)|(x>>40)); \
    } while(0)

    #pragma GCC ivdep
    for (uint32_t i = 0; i+16u <= n; i += 16u) {
        STEPX16( 0,r0); STEPX16( 1,r1); STEPX16( 2,r2); STEPX16( 3,r3);
        STEPX16( 4,r4); STEPX16( 5,r5); STEPX16( 6,r6); STEPX16( 7,r7);
        STEPX16( 8,r8); STEPX16( 9,r9); STEPX16(10,ra); STEPX16(11,rb);
        STEPX16(12,rc); STEPX16(13,rd); STEPX16(14,re); STEPX16(15,rf);
    }
    #undef STEPX16

    /* tree reduce 4 levels */
    uint64_t r = ((r0^r1)^(r2^r3)) ^ ((r4^r5)^(r6^r7))
               ^ ((r8^r9)^(ra^rb)) ^ ((rc^rd)^(re^rf));
    return geo_route_addr_guard(r, r);
}

/* ── pthread 2-thread: split array → 2 × optX parallel ─────────────── */
#include <pthread.h>

/* ── thread pool: create once, reuse per round via semaphore ────────── */
#include <pthread.h>
#include <semaphore.h>

typedef struct {
    const uint64_t *core;
    uint32_t        n;
    uint64_t        result;
    sem_t           go;     /* main → worker: start */
    sem_t           done;   /* worker → main: finished */
    int             quit;
} PoolArg;

static void *pool_worker(void *arg) {
    PoolArg *a = (PoolArg *)arg;
    while (1) {
        sem_wait(&a->go);
        if (a->quit) break;

        uint64_t r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0;
        const uint64_t *core = a->core;
        uint32_t n = a->n;

        #define ISX(x) ((x)&(((x)<<8)|((x)>>56))&(((x)<<16)|((x)>>48))&(((x)<<24)|((x)>>40)))
        #pragma GCC ivdep
        for (uint32_t i = 0; i+8u <= n; i += 8u) {
            r0^=ISX(core[i+0]); r1^=ISX(core[i+1]);
            r2^=ISX(core[i+2]); r3^=ISX(core[i+3]);
            r4^=ISX(core[i+4]); r5^=ISX(core[i+5]);
            r6^=ISX(core[i+6]); r7^=ISX(core[i+7]);
        }
        #undef ISX

        a->result = (r0^r1)^(r2^r3)^(r4^r5)^(r6^r7);
        sem_post(&a->done);
    }
    return NULL;
}

/* global pool — init once */
static PoolArg  g_pool[2];
static pthread_t g_threads[2];
static int       g_pool_ready = 0;

static void pool_init(void) {
    if (g_pool_ready) return;
    for (int i = 0; i < 2; i++) {
        sem_init(&g_pool[i].go,   0, 0);
        sem_init(&g_pool[i].done, 0, 0);
        g_pool[i].quit = 0;
        pthread_create(&g_threads[i], NULL, pool_worker, &g_pool[i]);
    }
    g_pool_ready = 1;
}

__attribute__((noinline))
static uint64_t bench_pool2(const uint64_t *core, uint32_t n) {
    pool_init();
    uint32_t half = (n / 2) & ~7u;

    g_pool[0].core = core;        g_pool[0].n = half;
    g_pool[1].core = core + half; g_pool[1].n = n - half;

    sem_post(&g_pool[0].go);
    sem_post(&g_pool[1].go);
    sem_wait(&g_pool[0].done);
    sem_wait(&g_pool[1].done);

    uint64_t r = g_pool[0].result ^ g_pool[1].result;
    return geo_route_addr_guard(r, r);
}

static void print_row(const char *label, double melem_s, double base) {
    if (base <= 0.0)
        printf("  %-28s %8.1f Melem/s  (baseline)\n", label, melem_s);
    else
        printf("  %-28s %8.1f Melem/s  %.2fx\n", label, melem_s, melem_s/base);
}

/* ── main ──────────────────────────────────────────────────────────── */
int main(void) {
    uint32_t n  = CYL_FULL_N;
    size_t   sz = (size_t)n * sizeof(uint64_t);

    uint64_t *raw  = (uint64_t*)malloc(sz);
    uint64_t *core = (uint64_t*)malloc(sz);

    for (uint32_t i = 0; i < n; i++)
        raw[i] = theta_mix64((uint64_t)i * UINT64_C(0x9E3779B97F4A7C15));
    build_core_raw(raw, core, n);

    printf("\n=== bench_s24  (n=%u, rounds=%u) ===\n\n", n, BENCH_ROUNDS);
    printf("  [S23 baselines]\n");

    volatile uint64_t sink = 0;
    double t0, t1, scalar_base = 0.0;

    #define TIMEIT(label, expr, base) \
        for (uint32_t _w=0; _w<WARMUP_ROUNDS; _w++) sink ^= (expr); \
        t0 = now_sec(); \
        for (uint32_t _r=0; _r<BENCH_ROUNDS; _r++) sink ^= (expr); \
        t1 = now_sec(); \
        print_row(label, ((double)BENCH_ROUNDS*n)/(t1-t0)/1e6, base)

    TIMEIT("scalar (no-vec)",       bench_scalar(core, n),       0.0);
    scalar_base = ((double)BENCH_ROUNDS*n)/(t1-t0)/1e6;

    TIMEIT("scalar + auto-AVX2",    bench_scalar_avx2(core, n),  scalar_base);

    printf("\n  [S24 new paths]\n");
    TIMEIT("x8 multi-acc (raw+seed)", bench_x8(raw, n),          scalar_base);
    TIMEIT("x8 multi-acc (core)",     bench_x8_core(core, n),    scalar_base);

    printf("\n  [S24 optimized]\n");
    TIMEIT("x8 optX (isect direct)",  bench_x8_optX(core, n),    scalar_base);
    TIMEIT("x8 optY (8 seeds indep)", bench_x8_optY(raw,  n),    scalar_base);
    TIMEIT("x16 optX (isect direct)", bench_x16_optX(core, n),   scalar_base);

    printf("\n  [C1: 1 load → 4 streams]\n");
    TIMEIT("C1 x4 rotated streams",   bench_x8_c1(core, n),       scalar_base);
    TIMEIT("C1 x4 + x2 unroll",       bench_x8_c1_unroll(core,n), scalar_base);

    printf("\n  [A: pthread 2-thread]\n");
    TIMEIT("2-thread pool × optX",     bench_pool2(core, n),      scalar_base);

    #undef TIMEIT

    printf("\n  sink=0x%llx\n\n", (unsigned long long)sink);
    free(raw); free(core);
    return 0;
}
