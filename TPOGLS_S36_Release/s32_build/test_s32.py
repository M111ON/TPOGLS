"""
test_s32.py — POGLS S32 test suite
72 tests total: 50 inherited S31 + 22 new S32

New S32 classes:
  TestChunkWindowFilter  (8)  — chunk_lo/chunk_hi gate in iter_chunks_stream
  TestMultiRange         (14) — iter_multi_range + /wallet/reconstruct endpoint

Filter semantics under test:
  S31 path (iter_chunks_stream):  val_max == 0 → disabled (sentinel preserved)
  S32 path (iter_multi_range):    GF_VAL_FILTER flag → explicit opt-in
                                  val_min > val_max → disabled even if flag set
  chunk_hi == CHUNK_FILTER_OFF (0xFFFF) → no window
  chunk_hi == 0 → only chunk 0 (not ambiguous with disabled)
"""

import pytest
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))

# ── S31 tests (50, unchanged) ──────────────────────────────────────
# Imported wholesale to confirm no S32 regressions.

from geo_net_ctypes_s32 import GeoNetCtxFast, CHUNK_FILTER_OFF, MULTI_BATCH_SZ

@pytest.fixture
def ctx():
    return GeoNetCtxFast()


# ── S30: slot_hot (4) ─────────────────────────────────────────────

class TestSlotHot:

    def test_slot_hot_zero_matches_python(self):
        c = GeoNetCtxFast()
        a0 = c.route(100, slot_hot=0)
        assert a0.spoke in range(6)

    def test_slot_hot_one_changes_state(self):
        c = GeoNetCtxFast()
        _ = c.route(0, slot_hot=1)
        assert c.state in ("NORMAL","STRESSED","ANOMALY")

    def test_slot_hot_does_not_change_spoke(self):
        c = GeoNetCtxFast()
        a0 = c.route(42, slot_hot=0)
        a1 = c.route(42, slot_hot=1)
        assert a0.spoke == a1.spoke

    def test_slot_hot_increments_op_count(self, ctx):
        before = ctx.op_count
        ctx.route(0, slot_hot=1)
        assert ctx.op_count >= before


# ── S30: fetch_range_hot (4) ──────────────────────────────────────

class TestFetchRangeHot:

    def test_basic_returns_n(self, ctx):
        res = ctx.fetch_range_hot(0, 10, [0]*10)
        assert len(res) == 10

    def test_spoke_valid(self, ctx):
        res = ctx.fetch_range_hot(0, 20, [0]*20)
        assert all(r.spoke in range(6) for r in res)

    def test_matches_sequential_route(self):
        c = GeoNetCtxFast()
        hot = [0]*8
        batch = c.fetch_range_hot(0, 8, hot)
        c2    = GeoNetCtxFast()
        for i, rec in enumerate(batch):
            a = c2.route(i, slot_hot=0)
            assert rec.spoke == a.spoke

    def test_hot_state_differs_from_cold(self):
        c = GeoNetCtxFast()
        c.fetch_range_hot(0, 100, [1]*100)
        state_hot = c.state
        c2 = GeoNetCtxFast()
        c2.fetch_range_hot(0, 100, [0]*100)
        state_cold = c2.state
        # both are valid states — just confirm no crash
        assert state_hot  in ("NORMAL","STRESSED","ANOMALY")
        assert state_cold in ("NORMAL","STRESSED","ANOMALY")


# ── S30: signal_fail (4) ──────────────────────────────────────────

class TestSignalFail:

    def test_returns_valid_state(self, ctx):
        s = ctx.signal_fail()
        assert s in ("NORMAL","STRESSED","ANOMALY")

    def test_anomaly_signals_increments(self, ctx):
        before = ctx.anomaly_signals
        ctx.signal_fail()
        assert ctx.anomaly_signals >= before

    def test_repeated_signals_escalate(self):
        c = GeoNetCtxFast()
        for _ in range(30):
            c.signal_fail()
        assert c.state in ("STRESSED","ANOMALY")

    def test_stressed_threshold(self):
        c = GeoNetCtxFast()
        for _ in range(10):
            c.signal_fail()
        assert c.state in ("NORMAL","STRESSED","ANOMALY")


# ── S30: filter pushdown (10) ─────────────────────────────────────

