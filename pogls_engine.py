"""
pogls_engine.py — ctypes binding for libpogls_v4.so
Encoding scheme: file bytes → 8-byte chunks → write(key=offset, val=uint64)
Decoding: read(key=offset) for each chunk → reconstruct bytes

Block-mode: .so has hard cap of 8192 slots (cpu.count).
Files larger than BLOCK_SLOTS*8 bytes are split into blocks;
each block gets its own engine open/close cycle.
"""
import ctypes
import struct
import os

LIB_PATH   = os.environ.get("POGLS_LIB", "/mnt/c/TPOGLS/libpogls_v4.so")
BLOCK_SLOTS = 5720
BLOCK_BYTES = BLOCK_SLOTS * 8   # 64 000 bytes per block

# sentinel stored at key=0xFFFFFFFFFFFFFFFF to record original file size
_SIZE_KEY = ctypes.c_uint64(0xFFFFFFFFFFFFFFFF)



# ── PoglsStatsOut — mirrors pogls_stats_out.h (19 × uint64) ─────────
_STATS_VERSION = 0x504F5302  # "POS\x02" — S19

_STATS_FIELDS = [
    "version",
    "ctx_writes", "ctx_reads", "ctx_qrpns",
    "l1_hits", "l1_misses", "l1_hit_pct_x100",
    "kv_capacity", "kv_live", "kv_tomb",
    "kv_load_pct_x100", "kv_tomb_pct_x100",
    "kv_puts", "kv_gets", "kv_misses", "kv_max_probe",
    "gpu_ring_pending", "gpu_ring_flushed", "gpu_ring_dropped",
]

class PoglsStatsOut(ctypes.Structure):
    _fields_ = [(f, ctypes.c_uint64) for f in _STATS_FIELDS]

    def to_dict(self) -> dict:
        d = {f: int(getattr(self, f)) for f in _STATS_FIELDS}
        d["kv_load_pct"] = round(d.pop("kv_load_pct_x100") / 100, 2)
        d["kv_tomb_pct"] = round(d.pop("kv_tomb_pct_x100") / 100, 2)
        d["l1_hit_pct"]  = round(d.pop("l1_hit_pct_x100")  / 100, 2)
        d["version_ok"]  = (d["version"] == _STATS_VERSION)
        return d


def _load():
    lib = ctypes.CDLL(LIB_PATH)
    lib.pogls_so_open.restype   = ctypes.c_void_p
    lib.pogls_so_open.argtypes  = []
    lib.pogls_so_close.restype  = None
    lib.pogls_so_close.argtypes = [ctypes.c_void_p]
    lib.pogls_so_write.restype  = None
    lib.pogls_so_write.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
    lib.pogls_so_read.restype   = ctypes.c_uint64
    lib.pogls_so_read.argtypes  = [ctypes.c_void_p, ctypes.c_uint64]
    lib.pogls_so_has.restype    = ctypes.c_int
    lib.pogls_so_has.argtypes   = [ctypes.c_void_p, ctypes.c_uint64]
    lib.pogls_so_stats.restype  = None
    lib.pogls_so_stats.argtypes = [ctypes.c_void_p]
    lib.pogls_so_stats_out.restype  = None
    lib.pogls_so_stats_out.argtypes = [ctypes.c_void_p, ctypes.POINTER(PoglsStatsOut)]
    lib.pogls_so_rewind.restype = None
    lib.pogls_so_rewind.argtypes= [ctypes.c_void_p]
    return lib


