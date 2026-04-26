"""
llm_adapter_s33.py — S33-B LLM Adapter (classify + annotate)
═════════════════════════════════════════════════════════════
Role: enrich records from /wallet/reconstruct stream
      → add {score, label, note} per chunk (non-destructive)

Design:
  - pluggable backend: OpenAI-compatible / Together.ai / local stub
  - streaming-first: infer() yields enriched records immediately
  - batch internally: N records → 1 LLM call → N enriched records
  - failure-safe: on LLM error → passthrough (original record preserved)

Record contract (in = out + enriched fields):
  in:  {layer_id, layer_name, chunk_global, chunk_i, addr, offset,
         spoke, mirror_mask, is_audit}
  out: in + {score: float, label: str, note: str, _llm_enriched: bool}

Labels (S33 initial set):
  "normal"    — no anomaly detected
  "anomaly"   — entropy / pattern outlier
  "hot"       — high-frequency access pattern
  "cold"      — low-value / sparse region
  "audit"     — marked audit chunk
"""

from __future__ import annotations

import json
import logging
import os
import time
from typing import Iterable, Iterator

import httpx

log = logging.getLogger("pogls.llm_adapter")

# ── Constants ──────────────────────────────────────────────────────
ADAPTER_BATCH_SIZE  = int(os.environ.get("LLM_ADAPTER_BATCH",  16))
ADAPTER_TIMEOUT     = float(os.environ.get("LLM_ADAPTER_TIMEOUT", 10.0))
ADAPTER_MODEL       = os.environ.get("LLM_ADAPTER_MODEL",  "mistralai/Mistral-7B-Instruct-v0.2")
ADAPTER_API_BASE    = os.environ.get("LLM_ADAPTER_API",    "https://api.together.xyz/v1")
ADAPTER_API_KEY     = os.environ.get("LLM_ADAPTER_KEY",    "")
ADAPTER_MAX_RETRIES = int(os.environ.get("LLM_ADAPTER_RETRIES", 2))

VALID_LABELS = {"normal", "anomaly", "hot", "cold", "audit"}

# ── Prompt builder ─────────────────────────────────────────────────

_SYSTEM_PROMPT = """\
You are a storage chunk analyzer. Given a batch of storage chunk records,
classify each chunk and return a JSON array with one object per chunk.

Each object must have exactly these fields:
  "chunk_i": <int>        — echo the input chunk_i
  "score":   <float 0-1>  — anomaly/interest score (1 = most interesting)
  "label":   <str>        — one of: normal, anomaly, hot, cold, audit
  "note":    <str>        — one concise sentence explaining the classification

Rules:
- High addr variance → anomaly
- is_audit=1 → label "audit", score >= 0.7
- spoke=0 and mirror_mask=0 → cold, score < 0.3
- Respond ONLY with a JSON array. No markdown, no explanation.
"""

def _build_user_prompt(batch: list[dict]) -> str:
    slim = [
        {
            "chunk_i":     r.get("chunk_i", 0),
            "addr":        r.get("addr", 0),
            "spoke":       r.get("spoke", 0),
            "mirror_mask": r.get("mirror_mask", 0),
            "is_audit":    r.get("is_audit", 0),
            "offset":      r.get("offset", 0),
        }
        for r in batch
    ]
    return json.dumps(slim)

# ── LLM call (OpenAI-compatible) ───────────────────────────────────

def _call_llm(batch: list[dict], api_base: str, api_key: str,
              model: str, timeout: float) -> list[dict] | None:
    """
    Single LLM call → parsed list of enrichment dicts.
    Returns None on any failure (caller falls back to passthrough).
    """
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    payload = {
        "model": model,
        "messages": [
            {"role": "system",  "content": _SYSTEM_PROMPT},
            {"role": "user",    "content": _build_user_prompt(batch)},
        ],
        "max_tokens":  512,
        "temperature": 0.0,   # deterministic — classification task
    }

    try:
        resp = httpx.post(
            f"{api_base}/chat/completions",
            headers=headers,
            json=payload,
            timeout=timeout,
        )
        resp.raise_for_status()
        text = resp.json()["choices"][0]["message"]["content"].strip()
        parsed = json.loads(text)
        if isinstance(parsed, list):
            return parsed
        log.warning("llm_adapter: unexpected response shape")
        return None
    except Exception as e:
        log.warning("llm_adapter: LLM call failed — %s", e)
        return None

# ── Enrichment merge ───────────────────────────────────────────────

def _passthrough_enrichment(record: dict) -> dict:
    """Safe fallback — enrich with defaults, mark as not LLM-enriched."""
    out = dict(record)
    out["score"]         = 0.5
    out["label"]         = "audit" if record.get("is_audit") else "normal"
    out["note"]          = "passthrough — LLM unavailable"
    out["_llm_enriched"] = False
    return out

