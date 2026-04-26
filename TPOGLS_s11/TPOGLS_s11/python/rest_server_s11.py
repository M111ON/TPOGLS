"""
rest_server_s11.py — POGLS v4 REST API (S11)
══════════════════════════════════════════════
Extends rest_server.py (S18 base) with file-pipeline endpoints:

  POST /encode              → encode raw bytes → POGLS blob (base64)
  POST /decode              → decode blob (base64) → original bytes (base64)
  GET  /read?blob=&offset=&len=  → partial byte slice from blob

All existing S18 endpoints unchanged:
  GET  /{ctx_id}/status
  POST /{ctx_id}/llm_report
  GET  /{ctx_id}/snapshot
  GET  /llm_stats
  GET  /health

Env:
  POGLS_LIB    default: /mnt/c/TPOGLS/libpogls_v4.so
  POGLS_HOST   default: 0.0.0.0
  POGLS_PORT   default: 8765

Run:
  uvicorn rest_server_s11:app --host 0.0.0.0 --port 8765 --reload
"""

from __future__ import annotations

import base64
import json
import logging
import os
import time
from contextlib import asynccontextmanager
from typing import Any, Optional

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel

from pogls_engine import PoglsEngine, BLOCK_SLOTS

try:
    from llm_advisor import LLMAdvisor, Act
    _HAS_ADVISOR = True
except ImportError:
    _HAS_ADVISOR = False
    logging.warning("llm_advisor not found — /llm_report will return NOOP")

log = logging.getLogger("pogls.server")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-8s %(name)s — %(message)s",
)

# ═══════════════════════════════════════════════════════════════
# CONFIG
# ═══════════════════════════════════════════════════════════════

_LIB_PATH = os.environ.get("POGLS_LIB",  "/mnt/c/TPOGLS/libpogls_v4.so")
_HOST     = os.environ.get("POGLS_HOST", "0.0.0.0")
_PORT     = int(os.environ.get("POGLS_PORT", 8765))

_TRIG_KV_LOAD = int(os.environ.get("LLM_TRIG_KV_LOAD", 75))
_TRIG_GPU_OVF = int(os.environ.get("LLM_TRIG_GPU_OVF",  0))
_TRIG_AUDIT   = int(os.environ.get("LLM_TRIG_AUDIT",    1))
_TRIG_TOMB    = int(os.environ.get("LLM_TRIG_TOMB",    30))

_POLICY_MASKS = {
    "KV_LOAD_HIGH":   (1 << Act.REHASH_NOW)       | (1 << Act.COMPACT_TOMBSTONE) if _HAS_ADVISOR else 0,
    "GPU_OVERFLOW":   (1 << Act.FLUSH_GPU_QUEUE)   | (1 << Act.DEGRADE_MODE)      if _HAS_ADVISOR else 0,
    "TOMB_HIGH":      (1 << Act.COMPACT_TOMBSTONE)                                 if _HAS_ADVISOR else 0,
    "AUDIT_DEGRADED": (1 << Act.DEGRADE_MODE)      | (1 << Act.RESET_CACHE_WINDOW) if _HAS_ADVISOR else 0,
}

# ═══════════════════════════════════════════════════════════════
# CONTEXT REGISTRY  (unchanged from S18)
# ═══════════════════════════════════════════════════════════════

class CtxRecord:
    __slots__ = ("ctx_id", "created_at", "total_writes", "total_reads",
                 "total_encodes", "epoch", "degrade_active")

    def __init__(self, ctx_id: str):
        self.ctx_id        = ctx_id
        self.created_at    = time.time()
        self.total_writes  = 0
        self.total_reads   = 0
        self.total_encodes = 0
        self.epoch         = 0
        self.degrade_active = False

    def to_status(self) -> dict:
        return {
            "ctx_id":        self.ctx_id,
            "epoch":         self.epoch,
            "total_ops":     self.total_writes + self.total_reads,
            "total_writes":  self.total_writes,
            "total_reads":   self.total_reads,
            "total_encodes": self.total_encodes,
            "drift_active":  False,
            "writes_frozen": False,
            "degrade_active": self.degrade_active,
            "shatter_count": 0,
            "lane_b_active": 0,
            "qrpn_state":    0,
            "qrpn_fails":    0,
            "gpu_fail_count": 0,
            "shat_stage":    0,
            "block_slots":   BLOCK_SLOTS,
            "lib_path":      _LIB_PATH,
            "uptime_s":      round(time.time() - self.created_at, 1),
        }


