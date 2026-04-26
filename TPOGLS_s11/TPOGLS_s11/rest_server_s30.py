"""
rest_server_s30.py — POGLS REST API (S30)
══════════════════════════════════════════
Builds on S29.

S30 changes:
  • POST /wallet/layer
      annotate=true → NDJSON stream now accepts spoke_mask + audit_only
      → filter runs in C before callback → Python sees only matching chunks
  • POST /wallet/layer/load → uses route_layer_fast (unchanged)
  • POST /{ctx_id}/signal_fail
      → geo_net_signal_fail in C → ThirdEye hot_slots++ → state may escalate
      → use after QRPN verify failure to keep .so state in sync with engine
  • /health += filter_pushdown, slot_hot_wired, anomaly_signals
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
    from geo_net_ctypes_s30 import GeoNetCtxFast as GeoNetCtx, _LIB as _GEONET_LIB
    _HAS_GEONET  = True
    _GEO_BACKEND = "C/libgeonet.so" if (_GEONET_LIB is not None) else "Python/fallback"
    _HAS_ITER    = getattr(_GEONET_LIB, "_has_iter", False) if _GEONET_LIB else False
except ImportError:
    try:
        from geo_net_ctypes_s29 import GeoNetCtxFast as GeoNetCtx   # type: ignore
        from geo_net_ctypes_s29 import _LIB as _GEONET_LIB           # type: ignore
        _HAS_GEONET  = True
        _GEO_BACKEND = "C/libgeonet.so" if _GEONET_LIB else "Python/fallback"
        _HAS_ITER    = getattr(_GEONET_LIB, "_has_iter", False) if _GEONET_LIB else False
    except ImportError:
        try:
            from geo_net_py import GeoNetCtx                          # type: ignore
            _HAS_GEONET = True; _GEO_BACKEND = "Python/fallback"
            _GEONET_LIB = None; _HAS_ITER = False
        except ImportError:
            _HAS_GEONET = False; _GEO_BACKEND = "unavailable"
            _GEONET_LIB = None; _HAS_ITER = False

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

def _get_ctx(ctx_id: str) -> CtxRecord:
    if ctx_id not in _contexts:
        _contexts[ctx_id] = CtxRecord(ctx_id)
        log.info("new ctx: %s", ctx_id)
    return _contexts[ctx_id]

# ── NDJSON stream (S30: with filter params) ────────────────────────

def _ndjson_stream(
    geo_ctx,
    file_idx:   int,
    n_bytes:    int,
    layer:      str,
    spoke_mask: int  = 0x3F,
    audit_only: bool = False,
) -> Iterator[bytes]:
    n_chunks   = max(1, (n_bytes + 63) // 64)
    spoke_dist = [0]*6; audit_cnt = 0; emit_n = 0

    for rec in geo_ctx.iter_chunks_stream(
        file_idx, n_bytes,
        spoke_mask=spoke_mask,
        audit_only=audit_only,
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
                        spoke_mask=0x3F, audit_only=False) -> Iterator[bytes]:
    buf = io.BytesIO(); gz = gzip.GzipFile(fileobj=buf, mode="wb")
    for chunk in _ndjson_stream(geo_ctx, file_idx, n_bytes, layer, spoke_mask, audit_only):
        gz.write(chunk); gz.flush()
        buf.seek(0); data = buf.read()
        buf.seek(0); buf.truncate()
        if data: yield data
    gz.close(); buf.seek(0); tail = buf.read()
    if tail: yield tail

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
    log.info("POGLS REST server (S30) starting — geo_backend=%s iter=%s port=%d",
             _GEO_BACKEND, _HAS_ITER, _PORT)
    _get_ctx("default")
    yield
    log.info("POGLS REST server shutdown")

app = FastAPI(
    title="POGLS v4 REST API (S30)",
    version="1.30.0",
    description="slot_hot wired | filter pushdown in C | QRPN feedback loop closed",
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
    # S30: filter pushdown
    spoke_mask: int  = 0x3F   # 0x3F = all spokes; 0x01 = spoke 0 only
    audit_only: bool = False
    ctx_id:     str  = "default"

class SignalFailBody(BaseModel):
    ctx_id: str = "default"
    count:  int = 1           # number of failures to signal in one call


# ── POST /wallet/load (unchanged) ─────────────────────────────────

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


# ── POST /wallet/layer — S30: filter pushdown ─────────────────────

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
        }
        log.info("wallet/layer STREAM ctx=%s layer=%s mask=%s audit=%s compress=%s",
                 body.ctx_id, body.layer, hex(spoke_mask), audit_only, body.compress)
        if body.compress:
            headers["Content-Encoding"] = "gzip"
            return StreamingResponse(
                _ndjson_stream_gzip(rec.geo_ctx, file_idx, arr.nbytes, body.layer,
                                    spoke_mask, audit_only),
                media_type="application/x-ndjson", headers=headers)
        return StreamingResponse(
            _ndjson_stream(rec.geo_ctx, file_idx, arr.nbytes, body.layer,
                           spoke_mask, audit_only),
            media_type="application/x-ndjson", headers=headers)

    # annotate=false: JSON fast path
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


# ── S30: POST /{ctx_id}/signal_fail ───────────────────────────────
# Close the QRPN feedback loop.
# Engine calls this after QRPN verify failure(s) — keeps .so ThirdEye
# state consistent with real engine pressure.

@app.post("/{ctx_id}/signal_fail")
def signal_fail(ctx_id: str, body: SignalFailBody):
    rec = _get_ctx(ctx_id)
    if rec.geo_ctx is None:
        raise HTTPException(503, "geo_ctx not available")
    count = max(1, min(body.count, 256))   # cap at 256 signals per call
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
        "contexts": len(_contexts), "version": "S30",
        "wallet_stack":      _HAS_WALLET,
        "geo_net":           _HAS_GEONET,
        "geo_backend":       _GEO_BACKEND,
        "iter_streaming":    _HAS_ITER,
        "filter_pushdown":   geo_s.get("filter_pushdown", False),
        "slot_hot_wired":    geo_s.get("slot_hot_wired",  False),
        "ndjson_batch":      _NDJSON_BATCH,
        "endpoints": [
            "POST /encode", "POST /decode", "GET  /read",
            "POST /wallet/load",
            "POST /wallet/layer  [annotate=false → JSON | annotate=true → NDJSON]",
            "POST /wallet/layer  [S30: spoke_mask + audit_only filter in C]",
            "POST /wallet/layer  [compress=true → gzip NDJSON]",
            "POST /{ctx_id}/signal_fail  [S30: QRPN feedback → ThirdEye hot_slots++]",
            "GET  /{ctx_id}/status", "POST /{ctx_id}/llm_report",
            "GET  /{ctx_id}/snapshot", "GET  /llm_stats",
        ],
    }

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("rest_server_s30:app", host=_HOST, port=_PORT,
                reload=False, log_level="info")
