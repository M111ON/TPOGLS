/*
 * geo_diamond_field.h — Diamond Cell Field (branch จาก geo_final_v1.h)
 *
 * แนวคิด: DiamondBlock เป็น 1 cell ของ field
 *   - core.raw = OO center (ข้อมูลจริง)
 *   - invert   = complement (pair invariant: core ^ invert == FULL_MASK)
 *   - quad_mirror = 4 rotated copies = 4 direction ใน 1 block
 *   - fold_fibo_intersect() = AND ทั้ง 4 = bits ที่คงอยู่ทุก direction
 *
 * pair sum invariant: ทุก pair บวกกันได้ค่าเดิมเสมอ
 *   → ถ้า intersect drift จาก baseline = cell ไม่ intact
 *   → ไม่ต้องมี external checksum
 *
 * 4 cells × 36 active bits = 144 = TE_CYCLE (sync กัน clock เอง)
 */

#ifndef GEO_DIAMOND_FIELD_H
#define GEO_DIAMOND_FIELD_H

#include <stdint.h>
#ifdef __AVX2__
#  include <immintrin.h>
#endif
#include "pogls_fold.h"

/* ── BridgeEntry / BRIDGE_MASK ─────────────────────────────────────
 * ของเดิมมาจาก geo_final_v1.h — self-contained ตรงนี้แทน
 * value(8B) = route_addr, addr(4B) = hop|drift|tag packed
 */
#ifndef BRIDGE_MASK
#  define BRIDGE_MASK 0xFFu   /* ring size 256 */
typedef struct __attribute__((packed)) {
    uint64_t value;
    uint32_t addr;
} BridgeEntry;
#endif

#ifndef unlikely
#  define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* ── pair invariant score ──────────────────────────────────────────
 * วัด drift ของ cell จาก baseline intersect
 * cost: fold_fibo_intersect (4 AND) + popcount + sub = ~5ns
 * return 0 = perfect, >0 = drift level
 */
static inline uint32_t diamond_drift_score(const DiamondBlock *b,
                                            uint64_t baseline)
{
    uint64_t intersect = fold_fibo_intersect(b);
    /* bits ที่หายไปจาก baseline = drift */
    uint64_t lost = baseline & ~intersect;
    return (uint32_t)__builtin_popcountll(lost);
}

/* ── cell integrity gate ───────────────────────────────────────────
 * L0: XOR pair check (ฟรี ~0.3ns)
 * L1: fibo intersect drift (cheap ~5ns, 1/8 sampling)
 * return: 0=drop, 1=pass, 2=pass+drift_detected
 */
static inline int diamond_gate(const DiamondBlock *b,
                                uint64_t baseline,
                                uint32_t *drift_acc)
{
    /* L0: hard gate */
    if (!fold_xor_audit(b)) return 0;

    /* L1: skip heuristic — sample 1/8 */
    if ((b->core.raw & 7u) == 0u) {
        uint32_t d = diamond_drift_score(b, baseline);
        *drift_acc += d;
        if (unlikely(*drift_acc > 72u)) {
            *drift_acc = 0;
            return 2;  /* drift detected → caller triggers verify */
        }
    }

    return 1;
}

/* ── cell route ────────────────────────────────────────────────────
 * ใช้ fibo_intersect popcount แทน addr parity
 * even popcount = MAIN (symmetric), odd = TEMPORAL (asymmetric)
 * ตรงกับ icosa/dodeca dual โดยไม่ต้องมี external addr
 */
static inline int diamond_route(const DiamondBlock *b)
{
    uint64_t intersect = fold_fibo_intersect(b);
    return (__builtin_popcountll(intersect) & 1u) ? 1 : 2;
    /* 1=MAIN(icosa), 2=TEMP(dodeca) */
}

/* ── skip over zero zones ──────────────────────────────────────────
 * ให้ cell วิ่งข้ามโซน 0 ด้วย ctz (count trailing zeros)
 * แทนที่จะ loop ทีละ bit ใช้ 1 instruction กระโดดข้าม
 * return: next active bit position, 64 = ไม่มีแล้ว
 */
