"""
rest_server.py — POGLS v4 REST API (FastAPI, S18)
══════════════════════════════════════════════════
Wire: pogls_engine.py (real .so) + llm_advisor_v3.py

Endpoints:
  GET  /{ctx_id}/status       → engine stats
  POST /{ctx_id}/llm_report   → LLM advisor action
  GET  /{ctx_id}/snapshot     → admin dump
  GET  /llm_stats             → advisor global stats

Env:
  POGLS_LIB    default: /mnt/c/TPOGLS/libpogls_v4.so
  POGLS_HOST   default: 0.0.0.0
  POGLS_PORT   default: 8765
  LLM_PRIMARY  default: http://localhost:8082/v1/chat/completions
  LLM_FALLBACK default: http://localhost:8083/v1/chat/completions

Run:
  uvicorn rest_server:app --host 0.0.0.0 --port 8765 --reload
"""

from __future__ import annotations

import json
import logging
import os
import time
from contextlib import asynccontextmanager
from typing import Any, Optional

from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel

# ── POGLS engine ──────────────────────────────────────────────────────
from pogls_engine import PoglsEngine, BLOCK_SLOTS

# ── LLM advisor ───────────────────────────────────────────────────────
# expects llm_advisor_v3.py renamed/symlinked → llm_advisor.py
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

# ══════════════════════════════════════════════════════════════════════
# CONFIG
# ══════════════════════════════════════════════════════════════════════

_LIB_PATH  = os.environ.get("POGLS_LIB",  "/mnt/c/TPOGLS/libpogls_v4.so")
_HOST      = os.environ.get("POGLS_HOST", "0.0.0.0")
_PORT      = int(os.environ.get("POGLS_PORT", 8765))

# LLM trigger thresholds (mirrors pogls_llm_hook.h)
_TRIG_KV_LOAD  = int(os.environ.get("LLM_TRIG_KV_LOAD",  75))
_TRIG_GPU_OVF  = int(os.environ.get("LLM_TRIG_GPU_OVF",   0))
_TRIG_AUDIT    = int(os.environ.get("LLM_TRIG_AUDIT",      1))
_TRIG_TOMB     = int(os.environ.get("LLM_TRIG_TOMB",      30))

_POLICY_MASKS = {
    "KV_LOAD_HIGH":   (1 << Act.REHASH_NOW)       | (1 << Act.COMPACT_TOMBSTONE) if _HAS_ADVISOR else 0,
    "GPU_OVERFLOW":   (1 << Act.FLUSH_GPU_QUEUE)   | (1 << Act.DEGRADE_MODE)      if _HAS_ADVISOR else 0,
    "TOMB_HIGH":      (1 << Act.COMPACT_TOMBSTONE)                                 if _HAS_ADVISOR else 0,
    "AUDIT_DEGRADED": (1 << Act.DEGRADE_MODE)      | (1 << Act.RESET_CACHE_WINDOW) if _HAS_ADVISOR else 0,
}

# ══════════════════════════════════════════════════════════════════════
# CONTEXT REGISTRY
# Per-ctx: keeps a lightweight stats dict (engine is stateless open/close)
# ══════════════════════════════════════════════════════════════════════

class CtxRecord:
    """Lightweight per-context state (engine itself is open-on-demand)."""
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


# ══════════════════════════════════════════════════════════════════════
# LLM HELPERS  (ported from rest_server_llm_patch.py)
# ══════════════════════════════════════════════════════════════════════

def _snap_from_ctx(rec: CtxRecord) -> dict:
    """Build snap dict from live engine stats (PoglsStatsOut via .so).
    Falls back to CtxRecord counters if engine is unavailable.
    """
    try:
        with PoglsEngine() as eng:
            raw = eng.stats_out()          # PoglsStatsOut dict (20 fields)
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
                "health":    int(rec.degrade_active),   # engine has no audit field yet
                "l1_hit_pct": raw["l1_hit_pct_x100"] / 100.0,
                "qrpns":     raw["ctx_qrpns"],
            },
            "hydra": {"fill_pct": 0},
            "_source": "engine",
        }
    except Exception as e:
        log.warning("_snap_from_ctx: engine unavailable, using proxy (%s)", e)
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
    mask = 1  # always NOOP
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


# ══════════════════════════════════════════════════════════════════════
# APP
# ══════════════════════════════════════════════════════════════════════

@asynccontextmanager
async def lifespan(app: FastAPI):
    log.info("POGLS REST server starting — lib=%s port=%d", _LIB_PATH, _PORT)
    _get_ctx("default")          # warm default ctx
    yield
    log.info("POGLS REST server shutdown")


app = FastAPI(
    title="POGLS v4 REST API",
    version="1.0.0",
    description="Geometric storage engine — real .so binding + LLM advisor",
    lifespan=lifespan,
)


# ══════════════════════════════════════════════════════════════════════
# MODELS
# ══════════════════════════════════════════════════════════════════════

class LLMReportBody(BaseModel):
    snap:  Optional[dict] = None   # override snapshot for testing
    force: bool           = False  # bypass advisor cache


# ══════════════════════════════════════════════════════════════════════
# ROUTES
# ══════════════════════════════════════════════════════════════════════

@app.get("/{ctx_id}/status")
def status(ctx_id: str):
    """Engine stats for a context."""
    rec = _get_ctx(ctx_id)
    rec.epoch += 1

    # Probe engine liveness (open → stats → close)
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
    """
    Ask LLM advisor what action to take.

    Body (optional):
      {"snap": {...}}   — override snapshot
      {"force": true}   — bypass cache

    Returns action, confidence, allowed_actions.
    """
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

    # force = bypass cache
    if body.force:
        _advisor._cache.invalidate(state_key)

    result       = _advisor.query(state_key, allowed_mask, json.dumps(snap))
    allowed_names = Act.from_mask(allowed_mask)

    # honour DEGRADE_MODE
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
    """Admin snapshot — full context dump."""
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
    """Global LLM advisor statistics."""
    if not _HAS_ADVISOR:
        return JSONResponse({"note": "advisor not loaded"})
    return JSONResponse(_advisor.stats())


@app.get("/health")
def health():
    return {"status": "ok", "lib": _LIB_PATH, "contexts": len(_contexts)}


# ══════════════════════════════════════════════════════════════════════
# ENTRYPOINT
# ══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("rest_server:app", host=_HOST, port=_PORT, reload=False, log_level="info")
