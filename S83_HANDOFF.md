# S83 HANDOFF — Wire POGLS Weight Stream → llama.cpp

## Mission
ปิด gap สำคัญที่สุด: ให้ LLM รันบน POGLS จริงๆ แทน vanilla llama.cpp
เป้าหมาย: RAM spike หาย + มีตัวเลขเปรียบเทียบจริง

---

## Context (ทำไมถึงมาถึงจุดนี้)

ระบบมี 2 เส้นที่ยังไม่เจอกัน:

```
เส้น 1: POGLS geometric storage (C/CUDA) ← สร้างมาแล้ว S10–S82
เส้น 2: LLM runnning on llama.cpp vanilla ← ยังไม่ผ่าน POGLS

gap = pogls_weight_stream.h ยังไม่ได้ wire เข้า inference จริง
```

ปัญหาที่เห็นได้ตอนนี้: RAM spike หนักเมื่อรัน 7B → llama.cpp โหลดทั้ง model เข้า RAM ครั้งเดียว

---

## Models ที่มีอยู่
```
qwen2.5-0.5b-instruct-q4_k_m.gguf     ~468 MB
qwen2.5-1.5b-coder-instruct-q4_k_m    ~1.1 GB  
qwen2.5-7b-instruct-q4_k_m.gguf       ~4.4 GB  ← target (RAM spike ที่นี่)
Qwen3-1.7B-Q8_0.gguf                  ~1.7 GB
```

---

## Headers ที่มีพร้อมใช้ใน MASTER zip

```
geo_headers/pogls_weight_stream.h   ← RAM-windowed layer streaming
geo_headers/pogls_llm_adapter.h     ← lane → specialist routing
geo_headers/pogls_model_index.h     ← layer record index
geo_headers/pogls_stream.h          ← underlying stream
geo_headers/pogls_reconstruct.h     ← decompression (DiamondBlock 64B)
geo_headers/pogls_chunk_index.h     ← chunk addressing
```

`pogls_weight_stream.h` มีเป้าหมายใน header ชัดเจน:
> "model 7B (14GB) → RAM used < 2GB at any point"

---

## Task S83-A: Baseline วัดก่อน

รัน vanilla llama.cpp แล้ว log:
```bash
# วัด RAM ขณะ inference
/usr/bin/time -v llama-cli -m qwen2.5-7b-instruct-q4_k_m.gguf \
  -p "hello" -n 50 2>&1 | grep -E "Maximum resident|tokens per second"
```

บันทึก:
- Peak RAM (MB)
- Load time (s)  
- Tokens/sec

---

## Task S83-B: Wire pogls_weight_stream → llama.cpp

llama.cpp มี backend interface สำหรับ custom weight loading:

```c
// Entry point ที่ต้อง implement
struct llama_model_loader {
    // override ตรงนี้ด้วย pogls_ws_load_layer()
};
```

**แนวทาง minimal:**

```c
#include "geo_headers/pogls_weight_stream.h"
#include "geo_headers/pogls_model_index.h"

// 1. สร้าง ModelIndex จาก .gguf
// 2. Init WeightStream กับ RAM budget 1.5GB
// 3. Hook เข้า llama_load_model_from_file() 
//    แทน mmap default ด้วย pogls_ws_load_layer()

uint8_t ram_window[WS_DEFAULT_BUDGET];  // 1.5GB fixed window
WeightStream ws;
pogls_ws_init(&ws, &mi, &rc, ram_window, WS_DEFAULT_BUDGET);

// inference loop
pogls_ws_run(&ws, infer_callback, user_ctx);
```

**Frozen rules (อย่าแตะ):**
- PHI constants จาก `pogls_platform.h` เท่านั้น
- DiamondBlock 64B — `DIAMOND_BLOCK_SIZE`
- integer only — no float ใน core
- GPU ไม่แตะ weight stream core
- `core/` ห้ามแตะ

---

## Task S83-C: วัด after

ตัวเลขเดิม vs หลัง wire:
```
metric          | vanilla | POGLS
----------------|---------|------
Peak RAM (MB)   |   ?     |  < 1536
Load time (s)   |   ?     |  ?
Tokens/sec      |   ?     |  ?
```

ตัวเลขนี้คือ proof of concept ที่ขายได้

---

## ลำดับก่อน-หลัง

```
S83-A  วัด baseline vanilla (30 นาที)
S83-B  Wire weight stream เข้า llama.cpp  
S83-C  วัด after + เปรียบเทียบ
S83-D  ถ้า RAM ลดได้จริง → wire LLM advisor ให้รันบน POGLS ตัวเอง
```

S83-D คือจุดที่ระบบ "รู้จักตัวเอง" จริงๆ — LLM advisor รันบน infrastructure ที่มันดูแลอยู่

---

## ไฟล์ที่ต้องส่งมาใน S83

```
TPOGLS_MASTER.zip          ← clean pack ล่าสุด (มีอยู่แล้ว)
qwen2.5-7b path            ← บอก path ที่เก็บ .gguf
llama.cpp version/build    ← บอก version ที่ใช้อยู่
baseline numbers           ← ผล S83-A
```

---

## Note

KV_Patch (geo_kv.h + kv_bridge.h 2026-04-10) ถูก merge เข้า MASTER แล้ว
patch แก้ silent data loss ใน KV table — สำคัญมากสำหรับ address resolution ที่ถูกต้อง
