/*
 * pogls_fold.c — POGLS V3.6 Geometric Fold — Batch Verify Pipeline
 * =================================================================
 *
 * GPU-optional: ตรวจสอบ N blocks พร้อมกัน
 *   - ถ้ามี CUDA  → offload L1+L2 ไป GPU kernel
 *   - ถ้ามี AVX2  → SIMD 4 blocks/cycle บน CPU
 *   - fallback    → scalar CPU ทำงานได้เสมอ
 *
 * ไม่มี dependency บังคับ:
 *   POGLS_HAVE_CUDA=1  → ใช้ GPU
 *   __AVX2__           → ใช้ AVX2 (compiler flag)
 *   nothing            → scalar fallback อัตโนมัติ
 *
 * compile scalar:
 *   gcc -O2 -o fold_test pogls_fold_test.c pogls_fold.c
 * compile AVX2:
 *   gcc -O2 -mavx2 -o fold_test pogls_fold_test.c pogls_fold.c
 */

#include "pogls_fold.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── detect backend ─────────────────────────────────────────────── */
#ifdef POGLS_HAVE_CUDA
#  define FOLD_BACKEND_GPU    1
#  define FOLD_BACKEND_NAME   "CUDA"
#elif defined(__AVX2__)
#  define FOLD_BACKEND_AVX2   1
#  define FOLD_BACKEND_NAME   "AVX2"
#else
#  define FOLD_BACKEND_SCALAR 1
#  define FOLD_BACKEND_NAME   "scalar"
#endif

/* ═══════════════════════════════════════════════════════════════════════
   BATCH RESULT ARRAY
   result[i]:
     FOLD_VERIFY_PASS    =  0
     FOLD_VERIFY_EJECT_1 = -1  (XOR fail)
     FOLD_VERIFY_EJECT_2 = -2  (Fibo fail — future)
     FOLD_VERIFY_NEED_L3 =  1  (Merkle required)
   ═══════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════
   SCALAR BACKEND  — always available, no dependencies
   ═══════════════════════════════════════════════════════════════════════ */

static void _fold_batch_scalar(const DiamondBlock *blocks,
                                int8_t             *results,
                                uint32_t            count)
{
    for (uint32_t i = 0; i < count; i++)
        results[i] = (int8_t)fold_verify(&blocks[i]);
}

/* ═══════════════════════════════════════════════════════════════════════
   AVX2 BACKEND  — 4 blocks per cycle (256-bit XOR)
   compile: gcc -mavx2
   ═══════════════════════════════════════════════════════════════════════ */

#ifdef FOLD_BACKEND_AVX2

#include <immintrin.h>

/*
 * _fold_batch_avx2 — Layer 1 XOR Audit บน 4 blocks พร้อมกัน
 *
 * Load 4 × (core.raw XOR invert) → ต้องเป็น 0xFFFFFFFFFFFFFFFF ทุกตัว
 * Layer 2 fallback to scalar (Fibo Intersect ไม่ได้ประโยชน์จาก 256-bit
 *   ในแบบง่ายเพราะต้องการ 4 AND ต่อ block ซึ่งยังเร็วพอ scalar)
 */
static void _fold_batch_avx2(const DiamondBlock *blocks,
                              int8_t             *results,
                              uint32_t            count)
{
    /* process 4 blocks at a time */
    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        /* load core XOR invert ของ 4 blocks ใส่ AVX2 register */
        uint64_t xr[4];
        xr[0] = blocks[i+0].core.raw ^ blocks[i+0].invert;
        xr[1] = blocks[i+1].core.raw ^ blocks[i+1].invert;
        xr[2] = blocks[i+2].core.raw ^ blocks[i+2].invert;
        xr[3] = blocks[i+3].core.raw ^ blocks[i+3].invert;

        __m256i v = _mm256_loadu_si256((const __m256i *)xr);
        __m256i mask = _mm256_set1_epi64x((int64_t)0xFFFFFFFFFFFFFFFFull);
        __m256i cmp  = _mm256_cmpeq_epi64(v, mask);
        int bits     = _mm256_movemask_epi8(cmp);   /* 32 bits, 8 per lane */

        /* ตรวจทีละ block */
        for (int j = 0; j < 4; j++) {
            int lane_ok = (bits >> (j * 8)) & 0xFF;
            if (lane_ok != 0xFF) {
                results[i+j] = FOLD_VERIFY_EJECT_1;
                continue;
            }
            /* Layer 2 scalar */
            results[i+j] = (int8_t)fold_verify(&blocks[i+j]);
        }
    }
    /* tail — scalar */
    for (; i < count; i++)
        results[i] = (int8_t)fold_verify(&blocks[i]);
}

