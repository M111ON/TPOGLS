# S83 HANDOFF v2 — Wire POGLS Weight Stream → llama.cpp
**Updated: 2026-04-13 | Baseline confirmed**

---

## Baseline (S83-A ✅ DONE)

```
metric              | vanilla llama.cpp (CUDA 12.4 / b8590)
--------------------|---------------------------------------
Peak RAM            | ~7.5 GB / 8.0 GB  (95% utilized)
Peak VRAM         | 3977 MB/4.00 GB (RTX 1050ti)
Available after load| 406 MB only
Committed (virtual) | 18.4 GB  ← swap หนักมาก = slowdown
Generation speed    | 3.2–3.7 t/s  (expected 10+ t/s ถ้าไม่ swap)
Model               | qwen2.5-7b-instruct-q4_k_m.gguf (~4.4 GB Q4)
Path                | I:\llama\models\
llama.cpp           | I:\llama\bin\llama-b8590-bin-win-cuda-12.4-x64
```

**Root cause:** llama.cpp default = mmap ทั้ง model เข้า address space ครั้งเดียว
→ 4.4 GB Q4 + KV cache + CUDA overhead = เกิน 8 GB RAM → swap

**Target after POGLS wire:**
```
Peak RAM    < 2 GB (WS_DEFAULT_BUDGET = 1.5 GB + ~0.5 GB headroom)
Speed       8–10+ t/s (ไม่ต้อง swap)
```

---

## Architecture Gap (ที่ต้อง close ใน S83-B)

```
ปัจจุบัน:
  .gguf ──mmap──▶ llama_model_loader ──▶ RAM ทั้งก้อน ──▶ inference
                      ↑
                  default path (full load)

เป้าหมาย:
  .gguf ──parse──▶ ModelIndex ──▶ WeightStream (1 layer/time)
                                    │
                              ram_window[1.5GB]
                                    │
                              pogls_ws_load_layer()
                                    │
                              infer_callback() ──▶ inference
                                    │
                              pogls_ws_clear()
                                    └──▶ next layer
```

เหมือน **streaming video** แทน **download แล้วค่อยเล่น**

---

## Task S83-B: Wire Plan (3 steps)

### Step 1 — Parse .gguf → ModelIndex

```c
#include "geo_headers/pogls_model_index.h"

// gguf format: มี tensor metadata ที่ชัดเจน
// แต่ละ tensor = 1 layer record ใน ModelIndex

ModelIndex mi;
pogls_model_index_init(&mi, "qwen2.5-7b-q4km", total_tensors);

// loop tensor metadata จาก .gguf header
for (int i = 0; i < total_tensors; i++) {
    pogls_model_index_add(&mi, i, tensor_name[i],
                          tensor_byte_offset[i],
                          tensor_byte_offset[i] + tensor_size[i]);
}
pogls_model_index_seal(&mi);
```

### Step 2 — Init WeightStream

```c
#include "geo_headers/pogls_weight_stream.h"
#include "geo_headers/pogls_reconstruct.h"

static uint8_t ram_window[WS_DEFAULT_BUDGET];  // 1.5 GB fixed, stack/static
WeightStream ws;
ReconContext rc;  // bind to .gguf file descriptor

// rc = wrap file I/O — read DiamondBlock 64B chunks on demand
pogls_ws_init(&ws, &mi, &rc, ram_window, WS_DEFAULT_BUDGET);
```

### Step 3 — Hook เข้า llama inference loop

```c
// llama.cpp b8590 มี ggml_backend_buffer ให้ override
// approach: custom ggml_backend ที่ delegate load → pogls_ws_load_layer()

static void my_infer_cb(const WeightLayerView *v, void *user_ctx) {
    // v->data = layer weights ใน ram_window (valid จนกว่า ws_clear)
    // v->layer_type = MIDX_LAYER_ATTN / MLP / etc.
    
    // ส่ง weights เข้า ggml tensor ที่ allocate ไว้แล้ว
    LlamaInferCtx *ctx = (LlamaInferCtx *)user_ctx;
    ggml_backend_tensor_set(ctx->cur_tensor, v->data, 0, v->size);
    
    // run layer computation
    llama_decode_layer(ctx, v->layer_id);
}

// แทน llama_load_model_from_file() ปกติ:
pogls_ws_run(&ws, my_infer_cb, &infer_ctx);
```

---

## Layer Routing (llm_adapter.h — ใช้ขั้น S83-D)

```
lane % 3 == 0  →  GENERAL   qwen2.5-0.5b    (fast, cheap)
lane % 3 == 1  →  CODER     qwen2.5-coder   (technical)
lane % 3 == 2  →  MATH/HVY  qwen2.5-7b      (heavy, via weight stream)
```

7B model ถูก route เฉพาะ heavy lanes — ไม่โหลดทั้งวัน

---

## Files ที่ต้องใช้ใน S83-B

```
geo_headers/pogls_weight_stream.h   ← main interface  ✅ ready
geo_headers/pogls_model_index.h     ← layer map       ✅ ready
geo_headers/pogls_reconstruct.h     ← DiamondBlock IO ✅ ready
geo_headers/pogls_stream.h          ← chunk stream    ✅ ready
geo_headers/pogls_chunk_index.h     ← chunk addressing✅ ready
core/pogls_platform.h               ← PHI constants   ✅ frozen
```

**ยังต้องเขียน (S83-B deliverable):**
```
gguf_to_model_index.c    ← parse .gguf header → ModelIndex
pogls_recon_file.c       ← wrap file I/O → ReconContext
llama_pogls_backend.c    ← custom ggml_backend hook
```

---

## Benchmark Target (S83-C)

```
metric          | vanilla  | POGLS target
----------------|----------|-------------
Peak RAM (MB)   | ~7,500   | < 2,048
Available (MB)  | ~406     | > 5,952
Swap pressure   | HEAVY    | NONE
Gen speed (t/s) | 3.2–3.7  | 8–10+
Load time (s)   | ?        | layer-by-layer (no full load)
```

---

## S83-D (หลัง RAM proof)

LLM advisor รันบน POGLS infrastructure ที่มันดูแลอยู่
= ระบบ "รู้จักตัวเอง" จริงๆ

```
pogls_engine → llm_advisor → route query → weight_stream 7B
                    ↑                              ↓
              /llm_report                  infer on POGLS storage
```

---

## KV_Patch Status
`geo_kv.h` + `kv_bridge.h` (2026-04-10) merged เข้า MASTER ✅
แก้ silent data loss ใน KV table — address resolution ถูกต้องแล้ว
