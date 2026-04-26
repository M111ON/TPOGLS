"""
rest_server_s32.py — POGLS REST API (S32)
══════════════════════════════════════════
Builds on S31.

S32 changes:
  • POST /wallet/reconstruct
      multi-layer coord streaming via persistent mount handle.
      body: handle_id, layers[{name, chunk_lo, chunk_hi}],
            spoke_mask, audit_only, val_min, val_max, val_filter, annotate.
      → StreamingResponse NDJSON (batch-encoded: 256 recs per JSON line batch)
      headers: X-Layer-Count, X-Chunk-Window, X-Val-Range, X-Filter-Flags,
               X-Multi-Range (signals S32 path)

  • GeoNetFilter update (transparent):
      audit_only → flags byte (bit0=GF_AUDIT_ONLY, bit1=GF_VAL_FILTER)
      _pad[2]    → chunk_lo/chunk_hi uint16
      S31 val_max==0 sentinel preserved on S31 endpoints.

  • /health version: S32, multi_range_stream: true

All S31 endpoints unchanged.
"""

from __future__ import annotations
import base64, json, logging, os, time, struct, gzip, io
from contextlib import asynccontextmanager
from typing import Any, Optional, Iterator

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel

# S51: bridge layer
try:
    from pogls_bridge import B62Middleware, make_bridge_endpoints, _b62enc as _b62enc_fn, _b62dec as _b62dec_fn, _is_b62 as _is_b62_fn
    _HAS_BRIDGE = True
except ImportError:
    _HAS_BRIDGE = False

# ── geo backend ────────────────────────────────────────────────────

try:
    from geo_net_ctypes_s32 import GeoNetCtxFast as GeoNetCtx, _LIB as _GEONET_LIB
    from geo_net_ctypes_s32 import CHUNK_FILTER_OFF
    _HAS_GEONET  = True
    _GEO_BACKEND = "C/libgeonet.so" if (_GEONET_LIB is not None) else "Python/fallback"
    _HAS_ITER    = getattr(_GEONET_LIB, "_has_iter",       False) if _GEONET_LIB else False
    _HAS_MULTI   = getattr(_GEONET_LIB, "_has_multi_range", False) if _GEONET_LIB else False
except ImportError:
    try:
        from geo_net_ctypes_s31 import GeoNetCtxFast as GeoNetCtx   # type: ignore
        from geo_net_ctypes_s31 import _LIB as _GEONET_LIB           # type: ignore
        CHUNK_FILTER_OFF = 0xFFFF
        _HAS_GEONET  = True
        _GEO_BACKEND = "C/libgeonet.so" if _GEONET_LIB else "Python/fallback"
        _HAS_ITER    = getattr(_GEONET_LIB, "_has_iter", False) if _GEONET_LIB else False
        _HAS_MULTI   = False
    except ImportError:
        try:
            from geo_net_py import GeoNetCtx                          # type: ignore
            _HAS_GEONET = True; _GEO_BACKEND = "Python/fallback"
            _GEONET_LIB = None; _HAS_ITER = False; _HAS_MULTI = False
            CHUNK_FILTER_OFF = 0xFFFF
        except ImportError:
            _HAS_GEONET = False; _GEO_BACKEND = "unavailable"
            _GEONET_LIB = None; _HAS_ITER = False; _HAS_MULTI = False
            CHUNK_FILTER_OFF = 0xFFFF

try:
    from pogls_engine import PoglsEngine, BLOCK_SLOTS
    _HAS_ENGINE = True
except ImportError:
    _HAS_ENGINE = False; BLOCK_SLOTS = 576
    class PoglsEngine:                                      # type: ignore[no-redef]
        def __enter__(self): return self
        def __exit__(self, *_): pass
        def encode(self, _): pass
        def decode(self): return b""
        def stats(self): return {}
        def stats_out(self):
            return {k: 0 for k in ("kv_load_pct_x100","kv_tomb_pct_x100",
                    "gpu_ring_pending","gpu_ring_flushed","l1_hit_pct_x100","ctx_qrpns")}

try:
    from pogls_mount         import VirtualFile
    from pogls_wallet_py     import WalletBuilder, WalletReader, WALLET_MODE_BUFFER, \
                                    DIAMOND_BLOCK_SIZE, chunk_seed, chunk_checksum
    from pogls_weight_stream import WeightStream, WeightStreamLoader
    _HAS_WALLET = True
except ImportError as _we:
    _HAS_WALLET = False
    logging.warning("wallet stack not found — /wallet/* disabled (%s)", _we)

try:
    from llm_advisor import LLMAdvisor, Act
    _HAS_ADVISOR = True
except ImportError:
    _HAS_ADVISOR = False

log = logging.getLogger("pogls.server")
logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)-8s %(name)s — %(message)s")

# ── P4 route layer (S41b) ──────────────────────────────────────────
# Dispatch chain (best → fallback):
#   1. libgeo_p4.so → geo_p4_run()  (real CUDA kernel, n≥10K → T4)
#   2. numpy         → pure vectorised Python (no .so needed)
#
# NOTE: geo_p4_bridge.h is header-only (static inline) — not exported
#       from .so.  We call geo_p4_ctx_init / geo_p4_run directly.
#
# Backend tag: "cuda" (n≥thresh, gpu_ok=1) | "so_cpu" (n<thresh) | "numpy"

_HAS_P4        = False
_p4_batcher    = None   # c_void_p handle (real GeoP4Batcher) or None
_P4_FLUSH_N      = int(os.environ.get("P4_FLUSH_N",      10_000))
_P4_GPU_THRESH   = int(os.environ.get("P4_GPU_THRESH",   10_000))
_P4_LIB_PATH     = os.environ.get("P4_LIB", "./libgeo_p4.so")
_P4_CAP          = int(os.environ.get("P4_CAP", 1 << 20))
# S43: streaming NDJSON threshold + chunk size
_P4_STREAM_THRESH = int(os.environ.get("P4_STREAM_THRESH", 50_000))   # n > this → stream
_P4_STREAM_CHUNK  = int(os.environ.get("P4_STREAM_CHUNK",  10_000))   # sigs per NDJSON line
# S42: push/flush counters (read from C batcher or fallback)
_p4_stat_pushed  = 0
_p4_stat_flushed = 0

# ── geo constants (mirror geo_cuda_p4.cu) ──────────────────────────
_P4_FULL_N     = 3456
_P4_SPOKES     = 6
_P4_MOD6_MAGIC = 10923

import bisect as _bisect_mod  # S49: range query support


# ── S46-S49: sig_index imports/globals (hoisted before try block) ──
import collections as _collections
import json as _json_mod
import os as _os_mod
import time as _time_mod
import threading as _threading_mod
import atexit as _atexit_mod

_P4_SIG_INDEX_CAP      = int(_os_mod.environ.get("P4_SIG_INDEX_CAP", 0))       # 0 = unlimited
_P4_SIG_INDEX_DUMP_DIR = _os_mod.environ.get("P4_SIG_INDEX_DUMP_DIR", "/tmp")
_P4_SIG_INDEX_TTL      = float(_os_mod.environ.get("P4_SIG_INDEX_TTL", 0))     # seconds, 0 = no TTL
_P4_SIG_INDEX_AUTO_DUMP = _os_mod.environ.get("P4_SIG_INDEX_AUTO_DUMP", "")    # path, "" = disabled

_p4_sig_index: "_collections.OrderedDict[int, list[int]]" = _collections.OrderedDict()
_p4_sig_index_ts: "dict[int, float]" = {}   # sig → last_seen timestamp (TTL support)
_p4_sig_index_sorted: "list[int]" = []      # S49: sorted mirror for O(log n) range query
_p4_sig_index_lock = _threading_mod.Lock()

def _sig_index_update(sigs: "list[int]", addrs: "list[int]") -> None:
    """Append sig→addr. LRU evict if capped. Stamp timestamp for TTL."""
    now = _time_mod.monotonic()
    with _p4_sig_index_lock:
        for sig, addr in zip(sigs, addrs):
            if sig == 0xFFFFFFFF:
                continue
            if sig in _p4_sig_index:
                _p4_sig_index.move_to_end(sig)
                _p4_sig_index[sig].append(addr)
            else:
                _p4_sig_index[sig] = [addr]
                _bisect_mod.insort(_p4_sig_index_sorted, sig)          # S49: sorted mirror
                if _P4_SIG_INDEX_CAP > 0:
                    while len(_p4_sig_index) > _P4_SIG_INDEX_CAP:
                        evicted, _ = _p4_sig_index.popitem(last=False)
                        _p4_sig_index_ts.pop(evicted, None)
                        _i = _bisect_mod.bisect_left(_p4_sig_index_sorted, evicted)
                        if _i < len(_p4_sig_index_sorted) and _p4_sig_index_sorted[_i] == evicted:
                            _p4_sig_index_sorted.pop(_i)               # S49: keep sorted mirror in sync
            _p4_sig_index_ts[sig] = now

def _sig_index_evict_ttl() -> int:
    """Remove entries older than TTL. Returns n_evicted."""
    if _P4_SIG_INDEX_TTL <= 0:
        return 0
    cutoff = _time_mod.monotonic() - _P4_SIG_INDEX_TTL
    with _p4_sig_index_lock:
        expired = [s for s, t in _p4_sig_index_ts.items() if t < cutoff]
        for s in expired:
            _p4_sig_index.pop(s, None)
            _p4_sig_index_ts.pop(s, None)
            _i = _bisect_mod.bisect_left(_p4_sig_index_sorted, s)     # S49: sorted mirror sync
            if _i < len(_p4_sig_index_sorted) and _p4_sig_index_sorted[_i] == s:
                _p4_sig_index_sorted.pop(_i)
    return len(expired)

def _sig_index_dump(path: str) -> int:
    """Serialize index to JSON. Returns n_sigs written."""
    with _p4_sig_index_lock:
        data = {str(k): v for k, v in _p4_sig_index.items()}
    with open(path, "w") as f:
        _json_mod.dump(data, f)
    return len(data)

def _sig_index_load(path: str, merge: bool = False) -> int:
    """Load index from JSON. merge=False clears first. Returns n_sigs loaded."""
    with open(path) as f:
        raw = _json_mod.load(f)
    # S49: support both plain {sig:addrs} and versioned snapshot {"version":..,"data":{sig:addrs}}
    data = raw.get("data", raw) if isinstance(raw, dict) and "version" in raw else raw
    now = _time_mod.monotonic()
    with _p4_sig_index_lock:
        if not merge:
            _p4_sig_index.clear()
            _p4_sig_index_ts.clear()
            _p4_sig_index_sorted.clear()
        for k, v in data.items():
            sig = int(k)
            if sig in _p4_sig_index:
                _p4_sig_index[sig].extend(v)
            else:
                _p4_sig_index[sig] = list(v)
                _bisect_mod.insort(_p4_sig_index_sorted, sig)         # S49
            _p4_sig_index_ts[sig] = now
    return len(data)

# S48: background TTL eviction thread (runs every TTL/2 seconds if TTL enabled)
def _ttl_evict_loop():
    interval = max(_P4_SIG_INDEX_TTL / 2, 1.0)
    while True:
        _time_mod.sleep(interval)
        n = _sig_index_evict_ttl()
        if n:
            import logging; logging.getLogger("uvicorn").debug("sig_index TTL evicted %d", n)

if _P4_SIG_INDEX_TTL > 0:
    _t = _threading_mod.Thread(target=_ttl_evict_loop, daemon=True, name="sig_idx_ttl")
    _t.start()

# S48: auto-dump on shutdown via atexit
def _sig_index_auto_dump():
    if _P4_SIG_INDEX_AUTO_DUMP:
        try:
            n = _sig_index_dump(_P4_SIG_INDEX_AUTO_DUMP)
            print(f"[sig_index] auto-dump → {_P4_SIG_INDEX_AUTO_DUMP} ({n} sigs)")
        except Exception as e:
            print(f"[sig_index] auto-dump failed: {e}")

_atexit_mod.register(_sig_index_auto_dump)

# ── P4 ctypes/numpy backend ─────────────────────────────────────────
try:
    import ctypes as _ct, numpy as _np

    # ── GeoP4Ctx (matches geo_cuda_p4.cu typedef) ──────────────────
    class _GeoP4Ctx(_ct.Structure):
        _fields_ = [
            ("d_in",        _ct.c_void_p),
            ("d_out_sig",   _ct.c_void_p),
            ("d_out_valid", _ct.c_void_p),
            ("d_compact",   _ct.c_void_p),
            ("d_count",     _ct.c_void_p),
            ("capacity",    _ct.c_uint32),
            ("stream",      _ct.c_void_p),   # cudaStream_t stored as void*
        ]

    _p4_lib = None
    _p4_ctx = None          # persistent GeoP4Ctx singleton
    _P4_BACKEND_SO = False

