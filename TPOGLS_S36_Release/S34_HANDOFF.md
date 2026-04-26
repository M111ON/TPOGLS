# POGLS S34 — Handoff
**Status:** ✅ COMPLETE | **Tests:** 10/10 (score_router) + S33 22/22 preserved + S32 76/76 preserved

---

## S34 Summary

### Problem (inherited from S33)
- spoke_mask ใน `/wallet/reconstruct` เป็น global static ทั้ง request
- LLMAdapter ส่ง `score` + `label` ต่อ record แต่ไม่มีใครใช้ signal นั้นสำหรับ routing
- ไม่มี "intelligent pre-filter" — ระบบยังไม่ self-improving

### Solution Delivered

| Component | File | Result |
|---|---|---|
| ScoreRouter | `score_router.py` | ✅ score+label → spoke_mask per record |
| REST wire | `rest_server_s34.py` | ✅ `route_mode=true` activates dynamic routing |
| Summary stats | `_summary.route_stats` | ✅ mask distribution per request |
| Header | `X-Route-Mode` | ✅ true/false |

---

## Architecture

```
request: { enrich: true, route_mode: true }
              ↓
LLMAdapter  → label + score per record          (S33, unchanged)
              ↓
ScoreRouter → spoke_mask per record             (S34 NEW)
              ↓
record["_spoke_mask"] = "0x34"                  stamped on each record
              ↓
_summary["route_stats"] = {"0x34": 120, ...}    distribution in summary
```

### GeoNet Spoke Layout (GEO_SPOKES=6)
```
bit  5    4    3    2    1    0
     S5   S4   S3   S2   S1   S0

Routing table:
  hot     score ≥ 0.7  → 0b110100  (S5,S4,S2)  high-load spokes
  cold    score ≤ 0.3  → 0b000001  (S0)         archive spoke
  audit   any          → 0b111111  (all 6)      broadcast
  anomaly score ≥ 0.5  → 0b100000  (S5)         quarantine spoke
  normal  any          → 0b010010  (S4,S1)      default lanes
  <miss>               → 0b010010               same as normal
```

---

## score_router.py

### Interface
```python
from score_router import ScoreRouter, ScoreRouterConfig, RouteRule

# default rules
router = ScoreRouter()

# single record
mask = router.resolve({"label": "hot", "score": 0.9})   # → 0b110100

# batch (yields record, mask tuples)
for rec, mask in router.route_batch(batch):
    ...

# stats
router.stats()   # → {"resolved": N, "fallback": N}
```

### Custom rules
```python
cfg = ScoreRouterConfig(
    rules=[
        RouteRule("hot",   0.7, 1.0, 0b110100),
        RouteRule("cold",  0.0, 0.3, 0b000001),
        RouteRule("audit", 0.0, 1.0, 0b111111),
    ],
    default_mask = 0b010010,
)
router = ScoreRouter(cfg)
```

### Failure-safe
```
LLM fail     → S33 passthrough (record preserved, _llm_enriched=False)
               ScoreRouter sees no score/label → default_mask applied
router miss  → default_mask = 0b010010
route_mode=false → zero overhead, no router call
```

---

## REST API Changes

### New field in POST /wallet/reconstruct
```json
{
  "handle_id": "wh_xxx",
  "layers": [...],
  "enrich": true,
  "route_mode": true
}
```

### Record fields added (when route_mode=true)
```json
{
  "layer_id": 0,
  "chunk_global": 42,
  "score": 0.91,
  "label": "hot",
  "_llm_enriched": true,
  "_spoke_mask": "0x34"     ← NEW S34
}
```

### Summary fields added
```json
{
  "_summary": true,
  "route_mode": true,
  "route_stats": {          ← NEW S34: mask distribution
    "0x34": 120,
    "0x01": 45,
    "0x3f": 12
  },
  "router_stats": {         ← NEW S34: resolver hit/fallback
    "resolved": 177,
    "fallback": 0
  }
}
```

### New header
```
X-Route-Mode: true
```

---

## Files Changed

| File | Change |
|---|---|
| `score_router.py` | NEW — standalone router module |
| `rest_server_s34.py` | fork from s33 + route_mode wire |

### Unchanged (S33 preserved)
- `geo_kv.h` — kv_put int return
- `kv_bridge.h` — cpu_fail / rehash_retry
- `llm_adapter_s33.py` — classify+annotate
- All S33 endpoints + wire formats

---

## Rebuild
```bash
# No C rebuild needed — S34 is Python-only

# Required deps (same as S33)
pip install httpx        # LLMAdapter
pip install msgpack      # binary wire (optional)

# score_router has zero external deps
```

---

## S35 Candidates

| Candidate | Readiness | Impact |
|---|---|---|
| **Feedback loop** — route_stats → auto-adjust rule thresholds | route_stats in summary ✅ | self-improving rules |
| **Per-layer route_mode** — different rules per layer | ReconstructLayer extendable | fine-grained control |
| **Usage metering** — per-mask billing units | route_stats already structured | billing-ready |
| **Twin Geo P4** AVX2+CUDA | filter+chunk_window+enrich all wired ✅ | throughput |
