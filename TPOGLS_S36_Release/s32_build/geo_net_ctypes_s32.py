"""
geo_net_ctypes_s32.py — S32 ctypes binding

New over S31:
  GeoNetFilter:
    audit_only → flags (uint8): bit0=GF_AUDIT_ONLY, bit1=GF_VAL_FILTER
    _pad[2]    → chunk_lo (uint16) + chunk_hi (uint16)
    sizeof = 10 bytes, unchanged.

  GeoMultiRec (36 bytes):
    layer_id, chunk_global, chunk_i, addr, offset, spoke, mirror_mask, is_audit

  iter_multi_range(reqs, filt_params) → Iterator[list[dict]]
    Yields batches (list of records) — one json.dumps() per batch in REST layer.

  S31 iter_chunks_stream: unchanged.
    val_max==0 sentinel preserved.
    audit_only kwarg maps to flags & GF_AUDIT_ONLY for S31 compat.

Filter semantics:
  S31 path (iter_chunks_stream):  val_max == 0         → range gate off
  S32 path (iter_multi_range):    GF_VAL_FILTER in flags → range gate on
                                  val_min > val_max    → gate disabled even if flag set

  chunk_hi = 0xFFFF (CHUNK_FILTER_OFF) → chunk window disabled (default)
  chunk_hi = 0                          → only chunk 0

All S29/S30/S31 methods unchanged.
"""

from __future__ import annotations
import ctypes
import os
import logging
from dataclasses import dataclass
from typing import Optional, Iterator, Callable

log = logging.getLogger("pogls.geonet_ctypes")

# ── constants ──────────────────────────────────────────────────────

CHUNK_SIZE       = 64
CHUNK_FILTER_OFF = 0xFFFF   # chunk_hi sentinel: no chunk window
CYL_FULL_N       = 3456

GF_AUDIT_ONLY = 0x01        # flags bit0
GF_VAL_FILTER = 0x02        # flags bit1 — S32 explicit opt-in

MULTI_BATCH_SZ = 256        # must match C MULTI_BATCH_SZ

# ── C structs ──────────────────────────────────────────────────────

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

# S32: GeoNetFilter — 10 bytes (audit_only → flags, _pad[2] → chunk_lo/hi)
class _CFilter(ctypes.Structure):
    _fields_ = [
        ("spoke_mask", ctypes.c_uint8),
        ("flags",      ctypes.c_uint8),    # S32: GF_AUDIT_ONLY | GF_VAL_FILTER
        ("val_min",    ctypes.c_uint16),
        ("val_max",    ctypes.c_uint16),
        ("chunk_lo",   ctypes.c_uint16),   # S32
        ("chunk_hi",   ctypes.c_uint16),   # S32: CHUNK_FILTER_OFF = disabled
    ]

# S32: GeoReq — per-layer request descriptor
class _CGeoReq(ctypes.Structure):
    _fields_ = [
        ("layer_id",  ctypes.c_uint32),
        ("file_idx",  ctypes.c_uint32),
        ("n_bytes",   ctypes.c_uint64),
    ]

# S32: GeoMultiRec — 36-byte packed output record
class _CGeoMultiRec(ctypes.Structure):
    _fields_ = [
        ("layer_id",     ctypes.c_uint32),
        ("chunk_global", ctypes.c_uint32),
        ("chunk_i",      ctypes.c_uint64),
        ("addr",         ctypes.c_uint64),
        ("offset",       ctypes.c_uint64),
        ("spoke",        ctypes.c_uint8),
        ("mirror_mask",  ctypes.c_uint8),
        ("is_audit",     ctypes.c_uint8),
        ("_pad",         ctypes.c_uint8),
    ]

_ChunkCbType = ctypes.CFUNCTYPE(
    None,
    ctypes.c_uint64, ctypes.c_uint64,
    ctypes.c_uint8,  ctypes.c_uint8,
    ctypes.c_uint8,  ctypes.c_uint8,
    ctypes.c_void_p,
)

