"""
geo_net_ctypes_s30.py — S30 ctypes binding

New over S29:
  slot_hot wired:
    route(addr, slot_hot=1)          → fe_tick gets real hot pressure
    fetch_range_hot(off, n, hot[])   → per-chunk slot_hot array → batch C loop

  Filter pushdown:
    iter_chunks_stream(..., spoke_mask=0x3F, audit_only=False)
    → filter runs in C before callback → Python never sees rejected chunks
    → spoke_mask=0x01 (spoke 0 only): ~83% data reduction
    → audit_only=True: ~87% data reduction (1/8 chunks pass)

  QRPN feedback:
    signal_fail()   → geo_net_signal_fail in C → ThirdEye hot_slots++
    .anomaly_signals property

All S29 methods unchanged.
"""

from __future__ import annotations
import ctypes
import os
import logging
from dataclasses import dataclass
from typing import Optional, Iterator, Callable

log = logging.getLogger("pogls.geonet_ctypes")

# ── packed structs ─────────────────────────────────────────────────

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

# S30: GeoNetFilter — mirrors C struct
class _CFilter(ctypes.Structure):
    _fields_ = [
        ("spoke_mask", ctypes.c_uint8),
        ("audit_only", ctypes.c_uint8),
        ("val_min",    ctypes.c_uint16),   # S31
        ("val_max",    ctypes.c_uint16),   # S31: 0=disabled
        ("_pad",       ctypes.c_uint8 * 2),
    ]

_ChunkCbType = ctypes.CFUNCTYPE(
    None,
    ctypes.c_uint64, ctypes.c_uint64,
    ctypes.c_uint8,  ctypes.c_uint8,
    ctypes.c_uint8,  ctypes.c_uint8,
    ctypes.c_void_p,
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

            lib.geonet_open.restype   = ctypes.c_void_p
            lib.geonet_open.argtypes  = []
            lib.geonet_close.restype  = None
            lib.geonet_close.argtypes = [ctypes.c_void_p]

            # S30: fetch_chunk gains slot_hot param
            lib.pogls_fetch_chunk.restype  = ctypes.c_int
            lib.pogls_fetch_chunk.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32,
                ctypes.c_char_p, ctypes.c_uint8,   # + slot_hot
            ]

            # unchanged
            lib.pogls_fetch_range.restype  = ctypes.c_int
            lib.pogls_fetch_range.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                               ctypes.c_uint64, ctypes.c_char_p]

            # S30: per-chunk hot array
            lib.pogls_fetch_range_hot.restype  = ctypes.c_int
            lib.pogls_fetch_range_hot.argtypes = [
                ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
                ctypes.c_char_p,   # hot[] uint8*
                ctypes.c_char_p,   # out uint8*
            ]

            lib.pogls_verify_batch.restype  = ctypes.c_int
            lib.pogls_verify_batch.argtypes = [ctypes.c_void_p, ctypes.c_int]

            # S30: filter pushdown — GeoNetFilter* (NULL ok)
            lib.pogls_iter_chunks.restype  = ctypes.c_int
            lib.pogls_iter_chunks.argtypes = [
                ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
                ctypes.POINTER(_CFilter),   # filt (NULL = no filter)
                _ChunkCbType,
                ctypes.c_void_p,
            ]
            lib._has_iter = True

            # S30: QRPN feedback
            lib.pogls_signal_fail.restype  = ctypes.c_uint8
            lib.pogls_signal_fail.argtypes = [ctypes.c_void_p]

            lib.geonet_anomaly_signals.restype  = ctypes.c_uint32
            lib.geonet_anomaly_signals.argtypes = [ctypes.c_void_p]

            lib.geonet_state.restype        = ctypes.c_uint8
            lib.geonet_state.argtypes       = [ctypes.c_void_p]
            lib.geonet_op_count.restype     = ctypes.c_uint32
            lib.geonet_op_count.argtypes    = [ctypes.c_void_p]
            lib.geonet_mirror_mask.restype  = ctypes.c_uint8
            lib.geonet_mirror_mask.argtypes = [ctypes.c_void_p, ctypes.c_uint8]

            log.info("libgeonet.so loaded: %s (S30 full)", path)
            return lib
        except OSError:
            continue
    return None


_LIB: Optional[ctypes.CDLL] = _load_lib()
_STATE_NAME = {0: "NORMAL", 1: "STRESSED", 2: "ANOMALY"}
CYL_FULL_N  = 3456


