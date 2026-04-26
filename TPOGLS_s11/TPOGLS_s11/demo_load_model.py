"""
demo_load_model.py — POGLS S20 demo
════════════════════════════════════
Load model weights from .pogwallet → numpy array

Usage:
    python demo_load_model.py                    # self-contained (builds demo wallet)
    python demo_load_model.py model.pogwallet    # load real wallet
"""

import sys, time
import numpy as np

sys.path.insert(0, "s14")
sys.path.insert(0, "s16_s19")

from pogls_wallet_py     import (WalletBuilder, WALLET_MODE_BUFFER,
                                  DIAMOND_BLOCK_SIZE, chunk_seed, chunk_checksum)
from pogls_mount         import VirtualFile
from pogls_weight_stream import load_weights_numpy, WeightStreamLoader

# ── mount_wallet: thin public entry point ────────────────────────────────────
def mount_wallet(path: str) -> VirtualFile:
    """Open .pogwallet → VirtualFile (trusted, fast path)."""
    blob = open(path, "rb").read()
    from pogls_wallet_py import WalletReader
    wr   = WalletReader(blob, skip_verify=True)
    # path mode: src_map points back to same file for each file_idx
    src_map = {i: path for i in range(wr.file_count)}
    return VirtualFile(blob, src_map=src_map, trusted=True)

# ── demo wallet builder (self-contained, no file needed) ────────────────────
def _build_demo_wallet(n_layers: int = 3, n_elem: int = 512) -> tuple:
    """Build in-memory demo wallet with n_layers of float32 weights."""
    CHUNK  = DIAMOND_BLOCK_SIZE
    layers = [np.random.rand(n_elem).astype("float32") for _ in range(n_layers)]

    wb = WalletBuilder(mode=WALLET_MODE_BUFFER)
    file_ids = []
    raw_bufs = []

    for li, layer in enumerate(layers):
        raw  = layer.tobytes()
        pad  = (-len(raw)) % CHUNK
        data = raw + b"\x00" * pad
        fid  = wb.add_file(f"layer_{li}.bin".encode(), len(raw), 0, 0)
        for i in range(len(data) // CHUNK):
            c = data[i * CHUNK:(i + 1) * CHUNK]
            wb.add_coord(fid, i * CHUNK,
                         seed=chunk_seed(c), checksum=chunk_checksum(c))
        file_ids.append(fid)
        raw_bufs.append(data)

    blob    = wb.seal()
    src_map = {i: raw_bufs[i] for i in range(n_layers)}
    return blob, src_map, layers

# ── main ─────────────────────────────────────────────────────────────────────
def main():
    print("POGLS Weight Stream — demo")
    print("─" * 40)

    if len(sys.argv) > 1:
        # real wallet file
        path = sys.argv[1]
        print(f"loading:  {path}")
        t0 = time.perf_counter()
        vf = mount_wallet(path)
        w  = load_weights_numpy(vf, file_idx=0)
        ms = (time.perf_counter() - t0) * 1000
        print(f"shape:    {w.shape}")
        print(f"dtype:    {w.dtype}")
        print(f"time:     {ms:.2f}ms")
        print(f"files:    {vf.file_count}")
    else:
        # self-contained demo
        N_LAYERS, N_ELEM = 3, 512
        print(f"mode:     demo (in-memory, {N_LAYERS} layers × {N_ELEM} float32)")
        blob, src_map, ground_truth = _build_demo_wallet(N_LAYERS, N_ELEM)
        print(f"wallet:   {len(blob)}B")

        vf     = VirtualFile(blob, src_map=src_map, trusted=True)
        loader = WeightStreamLoader(vf)

        print()
        t0 = time.perf_counter()
        for key in loader.keys():
            w   = loader[key]
            idx = int(key.split("_")[1])
            ok  = np.allclose(w, ground_truth[idx])
            print(f"  {key}  shape={w.shape}  match={ok}  {'✓' if ok else '✗'}")
        ms = (time.perf_counter() - t0) * 1000

        print()
        print(f"time:     {ms:.2f}ms  ({N_LAYERS} layers)")
        print(f"memory:   {sum(w.nbytes for w in ground_truth) / 1024:.1f}KB loaded")

    print()
    print("✓ done")

if __name__ == "__main__":
    main()
