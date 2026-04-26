"""test_s18_c3.py — C3 stream reconstruct pipeline tests"""
import sys, io, random
sys.path.insert(0, '/home/claude')

from pogls_compress_c3 import C3Stream, c3_pipe, c3_stream, C3WalletBridge, BS
from pogls_compress_c2 import c2_encode

PASS = FAIL = 0
def check(name, cond, detail=""):
    global PASS, FAIL
    if cond: PASS += 1; print(f"  PASS  {name}")
    else:    FAIL += 1; print(f"  FAIL  {name}" + (f"  [{detail}]" if detail else ""))

print("\n── S18 C3 stream pipeline tests ──")

random.seed(42)
data256  = bytes(range(256))
data1k   = bytes(random.getrandbits(8) for _ in range(1024))
data_big = bytes((i*13+7)%256 for i in range(BS*64))

c2_256  = c2_encode(data256)
c2_1k   = c2_encode(data1k)
c2_big  = c2_encode(data_big)

# C3-1: basic read() full
s = C3Stream(c2_256)
check("C3-1 read() full", s.read() == data256)

# C3-2: size property
s = C3Stream(c2_256)
check("C3-2 size", s.size == 256)

# C3-3: read(n) partial
s = C3Stream(c2_256)
check("C3-3 read(64)", s.read(64) == data256[:64])

# C3-4: seek + read
s = C3Stream(c2_256)
s.seek(128)
check("C3-4 seek+read", s.read(64) == data256[128:192])

# C3-5: seek SEEK_END
s = C3Stream(c2_256)
s.seek(-64, io.SEEK_END)
check("C3-5 SEEK_END", s.read() == data256[-64:])

# C3-6: seek SEEK_CUR
s = C3Stream(c2_256)
s.read(64); s.seek(32, io.SEEK_CUR)
check("C3-6 SEEK_CUR tell", s.tell() == 96)
check("C3-6 SEEK_CUR read", s.read(32) == data256[96:128])

# C3-7: read past EOF
s = C3Stream(c2_256)
s.seek(240)
check("C3-7 read past EOF", s.read(100) == data256[240:])

# C3-8: read at EOF
s = C3Stream(c2_256)
s.seek(0, io.SEEK_END)
check("C3-8 read at EOF", s.read() == b"")

# C3-9: readinto()
s   = C3Stream(c2_256)
buf = bytearray(64)
n   = s.readinto(buf)
check("C3-9 readinto n", n == 64)
check("C3-9 readinto data", bytes(buf) == data256[:64])

# C3-10: context manager
with C3Stream(c2_256) as s:
    d = s.read(64)
check("C3-10 ctx read", d == data256[:64])
check("C3-10 ctx closed", s.closed)

# C3-11: stream() generator
s = C3Stream(c2_1k)
pages = list(s.stream(chunk_size=128))
check("C3-11 stream pages", b"".join(pages) == data1k)

# C3-12: stream() from mid position
s = C3Stream(c2_1k)
s.seek(512)
pages = list(s.stream(chunk_size=128))
check("C3-12 stream from mid", b"".join(pages) == data1k[512:])

# C3-13: BufferedReader wrapping
s     = C3Stream(c2_big)
buf_r = io.BufferedReader(s, buffer_size=512)
check("C3-13 buffered read", buf_r.read(64) == data_big[:64])
buf_r.seek(1024)
check("C3-13 buffered seek+read", buf_r.read(64) == data_big[1024:1088])
buf_r.close()

# C3-14: c3_pipe full delivery
received = bytearray()
total = c3_pipe(c2_big, received.extend, page_size=BS*8)
check("C3-14 pipe total bytes", total == len(data_big))
check("C3-14 pipe correct data", bytes(received) == data_big)

# C3-15: c3_stream convenience
s = c3_stream(data256)
check("C3-15 c3_stream", s.read() == data256)

# C3-16: C3WalletBridge round-trip
from pogls_wallet_py import (
    WalletBuilder, WALLET_MODE_BUFFER, DIAMOND_BLOCK_SIZE,
    chunk_seed, chunk_checksum, file_identity_hash,
)
from pogls_mount import VirtualFile

def make_vf(srcs):
    wb = WalletBuilder(mode=WALLET_MODE_BUFFER)
    for raw in srcs:
        h = raw[:64].ljust(64,b"\x00"); t = raw[-64:].ljust(64,b"\x00")
        fid = wb.add_file(b"f", len(raw), 0, file_identity_hash(h,t))
        off = 0
        while off < len(raw):
            c = raw[off:off+DIAMOND_BLOCK_SIZE].ljust(DIAMOND_BLOCK_SIZE,b"\x00")
            wb.add_coord(fid, off, chunk_seed(c), chunk_checksum(c))
            off += DIAMOND_BLOCK_SIZE
    return VirtualFile(wb.seal(), {i:s for i,s in enumerate(srcs)})

src_a = bytes(range(256))
vf    = make_vf([src_a])
bridge = C3WalletBridge(vf, file_idx=0)
check("C3-16 bridge size", bridge.size == 256)
check("C3-16 bridge read", bridge.read() == src_a)

# C3-17: bridge seek + partial read
bridge2 = C3WalletBridge(vf, file_idx=0)
bridge2.seek(128)
check("C3-17 bridge seek+read", bridge2.read(64) == src_a[128:192])

# C3-18: large stream correctness
s = C3Stream(c2_big)
check("C3-18 large full rt", s.read() == data_big)

print(f"\n{'─'*36}")
print(f"  {PASS}/{PASS+FAIL} PASS", "✓" if FAIL == 0 else "✗")
if FAIL: sys.exit(1)
