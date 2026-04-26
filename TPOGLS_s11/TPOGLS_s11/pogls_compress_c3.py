"""
pogls_compress_c3.py — S18 C3: Stream Reconstruct Pipeline
═══════════════════════════════════════════════════════════
C3 wraps C2 as a streaming file-like object:
  - reads C2 data in pages (no full decompression upfront)
  - integrates with WalletFile / io.RawIOBase interface
  - supports seek + partial reconstruct

Architecture:
  ┌─────────────────────────────────────────────────────┐
  │  C3Stream(c2_data)  →  io.RawIOBase                 │
  │    .read(n)   — decode only needed chunks            │
  │    .seek(off) — jump to any byte position            │
  │    .tell()    — current position                     │
  │    .readinto(buf) — zero-copy into buffer            │
  │                                                      │
  │  c3_pipe(c2_data, sink_fn, chunk_size)               │
  │    — streaming pipeline, calls sink per page         │
  │                                                      │
  │  C3WalletBridge(vf, file_idx)                        │
  │    — compress-on-the-fly from VirtualFile            │
  └─────────────────────────────────────────────────────┘
"""

from __future__ import annotations
import io
from typing import Callable, Iterator, Optional

from pogls_compress_c2 import C2Decoder, c2_encode, BS


# ── C3Stream ──────────────────────────────────────────────────────────────────

class C3Stream(io.RawIOBase):
    """
    Streaming decode of C2 data as a seekable file-like object.
    Decodes chunks lazily — only what read() requires.
    """

    def __init__(self, c2_data: bytes):
        super().__init__()
        self._dec  = C2Decoder(c2_data)
        self._size = self._dec.orig_size
        self._pos  = 0

    # ── io.RawIOBase ──────────────────────────────────────────────────────

    def readable(self) -> bool: return True
    def writable(self) -> bool: return False
    def seekable(self) -> bool: return True

    @property
    def size(self) -> int:
        return self._size

    def readinto(self, b: bytearray) -> int:
        if self._pos >= self._size:
            return 0
        n     = min(len(b), self._size - self._pos)
        chunk = self._dec.decode_range(self._pos, n)
        b[:len(chunk)] = chunk
        self._pos += len(chunk)
        return len(chunk)

    def read(self, size: int = -1) -> bytes:
        if self.closed:
            raise ValueError("read on closed C3Stream")
        if self._pos >= self._size:
            return b""
        if size < 0:
            size = self._size - self._pos
        n    = min(size, self._size - self._pos)
        data = self._dec.decode_range(self._pos, n)
        self._pos += len(data)
        return data

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        if whence == io.SEEK_SET:   new = offset
        elif whence == io.SEEK_CUR: new = self._pos + offset
        elif whence == io.SEEK_END: new = self._size + offset
        else: raise ValueError(f"invalid whence={whence}")
        self._pos = max(0, min(new, self._size))
        return self._pos

    def tell(self) -> int:
        return self._pos

    # ── stream helper ─────────────────────────────────────────────────────

    def stream(self, chunk_size: int = BS * 16) -> Iterator[bytes]:
        """Yield decompressed pages from current position to EOF."""
        while self._pos < self._size:
            data = self.read(chunk_size)
            if not data:
                break
            yield data


# ── pipeline helper ───────────────────────────────────────────────────────────

def c3_pipe(
    c2_data: bytes,
    sink: Callable[[bytes], None],
    page_size: int = BS * 64,
) -> int:
    """
    Stream-decompress c2_data, calling sink(page) for each page.
    Returns total bytes passed to sink.
    """
    stream = C3Stream(c2_data)
    total  = 0
    for page in stream.stream(chunk_size=page_size):
        sink(page)
        total += len(page)
    return total


# ── WalletFile bridge ─────────────────────────────────────────────────────────

class C3WalletBridge(io.RawIOBase):
    """
    Compress a VirtualFile on-the-fly into C2 and expose as C3Stream.
    Useful for: streaming LLM weights from wallet → model loader.

    passthrough: skip C1/C2 encode entirely — raw bytes wrapped in
    a seekable BytesIO.  Use when data is known incompressible
    (float32 weights, encrypted blobs).  ~14× faster for such data.

    Usage:
        bridge = C3WalletBridge(vf, file_idx=0)              # auto
        bridge = C3WalletBridge(vf, file_idx=0, passthrough=True)   # force raw
        model  = torch.load(io.BufferedReader(bridge))
    """

    # compression ratio threshold: if C2 output ≥ this × input, switch to passthrough
    _PASSTHROUGH_RATIO = 0.98

    def __init__(self, vf, file_idx: Optional[int] = None,
                 window_size: int = 256, passthrough: Optional[bool] = None):
        super().__init__()
        # read raw bytes from VirtualFile
        if file_idx is not None:
            raw = vf.read_file(file_idx, 0, vf.file_size(file_idx))
        else:
            raw = vf.read(0, vf.size)

        self._orig_size = len(raw)

        # auto-detect passthrough: probe first 16 chunks
        if passthrough is None:
            passthrough = self._probe_incompressible(raw, window_size)

        if passthrough:
            self._stream = io.BytesIO(raw)
        else:
            from pogls_compress_c2 import C2Encoder
            c2 = C2Encoder(window_size=window_size).encode(raw)
            # fallback to passthrough if compression expanded the data
            if len(c2) >= len(raw) * self._PASSTHROUGH_RATIO:
                self._stream = io.BytesIO(raw)
            else:
                self._stream = C3Stream(c2)

    @staticmethod
    def _probe_incompressible(raw: bytes, window_size: int,
                               probe_chunks: int = 32) -> bool:
        """
        Encode first probe_chunks via C1 using _encode_one directly.
        frame[0] == 0x00 → RAW.  If >80% RAW → declare incompressible.
        """
        from pogls_compress_c1 import C1Encoder
        BS_  = BS
        n    = min(probe_chunks, len(raw) // BS_)
        if n == 0:
            return False
        enc  = C1Encoder(window_size=window_size)
        raw_ = 0
        for i in range(n):
            chunk  = raw[i * BS_:(i + 1) * BS_]
            frame  = enc._encode_one(i, chunk)
            if frame[0] == 0x00:   # RAW frame
                raw_ += 1
        return (raw_ / n) > 0.80   # >80% RAW → incompressible

    def readable(self) -> bool: return True
    def writable(self) -> bool: return False
    def seekable(self) -> bool: return True

    @property
    def size(self) -> int:
        return self._orig_size

    def readinto(self, b: bytearray) -> int:
        return self._stream.readinto(b)

    def read(self, size: int = -1) -> bytes:
        return self._stream.read(size)

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        return self._stream.seek(offset, whence)

    def tell(self) -> int:
        return self._stream.tell()


# ── convenience ───────────────────────────────────────────────────────────────

def c3_stream(data: bytes) -> C3Stream:
    """Compress data → C2, return as C3Stream."""
    return C3Stream(c2_encode(data))