static inline int diamond_next_active(uint64_t cell_mask, int from_pos)
{
    uint64_t remaining = cell_mask >> from_pos;
    if (!remaining) return 64;
    return from_pos + __builtin_ctzll(remaining);
}

/* ── 4-cell batch (144 ops = 1 TE_CYCLE) ──────────────────────────
 * process 4 DiamondBlock พร้อมกัน = 1 complete clock cycle
 * drift_acc shared across 4 cells = amortized cost
 */
static inline uint32_t diamond_batch4(const DiamondBlock b[4],
                                       uint64_t baseline,
                                       uint32_t *drift_acc,
                                       int routes[4])
{
    uint32_t passed = 0;
    for (int i = 0; i < 4; i++) {
        int g = diamond_gate(&b[i], baseline, drift_acc);
        if (g == 0) { routes[i] = 0; continue; }
        routes[i] = diamond_route(&b[i]);
        passed++;
    }
    return passed;
}

/* ── baseline derive ───────────────────────────────────────────────
 * สร้าง baseline intersect จาก "perfect" cell
 * ใช้ PHI_UP เป็น seed เพราะเป็น frozen constant ของระบบ
 * ทุก cell ที่ healthy ควร intersect ≥ baseline
 */
static inline uint64_t diamond_baseline(void)
{
    /* สร้าง perfect cell จาก PHI ratio โดยตรง
     * PHI_UP / PHI_DOWN = phi² → bit pattern กระจายทั่ว 64-bit
     * ใช้เป็น reference ว่า "healthy cell ควร intersect อะไร" */
    DiamondBlock ref;
    memset(&ref, 0, sizeof(ref));
    /* raw = interleave PHI_UP และ PHI_DOWN → bit dense */
    /* anchor pattern: ทุก byte = 9 = invariant ของ cell
     * survive rotation AND เพราะสมมาตรสมบูรณ์
     * popcount=16 = 16 anchor bits ต่อ cell */
    ref.core.raw = 0x0909090909090909ULL;
    ref.invert   = ~ref.core.raw;
    fold_build_quad_mirror(&ref);
    return fold_fibo_intersect(&ref);
}


/* ── reversible offset (symmetric shift) ──────────────────────────
 * +n shift ด้านหนึ่ง, -n กลับมาได้เสมอ
 * encode offset ไว้ใน cell เองผ่าน mirror_stride
 * shift_fwd = rotate left  n bits (icosa side)
 * shift_rev = rotate right n bits (dodeca side)
 * ทั้งคู่ใช้ n เดียวกัน → reversible โดย definition
 */
static inline uint64_t diamond_shift_fwd(uint64_t raw, uint8_t n) {
    n &= 63u;
    return (raw << n) | (raw >> (64u - n));
}
static inline uint64_t diamond_shift_rev(uint64_t raw, uint8_t n) {
    n &= 63u;
    return (raw >> n) | (raw << (64u - n));
}

/* ── compress overlap ──────────────────────────────────────────────
 * สองฝั่งที่ทับซ้อนกัน (intersect) บีบรวมเป็นค่าเดียว
 * เก็บ offset ที่ "ขาตัวเลข" = lower bits ของ result
 * overlap = AND (สิ่งที่เหมือนกันในทั้งสองฝั่ง)
 * unique_a = a & ~b, unique_b = b & ~a (ต่างกัน)
 * packed = overlap | (offset << popcount(overlap))
 */
static inline uint64_t diamond_compress_overlap(uint64_t a, uint64_t b,
                                                  uint8_t *offset_out) {
    uint64_t overlap  = a & b;
    uint64_t unique_a = a & ~b;
    uint64_t unique_b = b & ~a;
    /* offset = symmetric diff popcount → บอกว่าต่างกันแค่ไหน */
    *offset_out = (uint8_t)(__builtin_popcountll(unique_a ^ unique_b) & 0xFFu);
    /* compress: เก็บแค่ overlap + offset เล็กๆ */
    return overlap | (((uint64_t)*offset_out) << __builtin_popcountll(overlap));
}

