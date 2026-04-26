"""
test_s29.py — S29 tests: iter_chunks_stream + NDJSON endpoint
Run: python3 -m pytest test_s29.py -v

Dry-run friendly: no wallet stack needed.
All tests use GeoNetCtxFast (C/.so or Python fallback).
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

import json
import pytest
from geo_net_ctypes_s29 import GeoNetCtxFast, _LIB, CYL_FULL_N
from geo_net_py         import GeoNetCtx as PyCtx


# ── fixtures ────────────────────────────────────────────────────────

@pytest.fixture
def ctx():
    return GeoNetCtxFast()

@pytest.fixture
def py_ctx():
    return PyCtx()


# ── 1. iter_chunks_stream basic ────────────────────────────────────

class TestIterBasic:

    def test_yields_correct_count(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64 * 100))   # 100 chunks
        assert len(recs) == 100

    def test_yields_correct_count_partial_chunk(self, ctx):
        # 99.5 chunks → ceil → 100
        recs = list(ctx.iter_chunks_stream(0, 64 * 99 + 32))
        assert len(recs) == 100

    def test_chunk_i_sequential(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64 * 50))
        for i, r in enumerate(recs):
            assert r["chunk_i"] == i

    def test_addr_formula(self, ctx):
        file_idx = 3
        off      = file_idx * CYL_FULL_N
        recs     = list(ctx.iter_chunks_stream(file_idx, 64 * 20))
        for i, r in enumerate(recs):
            assert r["addr"] == off + i

    def test_spoke_valid_range(self, ctx):
        for r in ctx.iter_chunks_stream(0, 64 * 200):
            assert 0 <= r["spoke"] < 6

    def test_inv_spoke_formula(self, ctx):
        for r in ctx.iter_chunks_stream(0, 64 * 200):
            assert r["inv_spoke"] == (r["spoke"] + 3) % 6

    def test_is_audit_bool(self, ctx):
        for r in ctx.iter_chunks_stream(0, 64 * 50):
            assert isinstance(r["is_audit"], bool)

    def test_mirror_mask_in_range(self, ctx):
        for r in ctx.iter_chunks_stream(0, 64 * 50):
            assert 0 <= r["mirror_mask"] <= 0xFF

    def test_required_keys_present(self, ctx):
        expected = {"chunk_i", "addr", "spoke", "inv_spoke", "mirror_mask", "is_audit"}
        for r in ctx.iter_chunks_stream(0, 64 * 10):
            assert expected <= r.keys()


# ── 2. parity vs Python ────────────────────────────────────────────

class TestParity:

    def test_spoke_matches_python(self):
        ctx_c  = GeoNetCtxFast()
        ctx_py = PyCtx()
        N = 300
        recs = list(ctx_c.iter_chunks_stream(0, 64 * N))
        for i, r in enumerate(recs):
            a_py = ctx_py.route(i)
            assert r["spoke"]    == a_py.spoke,    f"i={i}"
            assert r["inv_spoke"] == a_py.inv_spoke, f"i={i}"
            assert r["is_audit"] == a_py.is_audit, f"i={i}"

    def test_parity_cross_file_idx(self):
        ctx_c  = GeoNetCtxFast()
        ctx_py = PyCtx()
        file_idx = 2
        off      = file_idx * CYL_FULL_N
        # advance py ctx to same state
        for i in range(off):
            ctx_py.route(i)
        recs = list(ctx_c.iter_chunks_stream(file_idx, 64 * 50))
        for i, r in enumerate(recs):
            a_py = ctx_py.route(off + i)
            assert r["spoke"] == a_py.spoke, f"file_idx={file_idx} i={i}"


# ── 3. O(1) memory — batch boundary ───────────────────────────────

class TestMemoryBehavior:

    def test_crosses_batch_boundary(self, ctx):
        """Iterator yields correctly across multiple 512-chunk batches."""
        N    = 1300   # > 2 batches of 512
        recs = list(ctx.iter_chunks_stream(0, 64 * N))
        assert len(recs) == N
        # chunk_i must be gapless
        for i, r in enumerate(recs):
            assert r["chunk_i"] == i, f"gap at i={i}"

    def test_large_layer_no_list_buildup(self, ctx):
        """
        Consume stream one at a time — simulate true O(1) consumer.
        Peak memory = BATCH records, not N.
        We can't measure RSS here, but we verify count + no exceptions.
        """
        N     = 5000
        count = 0
        for rec in ctx.iter_chunks_stream(0, 64 * N):
            count += 1
            _ = rec   # process and discard
        assert count == N

    def test_filter_fn_reduces_output(self, ctx):
        """filter_fn works — only audit chunks pass."""
        recs = list(ctx.iter_chunks_stream(
            0, 64 * 500,
            filter_fn=lambda r: r["is_audit"],
        ))
        assert all(r["is_audit"] for r in recs)
        # should be ~12.5% of 500 = ~62 (group_size=8 → every 8th is audit)
        assert 30 < len(recs) < 150

    def test_filter_fn_spoke_zero(self, ctx):
        recs = list(ctx.iter_chunks_stream(
            0, 64 * 600,
            filter_fn=lambda r: r["spoke"] == 0,
        ))
        assert all(r["spoke"] == 0 for r in recs)


# ── 4. NDJSON format helpers ───────────────────────────────────────

class TestNDJSON:

    def _collect_ndjson(self, lines: list[bytes]) -> tuple[list[dict], dict]:
        """Parse list of NDJSON bytes lines → (records, summary)."""
        parsed = [json.loads(ln) for ln in lines if ln.strip()]
        summary = parsed[-1]
        assert summary.get("_summary") is True
        return parsed[:-1], summary

    def _make_ndjson_lines(self, ctx, file_idx, n_bytes) -> list[bytes]:
        """Simulate _ndjson_stream without importing rest_server."""
        n_chunks   = max(1, (n_bytes + 63) // 64)
        spoke_dist = [0]*6; audit_cnt = 0; lines = []
        for rec in ctx.iter_chunks_stream(file_idx, n_bytes):
            spoke_dist[rec["spoke"]] += 1
            if rec["is_audit"]: audit_cnt += 1
            lines.append((json.dumps(rec) + "\n").encode())
        summary = {
            "_summary": True, "n_chunks": n_chunks,
            "spoke_dist": spoke_dist,
            "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
            "audit_points": audit_cnt,
            "qrpn_state_after": ctx.state,
        }
        lines.append((json.dumps(summary) + "\n").encode())
        return lines

    def test_each_line_valid_json(self, ctx):
        lines = self._make_ndjson_lines(ctx, 0, 64 * 100)
        for ln in lines:
            obj = json.loads(ln)
            assert isinstance(obj, dict)

    def test_summary_line_last(self, ctx):
        lines = self._make_ndjson_lines(ctx, 0, 64 * 50)
        last  = json.loads(lines[-1])
        assert last["_summary"] is True

    def test_summary_n_chunks_correct(self, ctx):
        N     = 80
        lines = self._make_ndjson_lines(ctx, 0, 64 * N)
        _, s  = self._collect_ndjson(lines)
        assert s["n_chunks"] == N

    def test_summary_spoke_dist_sums_to_n(self, ctx):
        N     = 200
        lines = self._make_ndjson_lines(ctx, 0, 64 * N)
        _, s  = self._collect_ndjson(lines)
        assert sum(s["spoke_dist"]) == N

    def test_dominant_spoke_correct(self, ctx):
        N     = 300
        lines = self._make_ndjson_lines(ctx, 0, 64 * N)
        _, s  = self._collect_ndjson(lines)
        dist  = s["spoke_dist"]
        assert s["dominant_spoke"] == dist.index(max(dist))

    def test_records_count_matches_n_chunks(self, ctx):
        N     = 60
        lines = self._make_ndjson_lines(ctx, 0, 64 * N)
        recs, _ = self._collect_ndjson(lines)
        assert len(recs) == N

    def test_qrpn_state_in_summary(self, ctx):
        lines = self._make_ndjson_lines(ctx, 0, 64 * 10)
        _, s  = self._collect_ndjson(lines)
        assert s["qrpn_state_after"] in ("NORMAL", "STRESSED", "ANOMALY")


# ── 5. iter_streaming flag ─────────────────────────────────────────

class TestIterFlag:

    def test_status_has_iter_streaming(self, ctx):
        s = ctx.status()
        assert "iter_streaming" in s

    def test_iter_flag_matches_lib(self, ctx):
        expected = getattr(_LIB, "_has_iter", False) if _LIB else False
        assert ctx.status()["iter_streaming"] == expected
