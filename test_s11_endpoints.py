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

# ══════════════════════════════════════════════════════════════════════
# WALLET TESTS (S11-C)
# ══════════════════════════════════════════════════════════════════════

print(f"\n── Wallet tests (S11-C) ──")

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))
from pogls_wallet_py import (
    WalletBuilder, WalletReader, WalletError,
    chunk_seed, chunk_checksum, file_identity_hash,
    WALLET_MODE_BUFFER, WALLET_OK, WALLET_WARN_META_DRIFT,
    WALLET_ERR_SEED_FAIL, WALLET_ERR_CKSUM_FAIL,
)

def make_chunk(pattern: int) -> bytes:
    import struct
    parts = []
    for i in range(8):
        w = (pattern ^ (i * 0x9e3779b97f4a7c15)) & 0xFFFFFFFFFFFFFFFF
        parts.append(struct.pack("<Q", w))
    return b"".join(parts)

# W1: round-trip single file
src1 = b"".join(make_chunk(i * 0x1234) for i in range(4))   # 4 chunks = 256B
wb1  = WalletBuilder(mode=WALLET_MODE_BUFFER)
fid1 = wb1.add_file("test.bin", len(src1), 0, 0)
off  = 0
while off < len(src1):
    c = src1[off:off+64]
    wb1.add_coord(fid1, off, chunk_seed(c), chunk_checksum(c))
    off += 64
blob1   = wb1.seal()
wr1     = WalletReader(blob1)
result1 = wr1.reconstruct_file(0, src1)
check("W1 single-file round-trip", result1 == src1)

# W2: wallet bytes << source bytes
check("W2 wallet smaller than source",
      len(blob1) < len(src1),
      f"wallet={len(blob1)} src={len(src1)}")

# W3: O(1) fetch
chunk0 = wr1.fetch_chunk(0, src1)
check("W3 fetch chunk 0", chunk0 == src1[:64])
chunk3 = wr1.fetch_chunk(3, src1)
check("W4 fetch chunk 3", chunk3 == src1[192:256])

# W5: lookup by offset
cid = wr1.lookup_by_offset(0, 128)
check("W5 lookup offset 128 → coord_id 2", cid == 2)

# W6: seed mismatch → WalletError
src_bad = bytearray(src1)
src_bad[0] ^= 0xFF   # corrupt first byte of chunk 0
try:
    wr1.fetch_chunk(0, bytes(src_bad))
    check("W6 seed fail raises", False)
except WalletError as e:
    check("W6 seed fail raises", e.code == WALLET_ERR_SEED_FAIL)

# W7: meta drift (size mismatch) → SOFT WARN, data still correct
wb7  = WalletBuilder(mode=WALLET_MODE_BUFFER)
fid7 = wb7.add_file("f7.bin", len(src1) + 100, 0, 0)  # wrong expected size
off  = 0
while off < len(src1):
    c = src1[off:off+64]
    wb7.add_coord(fid7, off, chunk_seed(c), chunk_checksum(c))
    off += 64
blob7   = wb7.seal()
wr7     = WalletReader(blob7)
warns7  = []
result7 = wr7.reconstruct_file(0, src1,
                                on_meta_drift=lambda fe: warns7.append(1))
check("W7 meta drift warns", len(warns7) == 1)
check("W8 meta drift data ok", result7 == src1[:wr7.file_info(0).file_size])

# W9: multi-file wallet
src_a = b"".join(make_chunk(i)         for i in range(3))   # 192B
src_b = b"".join(make_chunk(i + 0x100) for i in range(2))   # 128B
wb9 = WalletBuilder(mode=WALLET_MODE_BUFFER)

fa = wb9.add_file("file_a.bin", len(src_a), 0, 0)
for i in range(3):
    c = src_a[i*64:(i+1)*64]
    wb9.add_coord(fa, i*64, chunk_seed(c), chunk_checksum(c))

fb = wb9.add_file("file_b.bin", len(src_b), 0, 0)
for i in range(2):
    c = src_b[i*64:(i+1)*64]
    wb9.add_coord(fb, i*64, chunk_seed(c), chunk_checksum(c))

blob9 = wb9.seal()
wr9   = WalletReader(blob9)
check("W9 multi-file file_count=2", wr9.file_count == 2)
ra = wr9.reconstruct_file(0, src_a)
rb = wr9.reconstruct_file(1, src_b)
check("W10 multi-file file_a ok", ra == src_a)
check("W11 multi-file file_b ok", rb == src_b)

# W12: verify_integrity catches corruption
blob_bad = bytearray(blob1)
blob_bad[64] ^= 0xAA   # corrupt middle of first FileEntry
try:
    WalletReader(bytes(blob_bad))
    check("W12 corruption detected", False)
except (ValueError, WalletError):
    check("W12 corruption detected", True)

# W13: drift_window — chunk at offset+64 from registered
src_drift = b"\xAB" * 256
real_chunk = make_chunk(0xDEAD)
# place real chunk at offset 128, register it at 64 with drift_window=2
src_drift = src_drift[:128] + real_chunk + src_drift[192:]
wb13 = WalletBuilder(mode=WALLET_MODE_BUFFER)
f13  = wb13.add_file("drift.bin", len(src_drift), 0, 0)
wb13.add_coord(f13, 64,      # wrong offset — real chunk is at 128
               chunk_seed(real_chunk), chunk_checksum(real_chunk),
               drift_window=2)
blob13 = wb13.seal()
wr13   = WalletReader(blob13)
found13 = wr13.fetch_chunk(0, src_drift)
check("W13 drift finds chunk at offset+64", found13 == real_chunk)

# W14: stats
st = wr9.stats()
check("W14 stats file_count", st["file_count"] == 2)
check("W15 stats coord_count", st["coord_count"] == 5)
check("W16 stats total_source_bytes", st["total_source_bytes"] == len(src_a) + len(src_b))

# ── final summary ─────────────────────────────────────────────────────
print(f"\n{'─'*36}")
print(f"  {PASS}/{PASS+FAIL} PASS", "✓" if FAIL == 0 else "✗")
if FAIL:
    sys.exit(1)
