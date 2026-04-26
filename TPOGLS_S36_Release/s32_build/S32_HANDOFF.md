# POGLS S32 — Handoff
**Status:** ✅ COMPLETE | **Tests:** 76/76 passed

---

## S32: Streaming Reconstruct (B+C)

### Problem
S31: mount once → stream 1 layer at a time.
High-frequency callers wanting partial slices from N layers = N round-trips + N re-parse costs.

### Solution
Multi-layer coord streaming via persistent mount handle.
C iterates N layers in order, applies shared filter, emits batches → Python batch-encodes JSON.

---

## S32-A: GeoNetFilter final layout

```c
typedef struct {
    uint8_t  spoke_mask;   // S30  offset 0
    uint8_t  flags;        // S32  offset 1  (replaces audit_only)
    uint16_t val_min;      // S31  offset 2
    uint16_t val_max;      // S31  offset 4
    uint16_t chunk_lo;     // S32  offset 6
    uint16_t chunk_hi;     // S32  offset 8  0xFFFF = disabled
} GeoNetFilter;            // sizeof = 10 bytes, no padding
```

**flags bits:**
```c
#define GF_AUDIT_ONLY  0x01u   // bit0: audit chunks only (S30 compat via bit)
#define GF_VAL_FILTER  0x02u   // bit1: val_min/max gate active (S32 explicit opt-in)
// bits 2–7: reserved S33+
```

**Sentinel rules:**
| Field | Disabled sentinel | Layer |
|---|---|---|
| `val_max == 0` | range gate off | S31 path (`pogls_iter_chunks`) |
| `val_min > val_max` + `GF_VAL_FILTER` | range gate off | S32 path (`fetch_multi_range`) |
| `chunk_hi == 0xFFFF` | chunk window off | S32 |
| `chunk_hi == 0` | only chunk 0 | S32 (not disabled!) |

**ABI:** S31 `pogls_iter_chunks` behavior preserved exactly. S32 callers use `pogls_fetch_multi_range`.

---

## S32-B: New C types

```c
// Per-layer request descriptor
typedef struct {
    uint32_t layer_id;   // caller-assigned, echoed in GeoMultiRec
    uint32_t file_idx;   // wallet position
    uint64_t n_bytes;    // layer size → n_chunks = (n_bytes+63)/64
} GeoReq;

// 36-byte packed output record
typedef struct {
    uint32_t layer_id;      // offset  0
    uint32_t chunk_global;  // offset  4  — running index across ALL layers
    uint64_t chunk_i;       // offset  8  — index within this layer
    uint64_t addr;          // offset 16
    uint64_t offset;        // offset 24  — chunk_i * 64 (direct byte seek)
    uint8_t  spoke;         // offset 32
    uint8_t  mirror_mask;   // offset 33
    uint8_t  is_audit;      // offset 34
    uint8_t  _pad;          // offset 35
} GeoMultiRec;              // 36 bytes

// Batch callback
typedef void (*geo_multi_cb_batch)(
    const GeoMultiRec *recs,
    size_t             n,      // 1..256
    void              *userdata
);
```

---

## S32-C: Core C API

```c
int pogls_fetch_multi_range(
    GeoNetHandle         h,
    const GeoReq        *reqs,
    int                  n_reqs,
    const GeoNetFilter  *filt,       // shared filter for all layers
    geo_multi_cb_batch   cb,
    void                *userdata
);
```

**Filter gate chain (AND, all in C):**
```
spoke_mask → GF_AUDIT_ONLY → GF_VAL_FILTER (val_min>max=disabled) → chunk window → emit
```

**Batch mechanics:**
- C accumulates `GeoMultiRec buf[256]` on stack
- Flushes on `buf_n == 256` OR layer boundary
- `chunk_global` increments monotonically across all layers
- Python receives full batches → one `json.dumps(batch)` per flush

**Throughput delta vs S31:**
| Path | Per-record Python overhead |
|---|---|
| S31 `/wallet/layer/fast` | `json.dumps(rec)` + `yield` per record |
| S32 `/wallet/reconstruct` | `json.dumps(batch)` + `yield` per 256 records |

---

## S32-D: Python ctypes

New in `geo_net_ctypes_s32.py`:
```python
CHUNK_FILTER_OFF = 0xFFFF
GF_AUDIT_ONLY    = 0x01
GF_VAL_FILTER    = 0x02
MULTI_BATCH_SZ   = 256

class _CFilter:     # flags field replaces audit_only; chunk_lo/hi added
class _CGeoReq      # layer_id, file_idx, n_bytes
class _CGeoMultiRec # 36-byte mirror of C struct
_MultiCbBatchType   # CFUNCTYPE for batch callback

GeoNetCtxFast.iter_multi_range(reqs, spoke_mask, audit_only,
                                val_min, val_max, val_filter,
                                chunk_lo, chunk_hi) → Iterator[list[dict]]
```

