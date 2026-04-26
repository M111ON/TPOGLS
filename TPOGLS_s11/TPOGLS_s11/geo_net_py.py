"""
geo_net_py.py — S25: Python port of geo_net.h + geo_cylinder.h
Mirrors C logic exactly — integer only, no float.

Constants (frozen, from geo_config.h):
  CYL_FULL_N   = 3456   GEO_SPOKES = 6   GEO_SLOTS = 576
  GEO_FACE_UNITS = 64   GEO_CENTER_BASE = 512  GN_GROUP_SIZE = 8
"""
from __future__ import annotations
from dataclasses import dataclass

# ── constants ──────────────────────────────────────────────────────
CYL_FULL_N      = 3456
GEO_SPOKES      = 6
GEO_SLOTS       = 576       # per spoke
GEO_FACE_UNITS  = 64
GEO_CENTER_BASE = 512
GN_GROUP_SIZE   = 8

# ── geo_cylinder math ──────────────────────────────────────────────

def _mod6(n: int) -> int:
    """Barrett fast mod6 (mirrors C: (n*10923)>>16)"""
    q = (n * 10923) >> 16
    return n - q * 6

def geo_spoke(idx: int) -> int:       return _mod6(idx % CYL_FULL_N)
def geo_slot(idx: int) -> int:        return (idx % CYL_FULL_N) // GEO_SPOKES
def geo_spoke_invert(s: int) -> int:  return (s + 3) % GEO_SPOKES
def geo_slot_face(slot: int) -> int:  return slot // GEO_FACE_UNITS
def geo_slot_unit(slot: int) -> int:  return slot % GEO_FACE_UNITS
def geo_is_center(slot: int) -> bool: return slot >= GEO_CENTER_BASE

# ── route result ───────────────────────────────────────────────────

@dataclass
class GeoNetAddr:
    spoke:       int
    slot:        int
    face:        int
    unit:        int
    inv_spoke:   int
    group:       int
    is_center:   bool
    is_audit:    bool   # unit % 8 == 7

# ── stateless route (no ThirdEye — mirror_mask omitted) ───────────

def geo_net_route(addr: int) -> GeoNetAddr:
    """
    Route addr → GeoNetAddr.
    Stateless port: ThirdEye mirror_mask skipped (needs full te_tick state).
    """
    full_idx  = addr % CYL_FULL_N
    spoke     = _mod6(full_idx)
    slot      = full_idx // GEO_SPOKES
    face      = geo_slot_face(slot)
    unit      = geo_slot_unit(slot)
    inv       = geo_spoke_invert(spoke)
    group     = unit // GN_GROUP_SIZE
    is_center = geo_is_center(slot)
    is_audit  = (unit % GN_GROUP_SIZE) == (GN_GROUP_SIZE - 1)
    return GeoNetAddr(
        spoke=spoke, slot=slot, face=face, unit=unit,
        inv_spoke=inv, group=group,
        is_center=is_center, is_audit=is_audit,
    )

# ── ThirdEye constants ────────────────────────────────────────────
TE_CYCLE        = 144
TE_MAX_SNAP     = 6
QRPN_NORMAL     = 0
QRPN_STRESSED   = 1
QRPN_ANOMALY    = 2
QRPN_HOT_THRESH   = 64
QRPN_IMBAL_THRESH = 72
QRPN_ANOMALY_HOT  = 96

_STATE_NAME = {QRPN_NORMAL: "NORMAL", QRPN_STRESSED: "STRESSED", QRPN_ANOMALY: "ANOMALY"}

# ── ThirdEye ───────────────────────────────────────────────────────

