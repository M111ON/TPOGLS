"""Minimal geo_net_py stub for test environment (no .so, no geo_net_py)."""

QRPN_ANOMALY_HOT = 5
QRPN_ANOMALY     = 2

_STATE_NAME = {0: "NORMAL", 1: "STRESSED", 2: "ANOMALY"}

class _ThirdEye:
    def __init__(self):
        self._cur = {"hot_slots": 0}
        self.qrpn_state = 0
    def get_mask(self, spoke): return 0x3F

class GeoNetCtx:
    CYL_FULL_N = 3456
    GN_GROUP   = 6
    GN_GROUP_SIZE = 8

    def __init__(self):
        self._te    = _ThirdEye()
        self._ops   = 0
        self._a_sig = 0

    def route(self, addr, slot_hot=0):
        self._ops += 1
        if slot_hot:
            self._te._cur["hot_slots"] += 1
        from dataclasses import dataclass
        spoke     = int(addr) % 6
        inv_spoke = (spoke + 3) % 6
        face      = int(addr) % 9
        unit      = int(addr) % 54
        group     = unit // 6
        is_audit  = (unit % 8 == 7)
        is_center = (unit == 0)

        class A:
            pass
        a = A()
        a.spoke = spoke; a.inv_spoke = inv_spoke; a.face = face
        a.unit = unit;   a.group = group
        a.is_audit = is_audit; a.is_center = is_center
        a.mirror_mask = 0x3F
        return a

    def spoke_mirror_mask(self, spoke): return 0x3F

    @property
    def state(self):
        hs = self._te._cur["hot_slots"]
        if self._te.qrpn_state >= QRPN_ANOMALY: return "ANOMALY"
        if hs > QRPN_ANOMALY_HOT:               return "ANOMALY"
        if hs > 2:                               return "STRESSED"
        return "NORMAL"

    @property
    def op_count(self): return self._ops

    def status(self):
        return {
            "ops": self._ops, "qrpn_state": self.state,
            "anomaly_signals": self._a_sig,
            "filter_pushdown": False, "slot_hot_wired": False,
            "iter_streaming": False, "multi_range": False,
        }
