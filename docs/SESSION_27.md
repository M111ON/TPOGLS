# Twin Geometry — Session 27 Handoff

## Status: K6 verified, multi-stream next

---

## S26 benchmark final (T4, 1M packets)

| kernel | M-pkt/s | GB/s | vs K3 | pass |
|---|---|---|---|---|
| K1 fetch_bit | 50,607 | 101 | — | — |
| K2 geo_validate | 15,001 | 241 | 0.78x | 100% |
| K3 geo_isect | 19,263 | 154 | 1.00x ceiling | — |
| K3+K4 2-kernel | 8,172 | — | 0.42x | 100% |
| K5 fused 16B | 10,441 | 84 | 0.54x | 100% |
| K6 fused 8B | 14,312 | 114 | **0.74x** | 100% |

ceiling = K3 = 19,263 M-pkt/s
K6 gap = 26% (fixed: smem sync + result write — ไม่ใช่ bandwidth)

---

## S27 target: multi-stream K6

```c
#define N_STREAMS 4
cudaStream_t s[N_STREAMS];
for (int i = 0; i < N_STREAMS; i++) cudaStreamCreate(&s[i]);

// split N_PACKETS → 4 chunks, launch parallel
for (int i = 0; i < N_STREAMS; i++)
    geo_isect_fused32<<<grid/4, 256, 0, s[i]>>>(
        d_raw   + i*chunk*8,
        d_pkts32 + i*chunk,
        d_result + i*chunk,
        chunk/8);
```

expected: 4 × 14,312 → ~40,000–50,000 M-pkt/s (T4 มี 40 SM)

---

## Key constraints (carry-forward)

- `GEO_BUNDLE_WORDS` = 8
- phase mask index = `lane % 4` ≠ `packet.phase`
- degenerate guard: `if (sig==0) sig ^= PHI_PRIME ^ i`
- B1 branchless `torus_step` — geo_dodeca_torus.h
- compile: `nvcc -O2 -arch=sm_75 -o geo_kernel geomatrix_kernel.cu`
- GeoPacket32 = 8B canonical format จาก S26 ออกไป

---

## S28 candidates (หลัง multi-stream)

A — pipeline overlap: cudaMemcpyAsync H2D → K6 → D2H  
B — wire K6 async เข้า geo_wire_launch_async (geo_wire_kernel.cu)  
C — scale N_PACKETS → 16M แล้ว retest ceiling จริง
