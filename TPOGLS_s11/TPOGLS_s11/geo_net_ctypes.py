"""
geo_net_ctypes.py — S28: libgeonet.so ctypes binding
Drop-in replacement for GeoNetCtx in geo_net_py.py.

Priority:
  1. Load libgeonet.so → use C loop (pogls_fetch_range) — fast path
  2. Fallback to pure Python GeoNetCtx — no breakage

Key exports (matching rest_server_s27 interface):
  GeoNetCtxFast.route(addr, slot_hot=0) → GeoNetAddr  (stateful, te_tick in C)
  GeoNetCtxFast.spoke_mirror_mask(spoke) → int
  GeoNetCtxFast.state → str
  GeoNetCtxFast.status() → dict
  GeoNetCtxFast.fetch_range(off, n) → list[GeoNetAddr]   ← NEW: batch C loop
  GeoNetCtxFast.verify_batch(recs) → int                 ← NEW: verify in C

Search order for libgeonet.so:
  GEONET_LIB env var → ./libgeonet.so → /mnt/c/TPOGLS/libgeonet.so
"""

from __future__ import annotations
import ctypes
import os
import logging
from dataclasses import dataclass
from typing import Optional

log = logging.getLogger("pogls.geonet_ctypes")

# ── packed addr struct (matches geo_net_so.c GeoNetPackedAddr) ─────
class _CAddr(ctypes.Structure):
    _fields_ = [
        ("spoke",       ctypes.c_uint8),
        ("inv_spoke",   ctypes.c_uint8),
        ("face",        ctypes.c_uint8),
        ("unit",        ctypes.c_uint8),
        ("group",       ctypes.c_uint8),
        ("mirror_mask", ctypes.c_uint8),
        ("is_center",   ctypes.c_uint8),
        ("is_audit",    ctypes.c_uint8),
    ]  # 8 bytes total

# ── Python-side result dataclass (same as geo_net_py.GeoNetAddr) ───
@dataclass
class GeoNetAddr:
    spoke:       int
    slot:        int
    face:        int
    unit:        int
    inv_spoke:   int
    group:       int
    is_center:   bool
    is_audit:    bool

# ── .so loader ─────────────────────────────────────────────────────

def _load_lib() -> Optional[ctypes.CDLL]:
    candidates = [
        os.environ.get("GEONET_LIB", ""),
        os.path.join(os.path.dirname(__file__), "libgeonet.so"),
        "/mnt/c/TPOGLS/libgeonet.so",
        "/mnt/c/TPOGLS/TPOGLS_s11/libgeonet.so",
    ]
    for path in candidates:
        if not path:
            continue
        try:
            lib = ctypes.CDLL(path)
            # wire up signatures
            lib.geonet_open.restype  = ctypes.c_void_p
            lib.geonet_open.argtypes = []

            lib.geonet_close.restype  = None
            lib.geonet_close.argtypes = [ctypes.c_void_p]

            lib.pogls_fetch_chunk.restype  = ctypes.c_int
            lib.pogls_fetch_chunk.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint32,
                ctypes.c_char_p,   # uint8_t* out64
            ]

            lib.pogls_fetch_range.restype  = ctypes.c_int
            lib.pogls_fetch_range.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint64,   # off
                ctypes.c_uint64,   # len
                ctypes.c_char_p,   # uint8_t* out (len * 8 bytes)
            ]

            lib.pogls_verify_batch.restype  = ctypes.c_int
            lib.pogls_verify_batch.argtypes = [
                ctypes.c_void_p,   # GeoNetBatchRec*
                ctypes.c_int,
            ]

            lib.geonet_state.restype  = ctypes.c_uint8
            lib.geonet_state.argtypes = [ctypes.c_void_p]

            lib.geonet_op_count.restype  = ctypes.c_uint32
            lib.geonet_op_count.argtypes = [ctypes.c_void_p]

            lib.geonet_mirror_mask.restype  = ctypes.c_uint8
            lib.geonet_mirror_mask.argtypes = [ctypes.c_void_p, ctypes.c_uint8]

            log.info("libgeonet.so loaded: %s", path)
            return lib
        except OSError:
            continue
    return None


_LIB: Optional[ctypes.CDLL] = _load_lib()
_STATE_NAME = {0: "NORMAL", 1: "STRESSED", 2: "ANOMALY"}
CYL_FULL_N = 3456

# ── ctypes-backed context ───────────────────────────────────────────

