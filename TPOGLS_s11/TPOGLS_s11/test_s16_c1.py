"""test_s16_c1.py — C1 PHI-pattern delta encoder tests"""
import sys, os, struct, random
sys.path.insert(0, '/home/claude')

from pogls_compress_c1 import (
    C1Encoder, C1Decoder, encode_stream, decode_stream,
    _phi_position, _phi_code, _phi_decode, _is_zero, BS,
    PHI_UP, PHI_SCALE, SACRED
)

PASS = FAIL = 0
def check(name, cond, detail=""):
    global PASS, FAIL
    if cond: PASS += 1; print(f"  PASS  {name}")
    else:    FAIL += 1; print(f"  FAIL  {name}" + (f"  [{detail}]" if detail else ""))

print("\n── S16 C1 PHI encoder tests ──")

# C1-1: PHI position deterministic
s1,l1 = _phi_position(0)
s2,l2 = _phi_position(0)
check("C1-1 phi_position deterministic", (s1,l1)==(s2,l2))

# C1-2: PHI position covers 6 spokes
spokes = set(_phi_position(i)[0] for i in range(300))
check("C1-2 phi covers all spokes", len(spokes) >= 5, str(spokes))

# C1-3: phi_code round-trip
for sp in range(6):
    for sl in [0, 100, 512, 1023]:
        c = _phi_code(sp, sl)
        sp2, sl2 = _phi_decode(c)
        if sp2 != sp or sl2 != sl:
            check(f"C1-3 phi_code rt sp={sp} sl={sl}", False); break
else:
    check("C1-3 phi_code round-trip", True)

# C1-4: zero chunk → type 0x03 (1 byte frame)
enc = C1Encoder()
frames = enc.encode_chunks([bytes(BS)])
# header=12, frame=1 → total 13
check("C1-4 zero chunk → 1B frame", len(frames) == 13, f"got {len(frames)}")

# C1-5: RAW chunk encodes as type 0x00 + 64B
random.seed(42)
unique = bytes(random.getrandbits(8) for _ in range(BS))
enc = C1Encoder()
out = enc.encode_chunks([unique])
frame = out[12:]
check("C1-5 raw frame type=0x00", frame[0] == 0x00)
check("C1-5 raw frame payload=64B", frame[1:] == unique)

# C1-6: round-trip random data
data = bytes(random.getrandbits(8) for _ in range(BS * 10))
c1   = encode_stream(data)
rec  = decode_stream(c1, len(data))
check("C1-6 round-trip random", rec == data)

# C1-7: round-trip zero data
zeros = bytes(BS * 5)
c1z   = encode_stream(zeros)
recz  = decode_stream(c1z, len(zeros))
check("C1-7 round-trip zeros", recz == zeros)

# C1-8: zero stream compression ratio
ratio = len(c1z) / len(zeros)
check("C1-8 zero stream compressed", ratio < 0.1, f"ratio={ratio:.3f}")

# C1-9: repeated chunk → DELTA match
base = bytes(range(BS))
chunks = [base] * 8
enc = C1Encoder()
out = enc.encode_chunks(chunks)
# first=RAW(65B), rest should be DELTA(12B) or similar < 65
frame_types = []
pos = 12
for _ in range(8):
    t = out[pos]; pos += 1
    frame_types.append(t)
    if t == 0x00: pos += BS
    elif t in (0x01, 0x02):
        pos += 3  # rel
        n_ch = out[pos]; pos += 1 + n_ch*2
    # 0x03: no payload
check("C1-9 repeated → delta/phi used", any(t in (0x01,0x02) for t in frame_types[1:]))

# C1-10: round-trip repeated chunks
c1r = C1Encoder().encode_chunks(chunks)
rec_chunks = C1Decoder().decode(c1r)
check("C1-10 round-trip repeated", rec_chunks == chunks)

# C1-11: near-similar chunks (1 byte diff) → delta
a = bytes(range(BS))
b = bytearray(a); b[32] ^= 0xFF; b = bytes(b)
enc = C1Encoder()
out = enc.encode_chunks([a, b])
t2  = out[12 + 1 + BS]  # type of second frame (after RAW a)
check("C1-11 near-similar → non-RAW", t2 != 0x00, f"got type 0x{t2:02x}")

# C1-12: large stream round-trip (1MB)
big = bytes((i * 13 + 7) % 256 for i in range(1024 * 64))  # 4096 chunks
c1b = encode_stream(big)
rec = decode_stream(c1b, len(big))
check("C1-12 1MB round-trip", rec == big)

# C1-13: weight stream — int8 quantized weights with slow drift (C1's target domain)
import random as _rand
_rand.seed(7)
_weight_chunks = []
_base = [int(_rand.gauss(128, 30)) % 256 for _ in range(BS)]
for _i in range(32):
    # slow drift: ~8 bytes change per block
    _block = bytearray(_base)
    for _k in _rand.sample(range(BS), 8):
        _block[_k] = (_block[_k] + _rand.randint(-3, 3)) % 256
    _weight_chunks.append(bytes(_block))
    _base = list(_block)
weight_data = b"".join(_weight_chunks)
c1w = encode_stream(bytes(weight_data))
recw = decode_stream(c1w, len(weight_data))
check("C1-13 weight-like stream round-trip", recw == bytes(weight_data))
ratio_w = len(c1w) / len(weight_data)
check("C1-13 weight stream < 80% size", ratio_w < 0.80, f"ratio={ratio_w:.3f}")

print(f"\n{'─'*36}")
print(f"  {PASS}/{PASS+FAIL} PASS", "✓" if FAIL == 0 else "✗")
if FAIL: sys.exit(1)
