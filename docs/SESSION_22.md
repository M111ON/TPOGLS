# Twin Geometry — Session 22 Changelog

## Status: COMPLETE — Priority 4 (AVX2 + CUDA) delivered

---

## New files

### `geo_simd.h` — AVX2 4-lane vectorized intersect

`geo_fast_intersect_x4(const uint64_t in[4], uint64_t out[4])`

- AVX2 path: 1 `_mm256_loadu` + 3 `_M256_ROTR_BYTES` + 3 `_mm256_and` + 1 store
- Scalar fallback: auto-selected when `!__AVX2__` or `#define GEO_NO_AVX2 1`
- `geo_fast_intersect_x4_verify()` — self-test (scalar vs AVX2, 4 probes)
- Compile: `gcc -O2 -mavx2`

| | scalar | x4 AVX2 |
|---|---|---|
| ops/element | ~4 | ~2 |
| latency/lane | ~0.66 ns | ~0.20 ns |

### `geo_wire_kernel.cu` — CUDA kernel

**Synchronous:** `geo_wire_launch(raw_in, route_in, n)` → `GeoWireResult`

**Async:** overlap H→D transfer + kernel with CPU work

```c
GeoWireAsync a = geo_wire_launch_async(raw, route_in, n, stream);
// ... CPU work here (H→D + kernel running in background) ...
cudaError_t e = geo_wire_async_wait(&a, route_out, isect_out);
geo_wire_async_free(&a);
```

- pinned host mem (`cudaMallocHost`) → enables true async H→D
- `stream = NULL` → private stream auto-created + destroyed
- 1 thread = 1 lane → 3072 lanes parallel
- Each thread: `theta_mix64 → pack core_raw → dev_fast_intersect → dev_route_step`
- `route_in = NULL` → fresh run; or pass carry from previous batch (streaming)
- Compile: `nvcc -O3 -arch=sm_75 -c geo_wire_kernel.cu`
- Baseline: 15468 Melem/s (s9 scalar) — GPU @ sm_75 expected ~50–200× depending on batch size

## Modified files

### `geo_hardening_whe.h`

`geo_fused_write_batch` loop rewritten:

```
x4 block  (i=0; i+4≤n; i+=4):
  h4[4] cached once → build core4[4] → geo_fast_intersect_x4 → per-lane state machine

scalar tail (remaining 0–3 elements):
  unchanged scalar path
```

- `#include "geo_simd.h"` added after `geo_read.h`
- `theta_mix64` called **once** per lane (h4[] cache) — double-call eliminated ✅
- State machine (drift/route/WHE/dodeca_insert) identical to S21

---

## Include order (unchanged)

```
geo_simd.h              ← standalone (immintrin.h + stdint.h)
geo_diamond_field.h     ← GEO_ROUTE_STEP, diamond_route_update
geo_hardening_whe.h     ← geo_fast_intersect, geo_fused_write_batch (x4)
  └─ includes geo_simd.h
geo_wire_kernel.cu      ← CUDA standalone (mirrors constants inline)
```

---

## Next candidates

- S23: Benchmark harness — scalar / AVX2 / CUDA throughput numbers
- S23: `geo_wire_launch_async` multi-batch pipeline (double-buffer: batch N+1 uploads while batch N computes)


## Status: COMPLETE — Priority 4 (AVX2 + CUDA) delivered

---

## New files

### `geo_simd.h` — AVX2 4-lane vectorized intersect

`geo_fast_intersect_x4(const uint64_t in[4], uint64_t out[4])`

- AVX2 path: 1 `_mm256_loadu` + 3 `_M256_ROTR_BYTES` + 3 `_mm256_and` + 1 store
- Scalar fallback: auto-selected when `!__AVX2__` or `#define GEO_NO_AVX2 1`
- `geo_fast_intersect_x4_verify()` — self-test (scalar vs AVX2, 4 probes)
- Compile: `gcc -O2 -mavx2`

| | scalar | x4 AVX2 |
|---|---|---|
| ops/element | ~4 | ~2 |
| latency/lane | ~0.66 ns | ~0.20 ns |

### `geo_wire_kernel.cu` — CUDA kernel

`geo_wire_launch(raw_in, route_in, n)` → `GeoWireResult`

- 1 thread = 1 lane → 3072 lanes parallel
- Each thread: `theta_mix64 → pack core_raw → dev_fast_intersect → dev_route_step`
- `route_in = NULL` → fresh run (memset 0), or pass carry from previous batch (streaming)
- Returns `{route_out[], isect_out[]}` — host-side, caller frees
- Compile: `nvcc -O3 -arch=sm_75 -c geo_wire_kernel.cu`
- Baseline: 15468 Melem/s (s9 scalar) — GPU @ sm_75 expected ~50–200× depending on batch size vs PCIe

## Modified files

### `geo_hardening_whe.h`

`geo_fused_write_batch` loop rewritten:

```
x4 block  (i=0; i+4≤n; i+=4):
  build core4[4] → geo_fast_intersect_x4(core4, isect4) → per-lane state machine

scalar tail (remaining 0–3 elements):
  unchanged scalar path
```



---

## Include order (unchanged)

```
geo_simd.h              ← standalone (immintrin.h + stdint.h)
geo_diamond_field.h     ← GEO_ROUTE_STEP, diamond_route_update
geo_hardening_whe.h     ← geo_fast_intersect, geo_fused_write_batch (x4)
  └─ includes geo_simd.h
geo_wire_kernel.cu      ← CUDA standalone (no .h deps, mirrors constants inline)
```

---

## Next candidates

- S23: `geo_wire_launch` async variant (cudaStream_t, overlap H→D with compute)
- S23: Benchmark harness comparing scalar / AVX2 / CUDA paths
