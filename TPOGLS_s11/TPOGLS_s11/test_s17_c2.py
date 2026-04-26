"""test_s17_c2.py — C2 chunk index tests"""
import sys, struct, random
sys.path.insert(0, '/home/claude')

from pogls_compress_c2 import (
    C2Encoder, C2Decoder, c2_encode, c2_decode, c2_decode_range,
    MAGIC, BS, _split_chunks
)

PASS = FAIL = 0
def check(name, cond, detail=""):
    global PASS, FAIL
    if cond: PASS += 1; print(f"  PASS  {name}")
    else:    FAIL += 1; print(f"  FAIL  {name}" + (f"  [{detail}]" if detail else ""))

print("\n── S17 C2 chunk index tests ──")

random.seed(42)
data256  = bytes(range(256))                          # 4 chunks
data1k   = bytes(random.getrandbits(8) for _ in range(1024))  # 16 chunks
data_big = bytes((i*13+7)%256 for i in range(BS*64)) # 64 chunks

# C2-1: header magic correct
c2 = c2_encode(data256)
check("C2-1 magic", c2[:4] == MAGIC)

# C2-2: n_chunks in header
_, _, _, n, orig, seg = struct.unpack_from(">4sBBIQI", c2, 0)
check("C2-2 n_chunks=4", n == 4)
check("C2-2 orig_size", orig == 256)

# C2-3: index size = n×4 bytes after header
hdr=22; idx_size=n*4
check("C2-3 index present", len(c2) >= hdr + idx_size)

# C2-4: offset[0] == 0
off0 = struct.unpack_from(">I", c2, hdr)[0]
check("C2-4 offset[0]==0", off0 == 0)

# C2-5: offsets strictly increasing
offsets = list(struct.unpack_from(f">{n}I", c2, hdr))
check("C2-5 offsets increasing", all(offsets[i]<offsets[i+1] for i in range(n-1)))

# C2-6: full decode round-trip
check("C2-6 decode 256B", c2_decode(c2_encode(data256)) == data256)
check("C2-6 decode 1kB",  c2_decode(c2_encode(data1k))  == data1k)

# C2-7: large stream round-trip
check("C2-7 decode 4kB",  c2_decode(c2_encode(data_big)) == data_big)

# C2-8: decode_range — middle chunk exact
c2b = c2_encode(data_big)
# read bytes 128–192 (chunk 2, full)
got = c2_decode_range(c2b, 128, 64)
check("C2-8 range chunk-aligned", got == data_big[128:192])

# C2-9: decode_range — cross-chunk boundary
got = c2_decode_range(c2b, 96, 64)   # spans chunk 1[32:] + chunk 2[:32]
check("C2-9 range cross-chunk", got == data_big[96:160])

# C2-10: decode_range — single byte
got = c2_decode_range(c2b, 200, 1)
check("C2-10 range single byte", got == data_big[200:201])

# C2-11: random access doesn't decode whole stream
# decode chunk 60 (near end) — should still work via ref chain
dec = C2Decoder(c2b)
chunk60 = dec._decode_chunk(60)
check("C2-11 random chunk 60", chunk60 == data_big[60*BS:61*BS])

# C2-12: cache hit — second access same chunk
chunk60b = dec._decode_chunk(60)
check("C2-12 cache hit", chunk60 is chunk60b)  # same object

# C2-13: zero data compresses well
zeros = bytes(BS * 16)
c2z = c2_encode(zeros)
ratio = len(c2z) / len(zeros)
check("C2-13 zero compress", ratio < 0.15, f"ratio={ratio:.3f}")
check("C2-13 zero rt", c2_decode(c2z) == zeros)

# C2-14: int8 weight-like data compresses
_rand = random.Random(7)
base = [int(_rand.gauss(128,30))%256 for _ in range(BS)]
wchunks = []
for _ in range(32):
    blk = bytearray(base)
    for k in _rand.sample(range(BS), 8):
        blk[k] = (blk[k] + _rand.randint(-3,3)) % 256
    wchunks.append(bytes(blk)); base=list(blk)
wdata = b"".join(wchunks)
c2w = c2_encode(wdata)
ratio_w = len(c2w)/len(wdata)
check("C2-14 weight compress < 80%", ratio_w < 0.80, f"ratio={ratio_w:.3f}")
check("C2-14 weight rt", c2_decode(c2w) == wdata)

print(f"\n{'─'*36}")
print(f"  {PASS}/{PASS+FAIL} PASS", "✓" if FAIL == 0 else "✗")
if FAIL: sys.exit(1)
