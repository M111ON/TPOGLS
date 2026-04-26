"""
pogls_weight_stream.py — S19: LLM Weight Streaming
═══════════════════════════════════════════════════
Streams model weights from .pogwallet via C3 pipeline.

API:
  WeightStream(vf, file_idx)     → io.RawIOBase (C3-backed)
  load_weights_numpy(vf, ...)    → np.ndarray
  load_weights_torch(vf, ...)    → torch.Tensor  (if torch available)
  WeightStreamLoader(vf)         → dict-like, lazy per-layer access

Layer map format (optional JSON sidecar):
  {"layer_name": {"file_idx": 0, "offset": 0, "size": 1024, "dtype": "float32"}}
"""

from __future__ import annotations
import io
import json
import struct
from typing import Optional

from pogls_compress_c3 import C3WalletBridge, C3Stream, c3_stream
from pogls_compress_c2 import c2_encode

# ── WeightStream ──────────────────────────────────────────────────────────────

class WeightStream(io.RawIOBase):
    """
    Single file from VirtualFile exposed as C3-compressed stream.
    Drop-in for WalletFile — adds compression transparency.
    """

    def __init__(self, vf, file_idx: Optional[int] = None,
                 trusted: bool = True, window_size: int = 256):
        super().__init__()
        self._bridge = C3WalletBridge(vf, file_idx=file_idx,
                                      window_size=window_size)
        self._size = self._bridge.size

    def readable(self) -> bool: return True
    def writable(self) -> bool: return False
    def seekable(self) -> bool: return True

    @property
    def size(self) -> int: return self._size

    def read(self, size: int = -1) -> bytes:
        if self.closed: raise ValueError("closed")
        return self._bridge.read(size)

    def readinto(self, b: bytearray) -> int:
        return self._bridge.readinto(b)

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        return self._bridge.seek(offset, whence)

    def tell(self) -> int:
        return self._bridge.tell()


# ── numpy loader ──────────────────────────────────────────────────────────────

def load_weights_numpy(vf, file_idx: int = 0,
                       dtype: str = "float32",
                       offset: int = 0, count: int = -1):
    """
    Load weights from VirtualFile into numpy array via C3 stream.
    offset/count in elements (not bytes).
    """
    import numpy as np
    dt      = np.dtype(dtype)
    ws      = WeightStream(vf, file_idx=file_idx)
    total_b = ws.size
    n_elem  = total_b // dt.itemsize

    if offset:
        ws.seek(offset * dt.itemsize)
    if count < 0:
        count = (total_b - offset * dt.itemsize) // dt.itemsize

    raw = ws.read(count * dt.itemsize)
    return np.frombuffer(raw, dtype=dt)


# ── torch loader ──────────────────────────────────────────────────────────────

def load_weights_torch(vf, file_idx: int = 0,
                       dtype=None, device: str = "cpu"):
    """
    Load weights from VirtualFile into torch.Tensor via C3 stream.
    dtype: torch dtype (default float32)
    """
    import torch
    import numpy as np

    arr = load_weights_numpy(vf, file_idx=file_idx, dtype="float32")
    t   = torch.from_numpy(arr.copy())
    if dtype is not None:
        t = t.to(dtype)
    return t.to(device)


# ── lazy layer loader ─────────────────────────────────────────────────────────

class WeightStreamLoader:
    """
    Dict-like lazy loader for multi-file wallets with layer map.

    layer_map: {"layer_name": {"file_idx": int, "offset": int,
                                "size": int, "dtype": str}}
    If layer_map is None, exposes files as "layer_0", "layer_1", ...
    """

    def __init__(self, vf, layer_map: Optional[dict] = None):
        self._vf        = vf
        self._layer_map = layer_map
        self._cache: dict[str, object] = {}

        if layer_map is None:
            # auto-map: one entry per file
            self._layer_map = {
                f"layer_{i}": {"file_idx": i, "offset": 0,
                                "size": vf.file_size(i), "dtype": "float32"}
                for i in range(vf.file_count)
            }

    def keys(self):
        return self._layer_map.keys()

    def __contains__(self, name: str) -> bool:
        return name in self._layer_map

    def __getitem__(self, name: str):
        if name in self._cache:
            return self._cache[name]
        if name not in self._layer_map:
            raise KeyError(name)
        import numpy as np
        info    = self._layer_map[name]
        file_idx = info["file_idx"]
        dtype   = info.get("dtype", "float32")
        offset  = info.get("offset", 0) // np.dtype(dtype).itemsize
        count   = info.get("size", -1)
        if count > 0:
            count //= np.dtype(dtype).itemsize
        arr = load_weights_numpy(self._vf, file_idx=file_idx,
                                 dtype=dtype, offset=offset, count=count)
        self._cache[name] = arr
        return arr

    def load_all(self) -> dict:
        return {k: self[k] for k in self.keys()}

    @classmethod
    def from_json(cls, vf, json_path: str) -> "WeightStreamLoader":
        with open(json_path) as f:
            layer_map = json.load(f)
        return cls(vf, layer_map)