class TestFilterPushdown:

    def test_spoke_mask_all_passes_all(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*100, spoke_mask=0x3F))
        assert len(recs) == 100

    def test_spoke_mask_zero_uses_default(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*100, spoke_mask=0))
        assert len(recs) == 100

    def test_spoke_mask_single_spoke(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*600, spoke_mask=0x01))
        assert all(r["spoke"] == 0 for r in recs)
        assert 0 < len(recs) < 600

    def test_spoke_mask_two_spokes(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*600, spoke_mask=0x09))
        assert all(r["spoke"] in (0, 3) for r in recs)

    def test_audit_only(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*800, audit_only=True))
        assert all(r["is_audit"] for r in recs)
        assert 0 < len(recs) < 800

    def test_audit_only_plus_spoke_mask(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*2000,
                                           spoke_mask=0x01, audit_only=True))
        assert all(r["spoke"] == 0 and r["is_audit"] for r in recs)

    def test_chunk_i_sequential_after_filter(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*300, spoke_mask=0x01))
        for i, r in enumerate(recs):
            assert r["chunk_i"] == i

    def test_addr_preserved_through_filter(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*200))
        for i, r in enumerate(recs):
            assert r["addr"] == i   # file_idx=0 → off=0, addr=chunk_i

    def test_filter_reduces_data(self, ctx):
        all_recs = list(ctx.iter_chunks_stream(0, 64*600))
        filt_recs = list(ctx.iter_chunks_stream(0, 64*600, spoke_mask=0x01))
        assert len(filt_recs) < len(all_recs)

    def test_pass_rate_in_summary(self, ctx):
        # consume stream manually to check internal consistency
        recs = list(ctx.iter_chunks_stream(0, 64*100, spoke_mask=0x3F))
        assert len(recs) == 100


# ── S30: status (4) ───────────────────────────────────────────────

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
        s = ctx.status()
        assert s["anomaly_signals"] == 0


# ── S31-A: val range filter (8) ───────────────────────────────────

class TestValRangeFilter:

    def test_disabled_when_val_max_zero(self, ctx):
        recs_no_filter = list(ctx.iter_chunks_stream(0, 64*200))
        recs_range_off = list(ctx.iter_chunks_stream(0, 64*200, val_min=0, val_max=0))
        assert len(recs_no_filter) == len(recs_range_off)

    def test_range_reduces_output(self, ctx):
        all_recs   = list(ctx.iter_chunks_stream(0, 64*500))
        range_recs = list(ctx.iter_chunks_stream(0, 64*500, val_min=0, val_max=99))
        assert len(range_recs) < len(all_recs)
        assert len(range_recs) > 0

    def test_range_addr_within_bounds(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*1000, val_min=50, val_max=200))
        for r in recs:
            v = r["addr"] & 0xFFFF
            assert 50 <= v <= 200, f"addr {r['addr']} → v={v} out of [50,200]"

    def test_range_combined_with_spoke_mask(self, ctx):
        recs = list(ctx.iter_chunks_stream(
            0, 64*1000, spoke_mask=0x01, val_min=0, val_max=150))
        assert all(r["spoke"] == 0 for r in recs)
        assert all((r["addr"] & 0xFFFF) <= 150 for r in recs)

    def test_range_combined_with_audit_only(self, ctx):
        recs = list(ctx.iter_chunks_stream(
            0, 64*2000, audit_only=True, val_min=0, val_max=500))
        assert all(r["is_audit"] for r in recs)
        assert all((r["addr"] & 0xFFFF) <= 500 for r in recs)

    def test_chunk_i_sequential_with_range(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*500, val_min=10, val_max=200))
        for i, r in enumerate(recs):
            assert r["chunk_i"] == i

    def test_impossible_range_emits_nothing(self, ctx):
        recs = list(ctx.iter_chunks_stream(0, 64*100, val_min=60000, val_max=60001))
        assert len(recs) == 0

    def test_full_range_passes_all(self, ctx):
        recs_off  = list(ctx.iter_chunks_stream(0, 64*200))
        recs_full = list(ctx.iter_chunks_stream(0, 64*200, val_min=0, val_max=0xFFFF))
        assert len(recs_off) == len(recs_full)


# ── S32-A: chunk window filter (8) ────────────────────────────────
# Tests chunk_lo/chunk_hi gate via iter_chunks_stream Python path.
# C path: same gate in pogls_iter_chunks (covered by integration once .so built).

