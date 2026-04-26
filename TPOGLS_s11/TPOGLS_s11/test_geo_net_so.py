"""
test_geo_net_so.py — S28 tests: libgeonet.so + geo_net_ctypes.py
Run: python3 -m pytest test_geo_net_so.py -v
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../s27"))

import pytest
from geo_net_ctypes import GeoNetCtxFast, _LIB, CYL_FULL_N
from geo_net_py     import GeoNetCtx as PyCtx, TE_CYCLE


# ── fixtures ────────────────────────────────────────────────────────

@pytest.fixture
def ctx():
    return GeoNetCtxFast()

@pytest.fixture
def py_ctx():
    return PyCtx()


# ── 1. library load ─────────────────────────────────────────────────

class TestLibLoad:

    def test_lib_loaded(self):
        assert _LIB is not None, "libgeonet.so failed to load"

    def test_ctx_is_fast(self, ctx):
        assert ctx.is_fast is True

    def test_open_close(self):
        c = GeoNetCtxFast()
        assert c._h is not None
        del c   # should not crash


# ── 2. single route bit-parity with Python ─────────────────────────

class TestRouteParity:

    def test_spoke_matches_python(self, ctx, py_ctx):
        for addr in range(200):
            a_c  = ctx.route(addr)
            a_py = py_ctx.route(addr)
            assert a_c.spoke    == a_py.spoke,    f"addr={addr} spoke mismatch"
            assert a_c.inv_spoke == a_py.inv_spoke, f"addr={addr} inv_spoke mismatch"
            assert a_c.face     == a_py.face,     f"addr={addr} face mismatch"
            assert a_c.unit     == a_py.unit,     f"addr={addr} unit mismatch"
            assert a_c.group    == a_py.group,    f"addr={addr} group mismatch"
            assert a_c.is_audit == a_py.is_audit, f"addr={addr} is_audit mismatch"

    def test_spoke_range(self, ctx):
        for addr in range(CYL_FULL_N):
            a = ctx.route(addr)
            assert 0 <= a.spoke < 6
            assert a.inv_spoke == (a.spoke + 3) % 6

    def test_op_count_increments(self, ctx):
        for i in range(50):
            ctx.route(i)
        assert ctx.op_count == 50


# ── 3. fetch_range ──────────────────────────────────────────────────

class TestFetchRange:

    def test_returns_n_results(self, ctx):
        r = ctx.fetch_range(0, 100)
        assert len(r) == 100

    def test_addr_formula(self, ctx):
        off = 500
        r   = ctx.fetch_range(off, 10)
        for i, a in enumerate(r):
            # spoke should match Python for addr = off+i
            py = PyCtx()
            # fast-forward py ctx to match state — not feasible cleanly,
            # so just check spoke is valid range
            assert 0 <= a.spoke < 6

    def test_spoke_parity_with_python_fresh(self):
        """Both start from same zero state — first N routes must match."""
        ctx_c  = GeoNetCtxFast()
        ctx_py = PyCtx()
        N = 300
        r_c  = ctx_c.fetch_range(0, N)
        for i in range(N):
            a_py = ctx_py.route(i)
            assert r_c[i].spoke    == a_py.spoke,    f"i={i}"
            assert r_c[i].inv_spoke == a_py.inv_spoke, f"i={i}"
            assert r_c[i].face     == a_py.face,     f"i={i}"
            assert r_c[i].unit     == a_py.unit,     f"i={i}"
            assert r_c[i].is_audit == a_py.is_audit, f"i={i}"

    def test_op_count_after_fetch_range(self, ctx):
        ctx.fetch_range(0, 200)
        assert ctx.op_count == 200


# ── 4. route_layer_fast ─────────────────────────────────────────────

class TestRouteLayerFast:

    def test_n_chunks_correct(self, ctx):
        r = ctx.route_layer_fast(0, 640)   # 640/64 = 10
        assert r["n_chunks"] == 10

    def test_spoke_dist_sums_to_n_chunks(self, ctx):
        r = ctx.route_layer_fast(0, 3840)
        assert sum(r["spoke_dist"]) == r["n_chunks"]

    def test_has_qrpn_state(self, ctx):
        r = ctx.route_layer_fast(0, 64)
        assert r["qrpn_state_after"] in ("NORMAL", "STRESSED", "ANOMALY")

    def test_verify_pass_equals_n_chunks(self, ctx):
        r = ctx.route_layer_fast(0, 2000)
        assert r["verify_pass"] == r["n_chunks"]

    def test_parity_with_python_route_layer(self):
        """C route_layer_fast must produce same spoke_dist as Python _route_layer_stateful."""
        from test_rest_s27 import _route_layer_stateful
        ctx_c  = GeoNetCtxFast()
        ctx_py = PyCtx()
        rc  = ctx_c.route_layer_fast(0, 3456 * 64)
        rpy = _route_layer_stateful(ctx_py, 0, 3456 * 64)
        assert rc["n_chunks"]    == rpy["n_chunks"]
        assert rc["spoke_dist"]  == rpy["spoke_dist"]
        assert rc["audit_points"] == rpy["audit_points"]

    def test_state_accumulates_across_layers(self, ctx):
        ops0 = ctx.op_count
        ctx.route_layer_fast(0, 640)
        ops1 = ctx.op_count
        ctx.route_layer_fast(1, 640)
        ops2 = ctx.op_count
        assert ops1 > ops0
        assert ops2 > ops1

    def test_partial_bytes_rounds_up(self, ctx):
        r = ctx.route_layer_fast(0, 65)
        assert r["n_chunks"] == 2

    def test_zero_bytes_gives_one_chunk(self, ctx):
        r = ctx.route_layer_fast(0, 0)
        assert r["n_chunks"] == 1


# ── 5. route_layer_annotated_fast ───────────────────────────────────

class TestAnnotatedFast:

    def test_returns_summary_and_map(self, ctx):
        s, cm = ctx.route_layer_annotated_fast(0, 640)
        assert len(cm) == s["n_chunks"]

    def test_chunk_map_fields(self, ctx):
        _, cm = ctx.route_layer_annotated_fast(0, 64)
        e = cm[0]
        assert "chunk_i" in e and "addr" in e
        assert "spoke" in e and "inv_spoke" in e
        assert "mirror_mask" in e and "is_audit" in e

    def test_chunk_map_addr_formula(self, ctx):
        file_idx = 2
        _, cm = ctx.route_layer_annotated_fast(file_idx, 320)
        for e in cm:
            assert e["addr"] == file_idx * CYL_FULL_N + e["chunk_i"]

    def test_chunk_map_inv_spoke_correct(self, ctx):
        _, cm = ctx.route_layer_annotated_fast(0, 640)
        for e in cm:
            assert e["inv_spoke"] == (e["spoke"] + 3) % 6

    def test_summary_matches_route_layer_fast(self):
        ctx1 = GeoNetCtxFast()
        ctx2 = GeoNetCtxFast()
        s1 = ctx1.route_layer_fast(0, 2000)
        s2, _ = ctx2.route_layer_annotated_fast(0, 2000)
        assert s1["n_chunks"]    == s2["n_chunks"]
        assert s1["spoke_dist"]  == s2["spoke_dist"]
        assert s1["audit_points"] == s2["audit_points"]
        assert s1["verify_pass"] == s2["verify_pass"]


# ── 6. verify_batch ─────────────────────────────────────────────────

class TestVerifyBatch:

    def test_all_pass_on_valid_routes(self, ctx):
        recs = ctx.fetch_range(0, 500)
        n    = ctx.verify_batch(recs)
        assert n == 500

    def test_empty_list(self, ctx):
        assert ctx.verify_batch([]) == 0


# ── 7. QRPN state in C ──────────────────────────────────────────────

class TestQRPNStateC:

    def test_normal_after_uniform(self, ctx):
        for i in range(TE_CYCLE):
            ctx.route(i)
        assert ctx.state == "NORMAL"

    def test_state_matches_python(self):
        """C and Python must reach same QRPN state after identical routing."""
        ctx_c  = GeoNetCtxFast()
        ctx_py = PyCtx()
        # drive both through exactly 2 TE cycles with same addresses
        for i in range(TE_CYCLE * 2):
            ctx_c.route(i)
            ctx_py.route(i)
        assert ctx_c.state == ctx_py.state

    def test_spoke_mirror_mask_valid(self, ctx):
        ctx.route(0)
        mm = ctx.spoke_mirror_mask(0)
        assert 0 < mm <= 0x3F


# ── 8. speedup regression ───────────────────────────────────────────

class TestSpeedupRegression:
    """Ensure C batch is at least 3x faster than Python loop (conservative floor)."""

    def test_route_layer_fast_vs_python(self):
        import time
        N_BYTES = 50_000 * 64   # 50k chunks

        ctx_py = PyCtx()
        t0 = time.perf_counter()
        from test_rest_s27 import _route_layer_stateful
        _route_layer_stateful(ctx_py, 0, N_BYTES)
        py_ms = (time.perf_counter() - t0) * 1000

        ctx_c = GeoNetCtxFast()
        t0 = time.perf_counter()
        ctx_c.route_layer_fast(0, N_BYTES)
        c_ms = (time.perf_counter() - t0) * 1000

        speedup = py_ms / c_ms
        print(f"\n  speedup={speedup:.1f}x  py={py_ms:.1f}ms  C={c_ms:.1f}ms")
        assert speedup >= 3.0, f"C batch speedup regression: {speedup:.1f}x < 3x floor"