def _merge_enrichments(batch: list[dict],
                        enrichments: list[dict] | None) -> list[dict]:
    """
    Merge LLM enrichment results back onto original records.
    Key: chunk_i — robust to partial or reordered LLM responses.
    Falls back to passthrough for any unmatched record.
    """
    if not enrichments:
        return [_passthrough_enrichment(r) for r in batch]

    # build lookup by chunk_i
    enrich_map: dict[int, dict] = {}
    for e in enrichments:
        ci = e.get("chunk_i")
        if ci is not None:
            enrich_map[int(ci)] = e

    results = []
    for r in batch:
        ci  = int(r.get("chunk_i", -1))
        out = dict(r)
        if ci in enrich_map:
            e = enrich_map[ci]
            out["score"]         = float(e.get("score", 0.5))
            out["label"]         = e.get("label", "normal") if e.get("label") in VALID_LABELS else "normal"
            out["note"]          = str(e.get("note", ""))[:200]   # cap length
            out["_llm_enriched"] = True
        else:
            out["score"]         = 0.5
            out["label"]         = "audit" if r.get("is_audit") else "normal"
            out["note"]          = "passthrough — no LLM match"
            out["_llm_enriched"] = False
        results.append(out)
    return results

# ── LLMAdapter ─────────────────────────────────────────────────────

class LLMAdapter:
    """
    S33-B: classify + annotate chunks from /wallet/reconstruct stream.

    Usage:
        adapter = LLMAdapter()
        for enriched in adapter.infer(records):
            process(enriched)

    Config via env vars (see constants above) or constructor kwargs.
    """

    def __init__(
        self,
        api_base:   str   = ADAPTER_API_BASE,
        api_key:    str   = ADAPTER_API_KEY,
        model:      str   = ADAPTER_MODEL,
        batch_size: int   = ADAPTER_BATCH_SIZE,
        timeout:    float = ADAPTER_TIMEOUT,
        max_retries: int  = ADAPTER_MAX_RETRIES,
        stub_mode:  bool  = False,   # True = skip LLM, use rule-based only
    ):
        self.api_base    = api_base
        self.api_key     = api_key
        self.model       = model
        self.batch_size  = batch_size
        self.timeout     = timeout
        self.max_retries = max_retries
        self.stub_mode   = stub_mode  # local llama.cpp: no key required

        # metrics
        self.total_records  = 0
        self.total_batches  = 0
        self.llm_calls      = 0
        self.llm_failures   = 0
        self.passthrough_n  = 0

        if self.stub_mode:
            log.info("llm_adapter: stub mode (no API key — rule-based passthrough)")

    # ── public API ────────────────────────────────────────────────

    def infer(self, records: Iterable[dict]) -> Iterator[dict]:
        """
        Stream-in, stream-out.
        Buffers `batch_size` records → 1 LLM call → yield enriched records.
        Order preserved. Failure-safe.
        """
        buf: list[dict] = []
        for rec in records:
            buf.append(rec)
            if len(buf) >= self.batch_size:
                yield from self._process_batch(buf)
                buf = []
        if buf:
            yield from self._process_batch(buf)

    def stats(self) -> dict:
        return {
            "total_records":  self.total_records,
            "total_batches":  self.total_batches,
            "llm_calls":      self.llm_calls,
            "llm_failures":   self.llm_failures,
            "passthrough_n":  self.passthrough_n,
            "hit_rate":       round(
                1.0 - self.passthrough_n / max(1, self.total_records), 4
            ),
            "model":          self.model,
            "stub_mode":      self.stub_mode,
        }

    # ── internal ──────────────────────────────────────────────────

    def _process_batch(self, batch: list[dict]) -> list[dict]:
        self.total_batches  += 1
        self.total_records  += len(batch)

        if self.stub_mode:
            results = self._stub_enrich(batch)
            self.passthrough_n += sum(1 for r in results if not r["_llm_enriched"])
            return results

        enrichments = None
        for attempt in range(self.max_retries):
            enrichments = _call_llm(
                batch, self.api_base, self.api_key,
                self.model, self.timeout
            )
            self.llm_calls += 1
            if enrichments is not None:
                break
            log.warning("llm_adapter: retry %d/%d", attempt + 1, self.max_retries)

        if enrichments is None:
            self.llm_failures += 1

        results = _merge_enrichments(batch, enrichments)
        self.passthrough_n += sum(1 for r in results if not r["_llm_enriched"])
        return results

    def _stub_enrich(self, batch: list[dict]) -> list[dict]:
        """
        Rule-based enrichment (no LLM).
        Used when stub_mode=True or api_key missing.
        Deterministic — suitable for testing and dry-runs.
        """
        results = []
        for r in batch:
            out   = dict(r)
            addr  = r.get("addr", 0)
            spoke = r.get("spoke", 0)
            mm    = r.get("mirror_mask", 0)
            audit = r.get("is_audit", 0)

            if audit:
                label, score, note = "audit",   0.75, "audit chunk — flagged by geometry"
            elif spoke == 0 and mm == 0:
                label, score, note = "cold",    0.15, "zero spoke and mirror — sparse region"
            elif addr > (1 << 48):
                label, score, note = "anomaly", 0.88, "high address — entropy outlier"
            else:
                label, score, note = "normal",  0.40, "within expected range"

            out["score"]         = score
            out["label"]         = label
            out["note"]          = note
            out["_llm_enriched"] = False   # stub = rule-based, not LLM
            results.append(out)
        return results
