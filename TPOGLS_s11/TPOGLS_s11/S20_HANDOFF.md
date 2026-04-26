# POGLS S20 — Handoff Document
**Session:** S20  
**Date:** 2026-04-10  
**Status:** ✅ COMPLETE — 8/8 tests pass

---

## What was done

### B → A → C + PyTorch hook + C1 optimization

| Deliverable | File | Tests |
|---|---|---|
| Wiring test | `test_s20_wiring.py` | 3/3 ✓ |
| Demo + entry point | `demo_load_model.py` | runs ✓ |
| Public README | `README.md` | — |
| mount_wallet() wrapper | inside `demo_load_model.py` | ✓ |
| PyTorch hook | `pogls_wallet_device.py` | 5/5 ✓ |
| C1 numpy fast path | `pogls_compress_c1.py` | all pass ✓ |

---

## Stack (final S16–S20)

```
.pogwallet
    │  WalletBuilder / WalletReader  (pogls_wallet_py.py — S11)
    │  VirtualFile / MountRegistry   (pogls_mount.py — S12)
    ↓
C1 → C2 → C3 pipeline               (pogls_compress_c1/2/3.py — S16–S18)
    ↓
WeightStream / WeightStreamLoader    (pogls_weight_stream.py — S19)
    ↓
WalletDevice                         (pogls_wallet_device.py — S20) ← NEW
    ↓
torch.load() / np.ndarray
```

---

## Key facts

### .pogwallet format (S11, frozen)
```
[WalletHeader  128B]  magic=0x574C5447 "WLTG", ver=1
[FileEntry[]    64B]  path_offset, file_size, mtime, chunk_start, chunk_count
[string pool       ]
[CoordRecord[]  40B]  file_idx, scan_offset, seed, checksum, face, edge, z
[WalletFooter   16B]  magic=0x454E4457 "WDNE"
```

### WalletBuilder.add_coord() — exact signature
```python
add_coord(file_idx, scan_offset, seed, checksum,
          face=0, edge=0, z=0, drift_window=0) → int
```
Seed and checksum must use `chunk_seed(c)` / `chunk_checksum(c)` from `pogls_wallet_py` — not `sum(chunk)`.

### src_map contract (critical — was a gap before S20)
```python
# buffer mode (test/API)
vf = VirtualFile(blob, src_map={file_idx: raw_bytes}, trusted=True)

# path mode (production, mmap)
vf = VirtualFile(blob, src_map={file_idx: "/path/to/source.bin"}, trusted=True)
```
`src_map={}` → `WalletError: no source for file_idx=0` — always provide source.

### map_location callable — correct pattern (torch 2.11+)
```python
# ✓ correct
def __call__(self, storage, location):
    return torch.serialization.default_restore_location(storage, "cpu")

# ✗ wrong (breaks on torch 2.11)
def __call__(self, storage, location):
    return storage.to("cpu")
```

### C1 optimization (S20 — 8× speedup)
- Bottleneck: `_delta_lookup` / `_phi_lookup` used pure-Python `sum(a==b for a,b in zip(...))`
- Fix: numpy matrix compare — build `(window, 64)` uint8 array, vectorised `.sum(axis=1)`
- Result: 2310ms → 304ms on 134.9KB / 2159 chunks
- Fallback to pure-Python on numpy import error (zero breakage)

---

## Public API surface (minimal, stable)

```python
# one-liner load
from pogls_wallet_device import wallet_load
sd = wallet_load("model.pogwallet")          # → state_dict dict

# context manager
from pogls_wallet_device import WalletDevice
with WalletDevice("model.pogwallet") as dev:
    sd  = dev.load()                         # full state_dict
    t   = dev.layer("weight")               # single tensor (lazy)
    print(dev.info())

# drop-in for torch.load
sd = torch.load(dev.stream(), map_location=dev, weights_only=True)

# numpy direct
from pogls_mount import VirtualFile
from pogls_weight_stream import load_weights_numpy, WeightStreamLoader
vf = VirtualFile(blob, src_map={0: data}, trusted=True)
w  = load_weights_numpy(vf, file_idx=0)     # → np.ndarray
```

---

## Performance (Colab CPU, 134.9KB state_dict)

| Operation | Time |
|---|---|
| WeightStream read (post C1 opt) | ~300ms |
| load_weights_numpy small (1KB) | 0.77ms |
| WeightStream seek + partial read | 0.81ms |
| WeightStreamLoader lazy get (cached) | 0.35ms |

Bottleneck remaining: C1 encode-on-read per chunk (Python loop in C2 chunker).  
Next optimization target: C2 `_split_chunks` + batch encode.

---

## S21 candidates (priority order)

### 🥇 path mode + mmap benchmark
`src_map={i: "/path"}` already works — profile mmap vs buffer on large model.  
Expected: significant speedup on files > RAM cache.

### 🥈 C2 batch encode optimization
```
C2Encoder.encode() → _split_chunks → loop per chunk
```
Profile shows C1 now fast; C2 split loop is next bottleneck for large files.

### 🥉 JSON layer map → true per-layer lazy load
```python
# layer_map.json
{"attention.q": {"file_idx": 0, "offset": 0, "size": 4096, "dtype": "float32"}, ...}

dev = WalletDevice("model.pogwallet")
dev.load_layer_map("layer_map.json")
q = dev.layer("attention.q")   # load only this tensor
```
Infrastructure already exists in `WeightStreamLoader.from_json()` — just needs wiring.

---

## File inventory

```
s14/
  pogls_wallet_py.py       WalletBuilder / WalletReader / chunk_seed / chunk_checksum
  pogls_mount.py           VirtualFile / MountRegistry / _MmapCache

s16_s19/
  pogls_compress_c1.py     C1 encoder/decoder  ← MODIFIED S20 (numpy fast path)
  pogls_compress_c2.py     C2 chunk index + range decode
  pogls_compress_c3.py     C3 stream bridge (WalletBridge)
  pogls_weight_stream.py   WeightStream / load_weights_numpy / WeightStreamLoader
  pogls_wallet_device.py   WalletDevice / wallet_load  ← NEW S20

tests/
  test_s20_wiring.py       3/3 — VirtualFile → C3 → numpy round-trip
  test_s20_wallet_device.py  5/5 — WalletDevice / map_location / timing
  (test_s16_c1.py … test_s19_weight_stream.py  — S16–S19 tests unchanged)

demo_load_model.py         self-contained demo + mount_wallet() entry point
README.md                  3-question public doc
```

---

## Known limitations

1. `WalletDevice(path)` reads full blob into memory on mount — path mode mmap not yet wired in WalletDevice (only in VirtualFile directly)
2. C1 encodes on every stream open (no encode cache) — idempotent but wasteful for repeated reads of same file
3. `WeightStreamLoader.layer()` returns full-file numpy array — sub-tensor slicing requires JSON layer map (S21)