/* ── DNA write trigger ─────────────────────────────────────────────
 * เรียกตอน data วิ่งสุดทาง (intersect→0 หรือ drift fire)
 * "เขียนสิ่งที่เป็น" = route address ที่ผ่านมา + offset สุดท้าย
 * ไม่คำนวณอะไรเพิ่ม แค่บันทึก route_addr ลง honeycomb
 *
 * honeycomb layout (ใช้ของที่มีอยู่แล้ว):
 *   merkle_root (8B) = route_addr ของ flow นี้
 *   dna_count   (2B) = จำนวน hops ที่ผ่านมา
 *   reserved[0] (1B) = offset สุดท้าย
 */
static inline void diamond_dna_write(DiamondBlock *b,
                                      uint64_t route_addr,
                                      uint16_t hop_count,
                                      uint8_t  last_offset) {
    HoneycombSlot s;
    memset(&s, 0, sizeof(s));
    s.merkle_root = route_addr;   /* route address ของ flow = DNA */
    s.dna_count   = hop_count;
    s.reserved[0] = last_offset;
    honeycomb_write(b, &s);
}

/* ── end-of-flow detector ──────────────────────────────────────────
 * "สุดทาง" = intersect → 0 (ไม่มี geometric constant เหลือ)
 * หรือ drift_acc เกิน threshold
 * trigger DNA write ทันที ไม่รอ
 */
/* flow boundary reasons — explicit, ใช้เดียวกันทุกจุด */
typedef enum {
    FLOW_CONTINUE = 0,
    FLOW_END_DEAD = 1,   /* intersect == 0 */
    FLOW_END_DRIFT = 2,  /* drift_acc > 72 */
    FLOW_END_RING = 3    /* ring full */
} FlowEndReason;

/* isect passed in — caller already computed it, no recompute */
static inline FlowEndReason diamond_flow_end(const DiamondBlock *b,
                                              uint64_t baseline,
                                              uint64_t isect,
                                              uint64_t route_addr,
                                              uint16_t hop_count,
                                              uint32_t drift_acc) {
    FlowEndReason reason = FLOW_CONTINUE;

    if      (isect == 0)         reason = FLOW_END_DEAD;
    else if (drift_acc > 72u)    reason = FLOW_END_DRIFT;

    if (reason != FLOW_CONTINUE) {
        uint8_t offset = (reason == FLOW_END_DEAD)
            ? (uint8_t)__builtin_popcountll(baseline)
            : (uint8_t)(__builtin_popcountll(baseline & ~isect) & 0xFFu);
        diamond_dna_write((DiamondBlock*)b, route_addr, hop_count, offset);
    }
    return reason;
}

/* ── accumulated batch context ─────────────────────────────────────
 * สะสม route_addr และ hop_count ระหว่าง batch
 * route_addr = XOR ของ intersect ทุก cell ที่ผ่านมา
 *   → deterministic fingerprint ของ path โดยไม่ต้องเก็บ history
 * hop_count  = จำนวน cell ที่ผ่านจริง (ไม่นับ drop)
 */
typedef struct {
    uint64_t route_addr;   /* accumulated XOR of intersects */
    uint16_t hop_count;    /* cells passed (not dropped)    */
    uint16_t _pad;
    uint32_t drift_acc;    /* drift accumulator             */
} DiamondFlowCtx;

static inline void diamond_flow_init(DiamondFlowCtx *ctx) {
    ctx->route_addr = 0;
    ctx->hop_count  = 0;
    ctx->_pad       = 0;
    ctx->drift_acc  = 0;
}

/* ── invertible route constants ────────────────────────────────────
 * P     = golden-ratio prime (odd → invertible mod 2^64)
 * P_INV = modular inverse: P * P_INV ≡ 1 (mod 2^64)  [verified]
 * K     = rotate amount (must match encode + decode)
 */
