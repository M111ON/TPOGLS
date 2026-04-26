"""
test_s11_endpoints.py — unit test for S11 encode/decode/read logic
Runs WITHOUT server (tests helper functions directly)
"""
import base64, hashlib, struct, sys, os

# ── stub PoglsEngine for offline testing ─────────────────────────────
class _StubEngine:
    """Minimal stub — XOR-pad encode, identity decode (for logic test)."""
    def __enter__(self): return self
    def __exit__(self, *_): pass
    def encode(self, data: bytes):
        self._data = data
        self._orig = len(data)
        # pad to 8B boundary
        pad = (8 - len(data) % 8) % 8
        self._blocks = [data + b'\x00' * pad]
    def decode(self) -> bytes:
        return b''.join(self._blocks)
    def stats(self): pass
    def stats_out(self) -> dict:
        return {f: 0 for f in [
            "version","ctx_writes","ctx_reads","ctx_qrpns",
            "l1_hits","l1_misses","l1_hit_pct_x100",
            "kv_capacity","kv_live","kv_tomb",
            "kv_load_pct_x100","kv_tomb_pct_x100",
            "kv_puts","kv_gets","kv_misses","kv_max_probe",
            "gpu_ring_pending","gpu_ring_flushed","gpu_ring_dropped",
        ]}

# patch before import
import unittest.mock as mock
sys.modules.setdefault("pogls_engine", mock.MagicMock())

# ── inline the helpers (copy from rest_server_s11.py) ────────────────
def _engine_encode(data: bytes, _Eng=_StubEngine) -> bytes:
    with _Eng() as eng:
        eng.encode(data)
        recovered = eng.decode()
    hdr = struct.pack("<I", len(data))
    return hdr + recovered

def _engine_decode(blob: bytes, _Eng=_StubEngine) -> bytes:
    if len(blob) < 4:
        raise ValueError("blob too short")
    orig_len = struct.unpack("<I", blob[:4])[0]
    payload  = blob[4:]
    with _Eng() as eng:
        eng.encode(payload)
        out = eng.decode()
    return out[:orig_len]

# ══════════════════════════════════════════════════════════════════════
# TESTS
# ══════════════════════════════════════════════════════════════════════

PASS = 0
FAIL = 0

def check(name, cond, note=""):
    global PASS, FAIL
    if cond:
        print(f"  PASS  {name}")
        PASS += 1
    else:
        print(f"  FAIL  {name}  {note}")
        FAIL += 1

print("── S11 endpoint logic tests ──")

# T1: encode + decode round-trip (small)
data1 = b"Hello POGLS S11! " * 10
blob1 = _engine_encode(data1)
out1  = _engine_decode(blob1)
check("T1 round-trip small", out1 == data1, f"in={len(data1)} out={len(out1)}")

# T2: round-trip with binary data
data2 = bytes(range(256)) * 4
blob2 = _engine_encode(data2)
out2  = _engine_decode(blob2)
check("T2 round-trip binary", out2 == data2)

# T3: blob has 4-byte header
check("T3 blob header present", len(blob1) >= 4)
orig_in_hdr = struct.unpack("<I", blob1[:4])[0]
check("T4 blob header correct", orig_in_hdr == len(data1),
      f"got={orig_in_hdr} want={len(data1)}")

# T4: partial read — offset/length slicing
data3 = b"ABCDEFGHIJ" * 20   # 200 bytes
blob3 = _engine_encode(data3)
full  = _engine_decode(blob3)
slice_ = full[10:50]
check("T5 partial read slice", slice_ == data3[10:50],
      f"got={slice_.hex()[:20]} want={data3[10:50].hex()[:20]}")

# T5: offset clamp
check("T6 clamp end of file",  full[195:210] == data3[195:200])

# T6: non-8B-aligned length
data4 = b"x" * 17   # not multiple of 8
blob4 = _engine_encode(data4)
out4  = _engine_decode(blob4)
check("T7 non-aligned 17B", out4 == data4, f"len={len(out4)}")

# T7: 1 byte
data5 = b"\xAB"
blob5 = _engine_encode(data5)
out5  = _engine_decode(blob5)
check("T8 single byte", out5 == data5)

# T8: blob too short
try:
    _engine_decode(b"\x01\x00")
    check("T9 short blob raises", False)
except ValueError:
    check("T9 short blob raises", True)

# T9: sha256 integrity over large data
data6 = os.urandom(4096)
blob6 = _engine_encode(data6)
out6  = _engine_decode(blob6)
check("T10 sha256 4096B",
      hashlib.sha256(out6).digest() == hashlib.sha256(data6).digest())

# ── summary ──────────────────────────────────────────────────────────
print(f"\n{'─'*36}")
print(f"  {PASS}/{PASS+FAIL} PASS", "✓" if FAIL == 0 else "✗")
if FAIL:
    sys.exit(1)