# ── S46-S49: sig→addr reverse index ──────────────────────────────────


    try:
        _p4_lib = _ct.CDLL(_P4_LIB_PATH)

        # geo_p4_ctx_init(ctx*, capacity) → int
        _p4_lib.geo_p4_ctx_init.restype  = _ct.c_int
        _p4_lib.geo_p4_ctx_init.argtypes = [
            _ct.POINTER(_GeoP4Ctx), _ct.c_uint32]

        # geo_p4_run(ctx*, h_wire16*, n, spoke_filter, addr_base, h_out_sig*) → int
        _p4_lib.geo_p4_run.restype  = _ct.c_int
        _p4_lib.geo_p4_run.argtypes = [
            _ct.POINTER(_GeoP4Ctx),
            _ct.c_void_p,            # h_wire16 (GeoWire16[])
            _ct.c_uint32,            # n
            _ct.c_uint32,            # spoke_filter
            _ct.c_uint64,            # addr_base
            _ct.POINTER(_ct.c_uint32),  # h_out_sig[]
        ]

        # geo_p4_ctx_free(ctx*)
        _p4_lib.geo_p4_ctx_free.restype  = None
        _p4_lib.geo_p4_ctx_free.argtypes = [_ct.POINTER(_GeoP4Ctx)]

        # geo_p4_run_compact(ctx*, h_wire16*, n, spoke_filter, addr_base,
        #                    h_out_compact*, out_n*) → int  [S45]
        _p4_lib.geo_p4_run_compact.restype  = _ct.c_int
        _p4_lib.geo_p4_run_compact.argtypes = [
            _ct.POINTER(_GeoP4Ctx),
            _ct.c_void_p,               # h_wire16
            _ct.c_uint32,               # n
            _ct.c_uint32,               # spoke_filter
            _ct.c_uint64,               # addr_base
            _ct.POINTER(_ct.c_uint32),  # h_out_compact
            _ct.POINTER(_ct.c_uint32),  # out_n
        ]

        # init persistent ctx
        _p4_ctx = _GeoP4Ctx()
        rc = _p4_lib.geo_p4_ctx_init(_ct.byref(_p4_ctx), _ct.c_uint32(_P4_CAP))
        if rc != 0:
            raise RuntimeError(f"geo_p4_ctx_init rc={rc}")

        # ── S42: wire batcher API (geo_p4_batcher_create/push/flush) ──
        # geo_p4_batcher_create(flush_n) → void* handle (NULL on error)
        _p4_lib.geo_p4_batcher_create.restype  = _ct.c_void_p
        _p4_lib.geo_p4_batcher_create.argtypes = [_ct.c_uint32]

        # geo_p4_batcher_push(handle, h_wire16, n, spoke_filter, addr_base) → int
        _p4_lib.geo_p4_batcher_push.restype  = _ct.c_int
        _p4_lib.geo_p4_batcher_push.argtypes = [
            _ct.c_void_p,   # handle
            _ct.c_void_p,   # h_wire16
            _ct.c_uint32,   # n
            _ct.c_uint32,   # spoke_filter
            _ct.c_uint64,   # addr_base
        ]

        # geo_p4_batcher_flush(handle, out_sig*, out_n*) → int
        _p4_lib.geo_p4_batcher_flush.restype  = _ct.c_int
        _p4_lib.geo_p4_batcher_flush.argtypes = [
            _ct.c_void_p,                    # handle
            _ct.POINTER(_ct.c_uint32),       # out_sig (may be NULL)
            _ct.POINTER(_ct.c_uint32),       # out_n   (may be NULL)
        ]

        # geo_p4_batcher_stats(handle, pushed*, flushed*, pending*)
        _p4_lib.geo_p4_batcher_stats.restype  = None
        _p4_lib.geo_p4_batcher_stats.argtypes = [
            _ct.c_void_p,
            _ct.POINTER(_ct.c_uint32),
            _ct.POINTER(_ct.c_uint32),
            _ct.POINTER(_ct.c_uint32),
        ]

        # geo_p4_batcher_destroy(handle)
        _p4_lib.geo_p4_batcher_destroy.restype  = None
        _p4_lib.geo_p4_batcher_destroy.argtypes = [_ct.c_void_p]

        _P4_BACKEND_SO = True
        log.info("P4 .so loaded: %s  cap=%d", _P4_LIB_PATH, _P4_CAP)

    except OSError:
        log.info("P4 .so not found (%s) — numpy fallback", _P4_LIB_PATH)
    except Exception as _soe:
        log.warning("P4 .so load failed: %s — numpy fallback", _soe)

    # ── pack addrs → GeoWire16 (16B: sig|slot|spoke+phase|pad) ─────
    def _pack_wire16(addrs: "_np.ndarray") -> "_np.ndarray":
        fi    = addrs % _P4_FULL_N
        q     = (fi * _P4_MOD6_MAGIC) >> 16
        spoke = (fi - q * _P4_SPOKES).astype(_np.uint32)
        slot  = q.astype(_np.uint32)
        sig   = slot ^ (spoke << 9)
        phase = ((slot >> 3) & 7).astype(_np.uint32)
        wire  = _np.zeros((len(addrs), 4), dtype=_np.uint32)
        wire[:, 0] = sig
        wire[:, 1] = slot
        wire[:, 2] = spoke | (phase << 8)
        return wire   # contiguous uint32, 16B/row = GeoWire16

    # ── main dispatch ───────────────────────────────────────────────
    def _p4_route_batch(addrs: "_np.ndarray",
                        spoke_filter: int = 0) -> "tuple[_np.ndarray, str]":
        """Returns (sig32[], backend_tag).  backend: cuda | so_cpu | numpy"""
        n = len(addrs)

        if _P4_BACKEND_SO and _p4_ctx is not None:
            wire    = _pack_wire16(addrs.astype(_np.uint64))
            out_sig = _np.empty(n, dtype=_np.uint32)
            rc = _p4_lib.geo_p4_run(
                _ct.byref(_p4_ctx),
                wire.ctypes.data_as(_ct.c_void_p),
                _ct.c_uint32(n),
                _ct.c_uint32(spoke_filter),
                _ct.c_uint64(int(addrs[0]) if n > 0 else 0),
                out_sig.ctypes.data_as(_ct.POINTER(_ct.c_uint32)),
            )
            if rc == 0:
                backend = "cuda" if n >= _P4_GPU_THRESH else "so_cpu"
                # apply spoke_filter on cpu side if needed (kernel already filters)
                return out_sig, backend
            log.warning("geo_p4_run rc=%d — numpy fallback", rc)

        # ── numpy fallback ─────────────────────────────────────────
        fi    = addrs.astype(_np.uint64) % _P4_FULL_N
        q     = (fi * _P4_MOD6_MAGIC) >> 16
        spoke = (fi - q * _P4_SPOKES).astype(_np.uint32)
        slot  = q.astype(_np.uint32)
        sig   = slot ^ (spoke << 9)
        if spoke_filter:
            sig[~(((spoke_filter >> spoke) & 1).astype(bool))] = 0xFFFFFFFF
        return sig, "numpy"

    _HAS_P4 = True
    log.info("P4 route layer ready (so=%s, threshold=%d)",
             _P4_BACKEND_SO, _P4_GPU_THRESH)

except Exception as _p4e:
    log.warning("P4 route layer unavailable: %s", _p4e)
    def _p4_route_batch(addrs, spoke_filter=0):   # type: ignore[misc]
        raise RuntimeError("P4 not initialised")

_LIB_PATH     = os.environ.get("POGLS_LIB",  "/mnt/c/TPOGLS/libpogls_v4.so")
_HOST         = os.environ.get("POGLS_HOST", "0.0.0.0")
_PORT         = int(os.environ.get("POGLS_PORT", 8765))
_NDJSON_BATCH = int(os.environ.get("POGLS_NDJSON_BATCH", 512))

_TRIG_KV_LOAD = int(os.environ.get("LLM_TRIG_KV_LOAD", 75))
_TRIG_GPU_OVF = int(os.environ.get("LLM_TRIG_GPU_OVF",  0))
_TRIG_AUDIT   = int(os.environ.get("LLM_TRIG_AUDIT",    1))
_TRIG_TOMB    = int(os.environ.get("LLM_TRIG_TOMB",    30))
_POLICY_MASKS = {k: 0 for k in ("KV_LOAD_HIGH","GPU_OVERFLOW","TOMB_HIGH","AUDIT_DEGRADED")}
if _HAS_ADVISOR:
    _POLICY_MASKS = {
        "KV_LOAD_HIGH":   (1<<Act.REHASH_NOW)      | (1<<Act.COMPACT_TOMBSTONE),
        "GPU_OVERFLOW":   (1<<Act.FLUSH_GPU_QUEUE)  | (1<<Act.DEGRADE_MODE),
        "TOMB_HIGH":      (1<<Act.COMPACT_TOMBSTONE),
        "AUDIT_DEGRADED": (1<<Act.DEGRADE_MODE)     | (1<<Act.RESET_CACHE_WINDOW),
    }

# ── context registry ───────────────────────────────────────────────

class CtxRecord:
    __slots__ = ("ctx_id","created_at","total_writes","total_reads",
                 "total_encodes","epoch","degrade_active","geo_ctx")
    def __init__(self, ctx_id: str):
        self.ctx_id         = ctx_id
        self.created_at     = time.time()
        self.total_writes   = 0
        self.total_reads    = 0
        self.total_encodes  = 0
        self.epoch          = 0
        self.degrade_active = False
        self.geo_ctx: Optional[Any] = GeoNetCtx() if _HAS_GEONET else None

    def to_status(self) -> dict:
        d = {
            "ctx_id": self.ctx_id, "epoch": self.epoch,
            "total_ops":     self.total_writes + self.total_reads,
            "total_writes":  self.total_writes,
            "total_reads":   self.total_reads,
            "total_encodes": self.total_encodes,
            "drift_active": False, "writes_frozen": False,
            "degrade_active": self.degrade_active,
            "shatter_count": 0, "lane_b_active": 0,
            "qrpn_state": 0, "qrpn_fails": 0, "gpu_fail_count": 0,
            "shat_stage": 0, "block_slots": BLOCK_SLOTS,
            "lib_path": _LIB_PATH,
            "uptime_s": round(time.time() - self.created_at, 1),
        }
        if self.geo_ctx is not None:
            d["geo_status"] = self.geo_ctx.status()
        return d

_contexts: dict[str, CtxRecord] = {}
_advisor:  Optional[Any] = (LLMAdvisor() if _HAS_ADVISOR else None)

# ── S31-B: Persistent Wallet Mount pool (unchanged) ───────────────

import uuid as _uuid

class WalletMountRecord:
    __slots__ = ("handle_id","loader","layer_keys","mounted_at","last_access","access_count")
    def __init__(self, handle_id: str, loader):
        self.handle_id    = handle_id
        self.loader       = loader
        self.layer_keys   = list(loader.keys())
        self.mounted_at   = time.time()
        self.last_access  = self.mounted_at
        self.access_count = 0

    def touch(self):
        self.last_access = time.time()
        self.access_count += 1

    def info(self) -> dict:
        return {
            "handle_id":    self.handle_id,
            "n_layers":     len(self.layer_keys),
            "layer_keys":   self.layer_keys,
            "mounted_at":   self.mounted_at,
            "last_access":  self.last_access,
            "access_count": self.access_count,
            "age_s":        round(time.time() - self.mounted_at, 1),
        }

_mounts: dict[str, WalletMountRecord] = {}

def _get_mount(handle_id: str) -> WalletMountRecord:
    if handle_id not in _mounts:
        raise HTTPException(404, f"handle '{handle_id}' not found — mount first via POST /wallet/mount")
    return _mounts[handle_id]

def _get_ctx(ctx_id: str) -> CtxRecord:
    if ctx_id not in _contexts:
        _contexts[ctx_id] = CtxRecord(ctx_id)
        log.info("new ctx: %s", ctx_id)
    return _contexts[ctx_id]

# ── S31: NDJSON stream helpers (unchanged) ─────────────────────────

