"""
llm_advisor.py — POGLS LLM Advisor Bridge  v3
══════════════════════════════════════════════════════════════════════
v3 changes (vs v2):
  - SYSTEM_PROMPT strict — JSON only, zero tolerance
  - temperature=0.0, top_p=0.9 → deterministic
  - Short-circuit rules → zero LLM call for obvious states
  - _parse() hard-reject on any format deviation
  - fallback guard: retry if conf < 0.65 OR action not in allowed
  - cache keyed by state_hash (same as before, explicit doc)

Decision priority (highest → lowest):
  1. Cache hit          O(1)
  2. Short-circuit rule O(1)  ← new
  3. Primary LLM 0.5B        ~50ms
  4. Fallback LLM 1.5B       ~200ms (only if primary weak)
  5. NOOP                    (both down)
══════════════════════════════════════════════════════════════════════
"""

from __future__ import annotations

import ctypes, json, logging, os, time
from dataclasses import dataclass
from typing import Optional

import requests

log = logging.getLogger("llm_advisor")


# ══════════════════════════════════════════════════════════════════════
# 1. ACTION ENUM
# ══════════════════════════════════════════════════════════════════════

class Act:
    NOOP               = 0
    REHASH_NOW         = 1
    COMPACT_TOMBSTONE  = 2
    FLUSH_GPU_QUEUE    = 3
    RESET_CACHE_WINDOW = 4
    RESEED_LANE_SALT   = 5
    DEGRADE_MODE       = 6
    RECOVER_MODE       = 7

    _NAMES   = {0:"NOOP",1:"REHASH_NOW",2:"COMPACT_TOMBSTONE",
                3:"FLUSH_GPU_QUEUE",4:"RESET_CACHE_WINDOW",
                5:"RESEED_LANE_SALT",6:"DEGRADE_MODE",7:"RECOVER_MODE"}
    _BY_NAME = {v: k for k, v in _NAMES.items()}

    @classmethod
    def name(cls, c: int) -> str: return cls._NAMES.get(c, f"UNKNOWN({c})")
    @classmethod
    def from_name(cls, n: str) -> int: return cls._BY_NAME.get(n.upper(), cls.NOOP)
    @classmethod
    def from_mask(cls, mask: int) -> list[str]:
        return [cls._NAMES[i] for i in range(8) if mask & (1 << i)]


# ══════════════════════════════════════════════════════════════════════
# 2. CTYPES STRUCT
# ══════════════════════════════════════════════════════════════════════

class CAdvisorResult(ctypes.Structure):
    _fields_ = [
        ("action",     ctypes.c_int),
        ("confidence", ctypes.c_float),
        ("from_cache", ctypes.c_uint8),
        ("state_key",  ctypes.c_char * 32),
    ]


# ══════════════════════════════════════════════════════════════════════
# 3. ACTION CACHE  — state_key → CacheEntry
# ══════════════════════════════════════════════════════════════════════

@dataclass
class CacheEntry:
    action: int; confidence: float; hits: int = 0; created_at: float = 0.0

    def to_result(self, key: str) -> CAdvisorResult:
        r = CAdvisorResult()
        r.action, r.confidence, r.from_cache = self.action, self.confidence, 1
        r.state_key = key.encode()[:31]
        self.hits += 1
        return r


class ActionCache:
    def __init__(self, max_size: int = 256):
        self._store: dict[str, CacheEntry] = {}
        self._max = max_size

    def get(self, k: str) -> Optional[CacheEntry]: return self._store.get(k)

    def put(self, k: str, action: int, conf: float) -> None:
        if len(self._store) >= self._max:
            del self._store[next(iter(self._store))]
        self._store[k] = CacheEntry(action=action, confidence=conf,
                                     created_at=time.monotonic())

    def invalidate(self, k: str) -> None: self._store.pop(k, None)

    def stats(self) -> dict:
        return {"size": len(self._store),
                "total_hits": sum(e.hits for e in self._store.values())}


# ══════════════════════════════════════════════════════════════════════
# 4. SHORT-CIRCUIT RULES  — zero LLM, deterministic, O(1)
#    Ordered: most critical first
#    Returns (action, confidence) or None → proceed to LLM
# ══════════════════════════════════════════════════════════════════════

# Thresholds as config (easy to tune)
_SC_KV_CRITICAL    = int(os.environ.get("SC_KV_CRITICAL",   90))  # REHASH unconditional
_SC_TOMB_CRITICAL  = int(os.environ.get("SC_TOMB_CRITICAL", 50))  # COMPACT unconditional
_SC_GPU_ANY        = int(os.environ.get("SC_GPU_ANY",        0))  # any overflow → flush
_SC_AUDIT_OFFLINE  = int(os.environ.get("SC_AUDIT_OFFLINE",  2))  # health=OFFLINE → degrade