#define DIAMOND_ROUTE_P     0x9E3779B185EBCA87ULL
#define DIAMOND_ROUTE_P_INV 0x0887493432BADB37ULL
#define DIAMOND_ROUTE_K     7u
#ifndef DIAMOND_HOP_MAX
#  define DIAMOND_HOP_MAX 60000u
#endif

/* encode: accumulate intersect → invertible, replay exact ได้ */
static inline uint64_t diamond_route_update(uint64_t r, uint64_t intersect) {
    return ((r << DIAMOND_ROUTE_K) | (r >> (64u - DIAMOND_ROUTE_K)))
           ^ (intersect * DIAMOND_ROUTE_P);
}

/* ── batch loop with flow accumulation ────────────────────────────
 * process cells ต่อเนื่อง สะสม route_addr ไปเรื่อยๆ
 * ตอน flow_end fire → DNA ที่เขียนลง honeycomb คือ
 *   fingerprint ของ path จริงที่ data วิ่งผ่านมาทั้งหมด
 * return: จำนวน DNA writes ที่เกิดขึ้นใน batch นี้
 */
static inline uint32_t diamond_batch_run(
    DiamondBlock      *cells,
    uint32_t           n,
    uint64_t           baseline,
    DiamondFlowCtx    *ctx)
{
    uint32_t dna_writes = 0;

    for (uint32_t i = 0; i < n; i++) {
        /* L0: hard gate — free ~0.3ns */
        if (!fold_xor_audit(&cells[i])) continue;

        /* single intersect compute per cell — hot path only */
        uint64_t isect = fold_fibo_intersect(&cells[i]);

        /* ── hot path: encode-only, register-resident ── */
        uint64_t r_next = diamond_route_update(ctx->route_addr, isect);

        /* drift sample 1/8 */
        if ((cells[i].core.raw & 7u) == 0u) {
            ctx->drift_acc += (uint32_t)__builtin_popcountll(baseline & ~isect);
        }

        /* flow boundary check (uses pre-computed isect — no recompute) */
        FlowEndReason reason = diamond_flow_end(&cells[i], baseline, isect,
                                                 r_next, ctx->hop_count,
                                                 ctx->drift_acc);
        if (unlikely(reason != FLOW_CONTINUE)) {
            dna_writes++;
            ctx->route_addr = 0; ctx->hop_count = 0; ctx->drift_acc = 0;
            continue;
        }

        /* advance state — stay in register */
        ctx->route_addr = r_next;
        ctx->hop_count++;
    }

    return dna_writes;
}

/* ── temporal carry (ใช้ BridgeEntry ring เป็น buffer) ────────────
 * เมื่อ batch จบแต่ flow ยังไม่สิ้นสุด:
 *   → serialize DiamondFlowCtx → push เข้า temp_ring
 *   → batch ถัดไป: pop → deserialize → resume ctx
 *
 * ไม่ต้องสร้าง buffer ใหม่ temporal ring ที่มีอยู่แล้วรับงานนี้ได้
 *
 * packing: DiamondFlowCtx 12B → fit ใน BridgeEntry (value=8B + addr=4B)
 *   value (8B) = route_addr (8B)
 *   addr  (4B) = hop_count(16b) | drift_acc_hi(8b) | offset(8b)
 *   drift_acc ถูก cap ที่ 255 ตอน pack (ถ้าเกิน threshold ควร fire แล้ว)
 */
/* drift overflow guard: ถ้า drift_acc > 254 ควร fire DNA ก่อน push
 * caller ต้องเรียก diamond_flow_end() ก่อนถ้า drift_acc สูง
 * push นี้ใช้เฉพาะ incomplete flow ที่ drift ยังในขอบเขต */