**Chunk gate fix (Python fallback):**
```python
_chunk_gate = (chunk_hi != CHUNK_FILTER_OFF) or (chunk_lo > 0)
```
Gate activates when either bound is non-trivial — not just when `chunk_hi` is set.

---

## S32-E: REST Endpoint

```
POST /wallet/reconstruct
```

**Body:**
```json
{
  "handle_id":  "wh_abc123",
  "layers": [
    { "name": "embed.weight", "chunk_lo": 0,   "chunk_hi": 500  },
    { "name": "lm_head",      "chunk_lo": 200, "chunk_hi": 800  }
  ],
  "spoke_mask": 63,
  "audit_only": false,
  "val_min":    1,
  "val_max":    0,
  "val_filter": false,
  "annotate":   false,
  "ctx_id":     "default"
}
```

**Response headers:**
```
X-Layer-Count       — number of layers
X-Layer-Names       — comma-separated
X-Total-Chunks-Est  — estimated total chunks
X-Chunk-Window      — "lo-hi" or "off"
X-Val-Range         — "min-max" or "off"
X-Filter-Flags      — hex flags byte
X-Spoke-Mask        — hex
X-Multi-Range       — "true" if C path active
X-Handle-Id         — echo
```

**NDJSON format:**
```
[{layer_id, layer_name, chunk_global, chunk_i, addr, offset, spoke, mirror_mask, is_audit}, ...]  ← batch line
[...batch...]
{"_summary": true, "n_layers": 2, "total_records": ..., ...}  ← last line
```

Downstream assembly:
```python
for line in response.iter_lines():
    batch = json.loads(line)
    if isinstance(batch, dict) and batch.get("_summary"): break
    for rec in batch:
        layer_data[rec["layer_name"]][rec["chunk_i"]] = rec["offset"]
```

---

## Deploy

```bash
# 1. Rebuild .so (struct changed: audit_only→flags, _pad[2]→chunk_lo/hi)
gcc -O2 -march=native -shared -fPIC -I. -I./core \
    -o libgeonet_s32.so geo_net_so_s32.c -lm

# 2. Run
uvicorn rest_server_s32:app --host 0.0.0.0 --port 8765

# 3. Mount once (S31-B, unchanged)
curl -X POST .../wallet/mount \
  -d '{"wallet_b64":"...","src_b64":"..."}' \
  → {"handle_id": "wh_abc123", "n_layers": 12}

# 4. Reconstruct slice across layers
curl -X POST .../wallet/reconstruct \
  -d '{
    "handle_id": "wh_abc123",
    "layers": [
      {"name": "embed.weight", "chunk_lo": 0,   "chunk_hi": 100},
      {"name": "lm_head",      "chunk_lo": 50,  "chunk_hi": 150}
    ],
    "spoke_mask": 1,
    "val_filter": true, "val_min": 0, "val_max": 200
  }'
```

---

## Test Coverage

| Class | Tests | Covers |
|---|---|---|
| TestSlotHot | 4 | slot_hot C wiring (S30) |
| TestFetchRangeHot | 4 | hot array pressure (S30) |
| TestSignalFail | 4 | QRPN feedback (S30) |
| TestFilterPushdown | 10 | spoke_mask + audit_only (S30) |
| TestStatusS30 | 4 | status fields |
| TestValRangeFilter | 8 | val_min/max gate S31 sentinel |
| **TestChunkWindowFilter** | **8** | **chunk_lo/hi gate (S32)** |
| **TestMultiRange** | **14** | **iter_multi_range + /reconstruct** |
| TestWalletMount | 20 | mount/unmount/list/fast/health (S31-B + S32 health) |
| **Total** | **76** | |

---

## S33 Candidates

| Candidate | Readiness |
|---|---|
| **Twin Geo P4** — AVX2+CUDA `geo_fast_intersect_x4` | filter+chunk_window+val_range all wired ✅ |
| **`_pad[2]` face/unit filter** | extend GeoNetFilter (no space left → new flags bits or struct v2) |
| **Per-layer chunk window** — pass `chunk_lo/hi` per GeoReq | GeoReq has room for 2 uint16 fields |
| **Binary wire format** — `annotate=false` → msgpack batch | eliminates JSON encode bottleneck entirely |