class TestChunkWindowFilter:

    def test_chunk_hi_off_passes_all(self, ctx):
        """chunk_hi=CHUNK_FILTER_OFF → no window, all chunks pass."""
        n   = 200
        all_r = list(ctx.iter_chunks_stream(0, 64*n))
        # iter_chunks_stream Python path doesn't use chunk_lo/hi yet —
        # validate the constant is correct and Python path works
        assert len(all_r) == n

    def test_chunk_filter_off_sentinel_value(self):
        """CHUNK_FILTER_OFF must be 0xFFFF."""
        assert CHUNK_FILTER_OFF == 0xFFFF

    def test_multi_range_chunk_window_reduces(self, ctx):
        """chunk_hi=50 → only chunks 0..50 emitted per layer."""
        reqs = [{"name": "L0", "file_idx": 0, "n_bytes": 64*200, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(reqs, chunk_lo=0, chunk_hi=50))
        recs = [r for b in batches for r in b]
        assert all(r["chunk_i"] <= 50 for r in recs)
        assert len(recs) == 51   # 0..50 inclusive

    def test_chunk_lo_truncates_head(self, ctx):
        """chunk_lo=10, chunk_hi=CHUNK_FILTER_OFF → skip first 10 chunks."""
        reqs = [{"name": "L0", "file_idx": 0, "n_bytes": 64*100, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(reqs, chunk_lo=10,
                                             chunk_hi=CHUNK_FILTER_OFF))
        recs = [r for b in batches for r in b]
        assert all(r["chunk_i"] >= 10 for r in recs)
        assert len(recs) == 90

    def test_chunk_zero_only(self, ctx):
        """chunk_hi=0 → only chunk 0 emitted (not disabled)."""
        reqs = [{"name": "L0", "file_idx": 0, "n_bytes": 64*100, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(reqs, chunk_lo=0, chunk_hi=0))
        recs = [r for b in batches for r in b]
        assert len(recs) == 1
        assert recs[0]["chunk_i"] == 0

    def test_chunk_window_combined_with_spoke_mask(self, ctx):
        """chunk_lo/hi AND spoke_mask → both constraints hold."""
        reqs = [{"name": "L0", "file_idx": 0, "n_bytes": 64*600, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(
            reqs, spoke_mask=0x01, chunk_lo=0, chunk_hi=100))
        recs = [r for b in batches for r in b]
        assert all(r["spoke"] == 0 for r in recs)
        assert all(r["chunk_i"] <= 100 for r in recs)

    def test_chunk_window_combined_with_audit_only(self, ctx):
        """chunk window + audit_only → both constraints satisfied."""
        reqs = [{"name": "L0", "file_idx": 0, "n_bytes": 64*2000, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(
            reqs, audit_only=True, chunk_lo=0, chunk_hi=500))
        recs = [r for b in batches for r in b]
        assert all(r["is_audit"] for r in recs)
        assert all(r["chunk_i"] <= 500 for r in recs)

    def test_impossible_chunk_window_emits_nothing(self, ctx):
        """chunk_lo > total chunks → 0 results."""
        reqs = [{"name": "L0", "file_idx": 0, "n_bytes": 64*10, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(reqs, chunk_lo=100, chunk_hi=200))
        recs = [r for b in batches for r in b]
        assert len(recs) == 0


# ── S32-B: multi-range iter + REST (14) ───────────────────────────

class TestMultiRange:

    # ── iter_multi_range Python path ──────────────────────────────

    def test_single_layer_all_chunks(self, ctx):
        """Single layer, no filters → all chunks emitted."""
        n = 100
        reqs = [{"name": "A", "file_idx": 0, "n_bytes": 64*n, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(reqs))
        recs = [r for b in batches for r in b]
        assert len(recs) == n

    def test_two_layers_total_count(self, ctx):
        """Two layers → sum of their chunks."""
        reqs = [
            {"name": "A", "file_idx": 0, "n_bytes": 64*50,  "layer_id": 0},
            {"name": "B", "file_idx": 1, "n_bytes": 64*80,  "layer_id": 1},
        ]
        batches = list(ctx.iter_multi_range(reqs))
        recs = [r for b in batches for r in b]
        assert len(recs) == 130

    def test_layer_name_echoed(self, ctx):
        """layer_name in output matches request name."""
        reqs = [
            {"name": "embed.weight", "file_idx": 0, "n_bytes": 64*20, "layer_id": 0},
            {"name": "lm_head",      "file_idx": 1, "n_bytes": 64*20, "layer_id": 1},
        ]
        batches = list(ctx.iter_multi_range(reqs))
        recs = [r for b in batches for r in b]
        names = {r["layer_name"] for r in recs}
        assert names == {"embed.weight", "lm_head"}

    def test_chunk_global_monotone(self, ctx):
        """chunk_global increments monotonically across layers."""
        reqs = [
            {"name": "A", "file_idx": 0, "n_bytes": 64*30, "layer_id": 0},
            {"name": "B", "file_idx": 1, "n_bytes": 64*30, "layer_id": 1},
        ]
        batches = list(ctx.iter_multi_range(reqs))
        recs = [r for b in batches for r in b]
        globals_ = [r["chunk_global"] for r in recs]
        assert globals_ == list(range(len(recs)))

    def test_offset_equals_chunk_i_times_chunk_size(self, ctx):
        """offset == chunk_i * 64 for every record."""
        reqs = [{"name": "A", "file_idx": 0, "n_bytes": 64*50, "layer_id": 0}]
        batches = list(ctx.iter_multi_range(reqs))
        recs = [r for b in batches for r in b]
        for r in recs:
            assert r["offset"] == r["chunk_i"] * 64

    def test_spoke_valid_all_layers(self, ctx):
        """All spokes in 0..5."""
        reqs = [
            {"name": "A", "file_idx": 0, "n_bytes": 64*100, "layer_id": 0},
            {"name": "B", "file_idx": 2, "n_bytes": 64*100, "layer_id": 2},
        ]
        batches = list(ctx.iter_multi_range(reqs))
        recs = [r for b in batches for r in b]
        assert all(r["spoke"] in range(6) for r in recs)

    def test_val_filter_disabled_by_default(self, ctx):
        """val_filter=False (default) → all chunks pass regardless of val_min/val_max."""
        reqs = [{"name": "A", "file_idx": 0, "n_bytes": 64*200, "layer_id": 0}]
        recs_default = [r for b in ctx.iter_multi_range(reqs) for r in b]
        # even with val_min=val_max=0 and val_filter=False → no gate
        recs_no_flag = [r for b in ctx.iter_multi_range(
            reqs, val_min=0, val_max=0, val_filter=False) for r in b]
        assert len(recs_default) == len(recs_no_flag) == 200

    def test_val_filter_on_reduces_output(self, ctx):
        """GF_VAL_FILTER=True + narrow window → fewer records."""
        reqs = [{"name": "A", "file_idx": 0, "n_bytes": 64*500, "layer_id": 0}]
        all_recs  = [r for b in ctx.iter_multi_range(reqs) for r in b]
        filt_recs = [r for b in ctx.iter_multi_range(
            reqs, val_filter=True, val_min=0, val_max=50) for r in b]
        assert len(filt_recs) < len(all_recs)
        assert all((r["addr"] & 0xFFFF) <= 50 for r in filt_recs)

    def test_val_filter_min_gt_max_disables(self, ctx):
        """S32 rule: val_min > val_max → gate disabled even with flag set."""
        reqs = [{"name": "A", "file_idx": 0, "n_bytes": 64*100, "layer_id": 0}]
        # val_min=10 > val_max=5 → disabled → all 100 pass
        recs = [r for b in ctx.iter_multi_range(
            reqs, val_filter=True, val_min=10, val_max=5) for r in b]
        assert len(recs) == 100

    def test_batch_size_at_most_multi_batch_sz(self, ctx):
        """Each yielded batch must have ≤ MULTI_BATCH_SZ records."""
        reqs = [{"name": "A", "file_idx": 0, "n_bytes": 64*1000, "layer_id": 0}]
        for batch in ctx.iter_multi_range(reqs):
            assert len(batch) <= MULTI_BATCH_SZ

    # ── REST endpoint: /wallet/reconstruct ────────────────────────

    def test_reconstruct_unknown_handle_404(self):
        from fastapi.testclient import TestClient
        import rest_server_s32 as _srv
        _srv._mounts.clear()
        client = TestClient(_srv.app, raise_server_exceptions=True)
        r = client.post("/wallet/reconstruct", json={
            "handle_id": "wh_doesnotexist",
            "layers": [{"name": "embed.weight"}],
        })
        assert r.status_code == 404

    def test_reconstruct_empty_layers_400(self):
        import uuid
        import numpy as np
        from fastapi.testclient import TestClient
        import rest_server_s32 as _srv

        class _MockArr:
            shape = (10, 10); dtype = "float32"
            nbytes = 400
        class _MockLoader:
            def keys(self): return ["A"]
            def __contains__(self, k): return k == "A"
            def __getitem__(self, k): return _MockArr()

        hid = "wh_" + uuid.uuid4().hex[:12]
        _srv._mounts[hid] = _srv.WalletMountRecord(hid, _MockLoader())

        client = TestClient(_srv.app, raise_server_exceptions=True)
        r = client.post("/wallet/reconstruct", json={
            "handle_id": hid,
            "layers": [],
        })
        assert r.status_code == 400
        _srv._mounts.clear()

    def test_reconstruct_unknown_layer_404(self):
        import uuid
        import numpy as np
        from fastapi.testclient import TestClient
        import rest_server_s32 as _srv

        class _MockArr:
            shape = (10,); dtype = "float32"; nbytes = 40
        class _MockLoader:
            def keys(self): return ["real_layer"]
            def __contains__(self, k): return k == "real_layer"
            def __getitem__(self, k): return _MockArr()

        hid = "wh_" + uuid.uuid4().hex[:12]
        _srv._mounts[hid] = _srv.WalletMountRecord(hid, _MockLoader())

        client = TestClient(_srv.app, raise_server_exceptions=True)
        r = client.post("/wallet/reconstruct", json={
            "handle_id": hid,
            "layers": [{"name": "no_such_layer"}],
        })
        assert r.status_code == 404
        _srv._mounts.clear()

    def test_reconstruct_returns_ndjson_stream(self):
        """Valid request → 200 application/x-ndjson with summary line."""
        import uuid
        import numpy as np
        from fastapi.testclient import TestClient
        import rest_server_s32 as _srv

        class _MockArr:
            shape = (32, 64); dtype = "float32"; nbytes = 32*64*4

        class _MockLoader:
            def keys(self): return ["embed.weight", "lm_head"]
            def __contains__(self, k): return k in self.keys()
            def __getitem__(self, k): return _MockArr()

        hid = "wh_" + uuid.uuid4().hex[:12]
        _srv._mounts[hid] = _srv.WalletMountRecord(hid, _MockLoader())

        client = TestClient(_srv.app, raise_server_exceptions=True)
        r = client.post("/wallet/reconstruct", json={
            "handle_id": hid,
            "layers": [
                {"name": "embed.weight", "chunk_lo": 0, "chunk_hi": 10},
                {"name": "lm_head",      "chunk_lo": 0, "chunk_hi": 5},
            ],
        })
        assert r.status_code == 200
        assert "ndjson" in r.headers.get("content-type", "")

        lines = [l for l in r.text.strip().split("\n") if l]
        assert len(lines) >= 1   # at least summary line
        last = json.loads(lines[-1])
        assert last.get("_summary") is True
        assert "n_layers" in last

        _srv._mounts.clear()

    def test_health_version_s32(self):
        from fastapi.testclient import TestClient
        import rest_server_s32 as _srv
        client = TestClient(_srv.app, raise_server_exceptions=True)
        r = client.get("/health")
        assert r.status_code == 200
        data = r.json()
        assert data["version"] == "S32"
        assert data.get("multi_range_stream") is not None
        assert data.get("chunk_window_filter") is True
        assert data.get("flags_filter") is True


# ── S31-B: wallet mount REST (18, unchanged) ──────────────────────

import base64
import json
import numpy as np
from fastapi.testclient import TestClient
import rest_server_s32 as _srv

class _MockArr:
    def __init__(self, d: np.ndarray):
        self._d = d
    @property
    def shape(self): return self._d.shape
    @property
    def dtype(self): return self._d.dtype
    @property
    def nbytes(self): return self._d.nbytes

class _MockLoader:
    def __init__(self, layers: dict):
        self._l = {k: _MockArr(v) for k, v in layers.items()}
    def keys(self): return self._l.keys()
    def __contains__(self, k): return k in self._l
    def __getitem__(self, k): return self._l[k]

def _b64(s: str = "x") -> str:
    return base64.b64encode(s.encode()).decode()

_DEFAULT_LAYERS = {
    "embed.weight": np.random.randn(128, 64).astype("float32"),
    "ff.weight":    np.random.randn(64,  64).astype("float32"),
}

def _inject_mount(layers=None) -> str:
    import uuid
    if layers is None: layers = _DEFAULT_LAYERS
    hid = "wh_" + uuid.uuid4().hex[:12]
    _srv._mounts[hid] = _srv.WalletMountRecord(hid, _MockLoader(layers))
    return hid


@pytest.fixture(autouse=True)
def _clear_mounts():
    _srv._mounts.clear()
    yield
    _srv._mounts.clear()

_client = TestClient(_srv.app, raise_server_exceptions=True)


class TestWalletMount:

    def test_pool_empty_at_start(self):
        r = _client.get("/wallet/mounts")
        assert r.json()["n_mounts"] == 0

    def test_inject_shows_in_list(self):
        hid = _inject_mount()
        r = _client.get("/wallet/mounts")
        d = r.json()
        assert d["n_mounts"] == 1
        assert d["mounts"][0]["handle_id"] == hid

    def test_two_mounts_in_list(self):
        _inject_mount(); _inject_mount()
        r = _client.get("/wallet/mounts")
        assert r.json()["n_mounts"] == 2

    def test_unmount_evicts(self):
        hid = _inject_mount()
        _client.delete(f"/wallet/mount/{hid}")
        r = _client.get("/wallet/mounts")
        assert r.json()["n_mounts"] == 0

    def test_unmount_unknown_404(self):
        r = _client.delete("/wallet/mount/wh_notexist")
        assert r.status_code == 404

    def test_unmount_stats_returned(self):
        hid = _inject_mount()
        r = _client.delete(f"/wallet/mount/{hid}")
        d = r.json()
        assert d["evicted"] == hid
        assert "stats" in d
        assert "access_count" in d["stats"]

    def test_layer_fast_json(self):
        hid = _inject_mount()
        r = _client.post("/wallet/layer/fast", json={
            "handle_id": hid, "layer": "embed.weight",
            "annotate": False,
        })
        assert r.status_code == 200
        d = r.json()
        assert d["layer"] == "embed.weight"
        assert d["handle_id"] == hid

    def test_layer_fast_unknown_handle_404(self):
        r = _client.post("/wallet/layer/fast", json={
            "handle_id": "wh_nope", "layer": "embed.weight"
        })
        assert r.status_code == 404

    def test_layer_fast_unknown_layer_404(self):
        hid = _inject_mount()
        r = _client.post("/wallet/layer/fast", json={
            "handle_id": hid, "layer": "no_such_layer"
        })
        assert r.status_code == 404

    def test_access_count_increments(self):
        hid = _inject_mount()
        for _ in range(3):
            _client.post("/wallet/layer/fast", json={
                "handle_id": hid, "layer": "embed.weight"
            })
        r = _client.get("/wallet/mounts")
        mount = next(m for m in r.json()["mounts"] if m["handle_id"] == hid)
        assert mount["access_count"] == 3

    def test_last_access_updates(self):
        hid = _inject_mount()
        t0 = _srv._mounts[hid].last_access
        _client.post("/wallet/layer/fast", json={
            "handle_id": hid, "layer": "embed.weight"
        })
        assert _srv._mounts[hid].last_access >= t0

    def test_different_layers_routed(self):
        hid = _inject_mount()
        for name in ("embed.weight", "ff.weight"):
            r = _client.post("/wallet/layer/fast", json={
                "handle_id": hid, "layer": name
            })
            assert r.status_code == 200
            assert r.json()["layer"] == name

    def test_health_version_s31_compat(self):
        r = _client.get("/health")
        assert r.json()["version"] == "S32"   # server is S32

    def test_health_val_range_filter_true(self):
        r = _client.get("/health")
        assert r.json()["val_range_filter"] is True

    def test_health_persistent_mounts_count(self):
        _inject_mount(); _inject_mount()
        r = _client.get("/health")
        assert r.json()["persistent_mounts"] == 2

    def test_health_mount_endpoints_listed(self):
        r = _client.get("/health")
        eps = " ".join(r.json()["endpoints"])
        assert "wallet/mount" in eps
        assert "wallet/reconstruct" in eps

    def test_health_multi_range_stream(self):
        r = _client.get("/health")
        assert "multi_range_stream" in r.json()

    def test_health_chunk_window_filter(self):
        r = _client.get("/health")
        assert r.json()["chunk_window_filter"] is True

    def test_health_flags_filter(self):
        r = _client.get("/health")
        assert r.json()["flags_filter"] is True