# S32: batch callback — receives array pointer + count
_MultiCbBatchType = ctypes.CFUNCTYPE(
    None,
    ctypes.POINTER(_CGeoMultiRec),
    ctypes.c_size_t,
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
        os.path.join(os.path.dirname(__file__), "libgeonet_s32.so"),
        os.path.join(os.path.dirname(__file__), "libgeonet.so"),
        "/mnt/c/TPOGLS/libgeonet_s32.so",
        "/mnt/c/TPOGLS/libgeonet.so",
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

            lib.pogls_fetch_chunk.restype  = ctypes.c_int
            lib.pogls_fetch_chunk.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32,
                ctypes.c_char_p, ctypes.c_uint8,
            ]
            lib.pogls_fetch_range.restype  = ctypes.c_int
            lib.pogls_fetch_range.argtypes = [
                ctypes.c_void_p, ctypes.c_uint64,
                ctypes.c_uint64, ctypes.c_char_p,
            ]
            lib.pogls_fetch_range_hot.restype  = ctypes.c_int
            lib.pogls_fetch_range_hot.argtypes = [
                ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
                ctypes.c_char_p, ctypes.c_char_p,
            ]
            lib.pogls_verify_batch.restype  = ctypes.c_int
            lib.pogls_verify_batch.argtypes = [ctypes.c_void_p, ctypes.c_int]

            lib.pogls_iter_chunks.restype  = ctypes.c_int
            lib.pogls_iter_chunks.argtypes = [
                ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
                ctypes.POINTER(_CFilter),
                _ChunkCbType,
                ctypes.c_void_p,
            ]
            lib._has_iter = True

            # S32: multi-range batch API
            lib.pogls_fetch_multi_range.restype  = ctypes.c_int
            lib.pogls_fetch_multi_range.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(_CGeoReq),
                ctypes.c_int,
                ctypes.POINTER(_CFilter),
                _MultiCbBatchType,
                ctypes.c_void_p,
            ]
            lib._has_multi_range = True

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

            log.info("libgeonet.so loaded: %s (S32 full)", path)
            return lib
        except OSError:
            continue
    return None


_LIB: Optional[ctypes.CDLL] = _load_lib()
_STATE_NAME = {0: "NORMAL", 1: "STRESSED", 2: "ANOMALY"}


# ── helper: build _CFilter from keyword params ──────────────────────

def _make_filter(
    spoke_mask: int  = 0x3F,
    audit_only: bool = False,
    val_min:    int  = 0,
    val_max:    int  = 0,          # S31 path: 0=disabled
    chunk_lo:   int  = 0,
    chunk_hi:   int  = CHUNK_FILTER_OFF,
    # S32 explicit val gate (GF_VAL_FILTER)
    val_filter: bool = False,
) -> _CFilter:
    filt           = _CFilter()
    filt.spoke_mask = spoke_mask & 0x3F
    filt.flags      = (GF_AUDIT_ONLY if audit_only else 0) | \
                      (GF_VAL_FILTER  if val_filter  else 0)
    filt.val_min    = val_min  & 0xFFFF
    filt.val_max    = val_max  & 0xFFFF
    filt.chunk_lo   = chunk_lo & 0xFFFF
    filt.chunk_hi   = chunk_hi & 0xFFFF
    return filt