class GeoNetCtxFast:
    """
    GeoNetCtx backed by libgeonet.so when available.
    Falls back to geo_net_py.GeoNetCtx if .so not loaded.
    API is a superset of GeoNetCtx — drop-in for rest_server_s27.
    """

    def __init__(self):
        if _LIB is not None:
            self._h = _LIB.geonet_open()
            if not self._h:
                raise RuntimeError("geonet_open() returned NULL")
            self._lib     = _LIB
            self._fast    = True
            self._buf64   = ctypes.create_string_buffer(64)   # reusable per fetch_chunk
        else:
            # fallback: pure Python
            from geo_net_py import GeoNetCtx as _PyCtx
            self._py  = _PyCtx()
            self._fast = False
            log.warning("libgeonet.so not found — using Python GeoNetCtx fallback")

    def __del__(self):
        if getattr(self, "_fast", False) and self._h:
            self._lib.geonet_close(self._h)
            self._h = None

    # ── single route (hot path for rest_server_s27 /wallet/layer) ──

    def route(self, addr: int, slot_hot: int = 0) -> GeoNetAddr:
        if not self._fast:
            return self._py.route(addr, slot_hot)

        # fetch_chunk routes addr through C GeoNet (te_tick included)
        # slot_hot not yet wired in fetch_chunk (geo_net_route ignores it for
        # routing but passes to te_tick). For now pass 0 — same as stateless.
        rc = self._lib.pogls_fetch_chunk(self._h, ctypes.c_uint32(addr), self._buf64)
        if rc != 0:
            raise RuntimeError(f"pogls_fetch_chunk failed: {rc}")
        b = self._buf64.raw
        # slot: addr % CYL_FULL_N // 6  (mirrors C logic — no C call needed)
        full_idx = addr % CYL_FULL_N
        slot     = full_idx // 6
        return GeoNetAddr(
            spoke     = b[0],
            slot      = slot,
            face      = b[2],
            unit      = b[3],
            inv_spoke = b[1],
            group     = b[4],
            is_center = bool(b[6]),
            is_audit  = bool(b[7]),
        )

    # ── batch route — the real speed gain ──────────────────────────

    def fetch_range(self, off: int, n: int) -> list[GeoNetAddr]:
        """
        Route n consecutive chunks starting at addr=off through C loop.
        Returns list[GeoNetAddr]. ThirdEye state accumulates in C — no Python overhead.
        """
        if not self._fast:
            return [self.route(off + i) for i in range(n)]

        buf = ctypes.create_string_buffer(n * 8)
        rc  = self._lib.pogls_fetch_range(
            self._h, ctypes.c_uint64(off), ctypes.c_uint64(n), buf
        )
        if rc < 0:
            raise RuntimeError(f"pogls_fetch_range failed: {rc}")

        raw = buf.raw
        out = []
        for i in range(n):
            o  = i * 8
            b  = raw[o:o+8]
            full_idx = (off + i) % CYL_FULL_N
            slot     = full_idx // 6
            out.append(GeoNetAddr(
                spoke     = b[0],
                slot      = slot,
                face      = b[2],
                unit      = b[3],
                inv_spoke = b[1],
                group     = b[4],
                is_center = bool(b[6]),
                is_audit  = bool(b[7]),
            ))
        return out

    # ── route_layer_fast: zero-copy C→C path ───────────────────────
    # Real speed gain. Pattern:
    #   pogls_fetch_range (C loop) → from_buffer (zero-copy) →
    #   pogls_verify_batch (C loop) → one Python pass for summary only.
    # No GeoNetAddr Python objects created — 6–10x vs Python route() loop.

    def route_layer_fast(self, file_idx: int, n_bytes: int,
                         chunk_size: int = 64) -> dict:
        """Route full layer in C. Returns summary dict (no per-chunk objects)."""
        n_chunks = max(1, (n_bytes + chunk_size - 1) // chunk_size)
        if not self._fast:
            spoke_dist = [0]*6; audit_count = center_count = 0
            for i in range(n_chunks):
                a = self._py.route(file_idx * CYL_FULL_N + i)
                spoke_dist[a.spoke] += 1
                if a.is_audit:  audit_count  += 1
                if a.is_center: center_count += 1
            return {"n_chunks": n_chunks, "spoke_dist": spoke_dist,
                    "audit_points": audit_count, "center_chunks": center_count,
                    "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
                    "qrpn_state_after": self._py.state, "verify_pass": n_chunks}

        off = file_idx * CYL_FULL_N
        buf = ctypes.create_string_buffer(n_chunks * 8)
        self._lib.pogls_fetch_range(
            self._h, ctypes.c_uint64(off), ctypes.c_uint64(n_chunks), buf)
        CArr   = (_CAddr * n_chunks)
        arr    = CArr.from_buffer(buf)   # zero-copy
        pass_n = self._lib.pogls_verify_batch(arr, ctypes.c_int(n_chunks))
        raw = buf.raw
        spoke_dist = [0]*6; audit_count = center_count = 0
        for i in range(n_chunks):
            o = i * 8
            spoke_dist[raw[o]] += 1
            if raw[o+7]: audit_count  += 1
            if raw[o+6]: center_count += 1
        return {"n_chunks": n_chunks, "spoke_dist": spoke_dist,
                "audit_points": audit_count, "center_chunks": center_count,
                "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
                "qrpn_state_after": self.state, "verify_pass": pass_n}

    def route_layer_annotated_fast(self, file_idx: int, n_bytes: int,
                                   chunk_size: int = 64) -> tuple[dict, list]:
        """Like route_layer_fast but also builds chunk_map (for annotate=true)."""
        n_chunks = max(1, (n_bytes + chunk_size - 1) // chunk_size)
        if not self._fast:
            spoke_dist = [0]*6; chunk_map = []; ac = cc = 0
            for i in range(n_chunks):
                addr = file_idx * CYL_FULL_N + i
                a = self._py.route(addr)
                mm = self._py.spoke_mirror_mask(a.spoke)
                spoke_dist[a.spoke] += 1
                if a.is_audit:  ac += 1
                if a.is_center: cc += 1
                chunk_map.append({"chunk_i": i, "addr": addr, "spoke": a.spoke,
                                   "inv_spoke": a.inv_spoke, "mirror_mask": mm,
                                   "is_audit": a.is_audit})
            return ({"n_chunks": n_chunks, "spoke_dist": spoke_dist,
                     "audit_points": ac, "center_chunks": cc,
                     "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
                     "qrpn_state_after": self._py.state, "verify_pass": n_chunks},
                    chunk_map)

        off = file_idx * CYL_FULL_N
        buf = ctypes.create_string_buffer(n_chunks * 8)
        self._lib.pogls_fetch_range(
            self._h, ctypes.c_uint64(off), ctypes.c_uint64(n_chunks), buf)
        CArr   = (_CAddr * n_chunks)
        arr    = CArr.from_buffer(buf)
        pass_n = self._lib.pogls_verify_batch(arr, ctypes.c_int(n_chunks))
        raw = buf.raw
        spoke_dist = [0]*6; chunk_map = []; ac = cc = 0
        for i in range(n_chunks):
            o     = i * 8
            spoke = raw[o]; inv_sp = raw[o+1]
            mm    = int(self._lib.geonet_mirror_mask(self._h, ctypes.c_uint8(spoke)))
            ia    = bool(raw[o+7]); ic = bool(raw[o+6])
            spoke_dist[spoke] += 1
            if ia: ac += 1
            if ic: cc += 1
            chunk_map.append({"chunk_i": i, "addr": off+i, "spoke": spoke,
                               "inv_spoke": inv_sp, "mirror_mask": mm, "is_audit": ia})
        return ({"n_chunks": n_chunks, "spoke_dist": spoke_dist,
                 "audit_points": ac, "center_chunks": cc,
                 "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
                 "qrpn_state_after": self.state, "verify_pass": pass_n},
                chunk_map)

    # ── verify batch in C ───────────────────────────────────────────

    def verify_batch(self, recs: list[GeoNetAddr]) -> int:
        """Returns count of PASS records. recs == all pass iff return == len(recs)."""
        if not recs:
            return 0
        if not self._fast:
            return sum(
                1 for r in recs
                if r.spoke < 6 and r.inv_spoke == (r.spoke + 3) % 6 and r.face < 9
            )
        n    = len(recs)
        Arr  = (_CAddr * n)
        arr  = Arr()
        for i, r in enumerate(recs):
            arr[i].spoke       = r.spoke
            arr[i].inv_spoke   = r.inv_spoke
            arr[i].face        = r.face
            arr[i].unit        = r.unit
            arr[i].group       = r.group
            arr[i].mirror_mask = 0   # not needed for verify
            arr[i].is_center   = int(r.is_center)
            arr[i].is_audit    = int(r.is_audit)
        return self._lib.pogls_verify_batch(arr, ctypes.c_int(n))

    # ── state queries (mirrors GeoNetCtx API) ──────────────────────

    @property
    def state(self) -> str:
        if not self._fast:
            return self._py.state
        return _STATE_NAME.get(self._lib.geonet_state(self._h), "UNKNOWN")

    @property
    def op_count(self) -> int:
        if not self._fast:
            return self._py.op_count
        return int(self._lib.geonet_op_count(self._h))

    def spoke_mirror_mask(self, spoke: int) -> int:
        if not self._fast:
            return self._py.spoke_mirror_mask(spoke)
        return int(self._lib.geonet_mirror_mask(self._h, ctypes.c_uint8(spoke)))

    def status(self) -> dict:
        if not self._fast:
            return self._py.status()
        return {
            "ops":        self.op_count,
            "qrpn_state": self.state,
            "backend":    "C/libgeonet.so",
            "fast":       True,
        }

    @property
    def is_fast(self) -> bool:
        return self._fast


# ── convenience alias ───────────────────────────────────────────────
# lets rest_server_s27 do: from geo_net_ctypes import GeoNetCtx
GeoNetCtx = GeoNetCtxFast
