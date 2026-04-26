# POGLS S29 — Handoff
**Status:** ✅ COMPLETE (dry-run) | **Tests:** 28 designed

## What was done

### Problem solved
S28 `annotate=true` → builds full `chunk_map` list in Python → RAM ≈ n_chunks × ~200B.
Layer 50k chunks = ~10MB Python heap per request. Spiky. GC pressure.

### Solution: pogls_iter_chunks + StreamingResponse

**C side (`geo_net_so_s29.c`)**
```c
typedef void (*geonet_chunk_cb)(
    uint64_t chunk_i, uint64_t addr,
    uint8_t spoke, uint8_t inv_spoke,
    uint8_t mirror_mask, uint8_t is_audit,
    void *userdata
);

int pogls_iter_chunks(GeoNetHandle h, uint64_t off, uint64_t n,
                      const char *layer_filter,   // S30: filter pushdown
                      geonet_chunk_cb cb, void *userdata);
```
C drives the loop — callback fires per chunk — no Python list ever holds the full range.

**Python side (`geo_net_ctypes_s29.py`)**
```python
def iter_chunks_stream(file_idx, n_bytes, *, filter_fn=None) -> Iterator[dict]:
```
Batches C callbacks in BATCH=512 slices → yields each → RAM ceiling = BATCH × ~200B ≈ 100KB  
regardless of layer size.

**REST side (`rest_server_s29.py`)**
```
POST /wallet/layer
  annotate=false           → JSON (S28-compatible, unchanged)
  annotate=true            → StreamingResponse (NDJSON)
  annotate=true&compress=true → gzip NDJSON stream
```
Headers: `X-Chunk-Count`, `X-Layer-State`, `X-Geo-Backend`, `X-Iter-Streaming`

Last NDJSON line = summary:
```json
{"_summary": true, "n_chunks": …, "spoke_dist": […],
 "dominant_spoke": …, "audit_points": …, "qrpn_state_after": …}
```

## Memory model

| path | RAM | notes |
|---|---|---|
| S28 annotate=true | O(n_chunks) | full chunk_map list |
| S29 annotate=true | O(BATCH) ≈ 100KB | batch=512, tunable |
| S29 compress=true | O(BATCH) + zlib buf | ~200KB ceiling |

## API diff vs S28

```
WalletLayerBody += compress: bool = False

/wallet/layer response:
  annotate=false → JSON (unchanged)
  annotate=true  → StreamingResponse[NDJSON]  ← NEW

/health += iter_streaming, ndjson_batch
/wallet/layer response headers += X-Chunk-Count, X-Layer-State, …

GeoNetCtxFast += iter_chunks_stream()
geo_net_so += pogls_iter_chunks()   (rebuild required)
```

## Deploy

```bash
# 1. Rebuild .so with S29 C source
gcc -O2 -march=native -shared -fPIC -I. -I./core \
    -o libgeonet.so geo_net_so_s29.c -lm

# 2. Copy Python files
cp geo_net_ctypes_s29.py rest_server_s29.py .

# 3. Run
uvicorn rest_server_s29:app --host 0.0.0.0 --port 8765

# 4. Test stream
curl -s -X POST http://localhost:8765/wallet/layer \
  -H "Content-Type: application/json" \
  -d '{"wallet_b64":"…","src_b64":"…","layer":"…","annotate":true}' | head -5
```

## Graceful fallback chain

```
pogls_iter_chunks in .so → C callback loop (fastest)
     ↓ (old .so / not rebuilt)
route_layer_annotated_fast → S28 path (still works, RAM spike)
     ↓ (no .so at all)
Python GeoNetCtx loop (slowest, correct)
```

`iter_streaming: false` in `/health` = S28 path active (signal to rebuild .so)

## S30 candidates

1. **Filter pushdown** — pass `layer_filter` string to C; `pogls_iter_chunks`
   skips non-matching chunks before callback → data volume ↓ 90%+ for targeted queries
2. **slot_hot** — expose in C API → ThirdEye state consistent with engine
3. **Twin Geo P4** — AVX2+CUDA `geo_fast_intersect_x4` (`CYL_FULL_N=3072`)