class GeoNetCtxFast:
    """
    S32 additions:
      iter_multi_range(reqs, ...)  — multi-layer batch stream via C
        yields list[dict] per MULTI_BATCH_SZ records (batch JSON path)
        GeoMultiRec fields: layer_id, layer_name, chunk_global, chunk_i,
                            addr, offset, spoke, mirror_mask, is_audit

      iter_chunks_stream: unchanged from S31.
        val_max==0 → S31 sentinel (no range gate).
        audit_only kwarg → maps to flags GF_AUDIT_ONLY.

    All S29/S30/S31 methods unchanged.
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

    # ── S30: signal_fail ───────────────────────────────────────────

    def signal_fail(self) -> str:
        if not self._fast:
            from geo_net_py import QRPN_ANOMALY_HOT, QRPN_ANOMALY
            self._py_anomaly_signals = getattr(self, "_py_anomaly_signals", 0) + 1
            self._py._te._cur["hot_slots"] += 1
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

    # ── S30: route ─────────────────────────────────────────────────

    def route(self, addr: int, slot_hot: int = 0) -> GeoNetAddr:
        if not self._fast:
            return self._py.route(addr, slot_hot)
        rc = self._lib.pogls_fetch_chunk(
            self._h, ctypes.c_uint32(addr),
            self._buf64, ctypes.c_uint8(slot_hot)
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

    # ── S30: fetch_range_hot ───────────────────────────────────────

    def fetch_range_hot(self, off: int, n: int,
                        hot: list[int] | bytes) -> list[GeoNetAddr]:
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

    # ── S31: iter_chunks_stream — unchanged ────────────────────────
    # val_max==0 sentinel preserved for S31 backward compat.

    def iter_chunks_stream(
        self,
        file_idx:   int,
        n_bytes:    int,
        chunk_size: int  = 64,
        spoke_mask: int  = 0x3F,
        audit_only: bool = False,
        val_min:    int  = 0,
        val_max:    int  = 0,          # 0 = disabled (S31 sentinel)
        filter_fn:  Optional[Callable[[dict], bool]] = None,
    ) -> Iterator[dict]:
        n_chunks = max(1, (n_bytes + chunk_size - 1) // chunk_size)
        off      = file_idx * CYL_FULL_N

        if not self._fast:
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

        # S31 C path — flags bit0 = audit_only, val_max==0 sentinel
        filt = _make_filter(
            spoke_mask=spoke_mask,
            audit_only=audit_only,
            val_min=val_min,
            val_max=val_max,
            # S31: no chunk window, no GF_VAL_FILTER flag
            chunk_hi=CHUNK_FILTER_OFF,
            val_filter=False,
        )
        filt_ptr = ctypes.byref(filt)
        BATCH    = 512
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

    # ── S32: iter_multi_range — multi-layer batch stream ──────────
    #
    # reqs: list of dicts with keys:
    #   name     (str)  — layer name (echoed in output, not sent to C)
    #   file_idx (int)  — wallet position
    #   n_bytes  (int)  — layer byte size
    #   layer_id (int, optional) — defaults to position index
    #
    # Filter params (S32 semantics):
    #   val_filter=True + val_min/val_max → range gate (GF_VAL_FILTER)
    #   val_min > val_max                 → gate disabled
    #   chunk_lo/chunk_hi                 → chunk window per ALL layers
    #   chunk_hi=CHUNK_FILTER_OFF         → no chunk window (default)
    #
    # Yields: list[dict] — batch of MULTI_BATCH_SZ records
    #   {"layer_id", "layer_name", "chunk_global", "chunk_i",
    #    "addr", "offset", "spoke", "mirror_mask", "is_audit"}
    #
    # Python fallback: same output, Python-side filtering.

    def iter_multi_range(
        self,
        reqs:        list[dict],
        spoke_mask:  int  = 0x3F,
        audit_only:  bool = False,
        val_min:     int  = 1,          # S32 default: 1 > 0 = disabled
        val_max:     int  = 0,          # S32: val_min > val_max → off
        val_filter:  bool = False,      # set True to enable val gate
        chunk_lo:    int  = 0,
        chunk_hi:    int  = CHUNK_FILTER_OFF,
        filter_fn:   Optional[Callable[[dict], bool]] = None,
    ) -> Iterator[list[dict]]:
        # normalise layer_id
        _reqs = []
        _id_to_name: dict[int, str] = {}
        for idx, r in enumerate(reqs):
            lid = int(r.get("layer_id", idx))
            _reqs.append({
                "layer_id": lid,
                "file_idx": int(r["file_idx"]),
                "n_bytes":  int(r["n_bytes"]),
            })
            _id_to_name[lid] = str(r.get("name", f"layer_{lid}"))

        if not self._fast:
            yield from self._iter_multi_range_py(
                _reqs, _id_to_name,
                spoke_mask, audit_only,
                val_min, val_max, val_filter,
                chunk_lo, chunk_hi, filter_fn,
            )
            return

        # ── C batch path ───────────────────────────────────────────
        filt = _make_filter(
            spoke_mask=spoke_mask,
            audit_only=audit_only,
            val_min=val_min,
            val_max=val_max,
            val_filter=val_filter,
            chunk_lo=chunk_lo,
            chunk_hi=chunk_hi,
        )
        filt_ptr = ctypes.byref(filt)

        # build C GeoReq array
        n = len(_reqs)
        ReqArr  = _CGeoReq * n
        req_arr = ReqArr(*[
            _CGeoReq(
                layer_id=r["layer_id"],
                file_idx=r["file_idx"],
                n_bytes=r["n_bytes"],
            ) for r in _reqs
        ])

        # accumulate batches in Python list, yield when full or flush
        _batch: list[dict] = []

        def _cb_batch(recs_ptr, count, _ud):
            for i in range(count):
                r   = recs_ptr[i]
                lid = int(r.layer_id)
                rec = {
                    "layer_id":     lid,
                    "layer_name":   _id_to_name.get(lid, f"layer_{lid}"),
                    "chunk_global": int(r.chunk_global),
                    "chunk_i":      int(r.chunk_i),
                    "addr":         int(r.addr),
                    "offset":       int(r.offset),
                    "spoke":        int(r.spoke),
                    "mirror_mask":  int(r.mirror_mask),
                    "is_audit":     bool(r.is_audit),
                }
                if filter_fn is None or filter_fn(rec):
                    _batch.append(rec)

        cb_fn = _MultiCbBatchType(_cb_batch)

        rc = self._lib.pogls_fetch_multi_range(
            self._h,
            req_arr,
            ctypes.c_int(n),
            filt_ptr,
            cb_fn,
            None,
        )
        if rc < 0:
            raise RuntimeError(f"pogls_fetch_multi_range failed: {rc}")

        # yield whatever accumulated (C already flushed in batches via callback,
        # _batch holds all; split into MULTI_BATCH_SZ for consistent yield size)
        for i in range(0, len(_batch), MULTI_BATCH_SZ):
            yield _batch[i:i + MULTI_BATCH_SZ]

    def _iter_multi_range_py(
        self, reqs, id_to_name,
        spoke_mask, audit_only,
        val_min, val_max, val_filter,
        chunk_lo, chunk_hi, filter_fn,
    ) -> Iterator[list[dict]]:
        """Python fallback — same semantics as C path."""
        _smask      = spoke_mask if spoke_mask != 0 else 0x3F
        _chunk_gate = (chunk_hi != CHUNK_FILTER_OFF) or (chunk_lo > 0)
        # S32 val rule
        _val_gate   = val_filter and (val_min <= val_max)
        chunk_glob  = 0
        batch: list[dict] = []

        for r in reqs:
            lid      = r["layer_id"]
            n_chunks = max(1, (r["n_bytes"] + 63) // 64)
            off      = r["file_idx"] * CYL_FULL_N

            for i in range(n_chunks):
                if _chunk_gate and not (chunk_lo <= i <= chunk_hi):
                    continue
                addr = off + i
                a    = self._py.route(addr)
                if not ((_smask >> a.spoke) & 1): continue
                if audit_only and not a.is_audit:     continue
                if _val_gate:
                    v = addr & 0xFFFF
                    if v < val_min or v > val_max:    continue

                rec = {
                    "layer_id":     lid,
                    "layer_name":   id_to_name.get(lid, f"layer_{lid}"),
                    "chunk_global": chunk_glob,
                    "chunk_i":      i,
                    "addr":         addr,
                    "offset":       i * CHUNK_SIZE,
                    "spoke":        a.spoke,
                    "mirror_mask":  self._py.spoke_mirror_mask(a.spoke),
                    "is_audit":     a.is_audit,
                }
                if filter_fn is None or filter_fn(rec):
                    batch.append(rec)
                    chunk_glob += 1
                    if len(batch) == MULTI_BATCH_SZ:
                        yield batch
                        batch = []

        if batch:
            yield batch

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
            s["iter_streaming"]   = False
            s["multi_range"]      = False
            s["backend"]          = "Python/fallback"
            s["anomaly_signals"]  = getattr(self, "_py_anomaly_signals", 0)
            s["filter_pushdown"]  = False
            s["slot_hot_wired"]   = False
            return s
        return {
            "ops":              self.op_count,
            "qrpn_state":       self.state,
            "anomaly_signals":  self.anomaly_signals,
            "backend":          "C/libgeonet.so",
            "iter_streaming":   getattr(self._lib, "_has_iter",       False),
            "multi_range":      getattr(self._lib, "_has_multi_range", False),
            "filter_pushdown":  True,
            "slot_hot_wired":   True,
            "fast":             True,
        }

    @property
    def is_fast(self) -> bool:
        return self._fast


GeoNetCtx = GeoNetCtxFast
