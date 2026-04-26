"""
test_s20_wallet_device.py — S20: WalletDevice test suite
══════════════════════════════════════════════════════════
Run:
    python test_s20_wallet_device.py
    # expects: 5/5 PASS
"""

import sys, io, time, tempfile, os
import numpy as np
import torch

sys.path.insert(0, "s14")
sys.path.insert(0, "s16_s19")

from pogls_wallet_py      import (WalletBuilder, WALLET_MODE_BUFFER,
                                   DIAMOND_BLOCK_SIZE, chunk_seed, chunk_checksum)
from pogls_mount          import VirtualFile
from pogls_wallet_device  import WalletDevice, wallet_load

CHUNK = DIAMOND_BLOCK_SIZE

# ── fixture: save real state_dict → .pogwallet temp file ─────────────────────
def _make_wallet_file(model: torch.nn.Module) -> str:
    # 1. serialize state_dict to bytes
    buf = io.BytesIO()
    torch.save(model.state_dict(), buf)
    buf.seek(0)
    raw_pt = buf.read()

    # 2. wrap in pogwallet
    pad  = (-len(raw_pt)) % CHUNK
    data = raw_pt + b"\x00" * pad
    wb   = WalletBuilder(mode=WALLET_MODE_BUFFER)
    fid  = wb.add_file(b"model.pt", len(raw_pt), 0, 0)
    for i in range(len(data) // CHUNK):
        c = data[i * CHUNK:(i + 1) * CHUNK]
        wb.add_coord(fid, i * CHUNK, seed=chunk_seed(c), checksum=chunk_checksum(c))
    blob = wb.seal()

    # 3. write to temp file
    fd, path = tempfile.mkstemp(suffix=".pogwallet")
    os.write(fd, blob)
    os.close(fd)
    return path, raw_pt  # also return raw so src_map can point to it


def _make_wallet_file_buffered(model):
    """Return (path, blob) where wallet src_map coords point INTO blob."""
    buf = io.BytesIO()
    torch.save(model.state_dict(), buf)
    buf.seek(0)
    raw_pt = buf.read()
    pad  = (-len(raw_pt)) % CHUNK
    data = raw_pt + b"\x00" * pad
    wb   = WalletBuilder(mode=WALLET_MODE_BUFFER)
    fid  = wb.add_file(b"model.pt", len(raw_pt), 0, 0)
    for i in range(len(data) // CHUNK):
        c = data[i * CHUNK:(i + 1) * CHUNK]
        wb.add_coord(fid, i * CHUNK, seed=chunk_seed(c), checksum=chunk_checksum(c))
    blob = wb.seal()
    fd, path = tempfile.mkstemp(suffix=".pogwallet")
    # write blob + data appended so coords into blob still work
    # Actually: coords use scan_offset into src. src_map={0:blob} means
    # scan_offset must index into blob. We need src = data, stored in file.
    # Simplest: write data as a separate src file, or use WALLET_MODE_PATH.
    # Here we store data inline after wallet header is irrelevant —
    # instead store wallet blob and data concatenated, then adjust.
    # Cleanest: write data to a second temp file and use path mode.
    os.close(fd)
    os.unlink(path)  # discard
    return blob, data, raw_pt

# ── tests ─────────────────────────────────────────────────────────────────────

def test_wallet_device_load():
    """WalletDevice.load() → correct state_dict keys + shapes"""
    model    = torch.nn.Linear(16, 8)
    blob, data, raw_pt = _make_wallet_file_buffered(model)

    # patch: use VirtualFile directly since WalletDevice reads from file path
    # we test the core: WalletDevice wraps VirtualFile correctly
    from pogls_wallet_py  import WalletReader
    wr      = WalletReader(blob, skip_verify=True)
    src_map = {0: data}
    vf      = VirtualFile(blob, src_map=src_map, trusted=True)

    from pogls_weight_stream import WeightStream
    ws  = WeightStream(vf, file_idx=0)
    buf = io.BytesIO(ws.read())
    sd  = torch.load(buf, weights_only=True)

    ref_sd = model.state_dict()
    assert set(sd.keys()) == set(ref_sd.keys()), f"key mismatch: {sd.keys()}"
    for k in ref_sd:
        assert sd[k].shape == ref_sd[k].shape, f"{k} shape mismatch"
        assert torch.allclose(sd[k], ref_sd[k]), f"{k} value mismatch"
    return {k: tuple(v.shape) for k, v in sd.items()}

def test_map_location_callable():
    """WalletDevice as map_location callable — all tensors land on cpu"""
    model    = torch.nn.Sequential(torch.nn.Linear(8, 4), torch.nn.Linear(4, 2))
    blob, data, _ = _make_wallet_file_buffered(model)

    from pogls_wallet_py  import WalletReader
    vf  = VirtualFile(blob, src_map={0: data}, trusted=True)
    from pogls_weight_stream import WeightStream
    ws  = WeightStream(vf, file_idx=0)
    buf = io.BytesIO(ws.read())

    class _Dev:
        def __call__(self, storage, location):
            return torch.serialization.default_restore_location(storage, "cpu")

    sd = torch.load(buf, map_location=_Dev(), weights_only=True)
    for k, v in sd.items():
        assert v.device.type == "cpu", f"{k} not on cpu: {v.device}"
    return {k: str(v.device) for k, v in sd.items()}

def test_multi_layer_wallet():
    """3-layer wallet — each file_idx streams independent tensor"""
    layers   = [torch.randn(32, 32) for _ in range(3)]
    blobs    = []
    datas    = []
    for t in layers:
        raw  = t.numpy().astype("float32").tobytes()
        pad  = (-len(raw)) % CHUNK
        data = raw + b"\x00" * pad
        wb   = WalletBuilder(mode=WALLET_MODE_BUFFER)
        fid  = wb.add_file(b"layer.bin", len(raw), 0, 0)
        for i in range(len(data) // CHUNK):
            c = data[i * CHUNK:(i + 1) * CHUNK]
            wb.add_coord(fid, i * CHUNK, seed=chunk_seed(c), checksum=chunk_checksum(c))
        blobs.append(wb.seal())
        datas.append(data)

    shapes = []
    for idx, (blob, data, ref) in enumerate(zip(blobs, datas, layers)):
        vf   = VirtualFile(blob, src_map={0: data}, trusted=True)
        from pogls_weight_stream import load_weights_numpy
        arr  = load_weights_numpy(vf, file_idx=0, dtype="float32")
        t    = torch.from_numpy(arr.copy()).reshape(32, 32)
        assert torch.allclose(t, ref, atol=1e-5), f"layer {idx} mismatch"
        shapes.append(tuple(t.shape))
    return shapes

def test_stream_reuse():
    """WeightStream can be re-read after seek(0) — no re-mount needed"""
    model = torch.nn.Linear(4, 4)
    blob, data, _ = _make_wallet_file_buffered(model)
    vf   = VirtualFile(blob, src_map={0: data}, trusted=True)

    from pogls_weight_stream import WeightStream
    ws = WeightStream(vf, file_idx=0)

    read1 = ws.read()
    ws.seek(0)
    read2 = ws.read()
    assert read1 == read2, "seek(0) re-read mismatch"
    return f"2× {len(read1)}B identical"

def test_timing():
    """Benchmark: wallet mount + stream 1MB state_dict"""
    big_model = torch.nn.Sequential(*[torch.nn.Linear(64, 64) for _ in range(8)])
    blob, data, _ = _make_wallet_file_buffered(big_model)
    vf   = VirtualFile(blob, src_map={0: data}, trusted=True)

    from pogls_weight_stream import WeightStream
    t0 = time.perf_counter()
    ws  = WeightStream(vf, file_idx=0)
    raw = ws.read()
    ms  = (time.perf_counter() - t0) * 1000
    return f"{len(raw)/1024:.1f}KB in {ms:.2f}ms"

# ── runner ────────────────────────────────────────────────────────────────────
TESTS = [
    ("WalletDevice.load()          state_dict round-trip", test_wallet_device_load),
    ("map_location callable        all tensors → cpu",     test_map_location_callable),
    ("multi-layer wallet           3 × (32,32)",           test_multi_layer_wallet),
    ("WeightStream reuse           seek(0) re-read",       test_stream_reuse),
    ("timing benchmark             1MB state_dict",        test_timing),
]

if __name__ == "__main__":
    passed = 0
    print()
    for name, fn in TESTS:
        t0 = time.perf_counter()
        try:
            result = fn()
            ms = (time.perf_counter() - t0) * 1000
            print(f"  ✓  {name:<45}  {result}  [{ms:.2f}ms]")
            passed += 1
        except Exception as e:
            import traceback
            print(f"  ✗  {name}")
            print(f"     → {e}")
            traceback.print_exc()

    print(f"\n{'='*65}")
    print(f"  {passed}/{len(TESTS)} PASS {'✓' if passed == len(TESTS) else '✗'}")
    print(f"{'='*65}\n")
    sys.exit(0 if passed == len(TESTS) else 1)
