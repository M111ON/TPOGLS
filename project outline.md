# POGLS Storage & Streaming System — Master Plan
**Date**: 2026-04-07 | **Status**: Planning

---

## ภาพรวม

```
ไฟล์ / Model weights
       ↓
[Section 1: File Scanner]
       ↓
[Section 2: Compression Layer]
       ↓
[Section 3: POGLS Storage] ← ระบบที่มีอยู่แล้ว
       ↓
[Section 4: Reconstruct + Stream]
       ↓
[Section 5: LLM Streaming]
```

---

## Section 1: File Scanner
**เป้าหมาย:** แปลงไฟล์เป็น coordinate set แทนที่จะเก็บ content

### สิ่งที่ต้องสร้าง
| File | บทบาท |
|---|---|
| `pogls_scanner.h` | scan file → chunk → coordinate |
| `pogls_file_index.h` | เก็บ metadata (ชื่อ, ขนาด, type, chunk map) |

### Flow
```
input file
    ↓
detect file type → กรอง compressed ออก (.zip .jpg .mp4)
    ↓
split → N chunks (64B aligned = DiamondBlock)
    ↓
chunk → angular_mapper → geometric coordinate
    ↓
pogls_file_index บันทึก chunk map
```

### Rules
- ไฟล์ที่ compress แล้ว → store as-is ไม่ผ่าน compressor
- chunk size = 64B (DiamondBlock aligned)
- index เก็บแยกจาก data เสมอ

---

## Section 2: Compression Layer
**เป้าหมาย:** ลดขนาด coordinate data โดยยัง reconstruct ได้

### สิ่งที่ต้องสร้าง
| File | บทบาท |
|---|---|
| `pogls_compress.h` | compressor / decompressor |
| `pogls_chunk_index.h` | random access index |

### Compression strategy
```
1. Delta encode
   coord[i] = coord[i] - coord[i-1]  → diff เล็กกว่าค่าจริง

2. PHI symbol table
   PHI_UP=1696631, PHI_DOWN=648055, sacred numbers
   → แทนด้วย 1 byte แทน 4-8 bytes

3. Run-length encode
   pattern ซ้ำ → (value, count) แทน repeat

4. QRPN guard ต่อ chunk
   verify ก่อน output ทุก chunk
```

### Target
```
structured data (text, code, weights) → เป้า 60-90% reduction
compressed data (.zip, .jpg)          → ข้ามไป store as-is
```

---

## Section 3: POGLS Storage
**สถานะ: มีอยู่แล้ว ✅ ไม่ต้องแตะ**

```
pogls_compress.h  →  pogls_delta (store)
                  →  FiftyFourBridge (index)
                  →  Federation (commit)
                  →  libpogls_v4.so (API)
```

---

## Section 4: Reconstruct + Stream
**เป้าหมาย:** ดึงข้อมูลกลับมาได้ทั้งหมดหรือบางส่วน (partial)

### สิ่งที่ต้องสร้าง
| File | บทบาท |
|---|---|
| `pogls_reconstruct.h` | reconstruct pipeline |
| `pogls_stream.h` | partial / streaming output |

### Flow
```
request file (หรือบางส่วน)
    ↓
pogls_file_index → lookup chunk map
    ↓
fetch chunk_i → decompress → QRPN verify
    ↓
output chunk (พร้อมใช้ทันที ไม่ต้องรอครบ)
    ↓
fetch chunk_i+1 → ... (ต่อเนื่อง)
```

### Partial reconstruct
```
video/audio → เล่นได้ก่อนโหลดครบ
large file  → ใช้ส่วนต้นได้ก่อน
LLM weights → load layer ที่ต้องการก่อน
```

---

## Section 5: LLM Weight Streaming
**เป้าหมาย:** inference โดยไม่ต้องโหลด model ทั้งก้อนเข้า RAM

### concept
```
model.bin (หลาย GB)
    ↓
Section 1-2: scan + compress → เก็บเป็น POGLS coords
    ↓
inference time:
    - รู้ว่า layer ไหนต้องการ
    - fetch เฉพาะ chunk ของ layer นั้น
    - decompress → load เข้า RAM ชั่วคราว
    - inference → clear RAM
    - fetch layer ถัดไป
```

### สิ่งที่ต้องสร้าง
| File | บทบาท |
|---|---|
| `pogls_model_index.h` | map layer → chunk list |
| `pogls_weight_stream.h` | stream weights per layer |

### Target
```
model 7B (14GB) → RAM ที่ใช้จริง < 2GB
latency per layer → < 100ms (NVMe SSD)
```

---

## Build Order (Session Plan)

| Session | Section | Deliverable |
|---|---|---|
| S1 | 2: Compress | `pogls_compress.h` + round-trip test |
| S2 | 2: Index | `pogls_chunk_index.h` + random access test |
| S3 | 1: Scanner | `pogls_scanner.h` + file type filter |
| S4 | 1: File index | `pogls_file_index.h` + end-to-end store test |
| S5 | 4: Reconstruct | `pogls_reconstruct.h` + full reconstruct test |
| S6 | 4: Stream | `pogls_stream.h` + partial output test |
| S7 | 5: Model index | `pogls_model_index.h` + layer map test |
| S8 | 5: Weight stream | `pogls_weight_stream.h` + inference test |

---

## Frozen Rules (ต่อจากระบบเดิม)
- PHI constants จาก `pogls_platform.h` เท่านั้น
- DiamondBlock 64B format ไม่เปลี่ยน
- QRPN verify ทุก chunk ก่อน output
- GPU ไม่แตะ commit path
- integer only — no float
- core_c/ ห้ามแตะ

---

## Success Criteria
```
✅ ไฟล์ text/code/weights → compress แล้ว reconstruct ได้ครบ 100%
✅ partial reconstruct → chunk 1 พร้อมใช้ก่อน chunk N มาถึง
✅ LLM 7B → inference ได้โดยใช้ RAM < 2GB
✅ ไม่มี regression บน test ที่ผ่านมาแล้ว (2000+ cases)
```