static inline void diamond_ctx_push_temp(
    BridgeEntry    *temp_ring,
    uint32_t       *temp_head,
    const DiamondFlowCtx *ctx)
{
    uint32_t i = (*temp_head) & BRIDGE_MASK;
    temp_ring[i].value = ctx->route_addr;
    /* header: [ tag=0xD1(8) | ver=0(4) | flags(4) | drift(8) | reserved(8) ]
     * flags bit0=has_overflow  bit1=resumed  bit2=partial */
    uint8_t flags = 0u;
    if (ctx->drift_acc > 72u)  flags |= 0x1u;  /* near-threshold */
    uint8_t drift_packed = (uint8_t)(ctx->drift_acc > 254u ? 254u : ctx->drift_acc);
    /* encode: hop(16) | drift(8) | [ tag(4) ver(4) | flags(4) reserved(4) ] */
    temp_ring[i].addr  = ((uint32_t)ctx->hop_count & 0xFFFFu)
                       | ((uint32_t)drift_packed   << 16)
                       | (0xD0u                    << 24)   /* tag=0xD, ver=0 */
                       | ((uint32_t)(flags & 0xFu) << 20);  /* flags in bits 20-23 */
    (*temp_head)++;
}

static inline int diamond_ctx_pop_temp(
    BridgeEntry    *temp_ring,
    uint32_t       *temp_tail,
    uint32_t        temp_head,
    DiamondFlowCtx *ctx_out)
{
    if (*temp_tail == temp_head) return 0;  /* empty */
    uint32_t i = (*temp_tail) & BRIDGE_MASK;
    /* ตรวจ magic tag */
    if ((temp_ring[i].addr >> 28) != 0xDu) return 0;  /* tag check */
    ctx_out->route_addr = temp_ring[i].value;
    ctx_out->hop_count  = (uint16_t)(temp_ring[i].addr & 0xFFFFu);
    ctx_out->drift_acc  = (uint32_t)((temp_ring[i].addr >> 16) & 0xFFu);
    ctx_out->_pad       = 0;
    (*temp_tail)++;
    return 1;
}

/* ── batch with temporal carry ────────────────────────────────────
 * drop-in แทน diamond_batch_run() เพิ่ม:
 *   1. resume ctx จาก temp_ring ถ้ามี carry จาก batch ก่อน
 *   2. push ctx เข้า temp_ring ถ้า batch จบแต่ flow ยังไม่สิ้นสุด
 *
 * caller loop:
 *   DiamondFlowCtx ctx; diamond_flow_init(&ctx);
 *   uint32_t th=0, tt=0;
 *   for each batch:
 *       diamond_batch_temporal(cells, n, baseline, &ctx,
 *                              temp_ring, &th, &tt);
 */
static inline uint32_t diamond_batch_temporal(
    DiamondBlock      *cells,
    uint32_t           n,
    uint64_t           baseline,
    DiamondFlowCtx    *ctx,
    BridgeEntry       *temp_ring,
    uint32_t          *temp_head,
    uint32_t          *temp_tail)
{
    /* 1. resume: ถ้ามี carry จาก batch ก่อน pop กลับมาก่อน
     * light check: verify tag ก่อน merge กัน corrupt carry */
    DiamondFlowCtx resume;
    if (diamond_ctx_pop_temp(temp_ring, temp_tail, *temp_head, &resume)) {
        /* restore chain: resume เป็น base ต่อตรงๆ ไม่ XOR */
        ctx->route_addr  = resume.route_addr;
        ctx->hop_count  += resume.hop_count;  /* cell count only — resume overhead excluded */
        ctx->drift_acc  += resume.drift_acc;
        /* hops semantics: resume.hop_count = cell count จาก batch ก่อน
         * batch_run นับเฉพาะ cells จริง ไม่นับ resume overhead */
    }

    /* 2. run batch ปกติ */
    uint32_t dna_writes = diamond_batch_run(cells, n, baseline, ctx);

    /* 3. carry: push เฉพาะ incomplete flow ที่ยังไม่ fire DNA
     * ถ้า dna_writes > 0 แต่ยังมี hop → push leftover ของ flow ใหม่เท่านั้น
     * drift_acc reset แล้วใน batch_run ตอน overflow fire */
    if (ctx->hop_count > 0) {
        /* temp_ring full guard: ถ้าเต็มหรือ drift overflow → force DNA write */
        uint32_t used = (*temp_head - *temp_tail) & BRIDGE_MASK;
        int overflow = (used >= (uint32_t)(BRIDGE_MASK - 1)) || (ctx->drift_acc > 254u);
        if (overflow && n > 0) {
            /* force write DNA ด้วย cell สุดท้ายใน batch */
            diamond_dna_write(&cells[n-1], ctx->route_addr,
                              ctx->hop_count, (uint8_t)(ctx->drift_acc & 0xFFu));
        } else {
            diamond_ctx_push_temp(temp_ring, temp_head, ctx);
        }
        diamond_flow_init(ctx);
    }

    return dna_writes;
}

