"""
geo_net_ctypes_s29.py — S29: NDJSON streaming via pogls_iter_chunks callback

Key addition over S28:
  iter_chunks_stream(file_idx, n_bytes, *, filter_fn=None)
    → Generator[dict, None, None]
    → memory O(1): C fires callback per chunk, Python yields immediately
    → caller does:  for rec in ctx.iter_chunks_stream(...): process(rec)

All S28 API unchanged (route, fetch_range, route_layer_fast, etc.)
"""

from __future__ import annotations
import ctypes
import os
import logging
from dataclasses import dataclass
from typing import Optional, Iterator, Callable

log = logging.getLogger("pogls.geonet_ctypes")

# ── packed addr struct ─────────────────────────────────────────────
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
    ]

# ── S29: callback type matching C typedef ─────────────────────────
# void (*geonet_chunk_cb)(uint64 chunk_i, uint64 addr, u8 spoke,
#                         u8 inv_spoke, u8 mirror_mask, u8 is_audit,
#                         void* userdata)
_ChunkCbType = ctypes.CFUNCTYPE(
    None,
    ctypes.c_uint64,   # chunk_i
    ctypes.c_uint64,   # addr
    ctypes.c_uint8,    # spoke
    ctypes.c_uint8,    # inv_spoke
    ctypes.c_uint8,    # mirror_mask
    ctypes.c_uint8,    # is_audit
    ctypes.c_void_p,   # userdata (unused — closure carries state)
)

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

            # S28 exports
            lib.geonet_open.restype   = ctypes.c_void_p
            lib.geonet_open.argtypes  = []
            lib.geonet_close.restype  = None
            lib.geonet_close.argtypes = [ctypes.c_void_p]

            lib.pogls_fetch_chunk.restype  = ctypes.c_int
            lib.pogls_fetch_chunk.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p]

            lib.pogls_fetch_range.restype  = ctypes.c_int
            lib.pogls_fetch_range.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                               ctypes.c_uint64, ctypes.c_char_p]

            lib.pogls_verify_batch.restype  = ctypes.c_int
            lib.pogls_verify_batch.argtypes = [ctypes.c_void_p, ctypes.c_int]

            lib.geonet_state.restype        = ctypes.c_uint8
            lib.geonet_state.argtypes       = [ctypes.c_void_p]
            lib.geonet_op_count.restype     = ctypes.c_uint32
            lib.geonet_op_count.argtypes    = [ctypes.c_void_p]
            lib.geonet_mirror_mask.restype  = ctypes.c_uint8
            lib.geonet_mirror_mask.argtypes = [ctypes.c_void_p, ctypes.c_uint8]

            # S29: iter_chunks — wire if available (graceful if old .so)
            try:
                lib.pogls_iter_chunks.restype  = ctypes.c_int
                lib.pogls_iter_chunks.argtypes = [
                    ctypes.c_void_p,    # handle
                    ctypes.c_uint64,    # off
                    ctypes.c_uint64,    # n
                    ctypes.c_char_p,    # layer_filter (NULL ok)
                    _ChunkCbType,       # callback
                    ctypes.c_void_p,    # userdata
                ]
                lib._has_iter = True
            except AttributeError:
                lib._has_iter = False
                log.warning("pogls_iter_chunks not found in .so — rebuild with geo_net_so_s29.c")

            log.info("libgeonet.so loaded: %s (iter=%s)", path,
                     getattr(lib, "_has_iter", False))
            return lib
        except OSError:
            continue
    return None


_LIB: Optional[ctypes.CDLL] = _load_lib()
_STATE_NAME = {0: "NORMAL", 1: "STRESSED", 2: "ANOMALY"}
CYL_FULL_N  = 3456


# ── GeoNetCtxFast ──────────────────────────────────────────────────

