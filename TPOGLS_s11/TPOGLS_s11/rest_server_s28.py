"""
rest_server_s28.py — POGLS REST API (S28)
══════════════════════════════════════════
Builds on S27. One change: GeoNetCtx → GeoNetCtxFast (libgeonet.so).

S28 upgrades:
  • /wallet/load  → _route_layer_stateful replaced by ctx.route_layer_fast()
  • /wallet/layer → _route_layer_annotated replaced by ctx.route_layer_annotated_fast()
  • response adds "verify_pass" field (C verify in same loop)
  • /health reports geo_backend: "C/libgeonet.so" or "Python/fallback"

All S27 logic unchanged — only backend swapped.

Drop-in: copy next to geo_net_ctypes.py + libgeonet.so
"""

from __future__ import annotations
import base64, json, logging, os, time, struct
from contextlib import asynccontextmanager
from typing import Any, Optional

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import JSONResponse
from pydantic import BaseModel

# ── S28: prefer ctypes-backed GeoNetCtx, fallback to pure Python ──
try:
    from geo_net_ctypes import GeoNetCtxFast as GeoNetCtx, _LIB as _GEONET_LIB
    _HAS_GEONET = True
    _GEO_BACKEND = "C/libgeonet.so" if (_GEONET_LIB is not None) else "Python/fallback"
except ImportError:
    try:
        from geo_net_py import GeoNetCtx        # type: ignore[no-redef]
        _HAS_GEONET = True
        _GEO_BACKEND = "Python/fallback"
        _GEONET_LIB  = None
    except ImportError:
        _HAS_GEONET  = False
        _GEO_BACKEND = "unavailable"
        _GEONET_LIB  = None

try:
    from pogls_engine import PoglsEngine, BLOCK_SLOTS
    _HAS_ENGINE = True
except ImportError:
    _HAS_ENGINE = False
    BLOCK_SLOTS = 576
    class PoglsEngine:                                   # type: ignore[no-redef]
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
    logging.warning("wallet stack not found — /wallet/* endpoints disabled (%s)", _we)

try:
    from llm_advisor import LLMAdvisor, Act
    _HAS_ADVISOR = True
except ImportError:
    _HAS_ADVISOR = False

log = logging.getLogger("pogls.server")
logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)-8s %(name)s — %(message)s")

