"""
test_s20_wiring.py — S20-B: Full stack wiring test
════════════════════════════════════════════════════
S14 (WalletBuilder/VirtualFile) → S16-S19 (C1→C2→C3→WeightStream)

Run:
    python test_s20_wiring.py
    # expects: 3/3 PASS + timing
"""

import sys, time
import numpy as np

sys.path.insert(0, "s14")
sys.path.insert(0, "s16_s19")

from pogls_wallet_py import (
    WalletBuilder, WALLET_MODE_BUFFER,
    DIAMOND_BLOCK_SIZE, chunk_seed, chunk_checksum,
)
from pogls_mount       import VirtualFile
from pogls_weight_stream import load_weights_numpy, WeightStream, WeightStreamLoader

# ── config ────────────────────────────────────────────────────────────────────
CHUNK   = DIAMOND_BLOCK_SIZE   # 64B frozen
N_ELEM  = 256                  # float32 → 1024B
DTYPE   = "float32"

# ── fixture: build wallet from synthetic weights ──────────────────────────────
def _make_wallet(n_elem: int = N_ELEM):
    raw  = np.arange(n_elem, dtype=DTYPE).tobytes()
    pad  = (-len(raw)) % CHUNK
    data = raw + b"\x00" * pad

    wb  = WalletBuilder(mode=WALLET_MODE_BUFFER)
    fid = wb.add_file(b"weights.bin", len(raw), 0, 0)
    for i in range(len(data) // CHUNK):
        c = data[i * CHUNK : (i + 1) * CHUNK]
        wb.add_coord(fid, i * CHUNK,
                     seed=chunk_seed(c), checksum=chunk_checksum(c))
    return wb.seal(), data, raw

# ── tests ─────────────────────────────────────────────────────────────────────
def test_load_weights_numpy(blob, data, raw):
    """VirtualFile → C3 → numpy round-trip"""
    vf = VirtualFile(blob, src_map={0: data}, trusted=True)
    w  = load_weights_numpy(vf, file_idx=0, dtype=DTYPE)
    assert w.shape == (N_ELEM,),    f"shape: {w.shape}"
    assert np.allclose(w, np.arange(N_ELEM, dtype=DTYPE)), "value mismatch"
    return w.shape

def test_weightstream_seek(blob, data, raw):
    """WeightStream seek → partial read at offset"""
    SKIP_ELEM = 64                                  # jump 64 floats in
    vf = VirtualFile(blob, src_map={0: data}, trusted=True)
    ws = WeightStream(vf, file_idx=0)
    ws.seek(SKIP_ELEM * 4)                          # byte offset
    arr = np.frombuffer(ws.read(SKIP_ELEM * 4), dtype=DTYPE)
    assert arr.shape == (SKIP_ELEM,), f"shape: {arr.shape}"
    assert arr[0] == float(SKIP_ELEM), f"first elem: {arr[0]} ≠ {SKIP_ELEM}"
    return arr.shape

def test_weightstream_loader(blob, data, raw):
    """WeightStreamLoader lazy dict access"""
    vf     = VirtualFile(blob, src_map={0: data}, trusted=True)
    loader = WeightStreamLoader(vf)
    keys   = list(loader.keys())
    assert keys == ["layer_0"], f"keys: {keys}"
    layer  = loader["layer_0"]
    assert layer.shape == (N_ELEM,), f"shape: {layer.shape}"
    # cache: second access must return same object
    assert loader["layer_0"] is layer, "cache miss"
    return keys

# ── runner ────────────────────────────────────────────────────────────────────
TESTS = [
    ("load_weights_numpy  (VirtualFile→C3→ndarray)", test_load_weights_numpy),
    ("WeightStream seek   (offset read)",             test_weightstream_seek),
    ("WeightStreamLoader  (lazy dict + cache)",       test_weightstream_loader),
]

if __name__ == "__main__":
    blob, data, raw = _make_wallet()
    print(f"wallet: {len(blob)}B  |  data: {len(data)}B  |  raw: {len(raw)}B\n")

    passed = 0
    for name, fn in TESTS:
        t0 = time.perf_counter()
        try:
            result = fn(blob, data, raw)
            ms = (time.perf_counter() - t0) * 1000
            print(f"  ✓  {name:<45}  {result}  [{ms:.2f}ms]")
            passed += 1
        except Exception as e:
            print(f"  ✗  {name}")
            print(f"     → {e}")

    print(f"\n{'='*60}")
    print(f"  {passed}/{len(TESTS)} PASS {'✓' if passed == len(TESTS) else '✗'}")
    print(f"{'='*60}")
    sys.exit(0 if passed == len(TESTS) else 1)
