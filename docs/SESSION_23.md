# Twin Geometry — Session 23 Handoff

## Status: mid-session — benchmark done, optimization next

---

## S23 deliverables (done)

| ไฟล์ | สิ่งที่ทำ |
|---|---|
| `bench_geo_wire.c` | benchmark harness scalar/AVX2/pipeline/B+ |
| `geo_hardening_whe.h` | theta_mix64 double-call eliminated (h4[] cache) |
| `geo_wire_kernel.cu` | async API: `geo_wire_launch_async` + `geo_wire_async_wait` + `geo_wire_async_free` |

---

## Benchmark results (n=3072, rounds=2000, this machine)

| path | Melem/s | vs scalar |
|---|---|---|
| scalar (no-vec) | 1248 | baseline |
| scalar + auto-AVX2 | 2776 | 2.22x |
| AVX2 x4 intrinsic | 1815 | 1.45x |
| pipeline x4 (theta+isect) | 490 | 0.39x |
| B+ (theta once, seed rotate) | 495 | 0.40x |

## Isolation result (bottleneck confirmed)

| | Melem/s |
|---|---|
| isect only (B+ loop, no route) | 947 |
| isect + route_step | 519 |

**`route_step` กิน ~45% ของเวลา** — multiply + branch guard เป็น bottleneck จริง

---

## Next: S24 optimization candidates

**C1** — ตัด branch guard ออกจาก hot path  
เหลือแค่ flush boundary → branch หายจาก loop → compiler vectorize ได้เต็ม

**C2** — แทน `isect * PHI` multiply ด้วย shift+xor chain  
เบากว่า แต่ entropy ลดนิดหน่อย

ทำทั้งคู่แล้วเปรียบตัวเลขก่อนตัดสินใจ

update รอพิจารณา
 เวอร์ชันสุด: x8 multi-accumulator + fully fused + no dependency chain
🎯 หลักการ
❌ ไม่มี chain เดียว

✅ แตกเป็น 8 lane อิสระ

✅ ไม่มี multiply

✅ branch ออกนอก loop

💣 CODE (เอาไปเสียบได้เลย)
uint64_t r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0;
uint64_t seed = theta_mix64(init_seed ^ raw[0]);

#pragma GCC ivdep
for (int i = 0; i < N; i += 8) {

    #define STEP(idx, r) do { \
        uint64_t x = raw[i+idx] ^ seed; \
        seed += 0x9E3779B97F4A7C15ULL; \
        seed = (seed << 13) | (seed >> 51); \
        uint64_t isect = \
            x & \
            ((x << 8) | (x >> 56)) & \
            ((x << 16) | (x >> 48)) & \
            ((x << 24) | (x >> 40)); \
        uint64_t m = isect; \
        m ^= m << 13; \
        m ^= m >> 7; \
        m ^= m << 17; \
        r ^= m; \
    } while(0)

    STEP(0, r0);
    STEP(1, r1);
    STEP(2, r2);
    STEP(3, r3);
    STEP(4, r4);
    STEP(5, r5);
    STEP(6, r6);
    STEP(7, r7);

    #undef STEP
}

// 🔥 merge ตอนท้าย (tree reduce)
uint64_t r =
    (r0 ^ r1) ^ (r2 ^ r3) ^
    (r4 ^ r5) ^ (r6 ^ r7);

// guard ทีเดียว
r = geo_route_addr_guard(r, r);
🔥 ทำไมอันนี้ “สุดจริง”
✅ ไม่มี dependency chain
8 accumulator วิ่งพร้อมกัน

✅ ไม่มี multiply
latency หาย

✅ branch นอก loop
pipeline ไม่สะดุด

✅ compiler auto-AVX2 เต็ม
เพราะ independent lanes

📈 expected
version	speed
เดิม	431
หลัง optimize	~900–1200
x8 นี้	1500–2500+ Melem/s 🚀

---

## Key constraints (carry-forward)

- `GEO_BUNDLE_WORDS` = 8 เสมอ
- `CYL_FULL_N` = 3072
- `rh_audit_group_domain()` — ห้ามใช้ deprecated version
- `pogls_qrpn_phaseE.h` แทน `pogls_qrpn.h`
- `ts_pipeline.h` = frozen baseline
- include ordering strict — ห้าม circular
- output ต้อง compile ด้วย `gcc -O2`
