# POGLS S28 — Handoff
**Status:** ✅ COMPLETE | **Tests:** 29/29 passed

## What was done

### `.so` build — `geo_net_so.c` → `libgeonet.so`

```
gcc -O2 -march=native -shared -fPIC -I. -I./core -o libgeonet.so geo_net_so.c -lm
```

Exports:
| symbol | signature | use |
|---|---|---|
| `geonet_open/close` | `void* / void` | lifecycle |
| `pogls_fetch_chunk` | `(h, idx, out64*) → int` | single chunk route |
| `pogls_fetch_range` | `(h, off, len, out*) → int` | **hot path** — full layer in one C loop |
| `pogls_verify_batch` | `(recs*, n) → int` | verify spoke+inv+face in C |
| `geonet_state` | `(h) → uint8` | QRPN state query |
| `geonet_op_count` | `(h) → uint32` | op count |
| `geonet_mirror_mask` | `(h, spoke) → uint8` | ThirdEye mask |

**Fix applied:** `geo_net_patched.h` — `te_tick` 5-arg → 4-arg to match `geo_thirdeye.h` in this zip (same mismatch as S16/S17).

---

### `geo_net_ctypes.py` — drop-in ctypes binding

`GeoNetCtxFast` replaces `GeoNetCtx`. Auto-detects `libgeonet.so` via:
1. `GEONET_LIB` env var
2. `./libgeonet.so`
3. `/mnt/c/TPOGLS/libgeonet.so`

Falls back to pure Python `GeoNetCtx` silently if `.so` not found.

**Key methods added:**

| method | description |
|---|---|
| `route_layer_fast(file_idx, n_bytes)` | C loop → zero-copy verify → Python summary pass only |
| `route_layer_annotated_fast(file_idx, n_bytes)` | same + builds chunk_map |
| `fetch_range(off, n)` | batch route → list[GeoNetAddr] |
| `verify_batch(recs)` | C verify loop |

---

### Benchmark results

| path | N=50k chunks | speedup |
|---|---|---|
| Python `route()` loop | 73 ms | 1x |
| C `fetch_range` → Python list | 63 ms | 1.2x |
| **C `route_layer_fast` (zero-copy)** | **7.5 ms** | **9.7x** |
| pure C loop (no ctypes) | 0.16 ms | 456x |

**Root cause of ctypes overhead:**
- `ctypes` per-call cost ~1.5µs — grinds down speedup if Python still builds objects per chunk
- `from_buffer(buf)` zero-copy avoids allocation between C calls
- Python summary pass (one loop over raw bytes) is unavoidable but fast

**Key insight:** speedup only materializes when Python never touches individual chunk data. `route_layer_fast` follows this pattern; `route()` in a Python loop does not.

---

### `rest_server_s28.py` — REST upgrade

Diff vs S27:
```
import GeoNetCtxFast as GeoNetCtx   ← swapped
/wallet/load  → rec.geo_ctx.route_layer_fast()         ← C loop
/wallet/layer → rec.geo_ctx.route_layer_annotated_fast() ← C loop
response adds verify_pass field per layer
/health reports geo_backend: "C/libgeonet.so"
```

---

## WSL deploy

```bash
# in /mnt/c/TPOGLS/TPOGLS_s11/
gcc -O2 -march=native -shared -fPIC -I. -I./core -o libgeonet.so geo_net_so.c -lm

# copy to python/ dir or set env:
export GEONET_LIB=/mnt/c/TPOGLS/TPOGLS_s11/libgeonet.so

# run server:
uvicorn rest_server_s28:app --host 0.0.0.0 --port 8765
```

---

## S29 candidates

1. **NDJSON streaming** (Space↓) — `/wallet/layer?annotate=true` chunk_map อาจใหญ่มากถ้า layer หนัก → stream index/coord ทีละส่วน
2. **Twin Geometry Priority 4** — AVX2+CUDA `geo_fast_intersect_x4` ใน `geo_fused_write_batch` (`CYL_FULL_N=3072`)
3. **`pogls_fetch_range` + slot_hot** — expose `slot_hot` ใน C API เพื่อให้ ThirdEye state ใน `.so` ตรงกับ engine state จริง