class GeoNetCtxFast:
    """
    S30 additions:
      route(addr, slot_hot=1)              — slot_hot wired to C te_tick
      fetch_range_hot(off, n, hot[])       — per-chunk slot_hot batch
      iter_chunks_stream(..., spoke_mask,
                          audit_only)      — filter pushdown in C
      signal_fail()                        — QRPN feedback → force_anomaly
      .anomaly_signals                     — property
    """

    def __init__(self):
        if _LIB is not None:
            self._h     = _LIB.geonet_open()
            if not self._h:
                raise RuntimeError("geonet_open() returned NULL")
            self._lib   = _LIB
            self._fast  = True
            self._buf64 = ctypes.create_string_buffer(64)
        else:
            from geo_net_py import GeoNetCtx as _PyCtx
            self._py   = _PyCtx()
            self._fast = False
            log.warning("libgeonet.so not found — Python fallback")

    def __del__(self):
        if getattr(self, "_fast", False) and self._h:
            self._lib.geonet_close(self._h)
            self._h = None

    # ── S30: signal_fail (QRPN feedback) ──────────────────────────

    def signal_fail(self) -> str:
        """
        Signal a QRPN verify failure to ThirdEye.
        Increments hot_slots in C → may trigger STRESSED/ANOMALY transition.
        Returns new state string.
        """
        if not self._fast:
            from geo_net_py import QRPN_ANOMALY_HOT, QRPN_ANOMALY
            self._py_anomaly_signals = getattr(self, "_py_anomaly_signals", 0) + 1
            self._py._te._cur["hot_slots"] += 1
            # mirror C geo_net_signal_fail: bypass cycle, force state directly
            if self._py._te._cur["hot_slots"] > QRPN_ANOMALY_HOT:
                self._py._te.qrpn_state = QRPN_ANOMALY
            return self._py.state
        new_state = int(self._lib.pogls_signal_fail(self._h))
        return _STATE_NAME.get(new_state, "UNKNOWN")

    @property
    def anomaly_signals(self) -> int:
        if not self._fast:
            return getattr(self, "_py_anomaly_signals", 0)
        return int(self._lib.geonet_anomaly_signals(self._h))

    # ── S30: route with slot_hot wired ────────────────────────────

    def route(self, addr: int, slot_hot: int = 0) -> GeoNetAddr:
        if not self._fast:
            return self._py.route(addr, slot_hot)
        rc = self._lib.pogls_fetch_chunk(
            self._h, ctypes.c_uint32(addr),
            self._buf64, ctypes.c_uint8(slot_hot)   # S30: wired
        )
        if rc != 0:
            raise RuntimeError(f"pogls_fetch_chunk failed: {rc}")
        b        = self._buf64.raw
        full_idx = addr % CYL_FULL_N
        return GeoNetAddr(
            spoke=b[0], slot=full_idx // 6, face=b[2], unit=b[3],
            inv_spoke=b[1], group=b[4],
            is_center=bool(b[6]), is_audit=bool(b[7])
        )

    # ── S30: fetch_range_hot — per-chunk slot_hot ─────────────────

    def fetch_range_hot(self, off: int, n: int,
                        hot: list[int] | bytes) -> list[GeoNetAddr]:
        """
        Batch route with per-chunk slot_hot.
        hot: list/bytes of length n, each 0 or 1.
        ThirdEye state advances with real hot pressure per chunk.
        """
        if not self._fast:
            return [self.route(off + i, int(hot[i])) for i in range(n)]

        hot_buf = ctypes.create_string_buffer(bytes(hot) if not isinstance(hot, bytes) else hot)
        out_buf = ctypes.create_string_buffer(n * 8)
        rc = self._lib.pogls_fetch_range_hot(
            self._h, ctypes.c_uint64(off), ctypes.c_uint64(n),
            hot_buf, out_buf
        )
        if rc < 0:
            raise RuntimeError(f"pogls_fetch_range_hot failed: {rc}")
        raw = out_buf.raw; result = []
        for i in range(n):
            o = i * 8; b = raw[o:o+8]
            result.append(GeoNetAddr(
                spoke=b[0], slot=(off+i) % CYL_FULL_N // 6,
                face=b[2], unit=b[3], inv_spoke=b[1], group=b[4],
                is_center=bool(b[6]), is_audit=bool(b[7])
            ))
        return result

    # ── S30: iter_chunks_stream — filter pushdown ─────────────────

    def iter_chunks_stream(
        self,
        file_idx:   int,
        n_bytes:    int,
        chunk_size: int = 64,
        # S30 filter params (pushed to C)
        spoke_mask: int = 0x3F,
        audit_only: bool = False,
        # S31: value-range filter (addr & 0xFFFF in [val_min, val_max])
        val_min:    int = 0,
        val_max:    int = 0,       # 0 = disabled
        # Python-side post-filter (for complex predicates)
        filter_fn:  Optional[Callable[[dict], bool]] = None,
    ) -> Iterator[dict]:
        """
        Stream chunk annotations. O(1) memory.

        S30 filter pushdown (runs in C — Python never sees rejected chunks):
          spoke_mask=0x01   → only spoke 0   (~83% reduction)
          spoke_mask=0x09   → spokes 0+3     (~67% reduction)
          audit_only=True   → ~87% reduction (1-in-8 chunks)

        filter_fn: additional Python-side predicate (for complex logic).
        chunk_i in output = filtered index (0 = first passing chunk).
        """
        n_chunks = max(1, (n_bytes + chunk_size - 1) // chunk_size)
        off      = file_idx * CYL_FULL_N

        if not self._fast:
            # Python fallback with inline filter
            _smask = spoke_mask if spoke_mask != 0 else 0x3F
            _vrange = val_max > 0
            emit_i = 0
            for i in range(n_chunks):
                addr = off + i
                a    = self._py.route(addr)
                if not ((_smask >> a.spoke) & 1): continue
                if audit_only and not a.is_audit:     continue
                if _vrange:
                    v = addr & 0xFFFF
                    if v < val_min or v > val_max: continue
                mm  = self._py.spoke_mirror_mask(a.spoke)
                rec = {
                    "chunk_i":     emit_i,
                    "addr":        addr,
                    "spoke":       a.spoke,
                    "inv_spoke":   a.inv_spoke,
                    "mirror_mask": mm,
                    "is_audit":    a.is_audit,
                }
                if filter_fn is None or filter_fn(rec):
                    yield rec
                    emit_i += 1
            return

        # ── C callback path with filter struct ─────────────────────
        filt          = _CFilter()
        filt.spoke_mask = spoke_mask & 0x3F
        filt.audit_only = 1 if audit_only else 0
        filt.val_min    = val_min & 0xFFFF   # S31
        filt.val_max    = val_max & 0xFFFF   # S31
        filt_ptr = ctypes.byref(filt)

        BATCH   = 512
        _stage: list[dict] = []

        def _cb(chunk_i, addr, spoke, inv_spoke, mirror_mask, is_audit, _ud):
            _stage.append({
                "chunk_i":     int(chunk_i),
                "addr":        int(addr),
                "spoke":       int(spoke),
                "inv_spoke":   int(inv_spoke),
                "mirror_mask": int(mirror_mask),
                "is_audit":    bool(is_audit),
            })

        cb_fn = _ChunkCbType(_cb)

        i = 0
        while i < n_chunks:
            batch = min(BATCH, n_chunks - i)
            _stage.clear()
            rc = self._lib.pogls_iter_chunks(
                self._h,
                ctypes.c_uint64(off + i),
                ctypes.c_uint64(batch),
                filt_ptr,
                cb_fn,
                None,
            )
            if rc < 0:
                raise RuntimeError(f"pogls_iter_chunks failed at off={off+i}")
            for rec in _stage:
                if filter_fn is None or filter_fn(rec):
                    yield rec
            i += batch

    # ── route_layer_fast (unchanged) ──────────────────────────────

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

    # ── route_layer_annotated_fast (unchanged) ─────────────────────

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

    # ── fetch_range (unchanged) ────────────────────────────────────

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
            o = i * 8; b = raw[o:o+8]
            out.append(GeoNetAddr(
                spoke=b[0], slot=(off+i) % CYL_FULL_N // 6,
                face=b[2], unit=b[3], inv_spoke=b[1], group=b[4],
                is_center=bool(b[6]), is_audit=bool(b[7])
            ))
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
            s["anomaly_signals"] = getattr(self, "_py_anomaly_signals", 0)
            s["filter_pushdown"] = False
            s["slot_hot_wired"] = False
            return s
        return {
            "ops":              self.op_count,
            "qrpn_state":       self.state,
            "anomaly_signals":  self.anomaly_signals,
            "backend":          "C/libgeonet.so",
            "iter_streaming":   getattr(self._lib, "_has_iter", False),
            "filter_pushdown":  True,
            "slot_hot_wired":   True,
            "fast":             True,
        }

    @property
    def is_fast(self) -> bool:
        return self._fast


GeoNetCtx = GeoNetCtxFast