def short_circuit(snap: dict) -> Optional[tuple[int, float]]:
    """
    Returns (action, 1.0) for obvious states — no LLM needed.
    Rule order matters: more critical = higher priority.
    """
    kv    = snap.get("kv",    {})
    gpu   = snap.get("gpu",   {})
    audit = snap.get("audit", {})

    load  = kv.get("load_pct",    0)
    tomb  = kv.get("tomb_pct",    0)
    ovf   = gpu.get("overflow_pct", 0)
    hlth  = audit.get("health",   0)

    # Rule 1: KV critically full — must rehash NOW
    if load >= _SC_KV_CRITICAL:
        log.info("short-circuit: KV load=%d%% → REHASH_NOW", load)
        return Act.REHASH_NOW, 1.0

    # Rule 2: GPU overflow — drain immediately (safe, idempotent)
    if ovf > _SC_GPU_ANY:
        log.info("short-circuit: GPU overflow=%d → FLUSH_GPU_QUEUE", ovf)
        return Act.FLUSH_GPU_QUEUE, 1.0

    # Rule 3: Audit OFFLINE — degrade to stabilize
    if hlth >= _SC_AUDIT_OFFLINE:
        log.info("short-circuit: audit.health=%d → DEGRADE_MODE", hlth)
        return Act.DEGRADE_MODE, 1.0

    # Rule 4: Tombstone critical — compact before KV chokes
    if tomb >= _SC_TOMB_CRITICAL:
        log.info("short-circuit: tomb=%d%% → COMPACT_TOMBSTONE", tomb)
        return Act.COMPACT_TOMBSTONE, 1.0

    return None   # no rule matched → ask LLM


# ══════════════════════════════════════════════════════════════════════
# 5. CONFIG
# ══════════════════════════════════════════════════════════════════════

LLM_PRIMARY            = os.environ.get("LLM_PRIMARY",   "http://localhost:8082/v1/chat/completions")
LLM_FALLBACK           = os.environ.get("LLM_FALLBACK",  "http://localhost:8083/v1/chat/completions")
LLM_TIMEOUT            = float(os.environ.get("LLM_TIMEOUT_SEC",          "5.0"))
LLM_FALLBACK_THRESHOLD = float(os.environ.get("LLM_FALLBACK_THRESHOLD",   "0.65"))

# Strict system prompt — LLM must output exactly one JSON line
SYSTEM_PROMPT = (
    'You must output JSON only: {"action": "<ONE_OF_ALLOWED>", "confidence": 0.0-1.0}\n'
    "No explanation. No markdown. No extra text.\n"
    "Allowed values for action are listed in the user message.\n"
    "If unsure use NOOP."
)


# ══════════════════════════════════════════════════════════════════════
# 6. LLM CLIENT  — llama.cpp /v1/chat/completions
# ══════════════════════════════════════════════════════════════════════

def _call_model(endpoint: str, prompt: str) -> str:
    payload = {
        "model":       "local",
        "messages":    [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user",   "content": prompt},
        ],
        "temperature": 0.0,    # fully deterministic
        "top_p":       0.9,
        "max_tokens":  48,     # {"action":"COMPACT_TOMBSTONE","confidence":0.95} = ~46 chars
        "stream":      False,
    }
    resp = requests.post(endpoint, json=payload, timeout=LLM_TIMEOUT)
    resp.raise_for_status()
    return resp.json()["choices"][0]["message"]["content"].strip()


def _parse(raw: str, allowed_names: list[str]) -> tuple[int, float]:
    """
    Hard-reject any response that deviates from exact format.
    Returns (Act.NOOP, 0.0) on ANY failure — no partial acceptance.
    """
    try:
        # strip accidental fences (belt-and-suspenders)
        clean = raw.strip()
        if clean.startswith("```"):
            clean = clean.split("```")[1].lstrip("json").strip()

        data = json.loads(clean)

        # must have exactly these two keys (extra keys = reject)
        if set(data.keys()) - {"action", "confidence"}:
            log.warning("parse: unexpected keys %s → reject", set(data.keys()))
            return Act.NOOP, 0.0

        name = str(data["action"]).upper()
        conf = float(data["confidence"])
        conf = max(0.0, min(1.0, conf))

        if name not in allowed_names:
            log.warning("parse: '%s' not in allowed=%s → reject", name, allowed_names)
            return Act.NOOP, 0.0

        return Act.from_name(name), conf

    except (KeyError, ValueError, json.JSONDecodeError) as exc:
        log.warning("parse failed: %s | raw=%r", exc, raw[:120])
        return Act.NOOP, 0.0


