# Twin Geometry — Handoff Session 20
**Date:** 2026-04-03
**Status:** Compile clean ✅ | Tests: 11/11 (T15) + T14 + T16 passed | -O2 clean

---

## Files Delivered

| File | Status |
|------|--------|
| `stress_t15_writepath.c` | **NEW** — end-to-end write path test (11/11) |
| `stress_t16_2gb.c` | **NEW** — 2GB streaming carry test |

---

## Session 20 Results

### T15 — End-to-End Write Path (11/11 PASS)

Complete chain test ครั้งแรกที่ครอบ `theta_map → geo_route → diamond_flow → DNA`:

| Test | ตรวจอะไร |
|------|---------|
| T15a | theta_map range (0..11, 0..4) + coverage ทุก face/edge ใน 10k samples |
| T15b | geo_route invariants: state=0, advance(0)=identity, pack roundtrip |
| T15c | DNA determinism: same raw → same dna_count (×2 runs) |
| T15d | Isolation: raw A ≠ raw B → route_addr A ≠ route_addr B |
| T15e | 16 flows → dodeca dedup: count=30, hit=2500, miss=30 |

**Note T15d:** `route_addr` reset to 0 หลัง DNA fire ทุก batch — ต้องจับ mid-flow
(1 cell at a time จนได้ route_addr ≠ 0 ครั้งแรก)

**Note T15e dedup ratio:** 2500/2530 = ~98.8% hit rate — routes ส่วนใหญ่ซ้ำกัน
(geometry converges หลัง fold_block_init) นี่คือ design ที่ถูกต้อง

### T16 — 2GB Streaming Carry

Scale จาก T14 (1GB) → 2GB:

```
cells=65536  batches=500  mini_batch=128  total=1.95 GB
dna_oneshot=17,908,830  dna_split=17,908,830  match=YES
ring_empty=1
oneshot 0.75s (2656 MB/s)  split 0.74s (2700 MB/s)
```

carry consistency ยืนยันที่ 2GB — split overhead < 2% (split เร็วกว่า oneshot เล็กน้อย)

### Throughput Summary (local machine)

| Test | Scale | Throughput |
|------|-------|-----------|
| T14 (s19) | ~1 GB | 2851–3137 MB/s |
| T16 (s20) | ~2 GB | 2656–2700 MB/s |
| bench_s14 v5_scalar | — | 106 Mflows/s |
| bench_s14 v5x4_AVX2 | — | 278 Mflows/s |
| bench_s14 torus | — | 96 Mflows/s |

---

## Architecture: Write Path (สมบูรณ์)

```
raw uint64
  → theta_map()          [theta_map.h]       face/edge/z ← murmur mix + Lemire mod
  → geo_route_init()     [geo_route.h]       TorusNode, state=0
  → fill_from_raw()      fold_block_init()   DiamondBlock, torus_step per cell
  → diamond_batch_run_v5()                   route_addr accumulate
  → DNA write → DodecaTable insert/dedup
```

**Invariants:**
- `theta_map` = initial condition (ห้ามใช้ route_addr เป็น input)
- `diamond_flow` = evolution (route_addr = history)
- `DiamondBlock` heap alloc ต้องใช้ `aligned_alloc(64, ...)` เสมอ
- carry flush: drain `temp_ring` ก่อนตรวจ `ctx` โดยตรง

---

## Session 21 Open Items

| Priority | งาน | Note |
|----------|-----|------|
| 🔴 | T14/T16 on Colab T4 | benchmark hardware จริง, คาด > 10× |
| 🟡 | Read path design | route_addr → theta_map inverse? หรือ dodeca lookup? |
| 🟡 | streaming > 4GB | T14_CELLS=131072 หรือ T14_BATCHES=4000 |
| 🟢 | Python .so binding | pogls_write/read/rewind + ExecWindow batch |

---

## Compile

```bash
# T15 write path
gcc -O2 -mavx2 -I. stress_t15_writepath.c -o stress_t15 && ./stress_t15
# expected: 11/11 passed

# T16 2GB carry
gcc -O2 -mavx2 -I. stress_t16_2gb.c -o stress_t16 && ./stress_t16
# expected: 1/1 passed, match=YES

# T14 (baseline)
gcc -O2 -mavx2 -I. stress_t14_gb.c -o stress_t14 && ./stress_t14
```

**Full file list needed:**
`geo_config.h`, `pogls_fold.h`, `geo_diamond_field.h`, `geo_dodeca.h`,
`geo_diamond_field5.h`, `geo_diamond_v5x4.h`, `geo_dodeca_torus.h`,
`geo_fibo_clock.h`, `theta_map.h`, `geo_route.h`