/* ── replay tool — decode DNA → path info ──────────────────────────
 * อ่าน honeycomb ของ cell แล้ว reconstruct ว่า flow นั้น "เป็นอะไร"
 * ไม่ต้องมี history — route_addr encode path ทั้งหมดไว้แล้ว
 */
typedef struct {
    uint64_t route_addr;   /* positional XOR fingerprint ของ path */
    uint16_t hop_count;    /* จำนวน cells ที่ผ่านมา */
    uint8_t  offset;       /* drift distance จาก baseline */
    uint8_t  valid;        /* 1 = มี DNA จริง */
} DiamondReplay;

static inline DiamondReplay diamond_replay(const DiamondBlock *b) {
    DiamondReplay r;
    HoneycombSlot s = honeycomb_read(b);
    r.valid      = (s.dna_count > 0 || s.merkle_root != 0) ? 1u : 0u;
    r.route_addr = s.merkle_root;
    r.hop_count  = s.dna_count;
    r.offset     = s.reserved[0];
    return r;
}

/* decode exact intersect ที่ step k
 * ต้องการ r[step] และ r[step-1] (consecutive pair จาก route chain)
 * r[0] = diamond_route_update(0, i0)
 * r[n] = diamond_route_update(r[n-1], i_n)
 *
 * exact: i_k = ((curr ^ rotl(prev, K)) * P_INV) mod 2^64
 * stateless — ไม่ต้องเก็บ intersect[] เลย
 */
static inline uint64_t diamond_replay_step(uint64_t curr, uint64_t prev) {
    uint64_t x = (prev << DIAMOND_ROUTE_K) | (prev >> (64u - DIAMOND_ROUTE_K));
    return (curr ^ x) * DIAMOND_ROUTE_P_INV;
}

/* ── AVX2 replay — 4 flows in parallel ────────────────────────────
 * 1 lane = 1 independent flow (no cross-lane dependency)
 * requires: -mavx2
 *
 * usage:
 *   __m256i prev = _mm256_loadu_si256((__m256i*)prev_arr);  // r[k-1] x4
 *   __m256i curr = _mm256_loadu_si256((__m256i*)curr_arr);  // r[k]   x4
 *   __m256i out  = diamond_replay_step_x4(curr, prev);
 *   _mm256_storeu_si256((__m256i*)intersects, out);
 */
#ifdef __AVX2__
#include <immintrin.h>

/* rotl 64-bit x4 (compile-time k only — slli/srli require imm8) */
#define _diamond_rotl64_x4(x, k) \
    _mm256_or_si256(_mm256_slli_epi64((x), (k)), \
                    _mm256_srli_epi64((x), 64-(k)))

/* 64-bit lo multiply x4 via 32-bit split (AVX2 has no mullo_epi64)
 * result = (a * b) mod 2^64, per lane — verified correct */