def build_prompt(state_key: str, allowed_names: list[str], snap: dict) -> str:
    metrics = {
        "kv_load_pct":  snap.get("kv",    {}).get("load_pct",    0),
        "kv_tomb_pct":  snap.get("kv",    {}).get("tomb_pct",    0),
        "gpu_overflow": snap.get("gpu",   {}).get("overflow_pct", 0),
        "audit_health": snap.get("audit", {}).get("health",       0),
        "hydra_fill":   snap.get("hydra", {}).get("fill_pct",     0),
    }
    return (
        f"state_key: {state_key}\n"
        f"metrics: {json.dumps(metrics)}\n"
        f"allowed_actions: {json.dumps(allowed_names)}\n"
        "Select best action:"
    )


# ══════════════════════════════════════════════════════════════════════
# 7. DUAL-MODEL CALL  — primary → fallback with hardened guard
# ══════════════════════════════════════════════════════════════════════

def _query_with_fallback(
        prompt: str,
        allowed_names: list[str]) -> tuple[int, float, str]:
    """
    Returns (action, confidence, model_used).
    model_used: "primary" | "fallback" | "noop"

    Escalate to fallback if:
      - primary unreachable / error
      - confidence < LLM_FALLBACK_THRESHOLD
      - action not in allowed (parse already returns NOOP in this case,
        but we escalate instead of accepting NOOP silently)
    """
    action_p, conf_p = Act.NOOP, 0.0

    # ── primary ────────────────────────────────────────────────────────
    try:
        raw      = _call_model(LLM_PRIMARY, prompt)
        action_p, conf_p = _parse(raw, allowed_names)
        log.debug("primary → %s conf=%.2f", Act.name(action_p), conf_p)

        # accept only if confident AND valid (not forced NOOP by parse)
        if conf_p >= LLM_FALLBACK_THRESHOLD and action_p != Act.NOOP:
            return action_p, conf_p, "primary"

        log.info("primary weak (conf=%.2f action=%s) → fallback",
                 conf_p, Act.name(action_p))

    except requests.exceptions.ConnectionError:
        log.warning("primary unreachable → fallback")
    except Exception as exc:
        log.warning("primary error: %s → fallback", exc)

    # ── fallback ───────────────────────────────────────────────────────
    try:
        raw      = _call_model(LLM_FALLBACK, prompt)
        action_f, conf_f = _parse(raw, allowed_names)
        log.debug("fallback → %s conf=%.2f", Act.name(action_f), conf_f)

        if action_f != Act.NOOP or conf_f > 0:
            return action_f, conf_f, "fallback"

    except requests.exceptions.ConnectionError:
        log.warning("fallback unreachable → NOOP")
    except Exception as exc:
        log.warning("fallback error: %s → NOOP", exc)

    # ── last resort: if primary gave something, use it ─────────────────
    if action_p != Act.NOOP:
        log.info("using primary result as last resort: %s", Act.name(action_p))
        return action_p, conf_p, "primary_fallback"

    return Act.NOOP, 0.0, "noop"


# ══════════════════════════════════════════════════════════════════════
# 8. ADVISOR CORE
# ══════════════════════════════════════════════════════════════════════

class LLMAdvisor:
    def __init__(self):
        self._cache          = ActionCache()
        self._calls          = 0
        self._errors         = 0
        self._short_circuits = 0
        self._primary_hits   = 0
        self._fallback_hits  = 0

    def query(self, state_key: str, allowed_mask: int, snap_json: str) -> CAdvisorResult:
        allowed_names = Act.from_mask(allowed_mask)
        try:
            snap = json.loads(snap_json) if snap_json else {}
        except Exception:
            snap = {}

        # ── 1. cache ────────────────────────────────────────────────────
        entry = self._cache.get(state_key)
        if entry:
            log.debug("cache: %s → %s", state_key, Act.name(entry.action))
            return entry.to_result(state_key)

        # ── 2. short-circuit ────────────────────────────────────────────
        sc = short_circuit(snap)
        if sc is not None:
            action, conf = sc
            self._short_circuits += 1
            self._cache.put(state_key, action, conf)
            return self._make_result(state_key, action, conf, from_cache=0)

        # ── 3. LLM ──────────────────────────────────────────────────────
        self._calls += 1
        prompt = build_prompt(state_key, allowed_names, snap)
        action, conf, model = _query_with_fallback(prompt, allowed_names)

        if   model == "noop":             self._errors       += 1
        elif "fallback" in model:         self._fallback_hits += 1
        else:                             self._primary_hits  += 1

        self._cache.put(state_key, action, conf)
        log.info("advice: %s → %s conf=%.2f [%s]",
                 state_key, Act.name(action), conf, model)
        return self._make_result(state_key, action, conf, from_cache=0)

    @staticmethod
    def _make_result(key: str, action: int, conf: float, from_cache: int) -> CAdvisorResult:
        r = CAdvisorResult()
        r.action, r.confidence, r.from_cache = action, conf, from_cache
        r.state_key = key.encode()[:31]
        return r

    def stats(self) -> dict:
        return {
            "llm_calls":      self._calls,
            "llm_errors":     self._errors,
            "short_circuits": self._short_circuits,
            "primary_hits":   self._primary_hits,
            "fallback_hits":  self._fallback_hits,
            "cache":          self._cache.stats(),
        }


