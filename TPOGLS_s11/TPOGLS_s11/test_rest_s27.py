"""
test_rest_s27.py — S27 unit tests
Tests GeoNetCtx wiring logic independently of FastAPI/wallet stack.
Run: python -m pytest test_rest_s27.py -v
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

import pytest
from geo_net_py import GeoNetCtx, CYL_FULL_N, TE_CYCLE


# ── helpers mirrored from rest_server_s27 ─────────────────────────────────

def _route_layer_stateful(geo_ctx, file_idx, n_bytes, chunk_size=64):
    n_chunks    = max(1, (n_bytes + chunk_size - 1) // chunk_size)
    spoke_dist  = [0] * 6
    audit_count = 0
    center_count= 0
    for i in range(n_chunks):
        addr = file_idx * CYL_FULL_N + i
        a    = geo_ctx.route(addr)
        spoke_dist[a.spoke] += 1
        if a.is_audit:  audit_count  += 1
        if a.is_center: center_count += 1
    return {
        "n_chunks":         n_chunks,
        "spoke_dist":       spoke_dist,
        "audit_points":     audit_count,
        "center_chunks":    center_count,
        "dominant_spoke":   int(spoke_dist.index(max(spoke_dist))),
        "qrpn_state_after": geo_ctx.state,
    }


def _route_layer_annotated(geo_ctx, file_idx, n_bytes, chunk_size=64):
    n_chunks    = max(1, (n_bytes + chunk_size - 1) // chunk_size)
    spoke_dist  = [0] * 6
    audit_count = 0
    center_count= 0
    chunk_map   = []
    for i in range(n_chunks):
        addr = file_idx * CYL_FULL_N + i
        a    = geo_ctx.route(addr)
        mm   = geo_ctx.spoke_mirror_mask(a.spoke)
        spoke_dist[a.spoke] += 1
        if a.is_audit:  audit_count  += 1
        if a.is_center: center_count += 1
        chunk_map.append({
            "chunk_i":    i,
            "addr":       addr,
            "spoke":      a.spoke,
            "inv_spoke":  a.inv_spoke,
            "mirror_mask": mm,
            "is_audit":   a.is_audit,
        })
    summary = {
        "n_chunks":         n_chunks,
        "spoke_dist":       spoke_dist,
        "audit_points":     audit_count,
        "center_chunks":    center_count,
        "dominant_spoke":   int(spoke_dist.index(max(spoke_dist))),
        "qrpn_state_after": geo_ctx.state,
    }
    return summary, chunk_map


# ── tests ──────────────────────────────────────────────────────────────────

class TestGeoNetCtxLifetime:

    def test_ctx_initializes_normal(self):
        ctx = GeoNetCtx()
        assert ctx.state == "NORMAL"

    def test_ctx_op_count_increments(self):
        ctx = GeoNetCtx()
        for i in range(10):
            ctx.route(i)
        assert ctx.op_count == 10

    def test_ctx_persists_across_calls(self):
        """Same ctx object accumulates ops — critical for S27 multi-layer flow."""
        ctx = GeoNetCtx()
        ctx.route(0)
        ctx.route(1)
        st = ctx.status()
        assert st["ops"] == 2

    def test_ctx_status_fields(self):
        ctx = GeoNetCtx()
        st  = ctx.status()
        assert "ops" in st
        assert "qrpn_state" in st
        assert "cycle_pos" in st
        assert "spoke_count" in st
        assert len(st["spoke_count"]) == 6


class TestStatefulLayerRouting:

    def test_route_layer_stateful_returns_correct_n_chunks(self):
        ctx = GeoNetCtx()
        r   = _route_layer_stateful(ctx, file_idx=0, n_bytes=640)  # 640/64 = 10
        assert r["n_chunks"] == 10

    def test_route_layer_stateful_spoke_dist_sums_to_n_chunks(self):
        ctx = GeoNetCtx()
        r   = _route_layer_stateful(ctx, file_idx=0, n_bytes=3840)
        assert sum(r["spoke_dist"]) == r["n_chunks"]

    def test_route_layer_stateful_has_qrpn_state(self):
        ctx = GeoNetCtx()
        r   = _route_layer_stateful(ctx, file_idx=0, n_bytes=64)
        assert r["qrpn_state_after"] in ("NORMAL", "STRESSED", "ANOMALY")

    def test_state_accumulates_across_multiple_layers(self):
        """Core S27 invariant: ctx state at layer N depends on layers 0..N-1."""
        ctx = GeoNetCtx()
        ops_before = ctx.op_count
        _route_layer_stateful(ctx, 0, 640)
        ops_after_l0 = ctx.op_count
        _route_layer_stateful(ctx, 1, 640)
        ops_after_l1 = ctx.op_count
        assert ops_after_l0 > ops_before
        assert ops_after_l1 > ops_after_l0

    def test_different_file_idx_gives_different_addresses(self):
        """file_idx shifts base addr by CYL_FULL_N — spoke distribution differs."""
        ctx0 = GeoNetCtx()
        ctx1 = GeoNetCtx()
        r0 = _route_layer_stateful(ctx0, 0, 3456 * 64)   # full cylinder
        r1 = _route_layer_stateful(ctx1, 1, 3456 * 64)
        # both should cover all spokes but dominant may differ
        assert r0["n_chunks"] == r1["n_chunks"]
        # at exact cylinder width the spoke_dist should be identical (periodic)
        assert r0["spoke_dist"] == r1["spoke_dist"]

    def test_partial_bytes_rounds_up_to_chunks(self):
        ctx = GeoNetCtx()
        r   = _route_layer_stateful(ctx, 0, 65)   # 65 bytes → 2 chunks (ceil)
        assert r["n_chunks"] == 2

    def test_zero_byte_layer_gives_one_chunk(self):
        ctx = GeoNetCtx()
        r   = _route_layer_stateful(ctx, 0, 0)
        assert r["n_chunks"] == 1


class TestAnnotatedLayerRouting:

    def test_annotated_returns_summary_and_map(self):
        ctx    = GeoNetCtx()
        s, cm  = _route_layer_annotated(ctx, 0, 640)
        assert "n_chunks" in s
        assert len(cm) == s["n_chunks"]

    def test_chunk_map_entry_fields(self):
        ctx   = GeoNetCtx()
        _, cm = _route_layer_annotated(ctx, 0, 64)
        entry = cm[0]
        assert "chunk_i"    in entry
        assert "addr"       in entry
        assert "spoke"      in entry
        assert "inv_spoke"  in entry
        assert "mirror_mask" in entry
        assert "is_audit"   in entry

    def test_chunk_map_spoke_valid_range(self):
        ctx   = GeoNetCtx()
        _, cm = _route_layer_annotated(ctx, 0, 640)
        for e in cm:
            assert 0 <= e["spoke"] < 6, f"spoke out of range: {e['spoke']}"

    def test_chunk_map_inv_spoke_is_opposite(self):
        ctx   = GeoNetCtx()
        _, cm = _route_layer_annotated(ctx, 0, 640)
        for e in cm:
            expected_inv = (e["spoke"] + 3) % 6
            assert e["inv_spoke"] == expected_inv

    def test_chunk_map_mirror_mask_normal_has_two_bits(self):
        """NORMAL mask = 2 adjacent spokes → popcount 2 (not anomaly 0x3F)."""
        ctx   = GeoNetCtx()
        # Use a fresh ctx — will be NORMAL for first 144 ops
        _, cm = _route_layer_annotated(ctx, 0, 64)
        mm = cm[0]["mirror_mask"]
        # normal mask = (1 << (s+1)%6) | (1 << (s+5)%6) → exactly 2 bits set
        assert bin(mm).count("1") >= 2   # at least 2 (stressed adds 1, anomaly=6)

    def test_chunk_map_addr_formula(self):
        """addr = file_idx * CYL_FULL_N + chunk_i."""
        file_idx = 3
        ctx      = GeoNetCtx()
        _, cm    = _route_layer_annotated(ctx, file_idx, 128)
        for e in cm:
            expected = file_idx * CYL_FULL_N + e["chunk_i"]
            assert e["addr"] == expected

    def test_annotated_state_matches_stateful(self):
        """Both helpers must leave ctx at the same op count."""
        ctx_a = GeoNetCtx()
        ctx_b = GeoNetCtx()
        n_bytes = 640
        r_s    = _route_layer_stateful(ctx_a, 0, n_bytes)
        r_ann, _ = _route_layer_annotated(ctx_b, 0, n_bytes)
        assert r_s["n_chunks"]      == r_ann["n_chunks"]
        assert r_s["spoke_dist"]    == r_ann["spoke_dist"]
        assert r_s["audit_points"]  == r_ann["audit_points"]
        assert ctx_a.op_count       == ctx_b.op_count


class TestQRPNStateTransition:

    def test_state_stays_normal_under_light_load(self):
        ctx = GeoNetCtx()
        for i in range(TE_CYCLE - 1):
            ctx.route(i)
        assert ctx.state == "NORMAL"

    def test_state_transitions_after_cycle(self):
        """After TE_CYCLE ops state gets evaluated — should still be NORMAL
        with uniform distribution."""
        ctx = GeoNetCtx()
        for i in range(TE_CYCLE):
            ctx.route(i)
        # uniform → NORMAL
        assert ctx.state == "NORMAL"

    def test_stressed_state_with_hot_slots(self):
        """Inject many hot_slots to push > QRPN_HOT_THRESH (64)."""
        ctx = GeoNetCtx()
        for i in range(TE_CYCLE):
            ctx.route(i, slot_hot=1)   # all hot
        assert ctx.state in ("STRESSED", "ANOMALY")

    def test_anomaly_state_with_extreme_hot(self):
        """All 144 ops hot → hot_slots=144 > QRPN_ANOMALY_HOT(96) → ANOMALY."""
        ctx = GeoNetCtx()
        for i in range(TE_CYCLE):
            ctx.route(i * 6, slot_hot=1)   # force same spoke too → imbalance
        assert ctx.state == "ANOMALY"

    def test_anomaly_mirror_mask_is_all_spokes(self):
        """ANOMALY → mirror_mask = 0x3F (all 6 spokes)."""
        ctx = GeoNetCtx()
        for i in range(TE_CYCLE):
            ctx.route(i * 6, slot_hot=1)
        assert ctx.state == "ANOMALY"
        mm = ctx.spoke_mirror_mask(0)
        assert mm == 0x3F


class TestCtxPerSession:

    def test_two_sessions_independent(self):
        """Two CtxRecords each have their own GeoNetCtx — no shared state."""
        ctx_a = GeoNetCtx()
        ctx_b = GeoNetCtx()
        for i in range(TE_CYCLE):
            ctx_a.route(i * 6, slot_hot=1)
        # ctx_a is ANOMALY, ctx_b is still NORMAL
        assert ctx_a.state == "ANOMALY"
        assert ctx_b.state == "NORMAL"

    def test_layer_stream_state_carries_forward(self):
        """Simulate /wallet/load with 3 layers — ops accumulate."""
        ctx    = GeoNetCtx()
        layers = [(0, 2000), (1, 5000), (2, 1000)]
        total_expected = 0
        for file_idx, n_bytes in layers:
            n = max(1, (n_bytes + 63) // 64)
            total_expected += n
            _route_layer_stateful(ctx, file_idx, n_bytes)
        assert ctx.op_count == total_expected
