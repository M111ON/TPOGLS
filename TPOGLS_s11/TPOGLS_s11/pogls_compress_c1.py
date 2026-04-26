"""
pogls_compress_c1.py — S16 C1: PHI-pattern-aware delta encoder
═══════════════════════════════════════════════════════════════
PHI constants (frozen):
  PHI_UP   = 1696631   PHI_DOWN = 648055   PHI_SCALE = 2^20
  Sacred set: {17, 18, 54, 144, 162, 289, 720}

C1 encodes a chunk stream as:
  ┌──────────────────────────────────────────────────────┐
  │  HEADER  [magic 4B][version 1B][flags 1B][n_chunks 4B]
  │  FRAMES  [frame …] × n_chunks                        
  │  FRAME:  [type 1B][payload ...]                       
  │    type=0x00  RAW    64B verbatim                     
  │    type=0x01  DELTA  [ref_idx 3B][xor_mask 8B]        │ 11B vs 64B
  │    type=0x02  PHI    [phi_code 2B][xor_mask 8B]        │ 10B vs 64B
  │    type=0x03  ZERO   (empty — all-zero chunk)          │  1B vs 64B
  └──────────────────────────────────────────────────────┘

PHI_CODE encodes (spoke, slot) in sacred-aligned grid:
  upper 6 bits = spoke index (0–63, sacred multiples of 6)
  lower 10 bits = slot index (0–1023)

Selection priority: ZERO > PHI > DELTA > RAW
"""

from __future__ import annotations
import struct
import hashlib
from typing import Iterator

# ── PHI constants ─────────────────────────────────────────────────────────────
PHI_UP    = 1_696_631
PHI_DOWN  =   648_055
PHI_SCALE = 1 << 20        # 2^20
SACRED    = frozenset([17, 18, 54, 144, 162, 289, 720])
BS        = 64             # DIAMOND_BLOCK_SIZE

MAGIC   = b"PGC1"
VERSION = 0x01
FLAG_NONE = 0x00

# ── PHI lattice helpers ────────────────────────────────────────────────────────

def _phi_position(idx: int) -> tuple[int, int]:
    """Map chunk index → (spoke, slot) via PHI_UP step in sacred grid."""
    pos   = (idx * PHI_UP) % PHI_SCALE
    spoke = (pos * 6) // PHI_SCALE          # 0–5  (6 spokes)
    slot  = (pos * 144) // PHI_SCALE        # 0–143 (144 slots)
    return spoke, slot

def _phi_code(spoke: int, slot: int) -> int:
    return (spoke << 10) | (slot & 0x3FF)

def _phi_decode(code: int) -> tuple[int, int]:
    return (code >> 10) & 0x3F, code & 0x3FF

def _sparse_diff(a: bytes, b: bytes) -> bytes | None:
    """
    Encode diff of two 64B chunks as sparse list.
    Format: [n 1B][offset 1B, val 1B] × n  → max 1+2*64=129B
    Returns None if n_changed > SPARSE_MAX (use RAW instead).
    """
    changed = [(i, a[i] ^ b[i]) for i in range(BS) if a[i] != b[i]]
    if len(changed) > SPARSE_MAX:
        return None
    out = bytearray([len(changed)])
    for off, v in changed:
        out += bytes([off, v])
    return bytes(out)

def _apply_sparse(ref: bytes, sparse: bytes) -> bytes:
    n = sparse[0]
    out = bytearray(ref)
    for i in range(n):
        off = sparse[1 + i*2]
        val = sparse[2 + i*2]
        out[off] ^= val
    return bytes(out)

SPARSE_MAX = 28   # threshold: >28 changed bytes → RAW cheaper

def _is_zero(chunk: bytes) -> bool:
    return not any(chunk)

# ── encoder ───────────────────────────────────────────────────────────────────