# ══════════════════════════════════════════════════════════════════════
# 9. C-CALLABLE EXPORT
# ══════════════════════════════════════════════════════════════════════

_advisor = LLMAdvisor()

def pogls_llm_query_python(
        key_ptr:    ctypes.c_char_p,
        allowed:    ctypes.c_uint32,
        json_ptr:   ctypes.c_char_p,
        result_ptr: ctypes.POINTER(CAdvisorResult)) -> int:
    try:
        key       = key_ptr.decode()  if isinstance(key_ptr,  bytes) else key_ptr
        snap_json = json_ptr.decode() if isinstance(json_ptr, bytes) else (json_ptr or "")
        r = _advisor.query(key, int(allowed), snap_json)
        result_ptr[0].action, result_ptr[0].confidence = r.action, r.confidence
        result_ptr[0].from_cache, result_ptr[0].state_key = r.from_cache, r.state_key
        return 0
    except Exception as exc:
        log.error("pogls_llm_query_python: %s", exc)
        return -1


# ══════════════════════════════════════════════════════════════════════
# 10. STANDALONE TEST
# ══════════════════════════════════════════════════════════════════════

def _run_tests():
    logging.basicConfig(level=logging.DEBUG,
                        format="%(levelname)-8s %(name)s — %(message)s")

    cases = [
        # (desc, key, mask, snap_json, expected_path)
        ("KV critical 92%  [SC]",   "KL90T10G0H0",
         (1<<Act.REHASH_NOW)|1,
         '{"kv":{"load_pct":92,"tomb_pct":10},"gpu":{"overflow_pct":0},"audit":{"health":0},"hydra":{"fill_pct":60}}'),

        ("GPU overflow     [SC]",   "KL50T05G1H0",
         (1<<Act.FLUSH_GPU_QUEUE)|1,
         '{"kv":{"load_pct":50,"tomb_pct":5},"gpu":{"overflow_pct":2},"audit":{"health":0},"hydra":{"fill_pct":40}}'),

        ("Tomb critical    [SC]",   "KL60T55G0H0",
         (1<<Act.COMPACT_TOMBSTONE)|1,
         '{"kv":{"load_pct":60,"tomb_pct":55},"gpu":{"overflow_pct":0},"audit":{"health":0},"hydra":{"fill_pct":55}}'),

        ("KV mild 78%      [LLM]",  "KL80T10G0H0",
         (1<<Act.REHASH_NOW)|(1<<Act.COMPACT_TOMBSTONE)|1,
         '{"kv":{"load_pct":78,"tomb_pct":10},"gpu":{"overflow_pct":0},"audit":{"health":0},"hydra":{"fill_pct":60}}'),

        ("Cache hit repeat [CACHE]","KL80T10G0H0",
         (1<<Act.REHASH_NOW)|(1<<Act.COMPACT_TOMBSTONE)|1,
         '{"kv":{"load_pct":78,"tomb_pct":10},"gpu":{"overflow_pct":0},"audit":{"health":0},"hydra":{"fill_pct":60}}'),
    ]

    adv = LLMAdvisor()
    print(f"\n── POGLS LLM Advisor v3 Test ──")
    print(f"   primary  → {LLM_PRIMARY}")
    print(f"   fallback → {LLM_FALLBACK}")
    print(f"   SC thresholds: KV>={_SC_KV_CRITICAL}% tomb>={_SC_TOMB_CRITICAL}% gpu>{_SC_GPU_ANY}\n")

    for desc, key, mask, snap in cases:
        r   = adv.query(key, mask, snap)
        src = "CACHE" if r.from_cache else "LIVE "
        print(f"  [{src}] {desc:28} → {Act.name(r.action):22} conf={r.confidence:.2f}")

    print(f"\n  Stats: {adv.stats()}\n")


if __name__ == "__main__":
    import sys
    if "--test" in sys.argv:
        _run_tests()
    else:
        print("Usage: python3 llm_advisor.py --test")
        print("Env:   LLM_PRIMARY  LLM_FALLBACK  LLM_TIMEOUT_SEC")
        print("       LLM_FALLBACK_THRESHOLD  SC_KV_CRITICAL  SC_TOMB_CRITICAL")