static inline __m256i _diamond_mul64lo_x4(__m256i a, __m256i b) {
    const __m256i mask32 = _mm256_set1_epi64x(0xFFFFFFFFULL);
    __m256i a_lo = _mm256_and_si256(a, mask32);
    __m256i a_hi = _mm256_srli_epi64(a, 32);
    __m256i b_lo = _mm256_and_si256(b, mask32);
    __m256i b_hi = _mm256_srli_epi64(b, 32);

    __m256i lo_lo = _mm256_mul_epu32(a_lo, b_lo);
    __m256i mid   = _mm256_add_epi64(_mm256_mul_epu32(a_hi, b_lo),
                                     _mm256_mul_epu32(a_lo, b_hi));
    return _mm256_add_epi64(lo_lo, _mm256_slli_epi64(mid, 32));
}

/* decode 4 intersects from consecutive route pairs — exact, stateless */
static inline __m256i diamond_replay_step_x4(__m256i curr, __m256i prev) {
    __m256i x    = _diamond_rotl64_x4(prev, DIAMOND_ROUTE_K);
    __m256i diff = _mm256_xor_si256(curr, x);
    __m256i pinv = _mm256_set1_epi64x((int64_t)DIAMOND_ROUTE_P_INV);
    return _diamond_mul64lo_x4(diff, pinv);
}

/* unrolled: decode 8 flows (2x AVX2 registers) — hides mul latency */
static inline void diamond_replay_step_x8(
    const uint64_t curr[8], const uint64_t prev[8], uint64_t out[8])
{
    __m256i c0 = _mm256_loadu_si256((const __m256i*)curr);
    __m256i c1 = _mm256_loadu_si256((const __m256i*)(curr+4));
    __m256i p0 = _mm256_loadu_si256((const __m256i*)prev);
    __m256i p1 = _mm256_loadu_si256((const __m256i*)(prev+4));
    _mm256_storeu_si256((__m256i*)out,   diamond_replay_step_x4(c0, p0));
    _mm256_storeu_si256((__m256i*)(out+4), diamond_replay_step_x4(c1, p1));
}

#endif /* __AVX2__ */

#ifdef __AVX2__
/* ── SoA context for 4 parallel flows ─────────────────────────────
 * all fields uint64_t → each array = 1 __m256i load, no lane mismatch
 * hop/drift promoted to 64-bit to keep blendv_epi8 correct per-lane
 */
typedef struct {
    uint64_t route_addr[4];
    uint64_t hop_count[4];   /* u64 for SIMD lane alignment */
    uint64_t drift_acc[4];   /* u64 for SIMD lane alignment */
    /* pre-reset snapshot — valid only for lanes where bitmask bit is set */
    uint64_t snap_route[4];
    uint64_t snap_hop[4];
    uint64_t snap_drift[4];
} DiamondFlowCtx4;

static inline void diamond_flow4_init(DiamondFlowCtx4 *c) {
    _mm256_storeu_si256((__m256i*)c->route_addr, _mm256_setzero_si256());
    _mm256_storeu_si256((__m256i*)c->hop_count,  _mm256_setzero_si256());
    _mm256_storeu_si256((__m256i*)c->drift_acc,  _mm256_setzero_si256());
}

/* ── batch encode x4 — hot path, register-resident ────────────────
 * isect4[steps*4]: interleaved [f0,f1,f2,f3, f0,f1,f2,f3, ...]
 * baseline4: same baseline for all 4 lanes (broadcast)
 * DNA write on flow_end is caller responsibility (check ctx after)
 *
 * drift: popcount(baseline & ~isect) per lane
 *   AVX2 has no native popcnt64 — use scalar per lane (rare, 1/8 sample)
 *   cost: ~4 popcnt instructions, amortized over 8 steps
 */