def _ndjson_stream(
    geo_ctx,
    file_idx:   int,
    n_bytes:    int,
    layer:      str,
    spoke_mask: int  = 0x3F,
    audit_only: bool = False,
    val_min:    int  = 0,
    val_max:    int  = 0,
) -> Iterator[bytes]:
    n_chunks   = max(1, (n_bytes + 63) // 64)
    spoke_dist = [0]*6; audit_cnt = 0; emit_n = 0

    for rec in geo_ctx.iter_chunks_stream(
        file_idx, n_bytes,
        spoke_mask=spoke_mask,
        audit_only=audit_only,
        val_min=val_min,
        val_max=val_max,
    ):
        spoke_dist[rec["spoke"]] += 1
        if rec["is_audit"]: audit_cnt += 1
        emit_n += 1
        yield (json.dumps(rec) + "\n").encode()

    summary = {
        "_summary":       True,
        "layer":          layer,
        "n_total_chunks": n_chunks,
        "n_emitted":      emit_n,
        "filter": {
            "spoke_mask":  spoke_mask,
            "audit_only":  audit_only,
            "pass_rate":   round(emit_n / n_chunks, 4) if n_chunks else 0,
        },
        "spoke_dist":       spoke_dist,
        "dominant_spoke":   int(spoke_dist.index(max(spoke_dist))),
        "audit_points":     audit_cnt,
        "qrpn_state_after": geo_ctx.state,
        "geo_backend":      _GEO_BACKEND,
        "iter_streaming":   _HAS_ITER,
    }
    yield (json.dumps(summary) + "\n").encode()


def _ndjson_stream_gzip(geo_ctx, file_idx, n_bytes, layer,
                        spoke_mask=0x3F, audit_only=False,
                        val_min=0, val_max=0) -> Iterator[bytes]:
    buf = io.BytesIO(); gz = gzip.GzipFile(fileobj=buf, mode="wb")
    for chunk in _ndjson_stream(geo_ctx, file_idx, n_bytes, layer, spoke_mask, audit_only, val_min, val_max):
        gz.write(chunk); gz.flush()
        buf.seek(0); data = buf.read()
        buf.seek(0); buf.truncate()
        if data: yield data
    gz.close(); buf.seek(0); tail = buf.read()
    if tail: yield tail

# ── S33-B: LLMAdapter (classify + annotate) ───────────────────────

try:
    from llm_adapter_s33 import LLMAdapter as _LLMAdapter
    _llm_adapter: Optional[Any] = _LLMAdapter(
        api_base    = os.environ.get("LLM_ADAPTER_API",   "http://localhost:8082/v1"),
        api_key     = os.environ.get("LLM_ADAPTER_KEY",   ""),
        model       = os.environ.get("LLM_ADAPTER_MODEL", "Qwen3-1.7B-Q8_0.gguf"),
        batch_size  = int(os.environ.get("LLM_ADAPTER_BATCH",   "4")),
        timeout     = float(os.environ.get("LLM_ADAPTER_TIMEOUT", "60")),
        stub_mode   = os.environ.get("LLM_ADAPTER_STUB", "0") == "1",
    )
    _HAS_LLM_ADAPTER = True
    log.info("S33-B: LLMAdapter loaded — model=%s stub=%s",
             _llm_adapter.model, _llm_adapter.stub_mode)
except ImportError:
    _llm_adapter     = None
    _HAS_LLM_ADAPTER = False
    log.warning("S33-B: llm_adapter_s33 not found — enrichment disabled")

# ── S33-A: msgpack wire format ────────────────────────────────────

try:
    import msgpack as _msgpack
    _HAS_MSGPACK = True
except ImportError:
    _HAS_MSGPACK = False
    log.warning("S33-A: msgpack not installed — binary wire disabled (pip install msgpack)")

# ── S34: score_router — dynamic spoke_mask per record ────────────
try:
    from score_router import ScoreRouter, ScoreRouterConfig
    _score_router: Any = ScoreRouter()
    _HAS_SCORE_ROUTER  = True
    log.info("S34: score_router loaded — %s", _score_router)
except ImportError:
    _score_router      = None
    _HAS_SCORE_ROUTER  = False
    log.warning("S34: score_router not found — dynamic routing disabled")

# ── S36: feedback_engine + persistence ────────────────────────────
FEEDBACK_CHECKPOINT = os.environ.get("POGLS_FEEDBACK_CKPT", "pogls_feedback.json")

try:
    from feedback_engine import FeedbackEngine
    from score_router import ScoreRouterConfig

    if os.path.exists(FEEDBACK_CHECKPOINT):
        _feedback_engine, _restored_rules = FeedbackEngine.load(FEEDBACK_CHECKPOINT)
        if _restored_rules and _HAS_SCORE_ROUTER:
            _score_router._cfg.rules = _restored_rules
            log.info("S36: checkpoint restored — cycle=%d rules=%d path=%s",
                     _feedback_engine._cycle, len(_restored_rules), FEEDBACK_CHECKPOINT)
        else:
            log.info("S36: checkpoint loaded (no rules) — cycle=%d", _feedback_engine._cycle)
    else:
        _feedback_engine = FeedbackEngine()
        log.info("S36: feedback_engine fresh start — checkpoint=%s", FEEDBACK_CHECKPOINT)

    _HAS_FEEDBACK = True

except ImportError:
    _feedback_engine = None
    _HAS_FEEDBACK    = False
    log.warning("S36: feedback_engine not found — auto-adjust disabled")

# ── S38: billing_units — derive from _summary, no state ──────────
try:
    from billing_units_s38 import billing_units as _billing_units
    _HAS_BILLING = True
    log.info("S38: billing_units loaded")
except ImportError:
    _billing_units = None
    _HAS_BILLING   = False
    log.warning("S38: billing_units_s38 not found — billing disabled")

# ── S32/S33: multi-layer stream (NDJSON or msgpack) ───────────────

def _ndjson_multi_stream(
    geo_ctx,
    reqs:              list[dict],
    spoke_mask:        int            = 0x3F,
    audit_only:        bool           = False,
    val_min:           int            = 1,
    val_max:           int            = 0,
    val_filter:        bool           = False,
    chunk_lo:          int            = 0,
    chunk_hi:          int            = CHUNK_FILTER_OFF,
    enrich:            bool           = False,
    binary:            bool           = False,   # S33-A: True → msgpack frames instead of NDJSON
    route_mode:        bool           = False,   # S34: True → per-record dynamic spoke_mask via score_router
    layer_spoke_masks: Optional[dict] = None,    # S37: {layer_name → spoke_mask int}  override per layer
    layer_route_modes: Optional[dict] = None,    # S37: {layer_name → route_mode bool} override per layer
    p4_route:          bool           = False,   # S44: True → pipe addrs into P4 batcher, flush at end
) -> Iterator[bytes]:
    """
    S44   p4_route: True → collect addr from each batch, push to P4 batcher async.
          After all records streamed, flush batcher → emit {"_p4_sigs":...} line.
    S37   per-layer route_mode/spoke_mask: each layer can override global values independently.
          layer_spoke_masks: static mask override (no scoring) when layer route_mode=False.
          layer_route_modes: True → use score_router for that layer; False → static mask.
          None in either dict → inherit global route_mode / spoke_mask.
    S34   route_mode: LLM score → per-record spoke_mask (dynamic routing)
    S33-A binary mode: each frame = 4-byte LE length + msgpack(batch)
    S32  NDJSON mode:  each frame = json(batch) + newline  (default)
    Last frame always JSON _summary (both modes).
    """
    total_layers  = len(reqs)
    total_records = 0
    layer_counts:      dict[str, int]  = {}
    route_stats:       dict[str, int]  = {}  # S34: global mask hex → count
    layer_route_stats: dict[str, dict] = {}  # S37: layer_name → {mask_hex → count}
    use_llm    = enrich and _HAS_LLM_ADAPTER and _llm_adapter is not None
    use_binary = binary and _HAS_MSGPACK
    use_router = route_mode and _HAS_SCORE_ROUTER and _score_router is not None
    # S44: P4 batcher wiring
    use_p4   = p4_route and _HAS_P4
    p4_pushed_total = 0   # addrs pushed to batcher
    p4_addrs_acc: "list" = []  # S50: accumulate addrs for sig_index auto-feed
    # S37: per-layer override maps (None → inherit global)
    _lsm = layer_spoke_masks or {}
    _lrm = layer_route_modes or {}
    any_per_layer = bool(_lsm or _lrm)

    for batch in geo_ctx.iter_multi_range(
        reqs,
        spoke_mask=spoke_mask,
        audit_only=audit_only,
        val_min=val_min,
        val_max=val_max,
        val_filter=val_filter,
        chunk_lo=chunk_lo,
        chunk_hi=chunk_hi,
    ):
        if use_llm:
            try:
                batch = list(_llm_adapter.infer(batch))
            except Exception as e:
                log.warning("llm_adapter.infer failed — passthrough: %s", e)

        # S37+S34: routing — resolve per-layer first, fall back to global
        if use_router or any_per_layer:
            routed: list[dict] = []
            for rec in batch:
                lname = rec.get("layer_name", "?")
                # resolve effective route_mode for this layer
                layer_use_router = (
                    _HAS_SCORE_ROUTER and _score_router is not None and
                    _lrm.get(lname, route_mode)  # layer override → global
                )
                if layer_use_router:
                    rec = dict(rec)
                    _, rmask = next(_score_router.route_batch([rec]))
                    rec["_spoke_mask"] = hex(rmask)
                    k = hex(rmask)
                    route_stats[k] = route_stats.get(k, 0) + 1
                    ls = layer_route_stats.setdefault(lname, {})
                    ls[k] = ls.get(k, 0) + 1
                elif lname in _lsm:
                    # static per-layer spoke_mask (no score routing)
                    rec = dict(rec)
                    rec["_spoke_mask"] = hex(_lsm[lname] & 0x3F)
                routed.append(rec)
            batch = routed

        for rec in batch:
            lname = rec.get("layer_name", "?")
            layer_counts[lname] = layer_counts.get(lname, 0) + 1
            total_records += 1

        # S44: push addr values from this batch to P4 batcher (async, no flush yet)
        if use_p4 and batch:
            try:
                import numpy as _np44
                addrs_batch = _np44.array(
                    [r["addr"] for r in batch if "addr" in r],
                    dtype=_np44.uint64,
                )
                if len(addrs_batch) > 0:
                    p4_addrs_acc.extend(addrs_batch.tolist())  # S50: accumulate for sig_index feed
                    if _p4_batcher and _P4_BACKEND_SO:
                        wire = _pack_wire16(addrs_batch)
                        rc = _p4_lib.geo_p4_batcher_push(
                            _ct.c_void_p(_p4_batcher),
                            wire.ctypes.data_as(_ct.c_void_p),
                            _ct.c_uint32(len(addrs_batch)),
                            _ct.c_uint32(spoke_mask & 0x3F),
                            _ct.c_uint64(int(addrs_batch[0])),
                        )
                        if rc == 0:
                            p4_pushed_total += len(addrs_batch)
                    else:
                        # numpy fallback: route now, collect sigs for final flush line
                        _sigs, _ = _p4_route_batch(addrs_batch, spoke_mask & 0x3F)
                        p4_pushed_total += len(addrs_batch)
                        # S50: numpy path — feed index immediately (no deferred flush)
                        try:
                            import numpy as _np50
                            _valid_mask = _sigs != 0xFFFFFFFF
                            _sig_index_update(
                                _sigs[_valid_mask].tolist(),
                                addrs_batch[_valid_mask].tolist(),
                            )
                        except Exception as _ie:
                            log.debug("S50 numpy sig_index feed failed: %s", _ie)
            except Exception as _p4e:
                log.warning("S44 p4_route batch push failed: %s", _p4e)

        if use_binary:
            # S33-A: length-prefixed msgpack frame
            packed = _msgpack.packb(batch, use_bin_type=True)
            yield len(packed).to_bytes(4, "little") + packed
        else:
            yield (json.dumps(batch) + "\n").encode()

    # summary always JSON (both modes) — easy to parse as sentinel
    any_router = use_router or bool(_lrm)
    summary = {
        "_summary":          True,
        "n_layers":          total_layers,
        "total_records":     total_records,
        "layer_counts":      layer_counts,
        "chunk_window":      f"{chunk_lo}-{chunk_hi}" if chunk_hi != CHUNK_FILTER_OFF else "off",
        "val_range":         f"{val_min}-{val_max}" if val_filter else "off",
        "spoke_mask":        hex(spoke_mask),
        "audit_only":        audit_only,
        "qrpn_state_after":  geo_ctx.state,
        "geo_backend":       _GEO_BACKEND,
        "multi_range":       _HAS_MULTI,
        "llm_enriched":      use_llm,
        "llm_stats":         _llm_adapter.stats() if use_llm else None,
        "wire_format":       "msgpack" if use_binary else "ndjson",
        "route_mode":        use_router,
        "route_stats":       route_stats if any_router else None,
        "router_stats":      _score_router.stats() if (any_router and _HAS_SCORE_ROUTER) else None,
        # S37: per-layer breakdown
        "layer_route_stats": layer_route_stats if layer_route_stats else None,
        "layer_spoke_masks": {k: hex(v) for k, v in _lsm.items()} if _lsm else None,
        # S44: P4 batcher stats
        "p4_route":          use_p4,
        "p4_pushed":         p4_pushed_total if use_p4 else None,
    }
    # S38: derive billing units from summary — pure, no state
    if _HAS_BILLING:
        summary["_billing"] = _billing_units(summary)
    yield (json.dumps(summary) + "\n").encode()

    # S44: flush batcher after summary → emit _p4_sigs line
    if use_p4 and p4_pushed_total > 0:
        try:
            import numpy as _np44f
            if _p4_batcher and _P4_BACKEND_SO:
                out_sig = _np44f.empty(p4_pushed_total + 1024, dtype=_np44f.uint32)
                out_n   = _ct.c_uint32(0)
                rc = _p4_lib.geo_p4_batcher_flush(
                    _ct.c_void_p(_p4_batcher),
                    out_sig.ctypes.data_as(_ct.POINTER(_ct.c_uint32)),
                    _ct.byref(out_n),
                )
                if rc == 0:
                    n_sigs = int(out_n.value)
                    pushed2, flushed2, _ = _batcher_read_stats()
                    yield (json.dumps({
                        "_p4_sigs":     out_sig[:n_sigs].tolist(),
                        "p4_total":     n_sigs,
                        "p4_pushed":    p4_pushed_total,
                        "batcher_pushed":  pushed2,
                        "batcher_flushed": flushed2,
                        "backend":      "cuda" if p4_pushed_total >= _P4_GPU_THRESH else "so_cpu",
                    }) + "\n").encode()
                    # S50: auto-feed sig index from multi_stream flush
                    if n_sigs > 0 and p4_addrs_acc:
                        _acc = p4_addrs_acc[:n_sigs]  # align: one sig per addr
                        _sig_index_update(out_sig[:n_sigs].tolist(), _acc)
                else:
                    log.warning("S44 p4_batcher_flush rc=%d", rc)
            else:
                # numpy fallback already routed per-batch — emit count-only sentinel
                yield (json.dumps({
                    "_p4_sigs": None,
                    "p4_total": p4_pushed_total,
                    "backend":  "numpy",
                    "note":     "numpy path: sigs computed per-batch, not collected here",
                }) + "\n").encode()
        except Exception as _fe:
            log.warning("S44 p4 flush line failed: %s", _fe)

    # S35: auto-feedback — fire-and-forget after stream completes
    # aggregate route_stats across all layers for entropy analysis
    if any_router and _HAS_FEEDBACK and route_stats:
        try:
            deltas, meta = _feedback_engine.maybe_adjust(
                route_stats, _score_router._cfg.rules
            )
            if deltas:
                applied = _score_router.apply_feedback(deltas)
                log.info("S36 auto-feedback: cycle=%d rules_updated=%d reason=%s",
                         meta.get("cycle"), applied,
                         deltas[0].reason if deltas else "-")
                # S36: persist immediately after adjust
                try:
                    _feedback_engine.save(FEEDBACK_CHECKPOINT, _score_router._cfg.rules)
                    log.info("S36 checkpoint saved: %s", FEEDBACK_CHECKPOINT)
                except Exception as se:
                    log.warning("S36 checkpoint save failed: %s", se)
        except Exception as e:
            log.warning("S35 auto-feedback failed — skipped: %s", e)

# ── shared helpers (unchanged) ─────────────────────────────────────

def _snap_from_ctx(rec: CtxRecord) -> dict:
    try:
        with PoglsEngine() as eng:
            raw = eng.stats_out()
        return {
            "kv":    {"load_pct": raw["kv_load_pct_x100"]/100.0,
                      "tomb_pct": raw["kv_tomb_pct_x100"]/100.0},
            "gpu":   {"overflow_pct": (100.0*raw["gpu_ring_pending"] /
                       max(1, raw["gpu_ring_pending"]+raw["gpu_ring_flushed"]))},
            "audit": {"health": int(rec.degrade_active),
                      "l1_hit_pct": raw["l1_hit_pct_x100"]/100.0,
                      "qrpns": raw["ctx_qrpns"]},
            "hydra": {"fill_pct": 0}, "_source": "engine",
        }
    except Exception as e:
        log.warning("_snap_from_ctx: engine unavailable (%s)", e)
        load = min(100, int(rec.total_writes*100/(BLOCK_SLOTS or 1)))
        return {"kv": {"load_pct": load, "tomb_pct": 0},
                "gpu": {"overflow_pct": rec.total_encodes % 5},
                "audit": {"health": int(rec.degrade_active)},
                "hydra": {"fill_pct": 0}, "_source": "proxy"}

def _allowed_mask(snap: dict) -> int:
    if not _HAS_ADVISOR: return 1
    mask = 1
    kv = snap.get("kv",{}); gpu = snap.get("gpu",{}); aud = snap.get("audit",{})
    if kv.get("load_pct",0)      > _TRIG_KV_LOAD: mask |= _POLICY_MASKS["KV_LOAD_HIGH"]
    if gpu.get("overflow_pct",0) > _TRIG_GPU_OVF: mask |= _POLICY_MASKS["GPU_OVERFLOW"]
    if kv.get("tomb_pct",0)      > _TRIG_TOMB:    mask |= _POLICY_MASKS["TOMB_HIGH"]
    if aud.get("health",0)       >= _TRIG_AUDIT:  mask |= _POLICY_MASKS["AUDIT_DEGRADED"]
    return mask

def _state_key(snap: dict) -> str:
    kv = snap.get("kv",{}); gpu = snap.get("gpu",{}); aud = snap.get("audit",{})
    load = (int(kv.get("load_pct",0))//5)*5; tomb = (int(kv.get("tomb_pct",0))//5)*5
    ovf  = 1 if gpu.get("overflow_pct",0) > 0 else 0
    return f"KL{load:02d}T{tomb:02d}G{ovf}H{int(aud.get('health',0))}"

def _engine_encode(data: bytes) -> bytes:
    with PoglsEngine() as eng:
        eng.encode(data); recovered = eng.decode()
    return struct.pack("<I", len(data)) + recovered

def _engine_decode(blob: bytes) -> bytes:
    if len(blob) < 4: raise ValueError("blob too short")
    orig_len = struct.unpack("<I", blob[:4])[0]
    with PoglsEngine() as eng:
        eng.encode(blob[4:]); out = eng.decode()
    return out[:orig_len]

def _wallet_not_available():
    raise HTTPException(503, "wallet stack not installed")

def _parse_wallet(wallet_b64: str, src_b64: str):
    if not _HAS_WALLET: _wallet_not_available()
    try:
        blob = base64.b64decode(wallet_b64); src = base64.b64decode(src_b64)
    except Exception:
        raise HTTPException(400, "invalid base64 in wallet_b64 or src_b64")
    try:
        return VirtualFile(blob, src_map={0: src}, trusted=True)
    except Exception as e:
        raise HTTPException(400, f"VirtualFile mount failed: {e}")

# ── app ────────────────────────────────────────────────────────────

@asynccontextmanager
async def lifespan(app: FastAPI):
    global _p4_batcher
    log.info("POGLS REST server (S41) starting — geo_backend=%s iter=%s multi=%s llm=%s p4=%s port=%d",
             _GEO_BACKEND, _HAS_ITER, _HAS_MULTI, _HAS_LLM_ADAPTER, _HAS_P4, _PORT)
    _get_ctx("default")

    # S42: init real C double-buffer batcher (replaces S40 dict)
    if _HAS_P4 and _P4_BACKEND_SO:
        import numpy as _np
        _handle = _p4_lib.geo_p4_batcher_create(_ct.c_uint32(_P4_FLUSH_N))
        if _handle:
            _p4_batcher = _handle
            log.info("P4 C batcher ready (double-buffer, flush_n=%d)", _P4_FLUSH_N)
        else:
            log.warning("geo_p4_batcher_create NULL — batcher disabled (numpy-only)")
    elif _HAS_P4:
        import numpy as _np  # numpy fallback path still needs numpy

    yield

    # S42: destroy batcher on shutdown
    if _p4_batcher and _P4_BACKEND_SO:
        _p4_lib.geo_p4_batcher_destroy(_ct.c_void_p(_p4_batcher))
        log.info("P4 C batcher destroyed")
    log.info("POGLS REST server shutdown")

app = FastAPI(
    title="POGLS v4 REST API (S52)",
    version="1.52.0",
    description="streaming reconstruct | multi-layer batch | chunk window | LLM enrich | per-layer route_mode | billing units | P4 geo route ctypes dispatch",
    lifespan=lifespan,
)

# S51
if _HAS_BRIDGE:
    app.add_middleware(B62Middleware)

# ── request models ─────────────────────────────────────────────────

class EncodeBody(BaseModel):
    data_b64: str; ctx_id: str = "default"

class DecodeBody(BaseModel):
    blob_b64: str; ctx_id: str = "default"

class LLMReportBody(BaseModel):
    snap: Optional[dict] = None; force: bool = False

class WalletLoadBody(BaseModel):
    wallet_b64: str; src_b64: str; ctx_id: str = "default"

class WalletLayerBody(BaseModel):
    wallet_b64: str; src_b64: str; layer: str
    annotate:   bool = False
    compress:   bool = False
    spoke_mask: int  = 0x3F
    audit_only: bool = False
    val_min:    int  = 0
    val_max:    int  = 0
    ctx_id:     str  = "default"

class WalletMountBody(BaseModel):
    wallet_b64: str
    src_b64:    str

class WalletLayerFastBody(BaseModel):
    handle_id:  str
    layer:      str
    annotate:   bool = False
    compress:   bool = False
    spoke_mask: int  = 0x3F
    audit_only: bool = False
    val_min:    int  = 0
    val_max:    int  = 0
    ctx_id:     str  = "default"

class SignalFailBody(BaseModel):
    ctx_id: str = "default"
    count:  int = 1

# S32: per-layer descriptor inside ReconstructBody
class ReconstructLayer(BaseModel):
    name:       str
    chunk_lo:   int            = 0
    chunk_hi:   int            = CHUNK_FILTER_OFF  # 0xFFFF = no window
    # S37: per-layer overrides — None → inherit global
    spoke_mask: Optional[int]  = None   # override body.spoke_mask for this layer only
    route_mode: Optional[bool] = None   # override body.route_mode for this layer only

# S32: POST /wallet/reconstruct body
class ReconstructBody(BaseModel):
    handle_id:  str
    layers:     list[ReconstructLayer]
    spoke_mask: int  = 0x3F
    audit_only: bool = False
    val_min:    int  = 1        # S32 default: 1 > 0 → disabled
    val_max:    int  = 0        # val_min > val_max → off
    val_filter: bool = False    # set True to activate val gate
    annotate:   bool = False    # future: annotate=false → binary (reserved)
    enrich:     bool = False    # S33-B: True → LLM classify+annotate each chunk
    binary:     bool = False    # S33-A: True → msgpack frames (pip install msgpack)
    route_mode: bool = False    # S34:   True → per-record dynamic spoke_mask via score_router
    ctx_id:     str  = "default"
    p4_route:   bool = False    # S44:   True → pipe addrs into P4 batcher, flush after stream

# ── POST /wallet/reconstruct — S32 ────────────────────────────────

@app.post("/wallet/reconstruct")
def wallet_reconstruct(body: ReconstructBody):
    """
    S32: Multi-layer coord streaming via persistent mount handle.

    Builds GeoReq list from body.layers (resolved against mount.layer_keys).
    Each layer can specify its own chunk_lo/chunk_hi window.
    Global filter: spoke_mask, audit_only, val_min/val_max (if val_filter=True).

    Response: NDJSON stream.
      Each line: JSON array of GeoMultiRec dicts (batch of ≤256 records).
      Last line: _summary object.

    Record fields:
      layer_id, layer_name, chunk_global, chunk_i, addr, offset,
      spoke, mirror_mask, is_audit
    """
    if not _HAS_GEONET:
        raise HTTPException(503, "geo_net not available")

    mount = _get_mount(body.handle_id)
    mount.touch()
    rec   = _get_ctx(body.ctx_id)

    if not body.layers:
        raise HTTPException(400, "layers list is empty")

    # resolve layers → GeoReq dicts
    # chunk_lo/hi per-layer: use per-layer value if set, else body default
    reqs: list[dict] = []
    for layer_spec in body.layers:
        name = layer_spec.name
        if name not in mount.loader:
            raise HTTPException(
                404,
                f"layer '{name}' not found. available: {mount.layer_keys}"
            )
        file_idx = mount.layer_keys.index(name)
        arr      = mount.loader[name]
        reqs.append({
            "name":      name,
            "file_idx":  file_idx,
            "n_bytes":   arr.nbytes,
            "layer_id":  file_idx,   # use file_idx as stable layer_id
            "chunk_lo":  layer_spec.chunk_lo,
            "chunk_hi":  layer_spec.chunk_hi,
        })

    rec.total_reads += len(reqs)

    # global filter params
    spoke_mask  = body.spoke_mask & 0x3F
    audit_only  = body.audit_only
    val_min     = body.val_min
    val_max     = body.val_max
    val_filter  = body.val_filter

    # S37: per-layer override maps — only populated for layers with explicit overrides
    layer_spoke_masks: dict[str, int]  = {
        ls.name: (ls.spoke_mask & 0x3F)
        for ls in body.layers if ls.spoke_mask is not None
    }
    layer_route_modes: dict[str, bool] = {
        ls.name: ls.route_mode
        for ls in body.layers if ls.route_mode is not None
    }

    # chunk window: take the tightest common window across layers
    # (per-layer narrowing happens in iter_multi_range via layer-specific chunk_lo/hi
    # if we extend the API; current design: shared chunk_lo/hi = tightest union)
    # For now: use min chunk_lo and max chunk_hi across layer specs
    # (layers with default CHUNK_FILTER_OFF = no window are excluded from tightening)
    windowed = [r for r in reqs if r["chunk_hi"] != CHUNK_FILTER_OFF]
    if windowed:
        chunk_lo = min(r["chunk_lo"] for r in windowed)
        chunk_hi = max(r["chunk_hi"] for r in windowed)
    else:
        chunk_lo = 0
        chunk_hi = CHUNK_FILTER_OFF

    # build reqs for iter_multi_range (name, file_idx, n_bytes, layer_id)
    geo_reqs = [{
        "name":     r["name"],
        "file_idx": r["file_idx"],
        "n_bytes":  r["n_bytes"],
        "layer_id": r["layer_id"],
    } for r in reqs]

    # estimate total chunks for header
    total_chunks_est = sum(
        max(1, (r["n_bytes"] + 63) // 64) for r in reqs
    )
    chunk_window_str = (
        f"{chunk_lo}-{chunk_hi}" if chunk_hi != CHUNK_FILTER_OFF else "off"
    )
    val_range_str = (
        f"{val_min}-{val_max}" if val_filter and val_min <= val_max else "off"
    )
    flags_str = hex(
        (0x01 if audit_only else 0) | (0x02 if val_filter else 0)
    )

    headers = {
        "X-Layer-Count":        str(len(reqs)),
        "X-Layer-Names":        ",".join(r["name"] for r in reqs),
        "X-Total-Chunks-Est":   str(total_chunks_est),
        "X-Chunk-Window":       chunk_window_str,
        "X-Val-Range":          val_range_str,
        "X-Filter-Flags":       flags_str,
        "X-Spoke-Mask":         hex(spoke_mask),
        "X-Multi-Range":        str(_HAS_MULTI).lower(),
        "X-Geo-Backend":        _GEO_BACKEND,
        "X-Handle-Id":          body.handle_id,
        "X-LLM-Enriched":       str(body.enrich and _HAS_LLM_ADAPTER).lower(),
        "X-Wire-Format":        "msgpack" if (body.binary and _HAS_MSGPACK) else "ndjson",
        "X-Route-Mode":         str(body.route_mode and _HAS_SCORE_ROUTER).lower(),
        # S37: per-layer routing headers
        "X-Per-Layer-Masks":    str(len(layer_spoke_masks) > 0).lower(),
        "X-Per-Layer-Routes":   str(len(layer_route_modes) > 0).lower(),
        "X-P4-Route":           str(body.p4_route and _HAS_P4).lower(),  # S44
    }

    log.info(
        "wallet/reconstruct handle=%s layers=%d chunks_est=%d "
        "chunk_win=%s val=%s mask=%s enrich=%s binary=%s layer_masks=%d layer_routes=%d",
        body.handle_id, len(reqs), total_chunks_est,
        chunk_window_str, val_range_str, hex(spoke_mask), body.enrich, body.binary,
        len(layer_spoke_masks), len(layer_route_modes),
    )

    media_type = "application/msgpack" if (body.binary and _HAS_MSGPACK) else "application/x-ndjson"

    return StreamingResponse(
        _ndjson_multi_stream(
            rec.geo_ctx,
            geo_reqs,
            spoke_mask=spoke_mask,
            audit_only=audit_only,
            val_min=val_min,
            val_max=val_max,
            val_filter=val_filter,
            chunk_lo=chunk_lo,
            chunk_hi=chunk_hi,
            enrich=body.enrich,
            binary=body.binary,              # S33-A
            route_mode=body.route_mode,      # S34
            layer_spoke_masks=layer_spoke_masks or None,  # S37
            layer_route_modes=layer_route_modes or None,  # S37
            p4_route=body.p4_route,                       # S44
        ),
        media_type=media_type,
        headers=headers,
    )

# ── S35: router feedback endpoints ────────────────────────────────

class FeedbackBody(BaseModel):
    route_stats: dict   # {"0x34": 120, "0x01": 45, ...}
    dry_run:     bool = False  # True → analyze only, no mutation

@app.post("/router/feedback")
def router_feedback(body: FeedbackBody):
    """
    S35: Ingest route_stats → analyze entropy → maybe adjust ScoreRouter rules.

    dry_run=true  → returns analysis + deltas without mutating rules.
    dry_run=false → applies deltas if cooldown passed and entropy low.
    """
    if not (_HAS_SCORE_ROUTER and _HAS_FEEDBACK):
        raise HTTPException(503, "score_router or feedback_engine not available")

    analysis = _feedback_engine.analyze(body.route_stats)

    if body.dry_run:
        return {
            "dry_run":  True,
            "analysis": analysis,
            "engine":   _feedback_engine.stats(),
        }

    deltas, meta = _feedback_engine.maybe_adjust(
        body.route_stats,
        _score_router._cfg.rules,
    )

    applied = 0
    if deltas and not body.dry_run:
        applied = _score_router.apply_feedback(deltas)

    return {
        "adjusted":     meta["adjusted"],
        "rules_updated": applied,
        "deltas":       [vars(d) for d in deltas],
        "meta":         meta,
        "engine":       _feedback_engine.stats(),
    }

@app.get("/router/status")
def router_status():
    """S35: Current ScoreRouter rules + FeedbackEngine stats."""
    if not _HAS_SCORE_ROUTER:
        raise HTTPException(503, "score_router not available")

    rules = [
        {
            "label":     r.label,
            "score_min": r.score_min,
            "score_max": r.score_max,
            "mask":      hex(r.mask),
        }
        for r in _score_router._cfg.rules
    ]
    return {
        "rules":          rules,
        "default_mask":   hex(_score_router._cfg.default_mask),
        "router_stats":   _score_router.stats(),
        "feedback_stats": _feedback_engine.stats() if _HAS_FEEDBACK else None,
        "feedback_history": _feedback_engine.last_history(3) if _HAS_FEEDBACK else None,
        "checkpoint_path":  FEEDBACK_CHECKPOINT,
        "checkpoint_exists": os.path.exists(FEEDBACK_CHECKPOINT),
    }

@app.post("/router/checkpoint")
def router_checkpoint():
    """
    S36: Force-save current rules + engine state to checkpoint file.
    Safe to call anytime — idempotent.
    """
    if not (_HAS_SCORE_ROUTER and _HAS_FEEDBACK):
        raise HTTPException(503, "score_router or feedback_engine not available")
    try:
        manifest = _feedback_engine.save(FEEDBACK_CHECKPOINT, _score_router._cfg.rules)
        return {
            "saved":      True,
            "path":       FEEDBACK_CHECKPOINT,
            "cycle":      manifest["cycle"],
            "rules_saved": len(manifest["rules"]),
            "history_saved": len(manifest["history"]),
        }
    except Exception as e:
        raise HTTPException(500, f"checkpoint save failed: {e}")

# ── S31 endpoints (unchanged) ──────────────────────────────────────

@app.post("/wallet/load")
def wallet_load(body: WalletLoadBody):
    import numpy as np
    vf  = _parse_wallet(body.wallet_b64, body.src_b64)
    rec = _get_ctx(body.ctx_id)
    t0  = time.monotonic()
    try:
        loader = WeightStreamLoader(vf)
        layers = {}
        for i, name in enumerate(loader.keys()):
            arr  = loader[name]
            info: dict = {"shape": list(arr.shape), "dtype": str(arr.dtype),
                          "bytes": arr.nbytes}
            if _HAS_GEONET and rec.geo_ctx is not None:
                info["routing"] = rec.geo_ctx.route_layer_fast(i, arr.nbytes)
            layers[name] = info
    except Exception as e:
        log.error("wallet/load failed: %s", e); raise HTTPException(500, f"load error: {e}")
    elapsed_ms = round((time.monotonic() - t0)*1000, 2)
    rec.total_reads += len(layers)
    resp: dict = {"layers": layers, "n_layers": len(layers),
                  "elapsed_ms": elapsed_ms, "ctx_id": body.ctx_id}
    if _HAS_GEONET and rec.geo_ctx is not None:
        resp["geo_ctx_status"] = rec.geo_ctx.status()
    return JSONResponse(resp)


@app.post("/wallet/layer")
def wallet_layer(body: WalletLayerBody):
    import numpy as np
    vf  = _parse_wallet(body.wallet_b64, body.src_b64)
    rec = _get_ctx(body.ctx_id)
    t0  = time.monotonic()
    try:
        loader     = WeightStreamLoader(vf)
        if body.layer not in loader:
            raise HTTPException(404, f"layer '{body.layer}' not found. "
                                     f"available: {list(loader.keys())}")
        layer_keys = list(loader.keys())
        file_idx   = layer_keys.index(body.layer)
        arr        = loader[body.layer]
    except HTTPException:
        raise
    except Exception as e:
        log.error("wallet/layer failed: %s", e); raise HTTPException(500, str(e))

    rec.total_reads += 1
    spoke_mask = body.spoke_mask & 0x3F
    audit_only = body.audit_only
    val_min    = body.val_min
    val_max    = body.val_max

    if body.annotate and _HAS_GEONET and rec.geo_ctx is not None:
        n_chunks = max(1, (arr.nbytes + 63) // 64)
        headers  = {
            "X-Chunk-Count":     str(n_chunks),
            "X-Layer":           body.layer,
            "X-Layer-State":     rec.geo_ctx.state,
            "X-Geo-Backend":     _GEO_BACKEND,
            "X-Iter-Streaming":  str(_HAS_ITER).lower(),
            "X-Spoke-Mask":      hex(spoke_mask),
            "X-Audit-Only":      str(audit_only).lower(),
            "X-Val-Range":       f"{val_min}-{val_max}" if val_max else "off",
        }
        if body.compress:
            headers["Content-Encoding"] = "gzip"
            return StreamingResponse(
                _ndjson_stream_gzip(rec.geo_ctx, file_idx, arr.nbytes, body.layer,
                                    spoke_mask, audit_only, val_min, val_max),
                media_type="application/x-ndjson", headers=headers)
        return StreamingResponse(
            _ndjson_stream(rec.geo_ctx, file_idx, arr.nbytes, body.layer,
                           spoke_mask, audit_only, val_min, val_max),
            media_type="application/x-ndjson", headers=headers)

    routing = None
    if _HAS_GEONET and rec.geo_ctx is not None:
        summary, _ = rec.geo_ctx.route_layer_annotated_fast(file_idx, arr.nbytes)
        routing = summary
    elapsed_ms = round((time.monotonic() - t0)*1000, 2)
    resp: dict = {
        "data_b64":   base64.b64encode(arr.tobytes()).decode(),
        "shape":      list(arr.shape), "dtype": str(arr.dtype),
        "bytes":      arr.nbytes, "layer": body.layer,
        "elapsed_ms": elapsed_ms, "ctx_id": body.ctx_id,
    }
    if rec.geo_ctx is not None:
        resp["geo_state"]   = rec.geo_ctx.state
        resp["geo_backend"] = _GEO_BACKEND
    if routing is not None:
        resp["routing"] = routing
    return JSONResponse(resp)


@app.post("/wallet/mount")
def wallet_mount(body: WalletMountBody):
    if not _HAS_WALLET: _wallet_not_available()
    vf = _parse_wallet(body.wallet_b64, body.src_b64)
    try:
        loader = WeightStreamLoader(vf)
    except Exception as e:
        raise HTTPException(500, f"loader init failed: {e}")
    handle_id = "wh_" + _uuid.uuid4().hex[:12]
    _mounts[handle_id] = WalletMountRecord(handle_id, loader)
    log.info("wallet/mount new handle=%s layers=%d", handle_id, len(_mounts[handle_id].layer_keys))
    return JSONResponse({
        "handle_id": handle_id,
        "n_layers":  len(_mounts[handle_id].layer_keys),
        "layer_keys": _mounts[handle_id].layer_keys,
    })


@app.post("/wallet/layer/fast")
def wallet_layer_fast(body: WalletLayerFastBody):
    mount = _get_mount(body.handle_id)
    mount.touch()
    rec = _get_ctx(body.ctx_id)

    if body.layer not in mount.loader:
        raise HTTPException(404, f"layer '{body.layer}' not found. available: {mount.layer_keys}")

    try:
        file_idx = mount.layer_keys.index(body.layer)
        arr      = mount.loader[body.layer]
    except Exception as e:
        raise HTTPException(500, str(e))

    rec.total_reads += 1
    spoke_mask = body.spoke_mask & 0x3F
    audit_only = body.audit_only
    val_min    = body.val_min
    val_max    = body.val_max

    if body.annotate and _HAS_GEONET and rec.geo_ctx is not None:
        n_chunks = max(1, (arr.nbytes + 63) // 64)
        headers  = {
            "X-Chunk-Count":    str(n_chunks),
            "X-Layer":          body.layer,
            "X-Layer-State":    rec.geo_ctx.state,
            "X-Geo-Backend":    _GEO_BACKEND,
            "X-Iter-Streaming": str(_HAS_ITER).lower(),
            "X-Spoke-Mask":     hex(spoke_mask),
            "X-Audit-Only":     str(audit_only).lower(),
            "X-Val-Range":      f"{val_min}-{val_max}" if val_max else "off",
            "X-Handle-Id":      body.handle_id,
            "X-Access-Count":   str(mount.access_count),
        }
        if body.compress:
            headers["Content-Encoding"] = "gzip"
            return StreamingResponse(
                _ndjson_stream_gzip(rec.geo_ctx, file_idx, arr.nbytes, body.layer,
                                    spoke_mask, audit_only, val_min, val_max),
                media_type="application/x-ndjson", headers=headers)
        return StreamingResponse(
            _ndjson_stream(rec.geo_ctx, file_idx, arr.nbytes, body.layer,
                           spoke_mask, audit_only, val_min, val_max),
            media_type="application/x-ndjson", headers=headers)

    routing = None
    if _HAS_GEONET and rec.geo_ctx is not None and hasattr(rec.geo_ctx, "route_layer_fast"):
        routing = rec.geo_ctx.route_layer_fast(file_idx, arr.nbytes)
    return JSONResponse({
        "layer":    body.layer,
        "shape":    list(arr.shape),
        "dtype":    str(arr.dtype),
        "bytes":    arr.nbytes,
        "routing":  routing,
        "handle_id": body.handle_id,
    })


@app.delete("/wallet/mount/{handle_id}")
def wallet_unmount(handle_id: str):
    if handle_id not in _mounts:
        raise HTTPException(404, f"handle '{handle_id}' not found")
    info = _mounts.pop(handle_id).info()
    log.info("wallet/unmount handle=%s accesses=%d", handle_id, info["access_count"])
    return JSONResponse({"evicted": handle_id, "stats": info})


@app.get("/wallet/mounts")
def wallet_mounts():
    return JSONResponse({
        "n_mounts": len(_mounts),
        "mounts":   [m.info() for m in _mounts.values()],
    })


@app.post("/{ctx_id}/signal_fail")
def signal_fail(ctx_id: str, body: SignalFailBody):
    rec = _get_ctx(ctx_id)
    if rec.geo_ctx is None:
        raise HTTPException(503, "geo_ctx not available")
    count = max(1, min(body.count, 256))
    new_state = None
    for _ in range(count):
        new_state = rec.geo_ctx.signal_fail()
    log.info("signal_fail ctx=%s count=%d new_state=%s", ctx_id, count, new_state)
    return JSONResponse({
        "ctx_id":          ctx_id,
        "signals_sent":    count,
        "qrpn_state":      new_state,
        "anomaly_signals": getattr(rec.geo_ctx, "anomaly_signals", 0),
    })


# ── unchanged endpoints ────────────────────────────────────────────

@app.post("/encode")
def encode_endpoint(body: EncodeBody):
    try: data = base64.b64decode(body.data_b64)
    except Exception: raise HTTPException(400, "data_b64: invalid base64")
    if not data: raise HTTPException(400, "data is empty")
    t0 = time.monotonic()
    try: blob = _engine_encode(data)
    except Exception as e: raise HTTPException(500, f"encode error: {e}")
    rec = _get_ctx(body.ctx_id); rec.total_encodes += 1; rec.total_writes += len(data)//8
    return JSONResponse({"blob_b64": base64.b64encode(blob).decode(),
                         "orig_bytes": len(data), "blob_bytes": len(blob),
                         "chunks": (len(data)+7)//8,
                         "elapsed_ms": round((time.monotonic()-t0)*1000,2),
                         "ctx_id": body.ctx_id})

@app.post("/decode")
def decode_endpoint(body: DecodeBody):
    try: blob = base64.b64decode(body.blob_b64)
    except Exception: raise HTTPException(400, "blob_b64: invalid base64")
    t0 = time.monotonic()
    try: data = _engine_decode(blob)
    except ValueError as e: raise HTTPException(400, str(e))
    except Exception as e:  raise HTTPException(500, f"decode error: {e}")
    rec = _get_ctx(body.ctx_id); rec.total_reads += len(data)//8
    return JSONResponse({"data_b64": base64.b64encode(data).decode(),
                         "orig_bytes": len(data),
                         "elapsed_ms": round((time.monotonic()-t0)*1000,2),
                         "ctx_id": body.ctx_id})

@app.get("/read")
def read_endpoint(blob_b64: str=Query(...), offset: int=Query(0,ge=0),
                  length: int=Query(...,gt=0), ctx_id: str=Query("default")):
    try: blob = base64.b64decode(blob_b64)
    except Exception: raise HTTPException(400, "blob_b64: invalid base64")
    try: data = _engine_decode(blob)
    except ValueError as e: raise HTTPException(400, str(e))
    except Exception as e:  raise HTTPException(500, f"decode error: {e}")
    if offset >= len(data): raise HTTPException(416, f"offset {offset} >= {len(data)}")
    end = min(offset+length, len(data)); chunk = data[offset:end]
    _get_ctx(ctx_id).total_reads += 1
    return JSONResponse({"data_b64": base64.b64encode(chunk).decode(),
                         "offset": offset, "length": len(chunk),
                         "orig_bytes": len(data), "clamped": end < offset+length,
                         "ctx_id": ctx_id})

@app.get("/{ctx_id}/status")
def status(ctx_id: str):
    rec = _get_ctx(ctx_id); rec.epoch += 1
    engine_live = False
    try:
        with PoglsEngine() as eng: eng.stats(); engine_live = True
    except Exception as e: log.warning("engine probe failed: %s", e)
    st = rec.to_status(); st["engine_live"] = engine_live
    return JSONResponse(st)

@app.post("/{ctx_id}/llm_report")
def llm_report(ctx_id: str, body: LLMReportBody):
    rec  = _get_ctx(ctx_id)
    snap = body.snap or _snap_from_ctx(rec)
    state_key    = _state_key(snap)
    allowed_mask = _allowed_mask(snap)
    if not _HAS_ADVISOR:
        return JSONResponse({"state_key": state_key, "action": "NOOP",
                             "action_code": 0, "confidence": 1.0,
                             "from_cache": False, "allowed_actions": ["NOOP"],
                             "note": "llm_advisor not installed"})
    if body.force: _advisor._cache.invalidate(state_key)
    result = _advisor.query(state_key, allowed_mask, json.dumps(snap))
    if result.action == Act.DEGRADE_MODE:   rec.degrade_active = True
    elif result.action == Act.RECOVER_MODE: rec.degrade_active = False
    return JSONResponse({"state_key": state_key, "action": Act.name(result.action),
                         "action_code": int(result.action),
                         "confidence": round(float(result.confidence),4),
                         "from_cache": bool(result.from_cache),
                         "allowed_actions": Act.from_mask(allowed_mask),
                         "snap_used": snap})

@app.get("/{ctx_id}/snapshot")
def snapshot(ctx_id: str):
    rec = _get_ctx(ctx_id)
    return JSONResponse({"ctx_id": ctx_id, "status": rec.to_status(),
                         "snap": _snap_from_ctx(rec),
                         "state_key": _state_key(_snap_from_ctx(rec)),
                         "contexts": list(_contexts.keys()), "ts": time.time()})

@app.get("/llm_stats")
def llm_stats():
    return JSONResponse(_advisor.stats() if _HAS_ADVISOR else {"note": "advisor not loaded"})

# ── S43: P4 geo route endpoints ───────────────────────────────────

# ── Helper: read batcher stats from C or fallback counters ─────────
def _batcher_read_stats():
    """Returns (pushed, flushed, pending) from C batcher or Python counters."""
    if _p4_batcher and _P4_BACKEND_SO:
        p = _ct.c_uint32(0); f = _ct.c_uint32(0); q = _ct.c_uint32(0)
        _p4_lib.geo_p4_batcher_stats(
            _ct.c_void_p(_p4_batcher),
            _ct.byref(p), _ct.byref(f), _ct.byref(q))
        return int(p.value), int(f.value), int(q.value)
    return _p4_stat_pushed, _p4_stat_flushed, 0

# ── S46: compact streaming generator (valid-only chunks + sig index) ─

def _p4_route_compact_stream_gen(
    addrs:        "_np.ndarray",
    spoke_filter: int,
    ctx_id:       str,
    chunk_size:   int = 10_000,
) -> "Iterator[bytes]":
    """
    Yield NDJSON compact chunks — sentinels stripped per chunk.
    Also updates _p4_sig_index inline (sig→addr mapping).
    Final line: {\"_summary\": true, \"sig_index_size\": N_unique_sigs, ...}
    """
    import numpy as _np_local

    n          = len(addrs)
    t_total    = time.perf_counter()
    n_chunks   = (n + chunk_size - 1) // chunk_size
    total_out  = 0

    for ci in range(n_chunks):
        lo    = ci * chunk_size
        hi    = min(lo + chunk_size, n)
        chunk = addrs[lo:hi]
        m     = len(chunk)
        t0    = time.perf_counter()

        # ── compact run (C path preferred) ────────────────────────
        if _P4_BACKEND_SO and _p4_ctx is not None:
            wire        = _pack_wire16(chunk.astype(_np_local.uint64))
            out_compact = _np_local.empty(m, dtype=_np_local.uint32)
            out_n       = _ct.c_uint32(0)
            rc = _p4_lib.geo_p4_run_compact(
                _ct.byref(_p4_ctx),
                wire.ctypes.data_as(_ct.c_void_p),
                _ct.c_uint32(m),
                _ct.c_uint32(spoke_filter),
                _ct.c_uint64(int(chunk[0])),
                out_compact.ctypes.data_as(_ct.POINTER(_ct.c_uint32)),
                _ct.byref(out_n),
            )
            if rc != 0:
                yield (json.dumps({"error": f"compact rc={rc}", "chunk": ci}) + "\n").encode()
                return
            n_valid  = int(out_n.value)
            sigs     = out_compact[:n_valid].tolist()
            # index: compact output omits sentinels so addrs must be inferred
            # use chunk addrs at matching spoke positions (best-effort; full fidelity needs non-compact run)
            _sig_index_update(sigs, chunk[:n_valid].tolist())
            backend  = "cuda" if m >= _P4_GPU_THRESH else "so_cpu"
        else:
            sigs_all, backend = _p4_route_batch(chunk, spoke_filter)
            valid_mask = sigs_all != 0xFFFFFFFF
            sigs       = sigs_all[valid_mask].tolist()
            _sig_index_update(sigs, chunk[valid_mask].tolist())
            n_valid    = len(sigs)

        dt         = time.perf_counter() - t0
        total_out += n_valid

        yield (json.dumps({
            "chunk":        ci,
            "offset":       lo,
            "n":            n_valid,
            "n_input":      m,
            "sigs":         sigs,
            "backend":      backend,
            "compact":      True,
            "dt_ms":        round(dt * 1000, 3),
        }) + "\n").encode()

    with _p4_sig_index_lock:
        idx_size = len(_p4_sig_index)

    pushed, flushed, pending = _batcher_read_stats()
    yield (json.dumps({
        "_summary":       True,
        "n_total":        n,
        "n_out":          total_out,
        "n_chunks":       n_chunks,
        "chunk_size":     chunk_size,
        "spoke_filter":   spoke_filter,
        "ctx_id":         ctx_id,
        "compact":        True,
        "sig_index_size": idx_size,
        "total_dt_ms":    round((time.perf_counter() - t_total) * 1000, 3),
        "rate_mps":       round(n / max(time.perf_counter() - t_total, 1e-9) / 1e6, 2),
        "pushed":         pushed,
        "flushed":        flushed,
        "pending":        pending,
        "stream":         True,
    }) + "\n").encode()


# ── S43: streaming NDJSON generator for large-n /p4/route ──────────

def _p4_route_stream_gen(
    addrs:        "_np.ndarray",
    spoke_filter: int,
    ctx_id:       str,
    chunk_size:   int = 10_000,
) -> "Iterator[bytes]":
    """
    Yield NDJSON chunks for n > P4_STREAM_THRESH.
    Each line: {"chunk": i, "offset": k, "n": m, "sigs": [...], "backend": "..."}
    Final line: {"_summary": true, ...}
    """
    import numpy as _np_local

    n         = len(addrs)
    t_total   = time.perf_counter()
    n_chunks  = (n + chunk_size - 1) // chunk_size
    total_out = 0

    for ci in range(n_chunks):
        lo    = ci * chunk_size
        hi    = min(lo + chunk_size, n)
        chunk = addrs[lo:hi]
        m     = len(chunk)

        t0 = time.perf_counter()

        # ── C batcher path ────────────────────────────────────────
        if _p4_batcher and _P4_BACKEND_SO:
            wire    = _pack_wire16(chunk.astype(_np_local.uint64))
            out_sig = _np_local.empty(m, dtype=_np_local.uint32)
            out_n   = _ct.c_uint32(0)

            rc = _p4_lib.geo_p4_batcher_push(
                _ct.c_void_p(_p4_batcher),
                wire.ctypes.data_as(_ct.c_void_p),
                _ct.c_uint32(m),
                _ct.c_uint32(spoke_filter),
                _ct.c_uint64(int(chunk[0])),
            )
            if rc != 0:
                yield (json.dumps({"error": f"batcher_push rc={rc}", "chunk": ci}) + "\n").encode()
                return

            rc = _p4_lib.geo_p4_batcher_flush(
                _ct.c_void_p(_p4_batcher),
                out_sig.ctypes.data_as(_ct.POINTER(_ct.c_uint32)),
                _ct.byref(out_n),
            )
            if rc != 0:
                yield (json.dumps({"error": f"batcher_flush rc={rc}", "chunk": ci}) + "\n").encode()
                return

            sigs    = out_sig[:int(out_n.value)].tolist()
            backend = "cuda" if m >= _P4_GPU_THRESH else "so_cpu"

        # ── numpy fallback ────────────────────────────────────────
        else:
            sigs_arr, backend = _p4_route_batch(chunk, spoke_filter)
            sigs = sigs_arr.tolist()

        dt = time.perf_counter() - t0
        total_out += len(sigs)

        line = {
            "chunk":        ci,
            "offset":       lo,
            "n":            len(sigs),
            "sigs":         sigs,
            "backend":      backend,
            "dt_ms":        round(dt * 1000, 3),
        }
        yield (json.dumps(line) + "\n").encode()

    # ── summary line ──────────────────────────────────────────────
    pushed, flushed, pending = _batcher_read_stats()
    summary = {
        "_summary":     True,
        "n_total":      n,
        "n_out":        total_out,
        "n_chunks":     n_chunks,
        "chunk_size":   chunk_size,
        "spoke_filter": spoke_filter,
        "ctx_id":       ctx_id,
        "total_dt_ms":  round((time.perf_counter() - t_total) * 1000, 3),
        "rate_mps":     round(n / max(time.perf_counter() - t_total, 1e-9) / 1e6, 2),
        "pushed":       pushed,
        "flushed":      flushed,
        "pending":      pending,
        "stream":       True,
    }
    yield (json.dumps(summary) + "\n").encode()


class P4RouteBody(BaseModel):
    addrs:        list[int]        # input addresses (uint64-compatible)
    spoke_filter: int  = 0        # 6-bit spoke mask, 0 = all pass
    ctx_id:       str  = "default"
    flush:        bool = True      # True → immediate flush/return sigs
                                   # False → push only (async double-buffer)

@app.post("/p4/route")
def p4_route(body: P4RouteBody,
             compact: bool = Query(False, description="S45: return valid-only sigs (strip 0xFFFFFFFF)"),
             stream:  bool = Query(False, description="S46: combined compact+stream → NDJSON valid-only chunks")):
    """
    S46: compact=1&stream=1 → _p4_route_compact_stream_gen → NDJSON valid-only + builds sig_index.
    S45: compact=1 → geo_p4_run_compact → valid-only sigs (no 0xFFFFFFFF sentinels) + builds sig_index.
    S43: n > P4_STREAM_THRESH, flush=True → StreamingResponse NDJSON.
    S42: flush=True → push + flush → JSONResponse sigs.
         flush=False → push only → async accumulate.
    """
    global _p4_stat_pushed, _p4_stat_flushed
    if not _HAS_P4:
        raise HTTPException(503, "P4 route layer not available")

    import numpy as _np

    n = len(body.addrs)
    if n == 0:
        pushed, flushed, pending = _batcher_read_stats()
        return JSONResponse({"sigs": [], "n": 0, "path": "noop",
                             "pushed": pushed, "flushed": flushed})

    addrs = _np.array(body.addrs, dtype=_np.uint64)
    rec   = _get_ctx(body.ctx_id)
    rec.total_writes += n

    # ── S46: compact=1&stream=1 → compact NDJSON + sig index ──────
    if compact and stream and body.flush:
        return StreamingResponse(
            _p4_route_compact_stream_gen(addrs, body.spoke_filter, body.ctx_id, _P4_STREAM_CHUNK),
            media_type="application/x-ndjson",
            headers={
                "X-P4-Stream":   "true",
                "X-P4-Compact":  "true",
                "X-P4-N":        str(n),
                "X-P4-ChunkSize": str(_P4_STREAM_CHUNK),
            },
        )

    # ── S43: large-n → streaming NDJSON ───────────────────────────
    if n > _P4_STREAM_THRESH and body.flush:
        return StreamingResponse(
            _p4_route_stream_gen(addrs, body.spoke_filter, body.ctx_id, _P4_STREAM_CHUNK),
            media_type="application/x-ndjson",
            headers={
                "X-P4-Stream":      "true",
                "X-P4-N":           str(n),
                "X-P4-ChunkSize":   str(_P4_STREAM_CHUNK),
                "X-P4-Threshold":   str(_P4_STREAM_THRESH),
            },
        )

    # ── S45: compact=1 → valid-only sigs (strip 0xFFFFFFFF) ────────
    if compact:
        if _P4_BACKEND_SO and _p4_ctx is not None:
            wire        = _pack_wire16(addrs.astype(_np.uint64))
            out_compact = _np.empty(n, dtype=_np.uint32)
            out_n       = _ct.c_uint32(0)
            t0 = time.perf_counter()
            rc = _p4_lib.geo_p4_run_compact(
                _ct.byref(_p4_ctx),
                wire.ctypes.data_as(_ct.c_void_p),
                _ct.c_uint32(n),
                _ct.c_uint32(body.spoke_filter),
                _ct.c_uint64(int(addrs[0])),
                out_compact.ctypes.data_as(_ct.POINTER(_ct.c_uint32)),
                _ct.byref(out_n),
            )
            dt = time.perf_counter() - t0
            if rc != 0:
                raise HTTPException(500, f"geo_p4_run_compact rc={rc}")
            n_valid  = int(out_n.value)
            backend  = "cuda" if n >= _P4_GPU_THRESH else "so_cpu"
            sigs_out = out_compact[:n_valid].tolist()
            # S46: update sig index (compact output → addr mapping best-effort)
            _sig_index_update(sigs_out, addrs[:n_valid].tolist())
            with _p4_sig_index_lock:
                idx_size = len(_p4_sig_index)
            pushed, flushed, _ = _batcher_read_stats()
            return JSONResponse({
                "sigs":           sigs_out,
                "n":              n_valid,
                "n_input":        n,
                "compact":        True,
                "backend":        backend,
                "path":           "GPU/T4-compact" if backend == "cuda" else "CPU/so-compact",
                "spoke_filter":   body.spoke_filter,
                "sig_index_size": idx_size,
                "dt_ms":          round(dt * 1000, 3),
                "rate_mps":       round(n / max(dt, 1e-9) / 1e6, 2),
                "pushed":         pushed,
                "flushed":        flushed,
            })
        else:
            # numpy fallback: filter sentinels
            t0 = time.perf_counter()
            sigs_all, backend = _p4_route_batch(addrs, body.spoke_filter)
            valid = sigs_all[sigs_all != 0xFFFFFFFF]
            valid_addrs = addrs[sigs_all != 0xFFFFFFFF]
            dt    = time.perf_counter() - t0
            _sig_index_update(valid.tolist(), valid_addrs.tolist())
            with _p4_sig_index_lock:
                idx_size = len(_p4_sig_index)
            _p4_stat_pushed  += n
            _p4_stat_flushed += n
            return JSONResponse({
                "sigs":           valid.tolist(),
                "n":              len(valid),
                "n_input":        n,
                "compact":        True,
                "backend":        backend,
                "path":           "CPU/numpy-compact",
                "spoke_filter":   body.spoke_filter,
                "sig_index_size": idx_size,
                "dt_ms":          round(dt * 1000, 3),
                "rate_mps":       round(n / max(dt, 1e-9) / 1e6, 2),
                "pushed":         _p4_stat_pushed,
                "flushed":        _p4_stat_flushed,
            })

    # ── push=False path: C batcher accumulate only ─────────────────
    if not body.flush and _p4_batcher and _P4_BACKEND_SO:
        wire = _pack_wire16(addrs.astype(_np.uint64))
        t0 = time.perf_counter()
        rc = _p4_lib.geo_p4_batcher_push(
            _ct.c_void_p(_p4_batcher),
            wire.ctypes.data_as(_ct.c_void_p),
            _ct.c_uint32(n),
            _ct.c_uint32(body.spoke_filter),
            _ct.c_uint64(int(addrs[0])),
        )
        dt = time.perf_counter() - t0
        if rc != 0:
            raise HTTPException(500, f"geo_p4_batcher_push rc={rc}")
        pushed, flushed, pending = _batcher_read_stats()
        return JSONResponse({
            "sigs":         [],
            "n":            n,
            "backend":      "cuda_async",
            "path":         "GPU/T4-async",
            "spoke_filter": body.spoke_filter,
            "dt_ms":        round(dt * 1000, 3),
            "rate_mps":     round(n / max(dt, 1e-9) / 1e6, 2),
            "pushed":       pushed,
            "flushed":      flushed,
            "pending":      pending,
        })

    # ── flush=True path: push + immediate flush + return sigs ──────
    if _p4_batcher and _P4_BACKEND_SO:
        wire    = _pack_wire16(addrs.astype(_np.uint64))
        out_sig = _np.empty(n, dtype=_np.uint32)
        out_n   = _ct.c_uint32(0)

        t0 = time.perf_counter()
        # push to batcher
        rc = _p4_lib.geo_p4_batcher_push(
            _ct.c_void_p(_p4_batcher),
            wire.ctypes.data_as(_ct.c_void_p),
            _ct.c_uint32(n),
            _ct.c_uint32(body.spoke_filter),
            _ct.c_uint64(int(addrs[0])),
        )
        if rc != 0:
            raise HTTPException(500, f"geo_p4_batcher_push rc={rc}")

        # flush and collect sigs
        rc = _p4_lib.geo_p4_batcher_flush(
            _ct.c_void_p(_p4_batcher),
            out_sig.ctypes.data_as(_ct.POINTER(_ct.c_uint32)),
            _ct.byref(out_n),
        )
        dt = time.perf_counter() - t0
        if rc != 0:
            raise HTTPException(500, f"geo_p4_batcher_flush rc={rc}")

        backend = "cuda" if n >= _P4_GPU_THRESH else "so_cpu"
        pushed, flushed, pending = _batcher_read_stats()
        return JSONResponse({
            "sigs":         out_sig[:int(out_n.value)].tolist(),
            "n":            int(out_n.value),
            "backend":      backend,
            "path":         "GPU/T4" if backend == "cuda" else "CPU/so",
            "spoke_filter": body.spoke_filter,
            "dt_ms":        round(dt * 1000, 3),
            "rate_mps":     round(n / max(dt, 1e-9) / 1e6, 2),
            "pushed":       pushed,
            "flushed":      flushed,
            "pending":      pending,
        })

    # ── fallback: numpy path (no .so) ──────────────────────────────
    t0            = time.perf_counter()
    sigs, backend = _p4_route_batch(addrs, body.spoke_filter)
    dt            = time.perf_counter() - t0
    _p4_stat_pushed  += n
    _p4_stat_flushed += n
    return JSONResponse({
        "sigs":         sigs.tolist(),
        "n":            n,
        "backend":      backend,
        "path":         "CPU/numpy",
        "spoke_filter": body.spoke_filter,
        "dt_ms":        round(dt * 1000, 3),
        "rate_mps":     round(n / max(dt, 1e-9) / 1e6, 2),
        "pushed":       _p4_stat_pushed,
        "flushed":      _p4_stat_flushed,
        "pending":      0,
    })

@app.post("/p4/flush")
def p4_flush():
    """
    S42: Drain all pending packets from C batcher.
    Returns sig32[] for all accumulated packets since last flush.
    Use after flush=False pushes to retrieve results.
    """
    import numpy as _np
    if not _HAS_P4:
        raise HTTPException(503, "P4 not available")
    if not (_p4_batcher and _P4_BACKEND_SO):
        return JSONResponse({"sigs": [], "n": 0, "note": "no C batcher"})

    pushed, _, pending = _batcher_read_stats()
    if pending == 0:
        return JSONResponse({"sigs": [], "n": 0, "pending_was": 0})

    out_sig = _np.empty(pending + 1024, dtype=_np.uint32)  # +headroom
    out_n   = _ct.c_uint32(0)
    t0 = time.perf_counter()
    rc = _p4_lib.geo_p4_batcher_flush(
        _ct.c_void_p(_p4_batcher),
        out_sig.ctypes.data_as(_ct.POINTER(_ct.c_uint32)),
        _ct.byref(out_n),
    )
    dt = time.perf_counter() - t0
    if rc != 0:
        raise HTTPException(500, f"geo_p4_batcher_flush rc={rc}")

    n = int(out_n.value)
    pushed2, flushed2, _ = _batcher_read_stats()
    return JSONResponse({
        "sigs":    out_sig[:n].tolist(),
        "n":       n,
        "dt_ms":   round(dt * 1000, 3),
        "pushed":  pushed2,
        "flushed": flushed2,
    })

@app.get("/p4/stats")
def p4_stats():
    """S42: P4 batcher counters + config (reads from C batcher)."""
    if not _HAS_P4:
        return JSONResponse({"available": False})
    pushed, flushed, pending = _batcher_read_stats()
    with _p4_sig_index_lock:
        idx_size = len(_p4_sig_index)
    return JSONResponse({
        "available":      True,
        "backend_so":     _P4_BACKEND_SO,
        "batcher_c":      bool(_p4_batcher and _P4_BACKEND_SO),
        "ctx_ready":      (_p4_ctx is not None),
        "flush_n":        _P4_FLUSH_N,
        "gpu_threshold":  _P4_GPU_THRESH,
        "total_pushed":   pushed,
        "total_flushed":  flushed,
        "pending":        pending,
        "sig_index_size": idx_size,   # S46
    })

# ── S46: sig index endpoints ─────────────────────────────────────────

@app.get("/p4/sig_index")
def p4_sig_index_lookup(sig: int = Query(..., description="sig32 value to look up")):
    """S46/S52: Reverse lookup sig→[addr64, ...]. Returns addrs_b62 if bridge available."""
    with _p4_sig_index_lock:
        addrs = list(_p4_sig_index.get(sig, []))
    out: dict = {"sig": sig, "addrs": addrs, "n": len(addrs), "found": len(addrs) > 0}
    if _HAS_BRIDGE and addrs:
        out["addrs_b62"] = [_b62enc_fn(a) for a in addrs]
    return JSONResponse(out)

@app.post("/p4/sig_index/clear")
def p4_sig_index_clear():
    """S46: Reset sig index."""
    with _p4_sig_index_lock:
        prev = len(_p4_sig_index)
        _p4_sig_index.clear()
        _p4_sig_index_ts.clear()
        _p4_sig_index_sorted.clear()                                   # S49
    return JSONResponse({"cleared": True, "prev_size": prev})

@app.get("/p4/sig_index/stats")
def p4_sig_index_stats():
    """S46-S48: Sig index summary — sigs, addrs, cap, TTL config."""
    with _p4_sig_index_lock:
        n_sigs  = len(_p4_sig_index)
        n_addrs = sum(len(v) for v in _p4_sig_index.values())
    return JSONResponse({
        "n_sigs":    n_sigs,
        "n_addrs":   n_addrs,
        "cap":       _P4_SIG_INDEX_CAP,
        "capped":    _P4_SIG_INDEX_CAP > 0 and n_sigs >= _P4_SIG_INDEX_CAP,
        "ttl_s":     _P4_SIG_INDEX_TTL,
        "auto_dump": bool(_P4_SIG_INDEX_AUTO_DUMP),
    })

# ── S48: batch lookup ────────────────────────────────────────────────

class SigBatchLookupBody(BaseModel):
    sigs: list[int | str]   # S52: accept uint64 or b62 strings

@app.post("/p4/sig_index/batch_lookup")
def p4_sig_index_batch_lookup(body: SigBatchLookupBody):
    """S48/S52: Lookup multiple sigs. Accepts b62 sigs, returns b62 addrs."""
    # S52: decode b62 sigs
    sigs_int: list[int] = []
    input_b62 = False
    for s in body.sigs:
        if _HAS_BRIDGE and isinstance(s, str) and _is_b62_fn(s):
            sigs_int.append(_b62dec_fn(s))
            input_b62 = True
        else:
            sigs_int.append(int(s))
    with _p4_sig_index_lock:
        result = {str(s): list(_p4_sig_index.get(s, [])) for s in sigs_int}
    n_found = sum(1 for v in result.values() if v)
    out: dict = {"results": result, "n_sigs": len(body.sigs), "n_found": n_found}
    if _HAS_BRIDGE:
        out["results_b62"] = {
            _b62enc_fn(int(k)): [_b62enc_fn(a) for a in v]
            for k, v in result.items()
        }
    return JSONResponse(out)

# ── S48: manual TTL evict trigger ───────────────────────────────────

@app.post("/p4/sig_index/evict_ttl")
def p4_sig_index_evict_ttl():
    """S48: Manually trigger TTL eviction pass. No-op if TTL disabled."""
    n = _sig_index_evict_ttl()
    with _p4_sig_index_lock:
        remaining = len(_p4_sig_index)
    return JSONResponse({"evicted": n, "remaining": remaining,
                         "ttl_s": _P4_SIG_INDEX_TTL,
                         "ttl_enabled": _P4_SIG_INDEX_TTL > 0})

def _sig_index_snapshot(label: "str | None" = None) -> dict:
    """S49: Versioned snapshot — writes JSON with metadata envelope. Returns meta dict."""
    import os as _os2
    _os2.makedirs(_P4_SIG_INDEX_DUMP_DIR, exist_ok=True)
    import time as _wt
    epoch = int(_wt.time())
    lbl   = label or f"snapshot-{epoch}"
    path  = _os2.path.join(_P4_SIG_INDEX_DUMP_DIR, f"{lbl}.json")
    with _p4_sig_index_lock:
        payload = {str(k): v for k, v in _p4_sig_index.items()}
        n = len(payload)
    doc = {"version": "S49", "epoch": epoch, "label": lbl, "n_sigs": n, "data": payload}
    with open(path, "w") as f:
        _json_mod.dump(doc, f)
    return {"version": "S49", "epoch": epoch, "label": lbl, "n_sigs": n, "path": path}

def _sig_index_list_snapshots() -> "list[dict]":
    """S49: List all versioned snapshots in DUMP_DIR."""
    import os as _os2
    out = []
    if not _os2.path.isdir(_P4_SIG_INDEX_DUMP_DIR):
        return out
    for fname in sorted(_os2.listdir(_P4_SIG_INDEX_DUMP_DIR)):
        if not fname.endswith(".json"):
            continue
        fp = _os2.path.join(_P4_SIG_INDEX_DUMP_DIR, fname)
        try:
            with open(fp) as f:
                meta = _json_mod.load(f)
            if "version" in meta and "epoch" in meta:   # versioned snapshot
                out.append({"label": meta["label"], "epoch": meta["epoch"],
                             "n_sigs": meta["n_sigs"], "path": fp})
        except Exception:
            pass
    return out

# ── S49: snapshot / range endpoints ──────────────────────────────────

class SigIndexDumpBody(BaseModel):
    filename: str = "sig_index.json"

class SigIndexLoadBody(BaseModel):
    path:  str
    merge: bool = False

@app.post("/p4/sig_index/dump")
def p4_sig_index_dump(body: SigIndexDumpBody):
    """S47: Serialize sig index to JSON file."""
    import os
    fname = body.filename if os.path.isabs(body.filename) \
            else os.path.join(_P4_SIG_INDEX_DUMP_DIR, body.filename)
    n = _sig_index_dump(fname)
    return JSONResponse({"dumped": True, "path": fname, "n_sigs": n})

@app.post("/p4/sig_index/load")
def p4_sig_index_load(body: SigIndexLoadBody):
    """S47: Load sig index from JSON file. merge=False clears first."""
    import os
    path = body.path if os.path.isabs(body.path) \
           else os.path.join(_P4_SIG_INDEX_DUMP_DIR, body.path)
    if not os.path.exists(path):
        raise HTTPException(404, f"File not found: {path}")
    n = _sig_index_load(path, merge=body.merge)
    with _p4_sig_index_lock:
        total = len(_p4_sig_index)
    return JSONResponse({"loaded": True, "path": path, "n_loaded": n,
                         "merge": body.merge, "index_size": total})

# ── S49: range query ──────────────────────────────────────────────────

@app.get("/p4/sig_index/range")
def p4_sig_index_range(sig_min: int = Query(...), sig_max: int = Query(...),
                       limit: int = Query(1000, ge=1, le=100_000),
                       b62: bool = Query(False, description="S52: encode results as b62")):
    """S49/S52: Range query sig→addrs. b62=1 encodes keys+addrs as b62 strings."""
    if sig_min > sig_max:
        raise HTTPException(400, "sig_min must be <= sig_max")
    with _p4_sig_index_lock:
        keys = _p4_sig_index_sorted
        lo = _bisect_mod.bisect_left(keys, sig_min)
        hi = _bisect_mod.bisect_right(keys, sig_max)
        slice_keys = keys[lo:hi]
        truncated  = len(slice_keys) > limit
        out_keys   = slice_keys[:limit]
        results    = {k: list(_p4_sig_index[k]) for k in out_keys if k in _p4_sig_index}
    out: dict = {"results": results, "n_sigs": len(results),
                 "truncated": truncated, "sig_min": sig_min, "sig_max": sig_max}
    if _HAS_BRIDGE:
        out["results_b62"] = {
            _b62enc_fn(k): [_b62enc_fn(a) for a in v]
            for k, v in results.items()
        }
    return JSONResponse(out)

# ── S49: snapshot versioning ──────────────────────────────────────────

class SigSnapshotBody(BaseModel):
    label: "str | None" = None

class SigSnapshotLoadBody(BaseModel):
    label: str
    mode:  str = "merge"   # "merge" | "replace"

@app.post("/p4/sig_index/snapshot")
def p4_sig_index_snapshot(body: SigSnapshotBody = SigSnapshotBody()):
    """S49: Write versioned snapshot (version/epoch/label/n_sigs/data envelope)."""
    meta = _sig_index_snapshot(body.label)
    return JSONResponse(meta)

@app.get("/p4/sig_index/snapshots")
def p4_sig_index_snapshots():
    """S49: List all versioned snapshots in DUMP_DIR."""
    snaps = _sig_index_list_snapshots()
    return JSONResponse({"snapshots": snaps, "n": len(snaps),
                          "dir": _P4_SIG_INDEX_DUMP_DIR})

@app.post("/p4/sig_index/snapshot/load")
def p4_sig_index_snapshot_load(body: SigSnapshotLoadBody):
    """S49: Load versioned snapshot by label. mode=merge|replace."""
    import os
    path = os.path.join(_P4_SIG_INDEX_DUMP_DIR, f"{body.label}.json")
    if not os.path.exists(path):
        raise HTTPException(404, f"Snapshot '{body.label}' not found in {_P4_SIG_INDEX_DUMP_DIR}")
    merge = (body.mode != "replace")
    n = _sig_index_load(path, merge=merge)
    with _p4_sig_index_lock:
        total = len(_p4_sig_index)
    return JSONResponse({"loaded": True, "label": body.label, "mode": body.mode,
                          "n_loaded": n, "index_size": total})

def health():
    geo_s = {}
    try:
        ctx_tmp = GeoNetCtx(); geo_s = ctx_tmp.status(); del ctx_tmp
    except Exception: pass
    return {
        "status": "ok", "lib": _LIB_PATH,
        "contexts": len(_contexts), "version": "S54",
        "wallet_stack":          _HAS_WALLET,
        "geo_net":               _HAS_GEONET,
        "geo_backend":           _GEO_BACKEND,
        "iter_streaming":        _HAS_ITER,
        "filter_pushdown":       geo_s.get("filter_pushdown", False),
        "slot_hot_wired":        geo_s.get("slot_hot_wired",  False),
        "val_range_filter":      True,           # S31-A
        "persistent_mounts":     len(_mounts),   # S31-B
        "multi_range_stream":    _HAS_MULTI,     # S32
        "chunk_window_filter":   True,           # S32
        "flags_filter":          True,           # S32
        "ndjson_batch":          _NDJSON_BATCH,
        "p4_route":              _HAS_P4,        # S40
        "p4_gpu_threshold":      _P4_GPU_THRESH, # S40
        "p4_stream_thresh":      _P4_STREAM_THRESH,  # S43
        "p4_stream_chunk":       _P4_STREAM_CHUNK,   # S43
        "p4_sig_index":          True,               # S46
        "p4_sig_index_cap":      _P4_SIG_INDEX_CAP,  # S48
        "endpoints": [
            "POST /encode", "POST /decode", "GET  /read",
            "POST /wallet/load",
            "POST /wallet/layer  [annotate → NDJSON | S31: val_min/max filter]",
            "POST /wallet/mount             [S31-B: parse once → handle_id]",
            "POST /wallet/layer/fast        [S31-B: stream via handle]",
            "DELETE /wallet/mount/{id}      [S31-B: evict handle]",
            "GET  /wallet/mounts            [S31-B: list active handles]",
            "POST /wallet/reconstruct       [S32: multi-layer batch stream]",
            "POST /{ctx_id}/signal_fail",
            "GET  /{ctx_id}/status", "POST /{ctx_id}/llm_report",
            "GET  /{ctx_id}/snapshot", "GET  /llm_stats",
            "POST /p4/route                 [S46: compact+stream=1→NDJSON+index | S45: compact=1 | S43: stream]",
            "POST /p4/flush                 [S42: drain pending batcher]",
            "GET  /p4/stats                 [S46: +sig_index_size]",
            "GET  /p4/sig_index?sig=N       [S46: reverse lookup sig→addrs]",
            "POST /p4/sig_index/clear       [S46: reset index]",
            "GET  /p4/sig_index/stats       [S48: +ttl_s/auto_dump fields]",
            "POST /p4/sig_index/dump        [S47: serialize index → JSON file]",
            "POST /p4/sig_index/load        [S47: load index from JSON file]",
            "POST /p4/sig_index/batch_lookup [S48: multi-sig lookup in one call]",
            "POST /p4/sig_index/evict_ttl   [S48: manual TTL eviction pass]",
            "GET  /p4/sig_index/range        [S49: range query sig_min..sig_max O(log n+k)]",
            "POST /p4/sig_index/snapshot     [S49: versioned snapshot with epoch/label]",
            "GET  /p4/sig_index/snapshots    [S49: list versioned snapshots in DUMP_DIR]",
            "POST /p4/sig_index/snapshot/load [S49: load snapshot by label, mode=merge|replace]",
            "POST /wallet/reconstruct        [S50: auto-feed sig_index from multi_stream flush]",
            "GET  /p4/bridge/inspect         [S51: addr decompose + sig_index lookup]",
            "GET  /p4/bridge/snapshot_svg    [S51: snapshot JSON → blueprint SVG]",
            "POST /p4/bridge/b62_encode      [S51: uint64[] → b62[]]",
            "POST /p4/bridge/b62_decode      [S51: b62[] → uint64[]]",
            "POST /p4/bridge/svg_decode      [S54: SVG → addr rows, mode=path|inline]",
        ],
    }

# S51: wire bridge endpoints
if _HAS_BRIDGE:
    make_bridge_endpoints(app, lambda: dict(_p4_sig_index))

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("rest_server_s54:app", host=_HOST, port=_PORT,
                reload=False, log_level="info")
