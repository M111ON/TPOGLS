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

# ── S35: feedback_engine — adaptive threshold adjuster ───────────
try:
    from feedback_engine import FeedbackEngine
    _feedback_engine: Any = FeedbackEngine()
    _HAS_FEEDBACK          = True
    log.info("S35: feedback_engine loaded — cooldown=%.0fs alpha=%.2f",
             _feedback_engine.cooldown_s, _feedback_engine.smoothing_alpha)
except ImportError:
    _feedback_engine = None
    _HAS_FEEDBACK    = False
    log.warning("S35: feedback_engine not found — auto-adjust disabled")

# ── S32/S33: multi-layer stream (NDJSON or msgpack) ───────────────

def _ndjson_multi_stream(
    geo_ctx,
    reqs:        list[dict],
    spoke_mask:  int  = 0x3F,
    audit_only:  bool = False,
    val_min:     int  = 1,
    val_max:     int  = 0,
    val_filter:  bool = False,
    chunk_lo:    int  = 0,
    chunk_hi:    int  = CHUNK_FILTER_OFF,
    enrich:      bool = False,
    binary:      bool = False,   # S33-A: True → msgpack frames instead of NDJSON
    route_mode:  bool = False,   # S34: True → per-record dynamic spoke_mask via score_router
) -> Iterator[bytes]:
    """
    S34   route_mode: LLM score → per-record spoke_mask (dynamic routing)
    S33-A binary mode: each frame = 4-byte LE length + msgpack(batch)
    S32  NDJSON mode:  each frame = json(batch) + newline  (default)
    Last frame always JSON _summary (both modes).
    """
    total_layers  = len(reqs)
    total_records = 0
    layer_counts: dict[str, int] = {}
    route_stats:  dict[str, int] = {}  # S34: mask hex → count
    use_llm    = enrich and _HAS_LLM_ADAPTER and _llm_adapter is not None
    use_binary = binary and _HAS_MSGPACK
    use_router = route_mode and _HAS_SCORE_ROUTER and _score_router is not None

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

        # S34: score-based routing — stamp resolved mask onto each record
        if use_router:
            routed: list[dict] = []
            for rec, rmask in _score_router.route_batch(batch):
                rec = dict(rec)
                rec["_spoke_mask"] = hex(rmask)
                routed.append(rec)
                k = hex(rmask)
                route_stats[k] = route_stats.get(k, 0) + 1
            batch = routed

        for rec in batch:
            lname = rec.get("layer_name", "?")
            layer_counts[lname] = layer_counts.get(lname, 0) + 1
            total_records += 1

        if use_binary:
            # S33-A: length-prefixed msgpack frame
            packed = _msgpack.packb(batch, use_bin_type=True)
            yield len(packed).to_bytes(4, "little") + packed
        else:
            yield (json.dumps(batch) + "\n").encode()

    # summary always JSON (both modes) — easy to parse as sentinel
    summary = {
        "_summary":      True,
        "n_layers":      total_layers,
        "total_records": total_records,
        "layer_counts":  layer_counts,
        "chunk_window":  f"{chunk_lo}-{chunk_hi}" if chunk_hi != CHUNK_FILTER_OFF else "off",
        "val_range":     f"{val_min}-{val_max}" if val_filter else "off",
        "spoke_mask":    hex(spoke_mask),
        "audit_only":    audit_only,
        "qrpn_state_after": geo_ctx.state,
        "geo_backend":   _GEO_BACKEND,
        "multi_range":   _HAS_MULTI,
        "llm_enriched":  use_llm,
        "llm_stats":     _llm_adapter.stats() if use_llm else None,
        "wire_format":   "msgpack" if use_binary else "ndjson",
        "route_mode":    use_router,
        "route_stats":   route_stats if use_router else None,
        "router_stats":  _score_router.stats() if use_router else None,
    }
    yield (json.dumps(summary) + "\n").encode()

    # S35: auto-feedback — fire-and-forget after stream completes
    if use_router and _HAS_FEEDBACK and route_stats:
        try:
            deltas, meta = _feedback_engine.maybe_adjust(
                route_stats, _score_router._cfg.rules
            )
            if deltas:
                applied = _score_router.apply_feedback(deltas)
                log.info("S35 auto-feedback: cycle=%d rules_updated=%d reason=%s",
                         meta.get("cycle"), applied,
                         deltas[0].reason if deltas else "-")
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
    log.info("POGLS REST server (S33) starting — geo_backend=%s iter=%s multi=%s llm=%s port=%d",
             _GEO_BACKEND, _HAS_ITER, _HAS_MULTI, _HAS_LLM_ADAPTER, _PORT)
    _get_ctx("default")
    yield
    log.info("POGLS REST server shutdown")

app = FastAPI(
    title="POGLS v4 REST API (S33)",
    version="1.33.0",
    description="streaming reconstruct | multi-layer batch | chunk window | LLM enrich",
    lifespan=lifespan,
)

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
    name:      str
    chunk_lo:  int = 0
    chunk_hi:  int = CHUNK_FILTER_OFF  # 0xFFFF = no window

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
        "X-Layer-Count":       str(len(reqs)),
        "X-Layer-Names":       ",".join(r["name"] for r in reqs),
        "X-Total-Chunks-Est":  str(total_chunks_est),
        "X-Chunk-Window":      chunk_window_str,
        "X-Val-Range":         val_range_str,
        "X-Filter-Flags":      flags_str,
        "X-Spoke-Mask":        hex(spoke_mask),
        "X-Multi-Range":       str(_HAS_MULTI).lower(),
        "X-Geo-Backend":       _GEO_BACKEND,
        "X-Handle-Id":         body.handle_id,
        "X-LLM-Enriched":      str(body.enrich and _HAS_LLM_ADAPTER).lower(),
        "X-Wire-Format":        "msgpack" if (body.binary and _HAS_MSGPACK) else "ndjson",
        "X-Route-Mode":         str(body.route_mode and _HAS_SCORE_ROUTER).lower(),
    }

    log.info(
        "wallet/reconstruct handle=%s layers=%d chunks_est=%d "
        "chunk_win=%s val=%s mask=%s enrich=%s binary=%s",
        body.handle_id, len(reqs), total_chunks_est,
        chunk_window_str, val_range_str, hex(spoke_mask), body.enrich, body.binary
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
            binary=body.binary,     # S33-A
            route_mode=body.route_mode,  # S34
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
    }

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

@app.get("/health")
def health():
    geo_s = {}
    try:
        ctx_tmp = GeoNetCtx(); geo_s = ctx_tmp.status(); del ctx_tmp
    except Exception: pass
    return {
        "status": "ok", "lib": _LIB_PATH,
        "contexts": len(_contexts), "version": "S32",
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
        ],
    }

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("rest_server_s32:app", host=_HOST, port=_PORT,
                reload=False, log_level="info")
