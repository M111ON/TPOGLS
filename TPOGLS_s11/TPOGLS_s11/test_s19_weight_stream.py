"""test_s19_weight_stream.py — LLM weight streaming tests"""
import sys, io, random, struct
sys.path.insert(0, '/home/claude')

import numpy as np
from pogls_wallet_py import (
    WalletBuilder, WALLET_MODE_BUFFER, DIAMOND_BLOCK_SIZE,
    chunk_seed, chunk_checksum, file_identity_hash,
)
from pogls_mount import VirtualFile
from pogls_weight_stream import WeightStream, load_weights_numpy, WeightStreamLoader

PASS = FAIL = 0
def check(name, cond, detail=""):
    global PASS, FAIL
    if cond: PASS += 1; print(f"  PASS  {name}")
    else:    FAIL += 1; print(f"  FAIL  {name}" + (f"  [{detail}]" if detail else ""))

# ── fixture ───────────────────────────────────────────────────────────────────

BS = DIAMOND_BLOCK_SIZE

def make_vf(srcs):
    wb = WalletBuilder(mode=WALLET_MODE_BUFFER)
    for raw in srcs:
        h = raw[:64].ljust(64,b"\x00"); t = raw[-64:].ljust(64,b"\x00")
        fid = wb.add_file(b"f", len(raw), 0, file_identity_hash(h,t))
        off = 0
        while off < len(raw):
            c = raw[off:off+BS].ljust(BS,b"\x00")
            wb.add_coord(fid, off, chunk_seed(c), chunk_checksum(c))
            off += BS
    return VirtualFile(wb.seal(), {i:s for i,s in enumerate(srcs)})

# weight-like data: int8 quantized slow drift
def make_weights(n_chunks=16, seed=7):
    rng = random.Random(seed)
    base = [int(rng.gauss(128,30))%256 for _ in range(BS)]
    out = bytearray()
    for _ in range(n_chunks):
        blk = bytearray(base)
        for k in rng.sample(range(BS), 8):
            blk[k] = (blk[k]+rng.randint(-3,3))%256
        out += blk; base=list(blk)
    return bytes(out)

# float32 weights
def make_f32(n_floats=256, seed=3):
    rng = random.Random(seed)
    return struct.pack(f"{n_floats}f",
                       *[rng.gauss(0,0.1) for _ in range(n_floats)])

w8    = make_weights(16)          # 1024B int8
f32   = make_f32(256)             # 1024B float32
w8_2  = make_weights(8, seed=11)  # second "layer"

vf1 = make_vf([w8])
vf2 = make_vf([w8, w8_2])
vf3 = make_vf([f32])

print("\n── S19 WeightStream tests ──")

# WS-1: basic read round-trip
ws = WeightStream(vf1, file_idx=0)
check("WS-1 read rt", ws.read() == w8)

# WS-2: size
ws = WeightStream(vf1, file_idx=0)
check("WS-2 size", ws.size == len(w8))

# WS-3: seek + read
ws = WeightStream(vf1, file_idx=0)
ws.seek(128); got = ws.read(64)
check("WS-3 seek+read", got == w8[128:192])

# WS-4: SEEK_END
ws = WeightStream(vf1, file_idx=0)
ws.seek(-64, io.SEEK_END)
check("WS-4 SEEK_END", ws.read() == w8[-64:])

# WS-5: readinto
ws = WeightStream(vf1, file_idx=0)
buf = bytearray(128)
n = ws.readinto(buf)
check("WS-5 readinto", n == 128 and bytes(buf) == w8[:128])

# WS-6: BufferedReader wrapping
ws  = WeightStream(vf1, file_idx=0)
br  = io.BufferedReader(ws, buffer_size=256)
check("WS-6 buffered read", br.read(64) == w8[:64])
br.close()

# WS-7: numpy load full
arr = load_weights_numpy(vf1, file_idx=0, dtype="uint8")
check("WS-7 numpy uint8 shape", arr.shape == (len(w8),))
check("WS-7 numpy uint8 data",  bytes(arr.tobytes()) == w8)

# WS-8: numpy float32
arr_f = load_weights_numpy(vf3, file_idx=0, dtype="float32")
check("WS-8 numpy float32 shape", arr_f.shape == (256,))
check("WS-8 numpy float32 data",  arr_f.tobytes() == f32)

# WS-9: numpy partial load (offset + count)
arr_p = load_weights_numpy(vf1, file_idx=0, dtype="uint8", offset=64, count=64)
check("WS-9 numpy partial", bytes(arr_p.tobytes()) == w8[64:128])

# WS-10: WeightStreamLoader auto-map
loader = WeightStreamLoader(vf2)
check("WS-10 loader keys", set(loader.keys()) == {"layer_0","layer_1"})
check("WS-10 layer_0 data", bytes(loader["layer_0"].tobytes()) == w8)
check("WS-10 layer_1 data", bytes(loader["layer_1"].tobytes()) == w8_2)

# WS-11: cache hit
l0a = loader["layer_0"]
l0b = loader["layer_0"]
check("WS-11 cache hit", l0a is l0b)

# WS-12: load_all
all_layers = loader.load_all()
check("WS-12 load_all keys", set(all_layers.keys()) == {"layer_0","layer_1"})

# WS-13: custom layer_map
layer_map = {
    "embed": {"file_idx": 0, "offset": 0,   "size": 512, "dtype": "uint8"},
    "head":  {"file_idx": 0, "offset": 512,  "size": 512, "dtype": "uint8"},
}
ldr2 = WeightStreamLoader(vf1, layer_map=layer_map)
check("WS-13 custom map embed", bytes(ldr2["embed"].tobytes()) == w8[:512])
check("WS-13 custom map head",  bytes(ldr2["head"].tobytes())  == w8[512:1024])

# WS-14: context manager
with WeightStream(vf1, file_idx=0) as ws:
    d = ws.read(64)
check("WS-14 ctx read", d == w8[:64])
check("WS-14 ctx closed", ws.closed)

# WS-15: streaming via BufferedReader + np.frombuffer (torch.load pattern)
ws  = WeightStream(vf3, file_idx=0)
br  = io.BufferedReader(ws, buffer_size=512)
raw = br.read()
arr = np.frombuffer(raw, dtype="float32")
check("WS-15 frombuffer pattern", arr.tobytes() == f32)
br.close()

print(f"\n{'─'*36}")
print(f"  {PASS}/{PASS+FAIL} PASS", "✓" if FAIL == 0 else "✗")
if FAIL: sys.exit(1)