#endif /* FOLD_BACKEND_AVX2 */

/* ═══════════════════════════════════════════════════════════════════════
   GPU STUB  — placeholder สำหรับ CUDA integration
   ถ้า POGLS_HAVE_CUDA=1 ต้อง link pogls_fold_cuda.cu
   ═══════════════════════════════════════════════════════════════════════ */

#ifdef FOLD_BACKEND_GPU

/* declared in pogls_fold_cuda.cu */
extern void pogls_fold_cuda_batch(const DiamondBlock *blocks,
                                   int8_t             *results,
                                   uint32_t            count);

static void _fold_batch_gpu(const DiamondBlock *blocks,
                             int8_t             *results,
                             uint32_t            count)
{
    pogls_fold_cuda_batch(blocks, results, count);
}

#endif /* FOLD_BACKEND_GPU */

/* ═══════════════════════════════════════════════════════════════════════
   PUBLIC API — fold_batch_verify()
   ═══════════════════════════════════════════════════════════════════════ */

/*
 * fold_batch_verify — ตรวจสอบ N blocks
 *
 * blocks   : array ของ DiamondBlock จำนวน count
 * results  : caller allocate int8_t[count]
 * count    : จำนวน blocks
 *
 * เลือก backend อัตโนมัติ:
 *   GPU (CUDA) > AVX2 > scalar
 *
 * ไม่มี malloc, ไม่มี branch ใน hot path
 */
void fold_batch_verify(const DiamondBlock *blocks,
                       int8_t             *results,
                       uint32_t            count)
{
    if (!blocks || !results || count == 0) return;

#ifdef FOLD_BACKEND_GPU
    _fold_batch_gpu(blocks, results, count);
#elif defined(FOLD_BACKEND_AVX2)
    _fold_batch_avx2(blocks, results, count);
#else
    _fold_batch_scalar(blocks, results, count);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
   fold_batch_eject — กรอง eject ออก คืน count ที่ผ่าน
   ผล: blocks ที่ผ่านอยู่หน้า array, NEED_L3 อยู่ถัดมา
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t pass;     /* จำนวน block ที่ผ่าน (PASS + NEED_L3) */
    uint32_t eject;    /* จำนวน block ที่ถูก eject */
    uint32_t need_l3;  /* จำนวนที่ต้องการ Merkle */
} FoldBatchStats;

FoldBatchStats fold_batch_eject(DiamondBlock *blocks,
                                 int8_t       *results,
                                 uint32_t      count)
{
    FoldBatchStats s = {0, 0, 0};
    uint32_t write = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (results[i] == FOLD_VERIFY_EJECT_1 ||
            results[i] == FOLD_VERIFY_EJECT_2) {
            s.eject++;
            continue;   /* ทิ้ง — ไม่ถึง Fabric */
        }
        if (results[i] == FOLD_VERIFY_NEED_L3)
            s.need_l3++;
        /* move valid block ไปด้านหน้า (in-place compaction) */
        if (write != i) {
            blocks[write]  = blocks[i];
            results[write] = results[i];
        }
        write++;
        s.pass++;
    }
    return s;
}

/* ═══════════════════════════════════════════════════════════════════════
   fold_backend_name — รายงาน backend ที่ใช้อยู่
   ═══════════════════════════════════════════════════════════════════════ */

const char *fold_backend_name(void)
{
    return FOLD_BACKEND_NAME;
}

/* ═══════════════════════════════════════════════════════════════════════
   fold_world_split — แยก blocks เป็น World A / B
   เรียกหลัง fold_batch_verify() เพื่อ route ไป lane ถูก
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t count_a;   /* จำนวน World A blocks */
    uint32_t count_b;   /* จำนวน World B blocks */
} FoldSplitStats;

/*
 * fold_world_split — แยก blocks ตาม Switch Gate
 *
 * out_a, out_b : caller allocate [count] ขนาด
 * idx_a, idx_b : indices ของ blocks ที่ถูก route (optional, NULL ok)
 */
FoldSplitStats fold_world_split(const DiamondBlock *blocks,
                                 uint32_t            count,
                                 DiamondBlock       *out_a,
                                 DiamondBlock       *out_b)
{
    FoldSplitStats s = {0, 0};

    for (uint32_t i = 0; i < count; i++) {
        if (fold_switch_gate(&blocks[i]) == WORLD_A)
            out_a[s.count_a++] = blocks[i];
        else
            out_b[s.count_b++] = blocks[i];
    }
    return s;
}
