# POGLS × Twin Geo — S10 Integration Guide

## สถานะ ณ ต้น S10

POGLS (icosphere pipeline) และ Twin Geo (dodecahedron temporal index) เป็นสองระบบแยกที่ยังไม่เชื่อมกัน POGLS เขียน/อ่านผ่าน `geo_pipeline_step` → Geomatrix → Shatter FSM ส่วน Twin Geo มี `geo_fused_write_batch` + `geo_read` ที่พร้อมใช้แต่ยังไม่มีใครส่ง input ให้ `pogls_twin_bridge.h` (S10) คือตัวเชื่อมทั้งสองระบบผ่าน FiboClock เป็น shared timebase

---

## สิ่งที่ S10 ทำ

`twin_bridge_write(addr, value)` เรียกแทน `pipeline_wire_process` ทีละ op ภายในมี 4 ขั้น: (1) freeze `c144` tag ก่อน tick, (2) `fibo_clock_tick` เดิน shared clock, (3) POGLS pipeline ทำงานปกติ, (4) Twin Geo คำนวณ `geo_fast_intersect(raw)` → `diamond_route_update` → สะสมใน `DiamondFlowCtx` ทุก `FIBO_EV_FLUSH` (ทุก 144 op) จะ `dodeca_insert` 1 ครั้ง — ไม่ใช่ทุก op

หลังจาก write path เดิน dodeca มี route geometry ของทุก 144-op window เก็บไว้ read path (`geo_read`) ใช้ `dodeca_lookup` ก่อน ถ้า hit ไม่ต้อง replay POGLS pipeline เลย ถ้า miss ก็ fall through ไป replay เดิม — ไม่มีอะไรแตก

---

## Raw Mix Formula (CRITICAL)

```c
raw = addr ^ value ^ fibo.seed.gen3 ^ (uint64_t)clk.c144
```

`c144` ต้อง freeze ก่อน `fibo_clock_tick` เสมอ เพราะ tick จะเดิน counter ทำให้ค่าเปลี่ยน ถ้า read path ต้องการ reproduce raw ต้องรู้ `c144` ณ จุด write ซึ่งเก็บไว้ใน `dodeca.offset` (low bits) แล้ว

---

## การใช้งาน

```c
/* init */
TwinBridge bridge;
GeoSeed seed = { .gen2 = MY_SPATIAL_SEED, .gen3 = MY_TEMPORAL_SEED };
twin_bridge_init(&bridge, seed);

/* write loop */
for each (addr, value):
    FiboEvent ev = twin_bridge_write(&bridge, addr, value, 0, &res);
    if (ev & FIBO_EV_SIG_FAIL)  handle_sig_fail();
    if (ev & FIBO_EV_FLUSH)     /* optional: snapshot, log stats */

/* ก่อน shutdown หรือ force-read */
twin_bridge_flush(&bridge);

/* read — Twin Geo path */
ReadResult r = geo_read(cells_raw, n, &bridge.dodeca, baseline, sha);
if (r.status == READ_HIT) use(r);   /* O(1) */
else                       /* replay POGLS เดิม */
```

---

## ไฟล์ที่ต้องใช้ต่อจากนี้

### S10 ต้องมีเสมอ
| ไฟล์ | มาจาก | หน้าที่ |
|------|--------|---------|
| `pogls_twin_bridge.h` | S10 ใหม่ | bridge ทั้งระบบ |
| `geo_fibo_clock.h` | core.zip | shared timebase |
| `geo_thirdeye.h` | core.zip | ThirdEye observer (ถูก include โดย fibo_clock) |
| `pogls_pipeline_wire.h` | pogls_core.zip | POGLS pipeline |
| `geo_pipeline_wire.h` | pogls_core.zip | GeoPacketWire, geo_pipeline_step |
| `geo_hardening_whe.h` | core.zip | geo_fast_intersect, geo_route_addr_guard |
| `geo_diamond_field.h` | core.zip | DiamondFlowCtx, diamond_route_update |
| `geo_dodeca.h` | core.zip | DodecaTable, dodeca_insert, dodeca_lookup |
| `geo_read.h` | core.zip | read path (geo_read) |

### ส่งเฉพาะเมื่อทำงานกับ GPU
| ไฟล์ | หน้าที่ |
|------|---------|
| `pogls38_gpu_final.cu` | K7/K8 kernels |
| `pogls38_gpu_wire.h` | GPU→pipeline bridge (l38_gpu_batch_feed) |
| `geomatrix_gpu_wire.cu` | geomatrix fused kernel |

### ไม่ต้องส่งอีกแล้ว
- `geo_final_v1.h` — dead code, replaced by `geo_diamond_field.h`
- bench1–9.cu — ใช้งานเสร็จแล้ว, ผลอยู่ใน S9 notes

---

## Open Items ที่ยังต้องทำหลัง S10

**[S10 งานหลัก]** ทดสอบ `twin_bridge_write` vs `pipeline_wire_process` ให้ได้ผลตรงกัน — smoke test: write 144 ops แล้ว flush ดู `twin_writes == 1` และ `dodeca_lookup` hit

**[S10 งานต่อ]** implement read path integration — `geo_read` ใช้ `bridge.dodeca` แทน full replay ตรวจว่า `READ_HIT` rate เป็นไปตาม write density

**[S11 ถ้าต้องการ GPU]** ย้าย `geo_fast_intersect` batch ใน `twin_bridge_batch` ไปรัน K8 เมื่อ batch ≥ 64K — ทำได้ก็ต่อเมื่อ drop rate > 50% มิฉะนั้น PCIe overhead กิน benefit

**[open]** `rh_domain_xor` ยังใช้ geometry-derived domain data — ถ้าต้องการ real domain data ต้องส่งมาแยก

**[open]** World B lazy-open strategy ยังไม่ตัดสินใจ — ถ้า TwinBridge flush miss rate สูงจะต้องพิจารณา

**[open]** REFORM→NORMAL boundary ปัจจุบัน 2 cycles — อาจต้องปรับหลังเห็น flush pattern จริงจาก S10

---

## Architecture Map (frozen)

```
FiboCtx (shared)
   gen2 ──→ POGLS GeoNet seed (spatial)
   gen3 ──→ Twin Geo raw mix seed (temporal)
   c144 ──→ temporal tag per write window

per op:
  (addr, value)
       ├─→ pipeline_wire_process → GeoPacketWire → Geomatrix → Shatter
       └─→ geo_fast_intersect(addr^value^gen3^c144) → DiamondFlowCtx
                  every 144 ops → dodeca_insert → DodecaTable

read:
  dodeca_lookup (O(1)) → hit? done : replay POGLS pipeline
```

---

## หลักการตัดสินใจ (ใช้ทุก session)

เพิ่ม code ใหม่เมื่อมันลดงานของระบบจริง ไม่ใช่เพิ่มความสามารถที่ไม่มีคนเรียก ทุก struct ใหม่ต้องตอบได้ว่า "มันอยู่ที่ไหนใน pipeline และใครเป็นเจ้าของ memory" ทุก function ใหม่ต้องตอบได้ว่า "มันเพิ่ม latency ที่ hot path เท่าไหร่" ถ้าตอบไม่ได้ทั้งสองข้อ อย่าเพิ่มก่อน