_LIB_PATH = os.environ.get("POGLS_LIB",  "/mnt/c/TPOGLS/libpogls_v4.so")
_HOST     = os.environ.get("POGLS_HOST", "0.0.0.0")
_PORT     = int(os.environ.get("POGLS_PORT", 8765))
_TRIG_KV_LOAD = int(os.environ.get("LLM_TRIG_KV_LOAD", 75))
_TRIG_GPU_OVF = int(os.environ.get("LLM_TRIG_GPU_OVF",  0))
_TRIG_AUDIT   = int(os.environ.get("LLM_TRIG_AUDIT",    1))
_TRIG_TOMB    = int(os.environ.get("LLM_TRIG_TOMB",    30))
_POLICY_MASKS = {k: 0 for k in ("KV_LOAD_HIGH","GPU_OVERFLOW","TOMB_HIGH","AUDIT_DEGRADED")}
if _HAS_ADVISOR:
    _POLICY_MASKS = {
        "KV_LOAD_HIGH":   (1<<Act.REHASH_NOW)     |(1<<Act.COMPACT_TOMBSTONE),
        "GPU_OVERFLOW":   (1<<Act.FLUSH_GPU_QUEUE) |(1<<Act.DEGRADE_MODE),
        "TOMB_HIGH":      (1<<Act.COMPACT_TOMBSTONE),
        "AUDIT_DEGRADED": (1<<Act.DEGRADE_MODE)    |(1<<Act.RESET_CACHE_WINDOW),
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
            "total_ops": self.total_writes + self.total_reads,
            "total_writes": self.total_writes, "total_reads": self.total_reads,
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
_advisor: Optional[Any] = (LLMAdvisor() if _HAS_ADVISOR else None)

def _get_ctx(ctx_id: str) -> CtxRecord:
    if ctx_id not in _contexts:
        _contexts[ctx_id] = CtxRecord(ctx_id)
        log.info("new ctx: %s", ctx_id)
    return _contexts[ctx_id]

# ── LLM helpers (unchanged) ────────────────────────────────────────

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

# ── pipeline helpers (unchanged) ───────────────────────────────────

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

# ── wallet helpers ─────────────────────────────────────────────────

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
    log.info("POGLS REST server (S28) starting — geo_backend=%s port=%d",
             _GEO_BACKEND, _PORT)
    _get_ctx("default")
    yield
    log.info("POGLS REST server shutdown")

app = FastAPI(
    title="POGLS v4 REST API (S28)",
    version="1.28.0",
    description="GeoNet .so hot path — route_layer_fast + verify_batch in C",
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
    annotate: bool = False       # if True → include chunk_map in response
    ctx_id: str = "default"


# ── S28 UPGRADED: POST /wallet/load ───────────────────────────────
# Uses ctx.route_layer_fast() → C loop + C verify in one shot.
# Response now includes verify_pass per layer.

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
                # S28: C fast path — route + verify in one C loop
                info["routing"] = rec.geo_ctx.route_layer_fast(i, arr.nbytes)
            layers[name] = info
    except Exception as e:
        log.error("wallet/load failed: %s", e); raise HTTPException(500, f"load error: {e}")

    elapsed_ms = round((time.monotonic() - t0)*1000, 2)
    rec.total_reads += len(layers)
    log.info("wallet/load ctx=%s layers=%d ms=%.1f qrpn=%s backend=%s",
             body.ctx_id, len(layers), elapsed_ms,
             rec.geo_ctx.state if rec.geo_ctx else "N/A", _GEO_BACKEND)
    resp: dict = {"layers": layers, "n_layers": len(layers),
                  "elapsed_ms": elapsed_ms, "ctx_id": body.ctx_id}
    if _HAS_GEONET and rec.geo_ctx is not None:
        resp["geo_ctx_status"] = rec.geo_ctx.status()
    return JSONResponse(resp)


# ── S28 UPGRADED: POST /wallet/layer ──────────────────────────────
# Uses ctx.route_layer_annotated_fast() → C loop + C verify.
# annotate=true still supported (chunk_map built in Python after C pass).

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
        log.error("wallet/layer failed: %s", e); raise HTTPException(500, f"stream error: {e}")

    routing   = None
    chunk_map = None
    if _HAS_GEONET and rec.geo_ctx is not None:
        # S28: always use annotated_fast (builds chunk_map only if annotate=True)
        summary, cm = rec.geo_ctx.route_layer_annotated_fast(file_idx, arr.nbytes)
        routing = summary
        if body.annotate:
            chunk_map = cm

    elapsed_ms = round((time.monotonic() - t0)*1000, 2)
    rec.total_reads += 1
    log.info("wallet/layer ctx=%s layer=%s annotate=%s qrpn=%s backend=%s ms=%.1f",
             body.ctx_id, body.layer, body.annotate,
             rec.geo_ctx.state if rec.geo_ctx else "N/A", _GEO_BACKEND, elapsed_ms)

    resp: dict = {
        "data_b64": base64.b64encode(arr.tobytes()).decode(),
        "shape": list(arr.shape), "dtype": str(arr.dtype),
        "bytes": arr.nbytes, "layer": body.layer,
        "elapsed_ms": elapsed_ms, "ctx_id": body.ctx_id,
    }
    if rec.geo_ctx is not None:
        resp["geo_state"] = rec.geo_ctx.state
        resp["geo_backend"] = _GEO_BACKEND
    if routing   is not None: resp["routing"]   = routing
    if chunk_map is not None: resp["chunk_map"] = chunk_map
    return JSONResponse(resp)


# ── unchanged endpoints (S23/S27) ──────────────────────────────────

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
    state_key = _state_key(snap); allowed_mask = _allowed_mask(snap)
    if not _HAS_ADVISOR:
        return JSONResponse({"state_key": state_key, "action": "NOOP",
                             "action_code": 0, "confidence": 1.0,
                             "from_cache": False, "allowed_actions": ["NOOP"],
                             "note": "llm_advisor not installed"})
    if body.force: _advisor._cache.invalidate(state_key)
    result = _advisor.query(state_key, allowed_mask, json.dumps(snap))
    if result.action == Act.DEGRADE_MODE:  rec.degrade_active = True
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
    return {
        "status": "ok", "lib": _LIB_PATH,
        "contexts": len(_contexts), "version": "S28",
        "wallet_stack": _HAS_WALLET,
        "geo_net":      _HAS_GEONET,
        "geo_backend":  _GEO_BACKEND,
        "endpoints": [
            "POST /encode", "POST /decode", "GET  /read",
            "POST /wallet/load   [S28: C route_layer_fast + verify_batch]",
            "POST /wallet/layer  [S28: C route_layer_annotated_fast; annotate=true → chunk_map]",
            "GET  /{ctx_id}/status", "POST /{ctx_id}/llm_report",
            "GET  /{ctx_id}/snapshot", "GET  /llm_stats",
        ],
    }

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("rest_server_s28:app", host=_HOST, port=_PORT,
                reload=False, log_level="info")
