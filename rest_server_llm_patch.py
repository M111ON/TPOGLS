"""
rest_server_llm_patch.py — LLM Advisor endpoints for rest_server.py
══════════════════════════════════════════════════════════════════════
PATCH — add these 2 blocks into rest_server.py:

  BLOCK A: imports + advisor init  (after existing imports)
  BLOCK B: 2 new routes            (before if __name__ == "__main__")

New endpoints:
  POST /<ctx_id>/llm_report   → ask advisor, get action + confidence
  GET  /llm_stats             → cache hits, call count, guard rejections

══════════════════════════════════════════════════════════════════════
"""

# ══════════════════════════════════════════════════════════════════════
# BLOCK A — paste after existing imports in rest_server.py
# ══════════════════════════════════════════════════════════════════════

import json as _json
from llm_advisor import LLMAdvisor, Act

# Trigger thresholds (mirror of pogls_llm_hook.h — keep in sync)
_LLM_TRIG_KV_LOAD   = int(os.environ.get("LLM_TRIG_KV_LOAD",   75))
_LLM_TRIG_GPU_OVF   = int(os.environ.get("LLM_TRIG_GPU_OVF",    0))
_LLM_TRIG_AUDIT     = int(os.environ.get("LLM_TRIG_AUDIT",       1))
_LLM_TRIG_TOMB      = int(os.environ.get("LLM_TRIG_TOMB",       30))

# Action → allowed mask table (mirrors POGLS_STATE_POLICIES in .h)
_POLICY_MASKS = {
    "KV_LOAD_HIGH":   (1<<Act.REHASH_NOW)       | (1<<Act.COMPACT_TOMBSTONE),
    "GPU_OVERFLOW":   (1<<Act.FLUSH_GPU_QUEUE)   | (1<<Act.DEGRADE_MODE),
    "TOMB_HIGH":      (1<<Act.COMPACT_TOMBSTONE),
    "AUDIT_DEGRADED": (1<<Act.DEGRADE_MODE)      | (1<<Act.RESET_CACHE_WINDOW),
}

_advisor = LLMAdvisor()   # singleton — one per server process


def _build_snap_dict(st) -> dict:
    """
    Convert PoglsStatus (from pogls_v4.py ctypes wrapper) →
    snap dict keyed to match PoglsAdminSnapshot JSON fields exactly.

    Field mapping (pogls_status_t → AdminKVSnap / AdminGPUSnap / AdminAuditSnap):

      kv.load_pct   ← derived: (total_ops % KV_CAPACITY) / KV_CAPACITY * 100
                      NOTE: pogls_status_t has no direct kv fields — best proxy
                      is total_ops as write pressure indicator. Replace with
                      real AdminKVSnap when pogls_admin_snapshot_json() is
                      plumbed through REST (see TODO below).
      kv.tomb_pct   ← qrpn_fails as tombstone proxy (QRPN fail = stale entry)
      kv.live       ← not in pogls_status_t — set 0 until admin snap wired
      kv.tomb       ← not in pogls_status_t — set 0 until admin snap wired

      gpu.overflow_pct ← gpu_fail_count / max(total_ops,1) * 100
      gpu.available    ← gpu_fail_count field present = GPU build

      audit.health  ← qrpn_state: 0=NORMAL→0=OK, 1=STRESSED→1=DEGRADED, 2=ANOMALY→2=OFFLINE
                       (exact enum match between qrpn_state and audit_health_t)

      hydra.fill_pct ← lane_b_active / 252 * 100  (World B lane saturation proxy)

    TODO: when pogls_admin_snapshot_json() is exposed via REST or shared memory,
    replace this function with a direct JSON parse of the real snapshot.
    Fields to use:
      kv.load_pct   → snap["kv"]["load_pct"]
      kv.tomb_pct   → snap["kv"]["tomb_pct"]
      gpu.overflow_pct → snap["gpu"]["overflow_pct"]
      audit.health  → snap["audit"]["health"]
      hydra.fill_pct → snap["hydra"]["fill_pct"]
    """
    total_ops     = int(getattr(st, "total_ops",      0))
    qrpn_fails    = int(getattr(st, "qrpn_fails",     0))
    gpu_fail_count= int(getattr(st, "gpu_fail_count", 0))
    qrpn_state    = int(getattr(st, "qrpn_state",     0))
    lane_b_active = int(getattr(st, "lane_b_active",  0))
    writes_frozen = int(getattr(st, "writes_frozen",  0))
    shat_stage    = int(getattr(st, "shat_stage",     0))

    # kv.load_pct: use total_ops mod 8192 (one Hilbert cycle) as pressure ring
    KV_CAPACITY   = 5734   # matches kv_load_pct = live/5734*100 in AdminKVSnap
    kv_load_pct   = min(100.0, (total_ops % KV_CAPACITY) / KV_CAPACITY * 100.0)

    # kv.tomb_pct: QRPN fail rate as stale-entry proxy
    # qrpn_fails / max(total_ops,1) * 100, capped at 100
    tomb_pct      = min(100.0, qrpn_fails / max(total_ops, 1) * 100.0)

    # gpu.overflow_pct: GPU fail count / total ops * 100
    gpu_overflow  = min(100.0, gpu_fail_count / max(total_ops, 1) * 100.0)

    # audit.health: direct enum map qrpn_state → audit_health_t
    # 0=NORMAL→OK(0), 1=STRESSED→DEGRADED(1), 2=ANOMALY→OFFLINE(2)
    audit_health  = min(2, qrpn_state)

    # hydra.fill_pct: World B lane saturation (252 = max World B lanes)
    hydra_fill    = min(100.0, lane_b_active / 252.0 * 100.0)

    return {
        "kv": {
            "load_pct":    round(kv_load_pct, 2),
            "tomb_pct":    round(tomb_pct,    2),
            "live":        0,       # not in pogls_status_t
            "tomb":        0,       # not in pogls_status_t
            "flush_count": 0,
            "ring_backlog": [0, 0, 0, 0],
        },
        "gpu": {
            "overflow_pct": round(gpu_overflow, 2),
            "available":    1 if gpu_fail_count >= 0 else 0,
            "total_sent":   total_ops,
        },
        "audit": {
            "health":           audit_health,
            "tile_count":       0,
            "total_anomalies":  qrpn_fails,
            "signal_queue_depth": 0,
        },
        "hydra": {
            "fill_pct":     round(hydra_fill, 2),
            "active_count": lane_b_active,
        },
        "pipeline": {
            "total_ops":   total_ops,
            "qrpn_fails":  qrpn_fails,
            "writes_frozen": writes_frozen,
            "shat_stage":  shat_stage,
        },
    }


