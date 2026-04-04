# Twin Geometry — Handoff Session 18
**Date:** 2026-04-03
**Status:** Compile clean ✅ | New files: 3 | T14 ASAN pass / -O2 blocked (known bug)

---

## Files Delivered

| File | Status |
|------|--------|
| `theta_map.h` | **NEW** — raw uint64 → ThetaCoord (face, edge, z) |
| `geo_route.h` | **NEW** — ThetaCoord → TorusNode, integrates with diamond_flow |
| `stress_t14_gb.c` | **NEW** — T14 GB streaming carry test (ASAN ✅, -O2 blocked) |
| `ts_mirror.h` | **DROPPED** — ไม่มี read path runtime, ไม่ integrate |

---

## Session 18 Changes

### 1. theta_map.h
Entry point ของ write path — แปลง raw `uint64_t` เป็น geometry coordinate

```
ThetaCoord { face (0–11), edge (0–4), z (0–255) }
```

- mix: murmur finalizer (`THETA_MIX_A/B`) — avalanche ทุก bit ก่อน map
- face: Lemire fast reduce mod 12 — zero division
- edge: lower 32b reduce mod 5
- z: bit [23:16] หลัง mix — ไม่ซ้อน face/edge

smoke test: 5/5 pass (range, deterministic, distribution 1024 samples)

### 2. geo_route.h
Bridge layer ระหว่าง ThetaCoord กับ TorusNode

```c
TorusNode geo_route_from_raw(uint64_t raw);     // one-call shortcut
TorusNode geo_route_advance(TorusNode n, int steps);  // warm-up optional
uint64_t  geo_route_pack(const TorusNode *n);   // geometry descriptor (ไม่ใช่ route_addr)
```

TorusNode.state = 0 เสมอ (state เป็น runtime ของ torus ไม่ใช่ input)

### 3. Write Path Architecture (complete)

```
raw uint64
  → theta_map()         [theta_map.h]    ← NEW
  → geo_route_init()    [geo_route.h]    ← NEW
  → diamond_flow_step() [geo_diamond_field.h]
  → route_addr / DNA write
```

### 4. ts_mirror.h — DROP decision

MirrorBlock 10B (DiamondBlock 64B → 6.4× compression) test ผ่านแล้วใน T12
แต่ระบบ passive no runtime — ไม่มี read path ที่ต้องถือ compressed copy ค้างไว้
ไฟล์ยังอยู่ใน S17 delivery ถ้าต้องการ compact coordinate lookup ในอนาคต

### 5. T14 — GB Streaming Carry Test

Scale จาก T13 (256 cells × 500 batches = 7.8 MB) → 1 GB:
- `T14_CELLS = 16384`, `T14_BATCHES = 1000`, `T14_BSIZE = 128`
- ใช้ `fold_block_init` + `geo_route` เพื่อสร้าง structured cells (ไม่ใช่ random bytes)
- cells เดิน torus ต่อกัน → face diversity ทุก batch

**ASAN result:**
```
PASS T14 GB streaming:
  cells=16384  batches=1000  mini_batch=128  total=0.98 GB
  dna_oneshot=8,695,869  dna_split=8,695,869  match=YES
  ring_empty=1
  oneshot 3.59s (279 MB/s)  split 3.50s (286 MB/s)
```

**-O2 crash (blocked):** segfault จาก type mismatch ใน `geo_diamond_v5x4.h`:
```
DiamondFlowCtxV2 * → expected DiamondFlowCtx *  (line 58)
```
เมื่อ -O2 inline function ที่ pointer type ไม่ตรง → UB → crash

---

## Compile

```bash
# ASAN (working)
gcc -g -fsanitize=address -I. stress_t14_gb.c -o stress_t14_gb && ./stress_t14_gb

# -O2 (blocked until DiamondFlowCtxV2 fix)
gcc -O2 -mavx2 -I. stress_t14_gb.c -o stress_t14_gb && ./stress_t14_gb
```

ต้องมีไฟล์ครบ: `geo_config.h`, `pogls_fold.h`, `geo_diamond_field.h`, `geo_dodeca.h`,
`geo_diamond_field5.h`, `geo_diamond_v5x4.h`, `geo_dodeca_torus.h`,
`geo_fibo_clock.h`, `geo_thirdeye.h` (จาก Geo_third_eye.zip),
`theta_map.h`, `geo_route.h`

---

## Session 19 Open Items

| งาน | Priority | Note |
|-----|----------|------|
| Fix `geo_diamond_v5x4.h` DiamondFlowCtxV2 mismatch | 🔴 HIGH | root cause ของ T14 -O2 crash |
| T14 run with -O2 | ต่อจากข้างบน | re-verify dna match + throughput |
| T14 scale on Colab (GB จริง) | 🟡 MED | benchmark hardware จริง |
| Write path end-to-end test | 🟡 MED | theta_map → geo_route → diamond_flow → DNA |

---

## Architecture Notes (carry forward)

**Layer map สมบูรณ์:**
```
raw input          → theta_map     → ThetaCoord
ThetaCoord         → geo_route     → TorusNode
TorusNode + cells  → diamond_flow  → route_addr (history)
route_addr         → DNA write     → honeycomb
```

**กฎที่ต้องรักษา:**
- ห้ามใช้ `route_addr` (history) ย้อนกลับเป็น theta_map input
- `theta_map` = initial condition, `diamond_flow` = evolution, `route_addr` = history
- carry = `DiamondFlowCtx` (route_addr + hop_count + drift_acc) serialize ผ่าน BridgeEntry ring
- flush: drain `temp_ring` ก่อนตรวจ `ctx` โดยตรง (T13 root cause fix)
- DNA fire triggers: `intersect == 0` หรือ `drift_acc > 72`