_contexts: dict[str, CtxRecord] = {}
_advisor: Optional[Any] = LLMAdvisor() if _HAS_ADVISOR else None


def _get_ctx(ctx_id: str) -> CtxRecord:
    if ctx_id not in _contexts:
        _contexts[ctx_id] = CtxRecord(ctx_id)
        log.info("new ctx: %s", ctx_id)
    return _contexts[ctx_id]


# ═══════════════════════════════════════════════════════════════
# LLM HELPERS  (unchanged from S18)
# ═══════════════════════════════════════════════════════════════

def _snap_from_ctx(rec: CtxRecord) -> dict:
    try:
        with PoglsEngine() as eng:
            raw = eng.stats_out()
        return {
            "kv": {
                "load_pct":  raw["kv_load_pct_x100"] / 100.0,
                "tomb_pct":  raw["kv_tomb_pct_x100"] / 100.0,
            },
            "gpu": {
                "overflow_pct": (
                    100.0 * raw["gpu_ring_pending"] /
                    max(1, raw["gpu_ring_pending"] + raw["gpu_ring_flushed"])
                ),
            },
            "audit": {
                "health":     int(rec.degrade_active),
                "l1_hit_pct": raw["l1_hit_pct_x100"] / 100.0,
                "qrpns":      raw["ctx_qrpns"],
            },
            "hydra": {"fill_pct": 0},
            "_source": "engine",
        }
    except Exception as e:
        log.warning("_snap_from_ctx: engine unavailable (%s)", e)
        load = min(100, int(rec.total_writes * 100 / (BLOCK_SLOTS or 1)))
        return {
            "kv":    {"load_pct": load, "tomb_pct": 0},
            "gpu":   {"overflow_pct": rec.total_encodes % 5},
            "audit": {"health": int(rec.degrade_active)},
            "hydra": {"fill_pct": 0},
            "_source": "proxy",
        }


def _allowed_mask(snap: dict) -> int:
    if not _HAS_ADVISOR:
        return 1
    mask = 1
    kv  = snap.get("kv",    {})
    gpu = snap.get("gpu",   {})
    aud = snap.get("audit", {})
    if kv.get("load_pct",     0) > _TRIG_KV_LOAD: mask |= _POLICY_MASKS["KV_LOAD_HIGH"]
    if gpu.get("overflow_pct", 0) > _TRIG_GPU_OVF: mask |= _POLICY_MASKS["GPU_OVERFLOW"]
    if kv.get("tomb_pct",      0) > _TRIG_TOMB:    mask |= _POLICY_MASKS["TOMB_HIGH"]
    if aud.get("health",       0) >= _TRIG_AUDIT:  mask |= _POLICY_MASKS["AUDIT_DEGRADED"]
    return mask


