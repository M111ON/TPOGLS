# Twin Geometry — Handoff Session 19
**Date:** 2026-04-03
**Status:** Compile clean ✅ | Tests: 13/13 + T14 passed | -O2 clean

---

## Files Delivered

| File | Status |
|------|--------|
| `geo_diamond_v5x4.h` | **PATCHED** — DiamondFlowCtxV2 → DiamondFlowCtx (scalar fallback) |
| `stress_t14_gb.c` | **FIXED** — malloc → aligned_alloc(64, ...) |

---

## Session 19 Bugs Fixed

### Bug 1: DiamondFlowCtxV2 mismatch (geo_diamond_v5x4.h line 45)

`diamond_batch_run_v5x4_scalar` (non-AVX2 fallback) ใช้ `DiamondFlowCtxV2` ซึ่งไม่มี definition
แต่ `diamond_batch_run_v5` รับ `DiamondFlowCtx *` — type mismatch → UB ใน -O2

**Fix:** เปลี่ยน `DiamondFlowCtxV2 ctx[4]` → `DiamondFlowCtx ctx[4]` ใน scalar fallback

```c
// before
static inline uint8_t diamond_batch_run_v5x4_scalar(DiamondFlowCtxV2 ctx[4], ...)

// after
static inline uint8_t diamond_batch_run_v5x4_scalar(DiamondFlowCtx ctx[4], ...)
```

### Bug 2: malloc alignment (stress_t14_gb.c)

`DiamondBlock` มี `__attribute__((aligned(64)))` — malloc รับประกันแค่ 16B
`-O2` autovectorize `fold_build_quad_mirror` เป็น aligned store → segfault

**Fix:** `malloc` → `aligned_alloc(64, n * sizeof(DiamondBlock))`

root cause pattern: struct ที่มี `__attribute__((aligned(N)))` ทุกตัวต้องใช้ `aligned_alloc` หรือ `posix_memalign` ไม่ใช่ `malloc` ธรรมดา

---

## T14 Final Result

```
PASS T14 GB streaming:
  cells=16384  batches=1000  mini_batch=128  total=0.98 GB
  dna_oneshot=8,695,869  dna_split=8,695,869  match=YES
  ring_empty=1
  oneshot 0.32s (3137 MB/s)  split 0.33s (2992 MB/s)

13/13 + T14 passed
```

carry consistency verified ที่ ~1 GB, split overhead < 5%

---

## Compile

```bash
gcc -O2 -mavx2 -I. stress_t14_gb.c -o stress_t14_gb && ./stress_t14_gb
gcc -O2 -mavx2 -I. stress_test_s16.c -o stress_test_s16 && ./stress_test_s16
```

---

## Architecture Notes (carry forward)

**aligned_alloc rule (ใหม่):**
ทุก heap allocation ของ `DiamondBlock` array ต้องใช้ `aligned_alloc(64, ...)` ไม่ใช่ `malloc`
เพราะ `DiamondBlock` มี `__attribute__((aligned(64)))` และ `fold_build_quad_mirror` ใช้ aligned store

**Write path สมบูรณ์:**
```
raw uint64
  → theta_map()         [theta_map.h]
  → geo_route_init()    [geo_route.h]
  → diamond_flow_step() [geo_diamond_field.h]
  → route_addr / DNA write
```

**Carry serialize/deserialize (T13 root cause, ยังใช้อยู่):**
flush: drain `temp_ring` ก่อนตรวจ `ctx` โดยตรง

---

## Session 20 Open Items

| งาน | Note |
|-----|------|
| T14 scale on Colab | run บน Tesla T4 เพื่อ benchmark จริง, คาดว่า throughput สูงกว่า x10 |
| end-to-end write path test | theta_map → geo_route → diamond_flow → DNA ใน test case เดียว |
| streaming GB ที่ 2GB+ | เพิ่ม T14_CELLS=65536 หรือ T14_BATCHES=2000 |
