"""
pogls_compress_c2.py — S17 C2: Chunk Index
═══════════════════════════════════════════
C2 sits on top of C1 stream and provides:
  - seekable chunk index (offset table)
  - chunk-level random access without full decode
  - multi-stream partitioning (split large wallet into segments)

Wire format:
  ┌─────────────────────────────────────────────────────┐
  │  HEADER  [magic 4B][ver 1B][flags 1B][n_chunks 4B]  │
  │          [orig_size 8B][segment_size 4B]             │
  │  INDEX   [n_chunks × offset 4B]  ← byte offset in   │
  │           C1 stream per chunk frame                  │
  │  C1_DATA [raw C1 bytes]                              │
  └─────────────────────────────────────────────────────┘

Random access:
  seek_chunk(i) → read C1 frame at index[i]
               → decode with ref chain resolved
"""

from __future__ import annotations
import struct
from typing import Iterator

from pogls_compress_c1 import (
    C1Encoder, C1Decoder, encode_stream, decode_stream,
    BS, MAGIC as C1_MAGIC, SPARSE_MAX,
    _sparse_diff, _apply_sparse, _phi_position, _is_zero
)

MAGIC   = b"PGC2"
VERSION = 0x01
FLAG_NONE = 0x00

# ── C2 encoder ────────────────────────────────────────────────────────────────

class C2Encoder:
    """
    Encode chunks → C2 (C1 + seekable index).
    window_size: C1 lookback window
    """

    def __init__(self, window_size: int = 256):
        self._enc = C1Encoder(window_size=window_size)

    def encode(self, data: bytes) -> bytes:
        chunks = _split_chunks(data)
        n = len(chunks)

        # encode each frame individually, track byte offsets
        offsets: list[int] = []
        frames:  list[bytes] = []
        pos = 0
        for i, chunk in enumerate(chunks):
            frame = self._enc._encode_one(i, chunk)  # _record called inside
            offsets.append(pos)
            frames.append(frame)
            pos += len(frame)

        c1_data = b"".join(frames)
        orig_size = len(data)
        seg_size  = n * BS

        # header: magic4 + ver1 + flag1 + n4 + orig8 + seg4 = 22B
        header = struct.pack(">4sBBIQI",
            MAGIC, VERSION, FLAG_NONE,
            n, orig_size, seg_size)

        # index: n × u32 offsets
        index = struct.pack(f">{n}I", *offsets)

        return header + index + c1_data


# ── C2 decoder ────────────────────────────────────────────────────────────────

class C2Decoder:
    """
    Decode C2 stream with random-access chunk support.
    Caches decoded chunks for ref resolution.
    """

    def __init__(self, data: bytes):
        self._data = data
        # parse header
        magic, ver, flags, n, orig_size, seg_size = \
            struct.unpack_from(">4sBBIQI", data, 0)
        assert magic == MAGIC, f"bad magic {magic}"
        self._n         = n
        self._orig_size = orig_size
        self._hdr_size  = 22
        self._idx_size  = n * 4
        self._c1_start  = self._hdr_size + self._idx_size

        # load offset table
        self._offsets = list(struct.unpack_from(f">{n}I", data, self._hdr_size))
        self._cache: dict[int, bytes] = {}  # chunk_idx → decoded bytes

    @property
    def n_chunks(self) -> int:
        return self._n

    @property
    def orig_size(self) -> int:
        return self._orig_size

    def decode_all(self) -> bytes:
        """Decode full stream."""
        chunks = [self._decode_chunk(i) for i in range(self._n)]
        return b"".join(chunks)[:self._orig_size]

    def decode_range(self, byte_offset: int, size: int) -> bytes:
        """Decode only chunks covering [byte_offset, byte_offset+size)."""
        first_chunk = byte_offset // BS
        last_chunk  = min((byte_offset + size - 1) // BS, self._n - 1)
        parts = []
        for i in range(first_chunk, last_chunk + 1):
            parts.append(self._decode_chunk(i))
        raw = b"".join(parts)
        local_off = byte_offset % BS
        return raw[local_off:local_off + size]

    def _decode_chunk(self, idx: int) -> bytes:
        if idx in self._cache:
            return self._cache[idx]

        c1 = self._data
        frame_off = self._c1_start + self._offsets[idx]
        t = c1[frame_off]; pos = frame_off + 1

        if t == 0x03:
            chunk = bytes(BS)

        elif t == 0x00:
            chunk = c1[pos:pos+BS]

        elif t in (0x01, 0x02):
            rel = struct.unpack(">I", b"\x00" + c1[pos:pos+3])[0]; pos += 3
            n_ch = c1[pos]; sparse = c1[pos:pos+1+n_ch*2]
            ref_idx = idx - rel
            ref = self._decode_chunk(ref_idx)   # recursive, cache handles it
            chunk = _apply_sparse(ref, sparse)

        else:
            raise ValueError(f"unknown frame 0x{t:02x}")

        self._cache[idx] = chunk
        return chunk


# ── helpers ───────────────────────────────────────────────────────────────────

def _split_chunks(data: bytes) -> list[bytes]:
    out = []
    for off in range(0, len(data), BS):
        c = data[off:off+BS]
        if len(c) < BS:
            c = c.ljust(BS, b"\x00")
        out.append(c)
    return out


def c2_encode(data: bytes) -> bytes:
    return C2Encoder().encode(data)

def c2_decode(data: bytes) -> bytes:
    return C2Decoder(data).decode_all()

def c2_decode_range(data: bytes, offset: int, size: int) -> bytes:
    return C2Decoder(data).decode_range(offset, size)