class GeoNetCtxFast:
    """
    S29 additions over S28:
      .iter_chunks_stream(file_idx, n_bytes, *, filter_fn=None)
          → Generator[dict]  — O(1) memory, NDJSON-ready

    All S28 methods unchanged.
    """

    def __init__(self):
        if _LIB is not None:
            self._h       = _LIB.geonet_open()
            if not self._h:
                raise RuntimeError("geonet_open() returned NULL")
            self._lib     = _LIB
            self._fast    = True
            self._buf64   = ctypes.create_string_buffer(64)
        else:
            from geo_net_py import GeoNetCtx as _PyCtx
            self._py   = _PyCtx()
            self._fast = False
            log.warning("libgeonet.so not found — Python fallback")

    def __del__(self):
        if getattr(self, "_fast", False) and self._h:
            self._lib.geonet_close(self._h)
            self._h = None

    # ── S29: streaming iterator ────────────────────────────────────

    def iter_chunks_stream(
        self,
        file_idx:  int,
        n_bytes:   int,
        chunk_size: int = 64,
        filter_fn: Optional[Callable[[dict], bool]] = None,
    ) -> Iterator[dict]:
        """
        Stream chunk annotations one at a time — O(1) memory.

        Each yielded dict (NDJSON line):
          { chunk_i, addr, spoke, inv_spoke, mirror_mask, is_audit }

        filter_fn(rec) → bool: if provided, only yield matching records.
        (S30: push filter into C for further speedup.)

        Usage:
            for rec in ctx.iter_chunks_stream(file_idx, arr.nbytes):
                yield (json.dumps(rec) + "\\n").encode()
        """
        n_chunks = max(1, (n_bytes + chunk_size - 1) // chunk_size)
        off      = file_idx * CYL_FULL_N

        if not self._fast or not getattr(self._lib, "_has_iter", False):
            # Python fallback — still O(1) memory (yields immediately)
            for i in range(n_chunks):
                addr = off + i
                a    = self._py.route(addr)
                mm   = self._py.spoke_mirror_mask(a.spoke)
                rec  = {
                    "chunk_i":     i,
                    "addr":        addr,
                    "spoke":       a.spoke,
                    "inv_spoke":   a.inv_spoke,
                    "mirror_mask": mm,
                    "is_audit":    a.is_audit,
                }
                if filter_fn is None or filter_fn(rec):
                    yield rec
            return

        # ── C callback path ─────────────────────────────────────────
        # Collect into a small staging list so the generator can yield
        # after each C callback fires.
        # Pattern: callback appends to _buf, generator drains after each call.
        #
        # Note: ctypes callbacks run on the same thread — no threading needed.
        # The staging list acts as a 1-slot queue between C and Python.

        _stage: list[dict] = []

        def _cb(chunk_i, addr, spoke, inv_spoke, mirror_mask, is_audit, _ud):
            rec = {
                "chunk_i":     int(chunk_i),
                "addr":        int(addr),
                "spoke":       int(spoke),
                "inv_spoke":   int(inv_spoke),
                "mirror_mask": int(mirror_mask),
                "is_audit":    bool(is_audit),
            }
            _stage.append(rec)

        cb_fn = _ChunkCbType(_cb)

        # We can't interleave yield and C loop directly (C drives the loop).
        # Solution: run C loop collecting into _stage, drain after.
        # For true streaming: batch_size controls RAM peak = batch_size * ~200B.
        BATCH = 512   # tunable — 512 * 200B ≈ 100KB peak, << layer MB
        i = 0
        while i < n_chunks:
            batch = min(BATCH, n_chunks - i)
            _stage.clear()
            rc = self._lib.pogls_iter_chunks(
                self._h,
                ctypes.c_uint64(off + i),
                ctypes.c_uint64(batch),
                None,    # layer_filter — S30
                cb_fn,
                None,    # userdata
            )
            if rc < 0:
                raise RuntimeError(f"pogls_iter_chunks failed at off={off+i}")
            for rec in _stage:
                if filter_fn is None or filter_fn(rec):
                    yield rec
            i += batch

    # ── S29: summary-only fast path (route_layer_fast) ─────────────
    # Unchanged from S28 — used by /wallet/load (no chunk_map needed)

    def route_layer_fast(self, file_idx: int, n_bytes: int,
                         chunk_size: int = 64) -> dict:
        n_chunks = max(1, (n_bytes + chunk_size - 1) // chunk_size)
        if not self._fast:
            spoke_dist = [0]*6; ac = cc = 0
            for i in range(n_chunks):
                a = self._py.route(file_idx * CYL_FULL_N + i)
                spoke_dist[a.spoke] += 1
                if a.is_audit:  ac += 1
                if a.is_center: cc += 1
            return {"n_chunks": n_chunks, "spoke_dist": spoke_dist,
                    "audit_points": ac, "center_chunks": cc,
                    "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
                    "qrpn_state_after": self._py.state, "verify_pass": n_chunks}

        off = file_idx * CYL_FULL_N
        buf = ctypes.create_string_buffer(n_chunks * 8)
        self._lib.pogls_fetch_range(
            self._h, ctypes.c_uint64(off), ctypes.c_uint64(n_chunks), buf)
        CArr   = (_CAddr * n_chunks)
        arr    = CArr.from_buffer(buf)
        pass_n = self._lib.pogls_verify_batch(arr, ctypes.c_int(n_chunks))
        raw    = buf.raw
        spoke_dist = [0]*6; ac = cc = 0
        for i in range(n_chunks):
            o = i * 8
            spoke_dist[raw[o]] += 1
            if raw[o+7]: ac += 1
            if raw[o+6]: cc += 1
        return {"n_chunks": n_chunks, "spoke_dist": spoke_dist,
                "audit_points": ac, "center_chunks": cc,
                "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
                "qrpn_state_after": self.state, "verify_pass": pass_n}

    # ── S28: route_layer_annotated_fast kept for compatibility ──────
    # (still used as fallback when iter export unavailable)

    def route_layer_annotated_fast(self, file_idx: int, n_bytes: int,
                                   chunk_size: int = 64) -> tuple[dict, list]:
        n_chunks = max(1, (n_bytes + chunk_size - 1) // chunk_size)
        if not self._fast:
            spoke_dist = [0]*6; chunk_map = []; ac = cc = 0
            for i in range(n_chunks):
                addr = file_idx * CYL_FULL_N + i
                a    = self._py.route(addr)
                mm   = self._py.spoke_mirror_mask(a.spoke)
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
        raw    = buf.raw
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

    # ── single route ───────────────────────────────────────────────

    def route(self, addr: int, slot_hot: int = 0) -> GeoNetAddr:
        if not self._fast:
            return self._py.route(addr, slot_hot)
        rc = self._lib.pogls_fetch_chunk(self._h, ctypes.c_uint32(addr), self._buf64)
        if rc != 0:
            raise RuntimeError(f"pogls_fetch_chunk failed: {rc}")
        b        = self._buf64.raw
        full_idx = addr % CYL_FULL_N
        slot     = full_idx // 6
        return GeoNetAddr(spoke=b[0], slot=slot, face=b[2], unit=b[3],
                          inv_spoke=b[1], group=b[4],
                          is_center=bool(b[6]), is_audit=bool(b[7]))

    def fetch_range(self, off: int, n: int) -> list[GeoNetAddr]:
        if not self._fast:
            return [self.route(off + i) for i in range(n)]
        buf = ctypes.create_string_buffer(n * 8)
        rc  = self._lib.pogls_fetch_range(
            self._h, ctypes.c_uint64(off), ctypes.c_uint64(n), buf)
        if rc < 0:
            raise RuntimeError(f"pogls_fetch_range failed: {rc}")
        raw = buf.raw; out = []
        for i in range(n):
            o        = i * 8; b = raw[o:o+8]
            full_idx = (off + i) % CYL_FULL_N
            slot     = full_idx // 6
            out.append(GeoNetAddr(spoke=b[0], slot=slot, face=b[2], unit=b[3],
                                  inv_spoke=b[1], group=b[4],
                                  is_center=bool(b[6]), is_audit=bool(b[7])))
        return out

    def verify_batch(self, recs: list[GeoNetAddr]) -> int:
        if not recs: return 0
        if not self._fast:
            return sum(1 for r in recs
                       if r.spoke < 6 and r.inv_spoke == (r.spoke+3)%6 and r.face < 9)
        n   = len(recs); Arr = (_CAddr * n); arr = Arr()
        for i, r in enumerate(recs):
            arr[i].spoke=r.spoke; arr[i].inv_spoke=r.inv_spoke; arr[i].face=r.face
            arr[i].unit=r.unit; arr[i].group=r.group; arr[i].mirror_mask=0
            arr[i].is_center=int(r.is_center); arr[i].is_audit=int(r.is_audit)
        return self._lib.pogls_verify_batch(arr, ctypes.c_int(n))

    # ── state queries ──────────────────────────────────────────────

    @property
    def state(self) -> str:
        if not self._fast: return self._py.state
        return _STATE_NAME.get(self._lib.geonet_state(self._h), "UNKNOWN")

    @property
    def op_count(self) -> int:
        if not self._fast: return self._py.op_count
        return int(self._lib.geonet_op_count(self._h))

    def spoke_mirror_mask(self, spoke: int) -> int:
        if not self._fast: return self._py.spoke_mirror_mask(spoke)
        return int(self._lib.geonet_mirror_mask(self._h, ctypes.c_uint8(spoke)))

    def status(self) -> dict:
        if not self._fast:
            s = self._py.status()
            s["iter_streaming"] = False
            s["backend"] = "Python/fallback"
            return s
        return {"ops": self.op_count, "qrpn_state": self.state,
                "backend": "C/libgeonet.so",
                "iter_streaming": getattr(self._lib, "_has_iter", False),
                "fast": True}

    @property
    def is_fast(self) -> bool:
        return self._fast


GeoNetCtx = GeoNetCtxFast