static inline uint32_t diamond_batch_temporal_x4(
    DiamondFlowCtx4   *ctx,
    const uint64_t    *isect4,   /* [steps * 4], lane-interleaved */
    uint32_t           steps,
    uint64_t           baseline)
{
    __m256i r   = _mm256_loadu_si256((const __m256i*)ctx->route_addr);
    __m256i hop = _mm256_loadu_si256((const __m256i*)ctx->hop_count);
    __m256i drf = _mm256_loadu_si256((const __m256i*)ctx->drift_acc);

    const __m256i Pv      = _mm256_set1_epi64x((int64_t)DIAMOND_ROUTE_P);
    const __m256i one     = _mm256_set1_epi64x(1);
    const __m256i thr72   = _mm256_set1_epi64x(72);
    const __m256i zero    = _mm256_setzero_si256();
    uint32_t      resets  = 0;

    for (uint32_t i = 0; i < steps; i++) {
        __m256i i4 = _mm256_loadu_si256((const __m256i*)(isect4 + i*4));

        /* encode: r = rotl(r) ^ (isect * P) */
        __m256i rot = _mm256_or_si256(_mm256_slli_epi64(r, DIAMOND_ROUTE_K),
                                      _mm256_srli_epi64(r, 64 - DIAMOND_ROUTE_K));
        r = _mm256_xor_si256(rot, _diamond_mul64lo_x4(i4, Pv));

        /* hop++ unconditionally (reset handles it below) */
        hop = _mm256_add_epi64(hop, one);

        /* drift: scalar popcount, sample 1/8 (check low 3 bits of step) */
        if ((i & 7u) == 0u) {
            uint64_t tmp[4]; _mm256_storeu_si256((__m256i*)tmp, i4);
            uint64_t bl[4];  _mm256_storeu_si256((__m256i*)bl,
                _mm256_andnot_si256(i4, _mm256_set1_epi64x((int64_t)baseline)));
            uint64_t d[4];   _mm256_storeu_si256((__m256i*)d, drf);
            d[0] += (uint64_t)__builtin_popcountll(bl[0]);
            d[1] += (uint64_t)__builtin_popcountll(bl[1]);
            d[2] += (uint64_t)__builtin_popcountll(bl[2]);
            d[3] += (uint64_t)__builtin_popcountll(bl[3]);
            drf = _mm256_loadu_si256((const __m256i*)d);
        }

        /* flow_end mask: isect==0 OR drift>72 */
        __m256i dead  = _mm256_cmpeq_epi64(i4, zero);
        __m256i drift_over = _mm256_cmpgt_epi64(drf, thr72);
        __m256i end   = _mm256_or_si256(dead, drift_over);

        /* snapshot before reset — caller reads pre-reset values for DNA write */
        if (unlikely(!_mm256_testz_si256(end, end))) {
            uint64_t em[4]; _mm256_storeu_si256((__m256i*)em, end);
            resets |= ((em[0]!=0u)<<0) | ((em[1]!=0u)<<1)
                    | ((em[2]!=0u)<<2) | ((em[3]!=0u)<<3);
            /* save pre-reset state into ctx snapshot fields */
            _mm256_storeu_si256((__m256i*)ctx->snap_route, r);
            _mm256_storeu_si256((__m256i*)ctx->snap_hop,   hop);
            _mm256_storeu_si256((__m256i*)ctx->snap_drift, drf);
            /* selective reset */
            r   = _mm256_blendv_epi8(r,   zero, end);
            hop = _mm256_blendv_epi8(hop, zero, end);
            drf = _mm256_blendv_epi8(drf, zero, end);
        }
    }

    _mm256_storeu_si256((__m256i*)ctx->route_addr, r);
    _mm256_storeu_si256((__m256i*)ctx->hop_count,  hop);
    _mm256_storeu_si256((__m256i*)ctx->drift_acc,  drf);
    /* return bitmask: bit k = lane k fired DNA this batch
     * caller pattern:
     *   uint8_t mask = diamond_batch_temporal_x4(&ctx, isect4, steps, baseline);
     *   for (int f = 0; f < 4; f++)
     *       if (mask & (1u<<f))
     *           diamond_dna_write(&cells[f], ctx.snap_route[f],
     *                             (uint16_t)ctx.snap_hop[f],
     *                             (uint8_t)ctx.snap_drift[f]);
     */
    return resets;
}
#endif /* __AVX2__ (ctx4) */

#endif /* GEO_DIAMOND_FIELD_H */
