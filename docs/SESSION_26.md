# Twin Geometry — Session 26 Handoff

## Status: K6 bit-pack added, benchmark pending

---

## S26 deliverables

| ไฟล์ | สิ่งที่ทำ |
|---|---|
| `geomatrix_kernel.cu` | K6: geo_isect_fused32 — GeoPacket 16B → 8B |

---

## Kernel map (ครบ 6)

| # | kernel | pkt size | pattern |
|---|---|---|---|
| K1 | fetch_bit | 2B (idx) | 1 thread = 1 packet |
| K2 | geo_validate | 16B | fold XOR + phase mask |
| K3 | geo_isect | 8B (raw) | optX + smem reduce |
| K4 | geo_isect_validate | 8B (sig) | sig compare |
| K5 | geo_isect_fused | 16B | K3+K4 single kernel |
| K6 | geo_isect_fused32 | **8B** | K5 + bit-pack packet |

---

## GeoPacket32 layout (8B)

```
word0 [31:0]  = sig32   (upper 32-bit of sig64 — L1 validate)
word1 [31:23] = hpos    (9-bit, 0..511)
word1 [22:14] = idx     (9-bit, 0..511)
word1 [13:10] = phase   (4-bit)
word1 [9]     = bit     (1-bit)
word1 [8:0]   = pad
```

sig validate: upper 32-bit match (L1) — L2 full 64-bit verify ที่ endpoint

---

## Benchmark results so far (T4 GPU, 1M packets)

| kernel | M-pkt/s | pkt size | pass |
|---|---|---|---|
| K2 geo_validate | 14,832 | 16B | 100% |
| K3 geo_isect | 16,517 | 8B raw | — |
| K5 geo_isect_fused | 10,285 | 16B | 100% |
| K6 geo_isect_fused32 | **TBD** | **8B** | TBD |

expected K6: ~15,000–18,000 M-pkt/s (+1.5–2x vs K5)
ceiling = K3 = 16,517 (8B load)

---

## Key constraints (carry-forward)

- `GEO_BUNDLE_WORDS` = 8
- phase mask index = `lane % 4` (geometry) ≠ `packet.phase` (routing)
- degenerate guard: `if (sig==0) sig ^= PHI_PRIME ^ i`
- B1 branchless `torus_step` in `geo_dodeca_torus.h`
- compile: `nvcc -O2 -arch=sm_75 -o geo_kernel geomatrix_kernel.cu`

---

## S27 candidates (หลังได้ตัวเลข K6)

**A** — multi-stream: 4 × K6 parallel → ซ่อน latency, push >60K M-pkt/s

**B** — pipeline overlap: cudaMemcpyAsync H2D → K6 → D2H ใน 3 streams

**C** — async wire เข้า geo_wire_kernel.cu async API (geo_wire_launch_async)
