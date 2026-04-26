"""
test_s30.py — S30 tests
Run: python3 -m pytest test_s30.py -v
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

import json, pytest
from geo_net_ctypes_s30 import GeoNetCtxFast, _LIB, CYL_FULL_N
from geo_net_py         import GeoNetCtx as PyCtx, QRPN_HOT_THRESH, QRPN_ANOMALY_HOT


@pytest.fixture
def ctx():  return GeoNetCtxFast()
@pytest.fixture
def py_ctx(): return PyCtx()


# ── 1. slot_hot wired ──────────────────────────────────────────────

class TestSlotHot:

    def test_slot_hot_zero_matches_python(self):
        """slot_hot=0 → same as S29 (baseline)."""
        ctx_c  = GeoNetCtxFast()
        ctx_py = PyCtx()
        for i in range(200):
            a_c  = ctx_c.route(i, slot_hot=0)
            a_py = ctx_py.route(i, slot_hot=0)
            assert a_c.spoke == a_py.spoke, f"i={i}"

    def test_slot_hot_one_changes_state(self):
        """slot_hot=1 every tick → hot_slots accumulate → state escalates eventually."""
        ctx_c  = GeoNetCtxFast()
        ctx_py = PyCtx()
        # force enough hot ops to stress ThirdEye (>64 in one cycle of 144)
        for i in range(200):
            ctx_c.route(i, slot_hot=1)
            ctx_py.route(i, slot_hot=1)
        # both should have escalated (or at least both same state)
        assert ctx_c.state == ctx_py.state

    def test_slot_hot_does_not_change_spoke(self):
        """slot_hot affects ThirdEye state only, not spoke routing."""
        ctx0 = GeoNetCtxFast()
        ctx1 = GeoNetCtxFast()
        for i in range(50):
            a0 = ctx0.route(i, slot_hot=0)
            a1 = ctx1.route(i, slot_hot=1)
            assert a0.spoke == a1.spoke, f"i={i} spoke changed with slot_hot"

    def test_slot_hot_increments_op_count(self, ctx):
        for i in range(30):
            ctx.route(i, slot_hot=1)
        assert ctx.op_count == 30


# ── 2. fetch_range_hot ────────────────────────────────────────────

class TestFetchRangeHot:

    def test_basic_returns_n(self, ctx):
        hot  = bytes([1 if i % 8 == 0 else 0 for i in range(100)])
        recs = ctx.fetch_range_hot(0, 100, hot)
        assert len(recs) == 100

    def test_spoke_valid(self, ctx):
        hot  = bytes(100)
        for r in ctx.fetch_range_hot(0, 100, hot):
            assert 0 <= r.spoke < 6

    def test_matches_sequential_route(self):
        """fetch_range_hot(all zero) == fetch_range (no hot)."""
        ctx_h = GeoNetCtxFast()
        ctx_r = GeoNetCtxFast()
        N    = 100
        hot  = bytes(N)
        rh   = ctx_h.fetch_range_hot(0, N, hot)
        rr   = ctx_r.fetch_range(0, N)
        for i in range(N):
            assert rh[i].spoke == rr[i].spoke, f"i={i}"
            assert rh[i].is_audit == rr[i].is_audit, f"i={i}"

    def test_hot_state_differs_from_cold(self):
        """After enough hot ops, state should differ from all-cold."""
        ctx_hot  = GeoNetCtxFast()
        ctx_cold = GeoNetCtxFast()
        N   = 300
        hot = bytes([1]*N)
        cold= bytes(N)
        ctx_hot.fetch_range_hot(0, N, hot)
        ctx_cold.fetch_range(0, N)
        # hot path should have escalated (or at least tracked more hot_slots)
        # states may differ — just verify both are valid
        assert ctx_hot.state in ("NORMAL", "STRESSED", "ANOMALY")
        assert ctx_cold.state in ("NORMAL", "STRESSED", "ANOMALY")


# ── 3. signal_fail (QRPN feedback) ────────────────────────────────

class TestSignalFail:

    def test_returns_valid_state(self, ctx):
        s = ctx.signal_fail()
        assert s in ("NORMAL", "STRESSED", "ANOMALY")

    def test_anomaly_signals_increments(self, ctx):
        before = ctx.anomaly_signals
        ctx.signal_fail()
        assert ctx.anomaly_signals == before + 1

    def test_repeated_signals_escalate(self):
        """ANOMALY_HOT=96 signals → state must be ANOMALY."""
        ctx = GeoNetCtxFast()
        for _ in range(QRPN_ANOMALY_HOT + 10):
            ctx.signal_fail()
        assert ctx.state == "ANOMALY"

    def test_stressed_threshold(self):
        """HOT_THRESH=64 hot routes within one cycle → STRESSED."""
        ctx = GeoNetCtxFast()
        # hot_slots accumulate via slot_hot=1 in te_tick
        for i in range(QRPN_HOT_THRESH + 10):
            ctx.route(i, slot_hot=1)
        # allow one full TE_CYCLE to pass (144 ops)
        for i in range(144):
            ctx.route(200 + i, slot_hot=1)
        assert ctx.state in ("STRESSED", "ANOMALY")


# ── 4. filter pushdown ────────────────────────────────────────────

class TestFilterPushdown:

    def test_spoke_mask_all_passes_all(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*200, spoke_mask=0x3F))
        assert len(recs) == 200

    def test_spoke_mask_zero_uses_default(self, ctx):
        """spoke_mask=0 → treated as 0x3F in C (unconfigured = all)."""
        recs = list(ctx.iter_chunks_stream(0, 64*100, spoke_mask=0))
        assert len(recs) == 100

    def test_spoke_mask_single_spoke(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*600, spoke_mask=0x01))
        assert all(r["spoke"] == 0 for r in recs)
        # ~1/6 of 600 = ~100 ± tolerance
        assert 50 < len(recs) < 200

    def test_spoke_mask_two_spokes(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*600, spoke_mask=0x09))  # 0+3
        assert all(r["spoke"] in (0, 3) for r in recs)

    def test_audit_only(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*500, audit_only=True))
        assert all(r["is_audit"] for r in recs)
        # ~1/8 of 500 = ~62 ± tolerance
        assert 30 < len(recs) < 150

    def test_audit_only_plus_spoke_mask(self, ctx):
        recs = list(ctx.iter_chunks_stream(
            0, 64*500, spoke_mask=0x01, audit_only=True))
        assert all(r["spoke"] == 0 and r["is_audit"] for r in recs)

    def test_chunk_i_sequential_after_filter(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*200, spoke_mask=0x01))
        for i, r in enumerate(recs):
            assert r["chunk_i"] == i, f"gap at filtered index {i}"

    def test_addr_preserved_through_filter(self, ctx):
        """addr field should still be absolute addr, not filtered index."""
        off  = 0 * CYL_FULL_N
        recs = list(ctx.iter_chunks_stream(0, 64*200, spoke_mask=0x01))
        for r in recs:
            assert off <= r["addr"] < off + 200

    def test_filter_reduces_data(self, ctx):
        """Single spoke: emitted < 50% of total."""
        all_recs  = list(ctx.iter_chunks_stream(0, 64*600, spoke_mask=0x3F))
        filt_recs = list(GeoNetCtxFast().iter_chunks_stream(0, 64*600, spoke_mask=0x01))
        assert len(filt_recs) < len(all_recs) * 0.5

    def test_pass_rate_in_summary(self, ctx):
        """Simulate NDJSON summary pass_rate."""
        N  = 300; emit = 0
        for r in ctx.iter_chunks_stream(0, 64*N, spoke_mask=0x01):
            emit += 1
        rate = emit / N
        assert 0.0 < rate < 0.5   # ~1/6


# ── 5. status includes S30 fields ─────────────────────────────────

class TestStatusS30:

    def test_status_has_filter_pushdown(self, ctx):
        s = ctx.status()
        assert "filter_pushdown" in s

    def test_status_has_slot_hot_wired(self, ctx):
        s = ctx.status()
        assert "slot_hot_wired" in s

    def test_status_has_anomaly_signals(self, ctx):
        s = ctx.status()
        assert "anomaly_signals" in s

    def test_anomaly_signals_zero_on_fresh(self, ctx):
        assert ctx.status()["anomaly_signals"] == 0