def _state_key(snap: dict) -> str:
    kv  = snap.get("kv",    {})
    gpu = snap.get("gpu",   {})
    aud = snap.get("audit", {})
    load = (int(kv.get("load_pct",  0)) // 5) * 5
    tomb = (int(kv.get("tomb_pct",  0)) // 5) * 5
    ovf  =  1 if gpu.get("overflow_pct", 0) > 0 else 0
    hlth =  int(aud.get("health", 0))
    return f"KL{load:02d}T{tomb:02d}G{ovf}H{hlth}"


# ═══════════════════════════════════════════════════════════════
# PIPELINE HELPERS  (S11 new)
# ═══════════════════════════════════════════════════════════════

def _engine_encode(data: bytes) -> bytes:
    """Raw bytes → POGLS blob bytes using PoglsEngine block-cycling."""
    with PoglsEngine() as eng:
        eng.encode(data)
        recovered = eng.decode()
    # blob = recovered bytes (block-reassembled, padded to 8B boundary)
    # We prefix with a 4-byte orig_len so /decode can trim correctly
    import struct
    hdr  = struct.pack("<I", len(data))   # 4B little-endian orig size
    return hdr + recovered


def _engine_decode(blob: bytes) -> bytes:
    """POGLS blob bytes → original bytes."""
    import struct
    if len(blob) < 4:
        raise ValueError("blob too short")
    orig_len = struct.unpack("<I", blob[:4])[0]
    payload  = blob[4:]
    # re-encode to populate engine, decode, then trim
    with PoglsEngine() as eng:
        eng.encode(payload)
        out = eng.decode()
    return out[:orig_len]


# ═══════════════════════════════════════════════════════════════
# APP
# ═══════════════════════════════════════════════════════════════

@asynccontextmanager
async def lifespan(app: FastAPI):
    log.info("POGLS REST server (S11) starting — lib=%s port=%d", _LIB_PATH, _PORT)
    _get_ctx("default")
    yield
    log.info("POGLS REST server shutdown")


app = FastAPI(
    title="POGLS v4 REST API (S11)",
    version="1.1.0",
    description="Geometric storage engine — encode/decode/read pipeline endpoints",
    lifespan=lifespan,
)


# ═══════════════════════════════════════════════════════════════
# REQUEST MODELS
# ═══════════════════════════════════════════════════════════════

class EncodeBody(BaseModel):
    data_b64: str          # base64-encoded raw bytes to encode
    ctx_id:   str = "default"

class DecodeBody(BaseModel):
    blob_b64: str          # base64-encoded POGLS blob
    ctx_id:   str = "default"

class LLMReportBody(BaseModel):
    snap:  Optional[dict] = None
    force: bool           = False


# ═══════════════════════════════════════════════════════════════
# ROUTES — S11 NEW
# ═══════════════════════════════════════════════════════════════

@app.post("/encode")
def encode_endpoint(body: EncodeBody):
    """
    Encode raw bytes → POGLS blob.

    Request:  { "data_b64": "<base64 raw bytes>", "ctx_id": "..." }
    Response: { "blob_b64": "<base64 blob>", "orig_bytes": N,
                "blob_bytes": M, "chunks": K }
    """
    try:
        data = base64.b64decode(body.data_b64)
    except Exception:
        raise HTTPException(400, "data_b64: invalid base64")

    if len(data) == 0:
        raise HTTPException(400, "data is empty")

    t0 = time.monotonic()
    try:
        blob = _engine_encode(data)
    except Exception as e:
        log.error("encode failed: %s", e)
        raise HTTPException(500, f"encode error: {e}")
    elapsed_ms = round((time.monotonic() - t0) * 1000, 2)

    rec = _get_ctx(body.ctx_id)
    rec.total_encodes += 1
    rec.total_writes  += len(data) // 8

    log.info("encode ctx=%s orig=%d blob=%d ms=%.1f",
             body.ctx_id, len(data), len(blob), elapsed_ms)

    return JSONResponse({
        "blob_b64":   base64.b64encode(blob).decode(),
        "orig_bytes": len(data),
        "blob_bytes": len(blob),
        "chunks":     (len(data) + 7) // 8,
        "elapsed_ms": elapsed_ms,
        "ctx_id":     body.ctx_id,
    })


@app.post("/decode")
def decode_endpoint(body: DecodeBody):
    """
    Decode POGLS blob → original bytes.

    Request:  { "blob_b64": "<base64 blob>", "ctx_id": "..." }
    Response: { "data_b64": "<base64 original bytes>", "orig_bytes": N }
    """
    try:
        blob = base64.b64decode(body.blob_b64)
    except Exception:
        raise HTTPException(400, "blob_b64: invalid base64")

    t0 = time.monotonic()
    try:
        data = _engine_decode(blob)
    except ValueError as e:
        raise HTTPException(400, str(e))
    except Exception as e:
        log.error("decode failed: %s", e)
        raise HTTPException(500, f"decode error: {e}")
    elapsed_ms = round((time.monotonic() - t0) * 1000, 2)

    rec = _get_ctx(body.ctx_id)
    rec.total_reads += len(data) // 8

    log.info("decode ctx=%s orig=%d ms=%.1f",
             body.ctx_id, len(data), elapsed_ms)

    return JSONResponse({
        "data_b64":   base64.b64encode(data).decode(),
        "orig_bytes": len(data),
        "elapsed_ms": elapsed_ms,
        "ctx_id":     body.ctx_id,
    })


@app.get("/read")
def read_endpoint(
    blob_b64: str   = Query(..., description="base64-encoded POGLS blob"),
    offset:   int   = Query(0,   ge=0, description="byte offset into original data"),
    length:   int   = Query(..., gt=0, description="number of bytes to read"),
    ctx_id:   str   = Query("default"),
):
    """
    Partial read from POGLS blob.

    GET /read?blob_b64=<b64>&offset=0&length=256&ctx_id=default

    Response: { "data_b64": "<base64 slice>", "offset": N, "length": M,
                "orig_bytes": total, "clamped": bool }
    """
    try:
        blob = base64.b64decode(blob_b64)
    except Exception:
        raise HTTPException(400, "blob_b64: invalid base64")

    t0 = time.monotonic()
    try:
        data = _engine_decode(blob)
    except ValueError as e:
        raise HTTPException(400, str(e))
    except Exception as e:
        log.error("read/decode failed: %s", e)
        raise HTTPException(500, f"decode error: {e}")

    orig_len = len(data)
    if offset >= orig_len:
        raise HTTPException(416, f"offset {offset} >= orig_bytes {orig_len}")

    end     = min(offset + length, orig_len)
    clamped = (end < offset + length)
    chunk   = data[offset:end]
    elapsed_ms = round((time.monotonic() - t0) * 1000, 2)

    rec = _get_ctx(ctx_id)
    rec.total_reads += 1

    log.info("read ctx=%s offset=%d len=%d orig=%d ms=%.1f",
             ctx_id, offset, len(chunk), orig_len, elapsed_ms)

    return JSONResponse({
        "data_b64":   base64.b64encode(chunk).decode(),
        "offset":     offset,
        "length":     len(chunk),
        "orig_bytes": orig_len,
        "clamped":    clamped,
        "elapsed_ms": elapsed_ms,
        "ctx_id":     ctx_id,
    })


# ═══════════════════════════════════════════════════════════════
# ROUTES — existing (S18 unchanged)
# ═══════════════════════════════════════════════════════════════

@app.get("/{ctx_id}/status")
def status(ctx_id: str):
    rec = _get_ctx(ctx_id)
    rec.epoch += 1
    engine_live = False
    try:
        with PoglsEngine() as eng:
            eng.stats()
        engine_live = True
    except Exception as e:
        log.warning("engine probe failed: %s", e)
    st = rec.to_status()
    st["engine_live"] = engine_live
    return JSONResponse(st)


@app.post("/{ctx_id}/llm_report")
def llm_report(ctx_id: str, body: LLMReportBody):
    rec  = _get_ctx(ctx_id)
    snap = body.snap if body.snap is not None else _snap_from_ctx(rec)
    state_key    = _state_key(snap)
    allowed_mask = _allowed_mask(snap)

    if not _HAS_ADVISOR:
        return JSONResponse({
            "state_key":      state_key,
            "action":         "NOOP",
            "action_code":    0,
            "confidence":     1.0,
            "from_cache":     False,
            "allowed_actions": ["NOOP"],
            "note":           "llm_advisor not installed",
        })

    if body.force:
        _advisor._cache.invalidate(state_key)

    result        = _advisor.query(state_key, allowed_mask, json.dumps(snap))
    allowed_names = Act.from_mask(allowed_mask)

    if result.action == Act.DEGRADE_MODE:
        rec.degrade_active = True
    elif result.action == Act.RECOVER_MODE:
        rec.degrade_active = False

    return JSONResponse({
        "state_key":      state_key,
        "action":         Act.name(result.action),
        "action_code":    int(result.action),
        "confidence":     round(float(result.confidence), 4),
        "from_cache":     bool(result.from_cache),
        "allowed_actions": allowed_names,
        "snap_used":      snap,
    })


@app.get("/{ctx_id}/snapshot")
def snapshot(ctx_id: str):
    rec = _get_ctx(ctx_id)
    return JSONResponse({
        "ctx_id":    ctx_id,
        "status":    rec.to_status(),
        "snap":      _snap_from_ctx(rec),
        "state_key": _state_key(_snap_from_ctx(rec)),
        "contexts":  list(_contexts.keys()),
        "ts":        time.time(),
    })


@app.get("/llm_stats")
def llm_stats():
    if not _HAS_ADVISOR:
        return JSONResponse({"note": "advisor not loaded"})
    return JSONResponse(_advisor.stats())


@app.get("/health")
def health():
    return {
        "status":   "ok",
        "lib":      _LIB_PATH,
        "contexts": len(_contexts),
        "version":  "S11",
        "endpoints": [
            "POST /encode",
            "POST /decode",
            "GET  /read",
            "GET  /{ctx_id}/status",
            "POST /{ctx_id}/llm_report",
            "GET  /{ctx_id}/snapshot",
            "GET  /llm_stats",
        ],
    }


# ═══════════════════════════════════════════════════════════════
# ENTRYPOINT
# ═══════════════════════════════════════════════════════════════

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("rest_server_s11:app", host=_HOST, port=_PORT,
                reload=False, log_level="info")
