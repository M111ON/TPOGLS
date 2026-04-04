# Twin Geometry — Session 21 Changelog

## Status: COMPLETE → ready for Priority 4 (AVX2 + CUDA)

---

## What changed and why

### Read path correctness (geo_read.h)

`geo_read_by_raw` เดิม claim ว่า mirror `geo_fused_write_batch` แต่มี state divergence 2 จุด

**❶ guard placement (correctness bug)** — `geo_route_addr_guard` ถูกเรียกหลัง boundary check ทำให้ lookup ใช้ `r_next` ที่ยังไม่ผ่าน guard ถ้า topo dead zone (`lower48 == 0`) เกิดพร้อม boundary จะ MISS ผิดพลาดเสมอ fix คือย้าย guard ขึ้นมาก่อน `at_end` ให้เหมือน write path เป๊ะ

**❷ inline duplicate (maintenance debt)** — logic ของ `geo_fast_intersect` ถูก copy ไว้ใน loop body แทนที่จะเรียก function ตรงๆ ถ้าแก้ write side จะ drift โดยไม่มี compile error fix คือเรียก `geo_fast_intersect(core_raw)` โดยตรง

### Canonical macro (geo_diamond_field.h)

`geo_route_addr_guard` + constants (`GEO_ROUTE_SEED_GUARD` / `GEO_ROUTE_TOPO_MASK`) + macro `GEO_ROUTE_STEP` ย้ายมาอยู่ใน `geo_diamond_field.h` ถัดจาก `diamond_route_update` ทันที เหตุผลคือ `geo_hardening_whe.h` include `geo_read.h` อยู่แล้ว ถ้า macro อยู่ที่เดิมจะ circular `geo_diamond_field.h` เป็นจุดเดียวที่ทั้ง read และ write chain เห็นร่วมกันโดยไม่มี cycle

```c
/* ใช้อันนี้เท่านั้น — ห้ามเรียก diamond_route_update + guard แยก */
#define GEO_ROUTE_STEP(r, isect) \
    geo_route_addr_guard(diamond_route_update((r), (isect)), (isect))
```

write path ใน `geo_hardening_whe.h` และ read path ใน `geo_read.h` ทั้งคู่ใช้ `GEO_ROUTE_STEP` บรรทัดเดียวกัน diverge ไม่ได้อีก

### Debug assert (geo_read.h)

```c
#ifndef NDEBUG
    uint64_t _w = GEO_ROUTE_STEP(ctx.route_addr, isect);
    assert(_w == r_next && "geo_read: route diverged from write path");
#endif
```

assert นี้ compute `GEO_ROUTE_STEP` ซ้ำแล้วตรวจว่าเท่ากัน ปัจจุบัน always-pass แต่มีไว้เป็น tripwire — ถ้าใครแก้ macro หรือ `diamond_route_update` แล้วลืม read path จะ fire ตั้งแต่ unit test แรก ไม่รอถึง production zero cost ใน release build (`-DNDEBUG`)

### Canonicalization

| ไฟล์ | การเปลี่ยน |
|---|---|
| `geo_hardening_whe.h` | +`#include "geo_read.h"` — wire read path เข้า chain หลัก |
| `geo_diamond_field5.h` | DEPRECATED + `#pragma message` guard (opt-out ด้วย `GEO_ALLOW_V5_DEPRECATED`) |
| `geo_diamond_v5x4.h` | DEPRECATED + same guard |

### Bug fixes

| ไฟล์ | การเปลี่ยน |
|---|---|
| `geo_dodeca_torus.h` | `fibo_hop_fast(fclk, &proxy)` — ลบ `route_sig` arg ที่หายไปแล้ว |
| `geo_thirdeye.h` | `(int)QRPN_IMBAL_THRESH` — แก้ Wsign-compare 2 จุด |

### Test (test_read_write.c)

T7 เพิ่ม canonical enforcement + isolation guard: v5 write → fused read = `READ_MISS` พิสูจน์ว่า path แยกกันจริง read ไม่รับ artifact จาก v5 path

---

## Files modified in s21

```
geo_diamond_field.h   ← GEO_ROUTE_STEP + geo_route_addr_guard (canonical home)
geo_hardening_whe.h   ← ลบ guard/constants เดิม + ใช้ GEO_ROUTE_STEP + include geo_read.h
geo_read.h            ← geo_fast_intersect call + GEO_ROUTE_STEP + assert + include assert.h
geo_diamond_field5.h  ← DEPRECATED guard
geo_diamond_v5x4.h    ← DEPRECATED guard
geo_dodeca_torus.h    ← fibo_hop_fast signature fix
geo_thirdeye.h        ← Wsign-compare fix ×2
test_read_write.c     ← T7 canonical enforcement
```

---

## Next: Priority 4 (AVX2 + CUDA)

ต่อจาก `geo_fast_intersect` โดยขยายเป็น x4/x8 lanes ด้วย `__m256i` ตรงๆ ไม่ต้องแตะ v5x4 เดิม (deprecated แล้ว) entry point คือ `geo_fast_intersect_x4(const uint64_t *in, uint64_t *out)` ใช้ใน `geo_fused_write_batch` loop body เดิม แค่แทน scalar call ด้วย vectorized version ทุก 4 elements

CUDA: ขยาย `GEO_ROUTE_STEP` logic ออกเป็น kernel ที่รันบน `CYL_FULL_N=3072` lanes คู่ขนาน baseline s9 = 15468 Melem/s
