"""
feedback_engine.py — S35
Adaptive threshold adjuster for ScoreRouter rules.

Design constraints:
  1. Smoothing  — new = old * (1-α) + target * α       (gαn oscillation)
  2. Cooldown   — skip if now - last_update < COOLDOWN_S (gαn over-adjust)
  3. Entropy    — diversify only when H(dist) < H_MIN    (full-distribution view)

Entropy reference:
  H = -Σ p_i * log2(p_i)   (Shannon entropy, bits)
  H_MAX = log2(n_buckets)   e.g. 4 labels → H_MAX = 2.0 bits
  H_MIN = H_MAX * 0.5       below this = "too concentrated" → diversify
"""

from __future__ import annotations
import math
import time
from dataclasses import dataclass, field
from typing import Any

# ── config ─────────────────────────────────────────────────────────
SMOOTHING_ALPHA  = 0.10   # EWM weight for new signal
COOLDOWN_S       = 60.0   # minimum seconds between adjustments
ENTROPY_RATIO    = 0.50   # H < H_MAX * ratio → diversify
SCORE_STEP       = 0.05   # max threshold shift per cycle
SCORE_MIN        = 0.05   # floor — rule can't go below this
SCORE_MAX        = 0.95   # ceiling

# ── mask → canonical label map (inverse of ScoreRouter defaults) ──
MASK_TO_LABEL: dict[int, str] = {
    0b110100: "hot",
    0b000001: "cold",
    0b111111: "audit",
    0b100000: "anomaly",
    0b010010: "normal",
}

# ── delta descriptor ───────────────────────────────────────────────
@dataclass
class RuleDelta:
    label:          str
    old_score_min:  float
    old_score_max:  float
    new_score_min:  float
    new_score_max:  float
    reason:         str

