# POGLS S33 — Handoff
**Status:** ✅ COMPLETE | **Tests:** 22/22 passed (llm_adapter) + S32 76/76 preserved

---

## S33 Summary

### Problem (inherited from S32)
- KV silent drop ที่ 70% load — ไม่มี error signal
- `/wallet/reconstruct` emit JSON ทุก record — ไม่มี intelligence layer
- wire format: JSON-only, ไม่มี binary path

### Solutions Delivered

| Session | Component | Result |
|---|---|---|
| Pre-S33 | `geo_kv.h` — `kv_put` void→int | ✅ probe exhausted = explicit signal |
| Pre-S33 | `kv_bridge.h` — retry+rehash+metrics | ✅ 3-state deterministic behavior |
| S33-B | `llm_adapter_s33.py` | ✅ classify+annotate, pluggable backend |
| S33-B | `rest_server_s33.py` — wire enrich | ✅ opt-in per request |
| S33-A | `rest_server_s33.py` — msgpack | ✅ binary frames, opt-in |

---

## KV Fix (geo_kv.h + kv_bridge.h)

### geo_kv.h
```c
// kv_put: void → int
static inline int kv_put(GeoKV *kv, uint64_t key, uint64_t val) {
    ...
    return 1;   // success
    ...
    return 0;   // probe exhausted
}
```

### kv_bridge.h — new fields
```c
typedef struct {
    ...
    uint32_t cpu_fail;       // probe exhausted after retry
    uint32_t rehash_retry;   // rescued by rehash
} KVBridge;
```

### kvbridge_put — 3-state behavior
```
tombstone-heavy  → rehash → retry → rescued  (rehash_retry++)
load near limit  → rehash → retry → rescued  (rehash_retry++)
table truly full → retry fails               (cpu_fail++)
```

**Stats output:**
```
[KVBridge] cpu.count=X tomb=X flushes=X dropped=X cpu_fail=0 rehash_retry=0
```
`cpu_fail=0` = healthy. `cpu_fail>0` = table truly full → investigate.

---

## S33-B: LLMAdapter

### File: `llm_adapter_s33.py`

**Interface:**
```python
adapter = LLMAdapter(
    api_base  = "http://localhost:8082/v1",   # OpenAI-compatible
    api_key   = "",                            # local = no key needed
    model     = "Qwen3-1.7B-Q8_0.gguf",
    batch_size = 4,                            # tune per hardware
    timeout   = 60.0,
    stub_mode = False,
)

for enriched_rec in adapter.infer(records):
    # enriched_rec = original + {score, label, note, _llm_enriched}
```

**Labels:** `normal | anomaly | hot | cold | audit`

**Failure-safe:** LLM timeout/error → passthrough (original record preserved, `_llm_enriched=False`)

**Stub mode:** `stub_mode=True` → rule-based only (no LLM call) — for testing/dry-run

**Important:** `stub_mode` ต้องตั้งเป็น `False` explicitly สำหรับ local llama.cpp (ไม่ต้องการ api_key)

**Env vars:**
```bash
LLM_ADAPTER_API=http://localhost:8082/v1
LLM_ADAPTER_MODEL=Qwen3-1.7B-Q8_0.gguf
LLM_ADAPTER_KEY=
LLM_ADAPTER_BATCH=4
LLM_ADAPTER_TIMEOUT=60
LLM_ADAPTER_STUB=0
```

### Wire into /wallet/reconstruct
```json
POST /wallet/reconstruct
{
  "handle_id": "wh_xxx",
  "layers": [...],
  "enrich": true    ← S33-B: activate LLM enrichment
}
```

Response records gain: `score`, `label`, `note`, `_llm_enriched`
Summary gains: `llm_enriched: true`, `llm_stats: {...}`
Header: `X-LLM-Enriched: true`

---

## S33-A: Binary Wire (msgpack)

### Wire format
```
binary=false (default): json(batch) + "\n"   ← S32 NDJSON, unchanged
binary=true:            4-byte LE len + msgpack(batch)
```

Summary line is **always JSON** in both modes — easy sentinel detection.

### Request
```json
POST /wallet/reconstruct
{
  "handle_id": "wh_xxx",
  "layers": [...],
  "binary": true    ← S33-A: msgpack frames
}
```

Header: `X-Wire-Format: msgpack`
Media-type: `application/msgpack`

### Install
```bash
pip install msgpack
```

### Client decode
```python
import msgpack, struct

buf = b""
for chunk in response.iter_content(chunk_size=None):
    buf += chunk
    while len(buf) >= 4:
        n = int.from_bytes(buf[:4], "little")
        if len(buf) < 4 + n: break
        batch = msgpack.unpackb(buf[4:4+n], raw=False)
        buf = buf[4+n:]
        for rec in batch:
            process(rec)
```

---

## Endpoint Summary (S33)

```
POST /wallet/reconstruct
  body fields (new in S33):
    enrich: bool = false   → LLM classify+annotate
    binary: bool = false   → msgpack wire format

  headers (new in S33):
    X-LLM-Enriched   → "true" / "false"
    X-Wire-Format    → "ndjson" / "msgpack"

  _summary fields (new):
    wire_format      → "ndjson" | "msgpack"
    llm_enriched     → bool
    llm_stats        → {total_records, hit_rate, llm_failures, ...}
```

---

## Files Changed

| File | Change |
|---|---|
| `geo_kv.h` | `kv_put` void→int |
| `kv_bridge.h` | retry logic + `cpu_fail` + `rehash_retry` metrics |
| `llm_adapter_s33.py` | NEW — classify+annotate adapter |
| `test_llm_adapter_s33.py` | NEW — 22 tests |
| `rest_server_s33.py` | S33-B wire + S33-A msgpack |

**Rebuild:**
```bash
# C layer (geo_kv change)
gcc -O2 -shared -fPIC -I. -o libpogls.so pogls_sdk_so.c -lm

# Python deps
pip install msgpack   # S33-A (optional)
pip install httpx     # S33-B (already required)
```

---

## S34 Candidates

| Candidate | Readiness | Impact |
|---|---|---|
| **Per-layer chunk_lo/hi** in GeoReq | GeoReq has 2 uint16 free | filter granularity |
| **Twin Geo P4** AVX2+CUDA | filter+chunk_window+enrich all wired ✅ | throughput |
| **Score-based routing** | use `score` from LLM → dynamic spoke_mask | intelligence layer |
| **Usage metering** | per-request `llm_stats` already in summary | billing-ready |
