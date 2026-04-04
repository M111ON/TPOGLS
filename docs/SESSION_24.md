# Twin Geometry — Session 24 Handoff

## Status: ceiling confirmed, B1 applied, ready for GPU path

---

## S24 deliverables

| ไฟล์ | สิ่งที่ทำ |
|---|---|
| `geo_dodeca_torus.h` | B1: branchless `torus_step` |
| `bench_s24.c` | harness ครบ: scalar/x8/x16/optX/C1/thread |

---

## B1 patch — geo_dodeca_torus.h line 81

```c
// เดิม (S23)
if (m.flip) n->state ^= 1;   // branch → pipeline flush

// ใหม่ (S24)
n->state ^= m.flip;           // branchless — XOR ตรง, ผลเท่ากัน 100%
```

ผล: branch predictor หยุดทำงานตรงนี้ → ลด route_step overhead
(route_step เคยกิน 45% — B1 ลด branch flush ส่วนนั้น)

---

## Benchmark results (n=3072, rounds=2000)

| path | Melem/s | vs scalar |
|---|---|---|
| scalar (no-vec) | 1324 | baseline |
| scalar + auto-AVX2 | 2474 | 1.87x |
| x8 optX (isect direct) | 2446 | 1.85x |
| x16 optX | 2496 | 1.89x |
| C1 x4 rotated streams | 198 | 0.15x |
| 2-thread pool | 75 | 0.06x |

---

## Key findings S24

**1. Ceiling confirmed = L1 bandwidth ~20 GB/s**
- n=3072 → 24KB → อยู่ใน L1 cache ทั้งก้อน
- x8 / x16 / auto-AVX2 ล้วนชนเพดานเดียวกัน ~2500 Melem/s
- เพิ่ม lanes ไม่ช่วย เพราะ data pipeline เต็มแล้ว

**2. x8 optX = best CPU path**
- isect ตรงๆ ไม่มี xorshift chain → compiler auto-vectorize AVX2 เต็ม
- 8 accumulator อิสระ → ไม่มี dependency
- guard นอก loop — branchless

**3. C1 (1 load → 4 streams) ช้าลง**
- ISECT(ROTL(x,k)) = rotation ซ้อน rotation → 12 ops/element
- compute overhead > memory saving บน L1-bound workload

**4. Multi-thread ไม่คุ้มบน n=3072**
- thread overhead ~1–5µs vs compute ~1µs
- L1 cache ไม่ share ระหว่าง core → bandwidth ไม่รวม
- จะคุ้มเมื่อ n ≥ ~100K (L3 territory) หรือ GPU

---

## ทางทะลุ ceiling จริง

| วิธี | expected | เหตุผล |
|---|---|---|
| GPU (geo_wire_kernel.cu) | 50–300x | bandwidth 500+ GB/s |
| n ใหญ่ขึ้น + multi-thread | ~2x | L3 shared → thread คุ้ม |
| AVX-512 (ถ้า CPU support) | ~1.5x | 512-bit register |

---

## Key constraints (carry-forward)

- `GEO_BUNDLE_WORDS` = 8 เสมอ
- `CYL_FULL_N` = 3072
- `rh_audit_group_domain()` — ห้ามใช้ deprecated version
- `pogls_qrpn_phaseE.h` แทน `pogls_qrpn.h`
- `ts_pipeline.h` = frozen baseline
- include ordering strict — ห้าม circular
- output ต้อง compile ด้วย `gcc -O2`
- `geo_dodeca_torus.h` — ใช้ B1 version (branchless) เท่านั้น

---

## Best compile command

```bash
gcc -O2 -mavx2 -o bench_s24 bench_s24.c -lm -lpthread
```

## S25 candidates

**A** — GPU path: integrate `bench_s24` optX pattern เข้า `geo_wire_kernel.cu`

**B** — Production: wire optX เข้า `geo_fused_write_batch` แล้ววัด end-to-end throughput

**C** — Scale test: n=100K+ แล้ว retest multi-thread ceiling
