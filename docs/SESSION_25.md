# Twin Geometry — Session 25 Handoff

## Status: GPU pipeline complete, K5 fused kernel ready for benchmark

---

## S25 deliverables

| ไฟล์ | สิ่งที่ทำ |
|---|---|
| `geomatrix_kernel.cu` | K5: geo_isect_fused — single kernel ตัด inter-kernel sync |

---

## Kernel map (ครบ 5)

| kernel | input | output | pattern |
|---|---|---|---|
| K1 fetch_bit | bitboard + idx[] | bit/packet | 1 thread = 1 packet |
| K2 geo_validate | bitboard + packets | pass/fail | fold XOR + phase mask |
| K3 geo_isect | raw[] | isect_sig[] | optX + smem reduce |
| K4 geo_isect_validate | isect_sig + packets | pass/fail | sig compare |
| K5 geo_isect_fused | raw + packets | pass/fail | K3+K4 in one kernel |

---

## Benchmark results (1M packets, T4 GPU)

| kernel | throughput | GB/s | pass rate |
|---|---|---|---|
| K1 fetch_bit | 58,181 M-pkt/s | 116 | — |
| K2 geo_validate | 15,103 M-pkt/s | 241 | 100% |
| K3 geo_isect | 21,987 M-pkt/s | 175 | — |
| K3+K4 fused (2-kernel) | 8,723 M-pkt/s | — | 100% |
| K5 fused (1-kernel) | TBD | TBD | TBD ← รัน session ถัดไป |

---

## K5 design

```
1 block = 32 bundles × 8 threads = 256 threads
Phase 1: each thread → isect(raw[lane]) ^ phase_mask[lane%4] → smem
Phase 2: XOR-reduce tree (3 steps) → sig at smem[bundle*8]
Phase 3: all 8 threads read sig from lane-0 slot → validate packet
         ไม่มี global write ระหว่าง isect และ validate
```

key: `smem[threadIdx.x & ~7]` — lane-0 slot ของ bundle นั้น ไม่ต้อง sync เพิ่ม

expected gain vs K3+K4: ~1.5–2.5x เพราะตัด 1 kernel launch + global mem round-trip

---

## Key constraints (carry-forward)

- `GEO_BUNDLE_WORDS` = 8 เสมอ
- phase mask index = `lane % 4` (geometry domain) ≠ `packet.phase` (routing domain)
- degenerate guard: `if (sig==0) sig ^= PHI_PRIME ^ i`
- compile: `nvcc -O2 -arch=sm_75 -o geo_kernel geomatrix_kernel.cu`
- `geo_dodeca_torus.h` — B1 branchless version เท่านั้น

---

## S26 candidates

**A** — วัด K5 throughput จริง → compare vs K3+K4, tune block size (128/256/512)

**B** — wire K5 เข้า production: replace geo_validate ใน main pipeline ด้วย K5

**C** — async pipeline: K1 (fetch) → K5 (isect+validate) ด้วย cudaStream overlap