def _allowed_mask_from_snap(snap: dict) -> int:
    mask = 1  # always include NOOP (bit 0)
    kv   = snap.get("kv",    {})
    gpu  = snap.get("gpu",   {})
    aud  = snap.get("audit", {})

    if kv.get("load_pct",    0) > _LLM_TRIG_KV_LOAD: mask |= _POLICY_MASKS["KV_LOAD_HIGH"]
    if gpu.get("overflow_pct",0) > _LLM_TRIG_GPU_OVF: mask |= _POLICY_MASKS["GPU_OVERFLOW"]
    if kv.get("tomb_pct",    0) > _LLM_TRIG_TOMB:     mask |= _POLICY_MASKS["TOMB_HIGH"]
    if aud.get("health",     0) >= _LLM_TRIG_AUDIT:   mask |= _POLICY_MASKS["AUDIT_DEGRADED"]
    return mask


def _state_key_from_snap(snap: dict) -> str:
    kv  = snap.get("kv",    {})
    gpu = snap.get("gpu",   {})
    aud = snap.get("audit", {})
    load = (int(kv.get("load_pct",  0)) // 5) * 5
    tomb = (int(kv.get("tomb_pct",  0)) // 5) * 5
    ovf  =  1 if gpu.get("overflow_pct", 0) > 0 else 0
    hlth =  int(aud.get("health", 0))
    return f"KL{load:02d}T{tomb:02d}G{ovf}H{hlth}"


# ══════════════════════════════════════════════════════════════════════
# BLOCK B — paste before  if __name__ == "__main__"  in rest_server.py
# ══════════════════════════════════════════════════════════════════════

from flask import Flask, request, jsonify, abort   # already imported — shown for clarity

# app and _contexts already exist in rest_server.py — do NOT re-declare


@app.post("/<ctx_id>/llm_report")
def llm_report(ctx_id):
    """
    Ask the LLM advisor what action to take for this context.

    Optional JSON body:
      {"snap": {...}}   — override snapshot (for testing / GUI injection)
      {"force": true}   — bypass cache, always call LLM

    Response:
      {
        "state_key":  "KL80T10G0H0",
        "action":     "REHASH_NOW",
        "action_code": 1,
        "confidence": 0.92,
        "from_cache": false,
        "allowed_actions": ["NOOP","REHASH_NOW","COMPACT_TOMBSTONE"]
      }
    """
    ctx  = _get_ctx(ctx_id)
    body = request.get_json(silent=True) or {}

    # get snapshot — caller can inject custom snap for testing
    if "snap" in body:
        snap = body["snap"]
    else:
        try:
            st   = ctx.status()
            snap = _build_snap_dict(st)
        except PoglsError as e:
            return _err_response(e)

    state_key    = _state_key_from_snap(snap)
    allowed_mask = _allowed_mask_from_snap(snap)

    # force = bypass cache
    if body.get("force"):
        _advisor._cache._store.pop(state_key, None)

    result = _advisor.query(state_key, allowed_mask, _json.dumps(snap))

    return jsonify({
        "state_key":      state_key,
        "action":         Act.name(result.action),
        "action_code":    result.action,
        "confidence":     round(float(result.confidence), 4),
        "from_cache":     bool(result.from_cache),
        "allowed_actions": Act.from_mask(allowed_mask),
        "snap_used":      snap,
    })


@app.get("/llm_stats")
def llm_stats():
    """
    Global LLM advisor statistics across all contexts.

    Response:
      {
        "llm_calls":    12,
        "llm_errors":   0,
        "cache": {"size": 8, "total_hits": 47}
      }
    """
    return jsonify(_advisor.stats())


# ══════════════════════════════════════════════════════════════════════
# STANDALONE: run this file directly to test endpoints without C layer
# ══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    import os, sys
    logging.basicConfig(level=logging.DEBUG)

    # mock _get_ctx and PoglsError so we can run without C library
    class _MockStatus:
        total_ops = 80; qrpn_fails = 12; gpu_fail_count = 1
        qrpn_state = 2; shat_stage = 0; drift_active = 0
        writes_frozen = 0; shatter_count = 0; lane_b_active = 4; epoch = 99

    class _MockCtx:
        def status(self): return _MockStatus()

    class PoglsError(Exception):
        code = -1

    def _err_response(e): return jsonify({"error": str(e)}), 400
    def _get_ctx(ctx_id): return _MockCtx()

    app = Flask(__name__)

    # re-register routes with mock deps in scope
    app.add_url_rule("/<ctx_id>/llm_report", "llm_report", llm_report, methods=["POST"])
    app.add_url_rule("/llm_stats",            "llm_stats",  llm_stats,  methods=["GET"])

    PORT = int(os.environ.get("POGLS_PORT", 8765))
    log.info("LLM patch standalone on :%d", PORT)
    app.run(host="0.0.0.0", port=PORT, debug=True)
