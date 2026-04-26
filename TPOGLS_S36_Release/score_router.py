"""
score_router.py — S34
Score-based spoke_mask router for POGLS GeoNet (6-spoke, GEO_SPOKES=6)

Bit layout (6-bit mask):
    bit  5    4    3    2    1    0
         S5   S4   S3   S2   S1   S0

Default routing table (tunable via ScoreRouterConfig):
    hot     score ≥ 0.7  → 0b110100  (S5,S4,S2) — high-load spokes
    cold    score ≤ 0.3  → 0b000001  (S0)        — archive spoke
    audit   any score    → 0b111111  (all 6)     — broadcast
    anomaly score ≥ 0.5  → 0b100000  (S5)        — quarantine spoke
    normal  any score    → 0b010010  (S4,S1)     — default lanes
    <miss>               → 0b010010  (S4,S1)     — same as normal fallback
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Iterator

# ── constants ──────────────────────────────────────────────────────
GEO_SPOKES      = 6
MASK_ALL        = (1 << GEO_SPOKES) - 1   # 0x3F = 0b111111
MASK_DEFAULT    = 0b010010                 # S4,S1 — safe fallback

# ── rule primitive ─────────────────────────────────────────────────
@dataclass
class RouteRule:
    label:      str
    score_min:  float
    score_max:  float
    mask:       int

    def matches(self, label: str, score: float) -> bool:
        return self.label == label and self.score_min <= score <= self.score_max

# ── default ruleset ────────────────────────────────────────────────
DEFAULT_RULES: list[RouteRule] = [
    RouteRule("hot",     0.7, 1.0, 0b110100),  # S5,S4,S2
    RouteRule("cold",    0.0, 0.3, 0b000001),  # S0
    RouteRule("audit",   0.0, 1.0, 0b111111),  # all
    RouteRule("anomaly", 0.5, 1.0, 0b100000),  # S5 quarantine
    RouteRule("normal",  0.0, 1.0, 0b010010),  # S4,S1
]

# ── config ─────────────────────────────────────────────────────────
@dataclass
class ScoreRouterConfig:
    rules:        list[RouteRule] = field(default_factory=lambda: list(DEFAULT_RULES))
    default_mask: int             = MASK_DEFAULT
    allow_zero:   bool            = False  # False → replace 0 mask with default

# ── router ─────────────────────────────────────────────────────────
class ScoreRouter:
    """
    Resolve spoke_mask from enriched record (has 'score' + 'label' fields).
    Thread-safe for read (rules list is not mutated after init).
    """

    def __init__(self, cfg: ScoreRouterConfig | None = None):
        self._cfg   = cfg or ScoreRouterConfig()
        self._stats = {"resolved": 0, "fallback": 0}

    # ── public ─────────────────────────────────────────────────────

    def resolve(self, record: dict) -> int:
        """Return spoke_mask (int, 6-bit) for a single enriched record."""
        label = record.get("label", "normal")
        score = float(record.get("score", 0.5))

        for rule in self._cfg.rules:
            if rule.matches(label, score):
                mask = rule.mask & MASK_ALL
                if mask == 0 and not self._cfg.allow_zero:
                    mask = self._cfg.default_mask
                self._stats["resolved"] += 1
                return mask

        self._stats["fallback"] += 1
        return self._cfg.default_mask

    def route_batch(self, records: list[dict]) -> Iterator[tuple[dict, int]]:
        """Yield (record, spoke_mask) for each record in batch."""
        for rec in records:
            yield rec, self.resolve(rec)

    def stats(self) -> dict:
        return dict(self._stats)

    def reset_stats(self) -> None:
        self._stats = {"resolved": 0, "fallback": 0}

    def apply_feedback(self, deltas: list) -> int:
        """
        Apply RuleDelta list from FeedbackEngine → mutate rules in-place.
        Returns count of rules updated.
        Thread-note: call from single writer only.
        """
        updated = 0
        for delta in deltas:
            for rule in self._cfg.rules:
                if rule.label == delta.label:
                    rule.score_min = delta.new_score_min
                    rule.score_max = delta.new_score_max
                    updated += 1
        return updated

    # ── repr ───────────────────────────────────────────────────────
    def __repr__(self) -> str:
        rules = len(self._cfg.rules)
        return f"ScoreRouter(rules={rules}, default={bin(self._cfg.default_mask)})"