# ── engine ─────────────────────────────────────────────────────────
@dataclass
class FeedbackEngine:
    smoothing_alpha: float = SMOOTHING_ALPHA
    cooldown_s:      float = COOLDOWN_S
    entropy_ratio:   float = ENTROPY_RATIO
    score_step:      float = SCORE_STEP

    _last_update: float  = field(default=0.0,  init=False, repr=False)
    _history:     list   = field(default_factory=list, init=False, repr=False)
    _cycle:       int    = field(default=0,    init=False, repr=False)

    # ── public ─────────────────────────────────────────────────────

    def analyze(self, route_stats: dict[str, int]) -> dict:
        """
        Analyze route_stats distribution.
        Returns analysis dict (safe to call anytime — no mutation).
        """
        counts = {k: v for k, v in route_stats.items() if isinstance(v, int) and v > 0}
        total  = sum(counts.values()) or 1
        probs  = {k: v / total for k, v in counts.items()}
        n      = len(probs) or 1

        h     = _entropy(list(probs.values()))
        h_max = math.log2(n) if n > 1 else 1.0
        h_min = h_max * self.entropy_ratio

        dominant_mask = max(counts, key=counts.__getitem__) if counts else None
        dominant_pct  = probs.get(dominant_mask, 0.0) if dominant_mask else 0.0

        return {
            "total":          total,
            "n_buckets":      n,
            "entropy":        round(h, 4),
            "entropy_max":    round(h_max, 4),
            "entropy_min":    round(h_min, 4),
            "concentrated":   h < h_min,
            "dominant_mask":  dominant_mask,
            "dominant_pct":   round(dominant_pct, 4),
            "probs":          {k: round(v, 4) for k, v in probs.items()},
        }

    def maybe_adjust(
        self,
        route_stats: dict[str, int],
        rules: list[Any],   # list[RouteRule] — avoid circular import
    ) -> tuple[list[RuleDelta], dict]:
        """
        Attempt to adjust rule thresholds based on route_stats.
        Returns (deltas, meta).
        deltas = [] if cooldown active or no adjustment needed.
        """
        now  = time.monotonic()
        meta = {"cycle": self._cycle, "adjusted": False, "skip_reason": None}

        # cooldown guard
        elapsed = now - self._last_update
        if elapsed < self.cooldown_s:
            meta["skip_reason"] = f"cooldown ({elapsed:.1f}s < {self.cooldown_s}s)"
            return [], meta

        analysis = self.analyze(route_stats)
        meta["analysis"] = analysis

        if not analysis["concentrated"]:
            meta["skip_reason"] = (
                f"entropy OK ({analysis['entropy']:.3f} ≥ {analysis['entropy_min']:.3f})"
            )
            return [], meta

        # entropy too low → diversify dominant rule
        dominant_mask_hex = analysis["dominant_mask"]
        if dominant_mask_hex is None:
            meta["skip_reason"] = "no dominant mask"
            return [], meta

        dominant_mask_int = int(dominant_mask_hex, 16)
        dominant_label    = MASK_TO_LABEL.get(dominant_mask_int)

        if dominant_label is None:
            meta["skip_reason"] = f"unknown mask {dominant_mask_hex}"
            return [], meta

        deltas: list[RuleDelta] = []
        for rule in rules:
            if rule.label != dominant_label:
                continue

            # raise score_min → harder to qualify as dominant label
            # smoothed: new = old * (1-α) + target * α
            target_min = min(rule.score_min + self.score_step, SCORE_MAX)
            new_min    = _smooth(rule.score_min, target_min, self.smoothing_alpha)
            new_min    = max(SCORE_MIN, min(SCORE_MAX, new_min))

            # score_max untouched — only tighten entry, not exit
            delta = RuleDelta(
                label         = rule.label,
                old_score_min = rule.score_min,
                old_score_max = rule.score_max,
                new_score_min = round(new_min, 4),
                new_score_max = rule.score_max,
                reason        = (
                    f"entropy={analysis['entropy']:.3f} < min={analysis['entropy_min']:.3f}, "
                    f"dominant={dominant_label} ({analysis['dominant_pct']*100:.1f}%)"
                ),
            )
            deltas.append(delta)

        if deltas:
            self._last_update = now
            self._cycle      += 1
            self._history.append({
                "cycle":   self._cycle,
                "ts":      now,
                "deltas":  [vars(d) for d in deltas],
                "analysis": analysis,
            })
            meta["adjusted"]    = True
            meta["cycle"]       = self._cycle
            meta["skip_reason"] = None

        return deltas, meta

    def stats(self) -> dict:
        return {
            "cycle":          self._cycle,
            "last_update_ago": round(time.monotonic() - self._last_update, 1),
            "cooldown_s":     self.cooldown_s,
            "smoothing_alpha": self.smoothing_alpha,
            "history_len":    len(self._history),
        }

    def last_history(self, n: int = 5) -> list:
        return self._history[-n:]

    # ── persistence ────────────────────────────────────────────────

    def save(self, path: str, rules: list | None = None) -> dict:
        """
        Dump engine state + current rules to JSON.
        Returns manifest dict (written to file).
        """
        import json, os
        manifest = {
            "_version":       "S36",
            "cycle":          self._cycle,
            "last_update":    self._last_update,
            "config": {
                "smoothing_alpha": self.smoothing_alpha,
                "cooldown_s":      self.cooldown_s,
                "entropy_ratio":   self.entropy_ratio,
                "score_step":      self.score_step,
            },
            "rules": [
                {
                    "label":     r.label,
                    "score_min": r.score_min,
                    "score_max": r.score_max,
                    "mask":      r.mask,
                }
                for r in (rules or [])
            ],
            "history": self._history[-20:],   # keep last 20 cycles
        }
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
        return manifest

    @classmethod
    def load(cls, path: str) -> tuple["FeedbackEngine", list]:
        """
        Restore engine + rules from JSON checkpoint.
        Returns (engine, rules_list).
        rules_list is [] if checkpoint has no rules (first-run safe).
        """
        import json
        from score_router import RouteRule
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        cfg = data.get("config", {})
        eng = cls(
            smoothing_alpha = cfg.get("smoothing_alpha", SMOOTHING_ALPHA),
            cooldown_s      = cfg.get("cooldown_s",      COOLDOWN_S),
            entropy_ratio   = cfg.get("entropy_ratio",   ENTROPY_RATIO),
            score_step      = cfg.get("score_step",      SCORE_STEP),
        )
        eng._cycle       = data.get("cycle", 0)
        eng._last_update = data.get("last_update", 0.0)
        eng._history     = data.get("history", [])

        rules = [
            RouteRule(
                label      = r["label"],
                score_min  = r["score_min"],
                score_max  = r["score_max"],
                mask       = r["mask"],
            )
            for r in data.get("rules", [])
        ]
        return eng, rules

# ── helpers ────────────────────────────────────────────────────────

def _entropy(probs: list[float]) -> float:
    """Shannon entropy in bits."""
    h = 0.0
    for p in probs:
        if p > 0:
            h -= p * math.log2(p)
    return h

def _smooth(old: float, target: float, alpha: float) -> float:
    """EWM: new = old*(1-α) + target*α"""
    return old * (1.0 - alpha) + target * alpha