class ThirdEye:
    """Port of geo_thirdeye.h — stateful QRPN observer."""

    __slots__ = ("op_count", "qrpn_state", "_cur", "_ring")

    def __init__(self):
        self.op_count   = 0
        self.qrpn_state = QRPN_NORMAL
        self._cur       = {"spoke_count": [0]*6, "hot_slots": 0}
        self._ring      = []   # last TE_MAX_SNAP snapshots

    @staticmethod
    def _eval_state(snap: dict) -> int:
        sc = snap["spoke_count"]
        imbal = any(abs(sc[i] - sc[i+3]) > QRPN_IMBAL_THRESH for i in range(3))
        hot      = snap["hot_slots"] > QRPN_HOT_THRESH
        very_hot = snap["hot_slots"] > QRPN_ANOMALY_HOT
        if imbal or very_hot: return QRPN_ANOMALY
        if hot:               return QRPN_STRESSED
        return QRPN_NORMAL

    def tick(self, spoke: int, slot_hot: int = 0) -> None:
        self.op_count += 1
        self._cur["spoke_count"][spoke % 6] += 1
        if slot_hot:
            self._cur["hot_slots"] += 1

        if self.op_count % TE_CYCLE == 0:
            state = self._eval_state(self._cur)
            self.qrpn_state = state
            snap = {"spoke_count": self._cur["spoke_count"][:], "hot_slots": self._cur["hot_slots"]}
            self._ring.append(snap)
            if len(self._ring) > TE_MAX_SNAP:
                self._ring.pop(0)
            self._cur = {"spoke_count": [0]*6, "hot_slots": 0}

    def mirror_mask(self, spoke: int) -> int:
        s = self.qrpn_state
        if s == QRPN_ANOMALY: return 0x3F
        mask = (1 << ((spoke+1) % 6)) | (1 << ((spoke+5) % 6))
        if s >= QRPN_STRESSED:
            mask |= (1 << ((spoke+3) % 6))
        return mask

    @property
    def state_name(self) -> str:
        return _STATE_NAME[self.qrpn_state]


# ── stateful GeoNetCtx ─────────────────────────────────────────────

class GeoNetCtx:
    """
    Stateful geo_net context — mirrors GeoNet + ThirdEye.
    Call route(addr) instead of bare geo_net_route() for mirror_mask.
    """

    def __init__(self):
        self._te    = ThirdEye()
        self.op_count = 0

    def route(self, addr: int, slot_hot: int = 0) -> GeoNetAddr:
        full_idx  = addr % CYL_FULL_N
        spoke     = _mod6(full_idx)
        slot      = full_idx // GEO_SPOKES
        face      = geo_slot_face(slot)
        unit      = geo_slot_unit(slot)
        inv       = geo_spoke_invert(spoke)
        group     = unit // GN_GROUP_SIZE
        is_center = geo_is_center(slot)
        is_audit  = (unit % GN_GROUP_SIZE) == (GN_GROUP_SIZE - 1)

        self._te.tick(spoke, slot_hot)
        self.op_count += 1

        return GeoNetAddr(
            spoke=spoke, slot=slot, face=face, unit=unit,
            inv_spoke=inv, group=group,
            is_center=is_center, is_audit=is_audit,
        )

    @property
    def state(self) -> str:
        return self._te.state_name

    @property
    def mirror_mask(self) -> int:
        """Current mirror mask (last routed spoke)."""
        return self._te.mirror_mask(0)   # caller can pass spoke explicitly

    def spoke_mirror_mask(self, spoke: int) -> int:
        return self._te.mirror_mask(spoke)

    def status(self) -> dict:
        te = self._te
        return {
            "ops":        self.op_count,
            "qrpn_state": te.state_name,
            "cycle_pos":  te.op_count % TE_CYCLE,
            "snaps":      len(te._ring),
            "spoke_count": te._cur["spoke_count"][:],
            "hot_slots":  te._cur["hot_slots"],
        }


# ── layer routing summary ──────────────────────────────────────────

def route_layer(file_idx: int, n_bytes: int, chunk_size: int = 64) -> dict:
    """
    Simulate geo_net routing for a layer (file_idx, n_bytes).
    Treats each 64B chunk as one addr = file_idx * CYL_FULL_N + chunk_i.
    Returns summary: spoke distribution, face distribution, audit count.
    """
    n_chunks    = max(1, (n_bytes + chunk_size - 1) // chunk_size)
    spoke_dist  = [0] * GEO_SPOKES
    face_dist   = [0] * (GEO_FACE_UNITS // GN_GROUP_SIZE + 1)  # 9 faces
    audit_count = 0
    center_count= 0

    for i in range(n_chunks):
        addr = file_idx * CYL_FULL_N + i
        a    = geo_net_route(addr)
        spoke_dist[a.spoke] += 1
        face_dist[a.face]   += 1
        if a.is_audit:   audit_count  += 1
        if a.is_center:  center_count += 1

    return {
        "n_chunks":     n_chunks,
        "spoke_dist":   spoke_dist,
        "face_dist":    face_dist[:9],
        "audit_points": audit_count,
        "center_chunks":center_count,
        "dominant_spoke": int(spoke_dist.index(max(spoke_dist))),
    }
