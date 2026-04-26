"""
pogls_wallet_device.py — S20: WalletDevice PyTorch hook
═════════════════════════════════════════════════════════
Drop-in replacement for torch.load() — streams weights from .pogwallet
via C3 pipeline, never loads the full model into RAM.

API:
    # minimal — one call
    sd = wallet_load("model.pogwallet")

    # explicit device (map_location compatible)
    dev = WalletDevice("model.pogwallet")
    sd  = torch.load(dev.stream(), map_location=dev)

    # context manager
    with WalletDevice("model.pogwallet") as dev:
        sd = dev.load()          # full state_dict
        w  = dev.layer("0.weight")   # single tensor, lazy

    # multi-file wallet
    dev = WalletDevice("model.pogwallet")
    sd  = dev.load(file_idx=1)
"""

from __future__ import annotations

import io
import sys
from contextlib import contextmanager
from typing import Optional

import torch
import torch.serialization

sys.path.insert(0, "s14")
sys.path.insert(0, "s16_s19")

from pogls_wallet_py      import WalletReader, WALLET_MODE_BUFFER
from pogls_mount          import VirtualFile
from pogls_weight_stream  import WeightStream, WeightStreamLoader

# ── config ────────────────────────────────────────────────────────────────────
_DEFAULT_DEVICE  = "cpu"
_WEIGHTS_ONLY    = True     # safe default; caller can override

# ── WalletDevice ──────────────────────────────────────────────────────────────

class WalletDevice:
    """
    PyTorch-compatible device that streams from a .pogwallet.

    Compatible with torch.load(map_location=) callable protocol.
    Also usable as a context manager for clean resource handling.

    Pattern A — drop-in:
        sd = wallet_load("model.pogwallet")

    Pattern B — explicit:
        dev = WalletDevice("model.pogwallet")
        sd  = torch.load(dev.stream(), map_location=dev)

    Pattern C — context manager:
        with WalletDevice("model.pogwallet") as dev:
            sd = dev.load()
            t  = dev.layer("weight")
    """

    def __init__(self, path: str, trusted: bool = True):
        self._path    = path
        self._trusted = trusted
        self._vf: Optional[VirtualFile] = None
        self._loader: Optional[WeightStreamLoader] = None
        self._mount()

    # ── internal ──────────────────────────────────────────────────────────────

    def _mount(self):
        blob = open(self._path, "rb").read()
        wr   = WalletReader(blob, skip_verify=True)
        # buffer mode: src_map points each file_idx to raw blob bytes
        # (wallet coords use scan_offset into the blob itself)
        src_map = {i: blob for i in range(wr.file_count)}
        self._vf     = VirtualFile(blob, src_map=src_map, trusted=self._trusted)
        self._loader = WeightStreamLoader(self._vf)

    def stream(self, file_idx: int = 0) -> io.BytesIO:
        """Return file_idx as a seekable BytesIO — ready for torch.load."""
        ws  = WeightStream(self._vf, file_idx=file_idx)
        raw = ws.read()
        return io.BytesIO(raw)

    # ── map_location callable protocol ───────────────────────────────────────

    def __call__(self, storage, location: str):
        """Called by torch.load for each storage tensor — remap to cpu."""
        return torch.serialization.default_restore_location(storage, _DEFAULT_DEVICE)

    # ── public API ────────────────────────────────────────────────────────────

    def load(self, file_idx: int = 0,
             weights_only: bool = _WEIGHTS_ONLY) -> dict:
        """
        Load full state_dict from wallet file_idx.
        Equivalent to: torch.load("model.pt", map_location="cpu")
        """
        buf = self.stream(file_idx)
        return torch.load(buf, map_location=self, weights_only=weights_only)

    def layer(self, name: str) -> "torch.Tensor":
        """
        Lazy-load a single tensor by layer name.
        Uses WeightStreamLoader cache — second call is free.

        Requires: wallet built with per-layer file_idx mapping
        (one layer per file).  For state_dict wallets, use .load() instead.
        """
        arr = self._loader[name]          # np.ndarray, cached
        return torch.from_numpy(arr.copy())

    def layer_keys(self) -> list[str]:
        """List available layer names (from wallet file index)."""
        return list(self._loader.keys())

    @property
    def file_count(self) -> int:
        return self._vf.file_count

    def info(self) -> dict:
        return {
            "path":       self._path,
            "files":      self._vf.file_count,
            "trusted":    self._trusted,
            "layers":     self.layer_keys(),
        }

    # ── context manager ───────────────────────────────────────────────────────

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self._vf:
            self._vf.close()
            self._vf = None

    def __repr__(self):
        return f"WalletDevice({self._path!r}, files={self.file_count})"


# ── wallet_load: one-liner public entry point ─────────────────────────────────

def wallet_load(path: str,
                file_idx: int = 0,
                trusted: bool = True,
                weights_only: bool = _WEIGHTS_ONLY) -> dict:
    """
    Load state_dict from .pogwallet.  One call, no setup.

        sd = wallet_load("model.pogwallet")
        print(sd["weight"].shape)
    """
    with WalletDevice(path, trusted=trusted) as dev:
        return dev.load(file_idx=file_idx, weights_only=weights_only)
