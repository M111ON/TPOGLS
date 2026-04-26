# POGLS S36 — Handoff
**Status:** ✅ COMPLETE | **Tests:** 5/5 (persistence) + 7/7 S35 + 22/22 S33 + 76/76 S32

---

## S36 Summary

### Problem (from S35)
- FeedbackEngine state อยู่ใน RAM — restart ทุกครั้ง rules กลับ default
- ทุก cycle ที่ปรับไปหายหมด (cycle count, history, adjusted thresholds)

### Solution
```
adjust → save JSON → restart → load JSON → rules restored
```

---

## Persistence Flow

```
startup
  ├── pogls_feedback.json exists? → FeedbackEngine.load() → restore cycle+rules+history
  └── not exists?                 → FeedbackEngine()      → fresh defaults

every auto-feedback adjust
  └── _feedback_engine.save(path, rules) → write immediately

POST /router/checkpoint
  └── force-save anytime (idempotent)

GET /router/status
  └── + checkpoint_path, checkpoint_exists fields
```

---

## Checkpoint Format (pogls_feedback.json)

```json
{
  "_version": "S36",
  "cycle": 12,
  "last_update": 1712345678.9,
  "config": {
    "smoothing_alpha": 0.1,
    "cooldown_s": 60.0,
    "entropy_ratio": 0.5,
    "score_step": 0.05
  },
  "rules": [
    {"label": "hot",     "score_min": 0.76, "score_max": 1.0,  "mask": 52},
    {"label": "cold",    "score_min": 0.0,  "score_max": 0.3,  "mask": 1},
    {"label": "audit",   "score_min": 0.0,  "score_max": 1.0,  "mask": 63},
    {"label": "anomaly", "score_min": 0.5,  "score_max": 1.0,  "mask": 32},
    {"label": "normal",  "score_min": 0.0,  "score_max": 1.0,  "mask": 18}
  ],
  "history": [...]   // last 20 cycles
}
```

---

## API Changes (S36)

### GET /router/status (extended)
```json
{
  "rules": [...],
  "checkpoint_path":   "pogls_feedback.json",
  "checkpoint_exists": true
}
```

### POST /router/checkpoint (NEW)
```json
// Request: no body needed
// Response:
{
  "saved": true,
  "path": "pogls_feedback.json",
  "cycle": 12,
  "rules_saved": 5,
  "history_saved": 12
}
```

### Env var
```bash
POGLS_FEEDBACK_CKPT=./checkpoints/feedback.json   # default: pogls_feedback.json
```

---

## Files Changed

| File | Change |
|---|---|
| `feedback_engine.py` | +`save()` +`load()` classmethod |
| `rest_server_s36.py` | startup auto-load + auto-save on adjust + `POST /router/checkpoint` |
| `score_router.py` | unchanged from S35 |

---

## S37 Candidates

| Candidate | Readiness | Impact |
|---|---|---|
| **Per-layer route_mode** | ReconstructLayer extendable | fine-grained per-layer routing |
| **Usage metering** | route_stats + cycle ready | billing per spoke usage |
| **Twin Geo P4** AVX2+CUDA | all layers wired ✅ | throughput |