class C1Encoder:
    """
    Encode a sequence of 64B chunks into C1 compressed stream.

    window_size: max lookback for DELTA match (default 256 chunks)
    phi_buckets: PHI lattice bucket count for fast near-match lookup
    """

    def __init__(self, window_size: int = 256, phi_buckets: int = 144):
        self._window_size  = window_size
        self._phi_buckets  = phi_buckets
        self._history: list[bytes] = []          # recent chunks
        self._phi_map: dict[int, list[int]] = {} # bucket → [chunk_idx list]

    def encode_chunks(self, chunks: list[bytes]) -> bytes:
        """Encode list of 64B chunks → C1 bytes."""
        frames = []
        for i, chunk in enumerate(chunks):
            assert len(chunk) == BS, f"chunk {i} must be {BS}B"
            frames.append(self._encode_one(i, chunk))

        header = struct.pack(">4sBBH", MAGIC, VERSION, FLAG_NONE, len(chunks))
        # pad header to 8B
        header += struct.pack(">I", len(chunks))  # n_chunks as u32 (full)
        return header + b"".join(frames)

    def _encode_one(self, idx: int, chunk: bytes) -> bytes:
        # ZERO
        if _is_zero(chunk):
            self._record(idx, chunk)
            return b"\x03"

        # PHI match
        spoke, slot = _phi_position(idx)
        bucket = spoke * self._phi_buckets // 6
        phi_match = self._phi_lookup(chunk, bucket)
        if phi_match is not None:
            ref_chunk = self._history[phi_match]
            sparse = _sparse_diff(chunk, ref_chunk)
            if sparse is not None:
                rel = (len(self._history) - phi_match) & 0xFFFFFF
                self._record(idx, chunk)
                return b"\x02" + struct.pack(">I", rel)[1:] + sparse  # 1+3+sparse

        # DELTA match
        delta_match = self._delta_lookup(chunk)
        if delta_match is not None:
            ref_chunk = self._history[delta_match]
            sparse = _sparse_diff(chunk, ref_chunk)
            if sparse is not None:
                rel = (len(self._history) - delta_match) & 0xFFFFFF
                self._record(idx, chunk)
                return b"\x01" + struct.pack(">I", rel)[1:] + sparse  # 1+3+sparse

        # RAW fallback
        self._record(idx, chunk)
        return b"\x00" + chunk

    def _record(self, idx: int, chunk: bytes):
        self._history.append(chunk)
        # update PHI bucket index
        spoke, slot = _phi_position(idx)
        bucket = spoke * self._phi_buckets // 6
        if bucket not in self._phi_map:
            self._phi_map[bucket] = []
        self._phi_map[bucket].append(len(self._history) - 1)
        # trim window
        if len(self._history) > self._window_size:
            self._history = self._history[-self._window_size:]
            # rebuild phi_map with adjusted indices
            self._phi_map = {}
            for j, c in enumerate(self._history):
                sp, sl = _phi_position(j)
                bk = sp * self._phi_buckets // 6
                self._phi_map.setdefault(bk, []).append(j)

    def _phi_lookup(self, chunk: bytes, bucket: int) -> int | None:
        """Return history index of best PHI-bucket match, or None."""
        candidates = self._phi_map.get(bucket, [])
        if not candidates:
            return None
        # pick most recent candidate with similarity > threshold
        for hi in reversed(candidates[-8:]):  # check last 8 in bucket
            ref = self._history[hi]
            # similarity: count matching bytes
            sim = sum(a == b for a, b in zip(chunk, ref))
            if sim >= 32:   # ≥50% byte match
                return hi
        return None

    def _delta_lookup(self, chunk: bytes) -> int | None:
        """Sliding window: find recent chunk with ≥40 matching bytes."""
        start = max(0, len(self._history) - self._window_size)
        for hi in range(len(self._history) - 1, start - 1, -1):
            ref = self._history[hi]
            sim = sum(a == b for a, b in zip(chunk, ref))
            if sim >= 40:
                return hi
        return None


# ── decoder ───────────────────────────────────────────────────────────────────

class C1Decoder:
    """Decode C1 stream → list of 64B chunks."""

    def decode(self, data: bytes) -> list[bytes]:
        # parse header (12B: magic4 + ver1 + flag1 + pad2 + n4)
        magic, ver, flags, _pad, n = struct.unpack_from(">4sBBHI", data, 0)
        assert magic == MAGIC, f"bad magic {magic}"
        pos = 12

        history:  list[bytes] = []
        h_bucket: list[int]   = []   # parallel: phi bucket per history entry
        chunks:   list[bytes] = []
        PHI_BUCKETS = 144

        for i in range(n):
            t = data[pos]; pos += 1

            if t == 0x03:   # ZERO
                chunk = bytes(BS)

            elif t == 0x00: # RAW
                chunk = data[pos:pos+BS]; pos += BS

            elif t == 0x01: # DELTA sparse
                rel_bytes = b"\x00" + data[pos:pos+3]; pos += 3
                rel = struct.unpack(">I", rel_bytes)[0]
                n_ch = data[pos]; sparse = data[pos:pos+1+n_ch*2]; pos += 1+n_ch*2
                chunk = _apply_sparse(history[len(history)-rel], sparse)

            elif t == 0x02: # PHI sparse (same wire format)
                rel_bytes = b"\x00" + data[pos:pos+3]; pos += 3
                rel = struct.unpack(">I", rel_bytes)[0]
                n_ch = data[pos]; sparse = data[pos:pos+1+n_ch*2]; pos += 1+n_ch*2
                chunk = _apply_sparse(history[len(history)-rel], sparse)

            else:
                raise ValueError(f"unknown frame type 0x{t:02x} at pos {pos-1}")

            # record with actual PHI bucket for this position
            sp, sl = _phi_position(i)
            h_bucket.append(sp * PHI_BUCKETS // 6)
            history.append(chunk)
            chunks.append(chunk)

        return chunks

    def _apply_mask(self, ref: bytes, mask8: bytes) -> bytes:
        """Expand 8B mask back to 64B and XOR with ref."""
        full_mask = bytes(mask8[i % 8] for i in range(BS))
        return bytes(a ^ b for a, b in zip(ref, full_mask))

    def _phi_find(self, history: list[bytes], buckets: list[int],
                  target_bucket: int) -> bytes | None:
        """Find most recent history chunk with matching PHI bucket."""
        for j in range(len(history) - 1, max(-1, len(history) - 64), -1):
            if buckets[j] == target_bucket:
                return history[j]
        return None


# ── stream encode/decode helpers ──────────────────────────────────────────────

def encode_stream(data: bytes) -> bytes:
    """Encode arbitrary bytes (padded to BS) → C1."""
    chunks = []
    for off in range(0, len(data), BS):
        c = data[off:off+BS]
        if len(c) < BS:
            c = c.ljust(BS, b"\x00")
        chunks.append(c)
    return C1Encoder().encode_chunks(chunks)

def decode_stream(c1_data: bytes, original_size: int) -> bytes:
    """Decode C1 → bytes, trim to original_size."""
    chunks = C1Decoder().decode(c1_data)
    return b"".join(chunks)[:original_size]