class PoglsEngine:
    """Context manager — open/close .so handle automatically."""

    def __init__(self):
        self._lib = _load()
        self._ctx = None

    def __enter__(self):
        self._ctx = self._lib.pogls_so_open()
        if not self._ctx:
            raise RuntimeError("pogls_so_open returned NULL")
        return self

    def __exit__(self, *_):
        if self._ctx:
            self._lib.pogls_so_close(ctypes.c_void_p(self._ctx))
            self._ctx = None

    # ── low-level single-block helpers ───────────────────────────────

    def _write_block(self, block: bytes) -> int:
        """Write one block (must fit in BLOCK_SLOTS). Returns chunk count."""
        pad = (8 - len(block) % 8) % 8
        buf = block + b'\x00' * pad
        n   = len(buf) // 8
        for i in range(n):
            val = struct.unpack('<Q', buf[i*8:(i+1)*8])[0]
            self._lib.pogls_so_write(
                ctypes.c_void_p(self._ctx),
                ctypes.c_uint64(i),
                ctypes.c_uint64(val),
            )
        return n

    def _read_block(self, n_chunks: int, orig_len: int) -> bytes:
        """Read n_chunks back, trim to orig_len."""
        out = bytearray()
        for i in range(n_chunks):
            val = int(self._lib.pogls_so_read(
                ctypes.c_void_p(self._ctx), ctypes.c_uint64(i)
            ))
            out += struct.pack('<Q', val)
        return bytes(out[:orig_len])

    # ── public encode / decode (multi-block aware) ───────────────────

    def encode(self, data: bytes) -> int:
        """
        Write bytes into engine via block cycling.
        Returns total chunk count.
        Stores decoded bytes internally in self._blocks for decode().
        """
        self._orig_len = len(data)
        self._blocks   = []          # list of decoded bytes per block
        total_chunks   = 0

        offset = 0
        while offset < len(data):
            blk = data[offset: offset + BLOCK_BYTES]
            n   = self._write_block(blk)
            # immediately read back to verify + store
            back = self._read_block(n, len(blk))
            self._blocks.append(back)
            total_chunks += n
            offset += BLOCK_BYTES

            # reopen engine for next block (reset slot table)
            if offset < len(data):
                self._lib.pogls_so_close(ctypes.c_void_p(self._ctx))
                self._ctx = self._lib.pogls_so_open()
                if not self._ctx:
                    raise RuntimeError("pogls_so_open returned NULL on reopen")

        # store size sentinel in final open context
        self._lib.pogls_so_write(
            ctypes.c_void_p(self._ctx),
            _SIZE_KEY,
            ctypes.c_uint64(self._orig_len),
        )
        return total_chunks

    def decode(self) -> bytes:
        """Return reassembled bytes from encode() block cache."""
        if not hasattr(self, '_blocks'):
            raise RuntimeError("Call encode() before decode()")
        return b''.join(self._blocks)

    def has(self, key: int) -> bool:
        return bool(self._lib.pogls_so_has(
            ctypes.c_void_p(self._ctx), ctypes.c_uint64(key)
        ))

    def stats(self):
        self._lib.pogls_so_stats(ctypes.c_void_p(self._ctx))

    def stats_out(self) -> dict:
        """Return real kv.live/tomb/l1 counts as dict via pogls_so_stats_out()."""
        out = PoglsStatsOut()
        self._lib.pogls_so_stats_out(
            ctypes.c_void_p(self._ctx),
            ctypes.byref(out),
        )
        return out.to_dict()


if __name__ == "__main__":
    import hashlib, sys

    test_data = b"Hello POGLS engine! " * 500 + bytes(range(256))
    print(f"Input : {len(test_data)} bytes  sha={hashlib.sha256(test_data).hexdigest()[:16]}...")

    with PoglsEngine() as eng:
        chunks = eng.encode(test_data)
        print(f"Wrote : {chunks} chunks into engine")
        recovered = eng.decode()
        eng.stats()

    sha_in  = hashlib.sha256(test_data).hexdigest()
    sha_out = hashlib.sha256(recovered).hexdigest()
    print(f"In    : {sha_in[:32]}...")
    print(f"Out   : {sha_out[:32]}...")

    if test_data == recovered:
        print("✓  ROUND-TRIP PASS — engine encode/decode bit-perfect")
    else:
        print("✗  MISMATCH")
        sys.exit(1)
