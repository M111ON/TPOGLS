"""
pogls_wallet_py.py — Python wallet builder/reader (S11-C)
══════════════════════════════════════════════════════════
Pure Python implementation of pogls_coord_wallet.h.

Mirrors C layout exactly so .pogwallet files are interchangeable
between Python and C implementations.

Binary layout (.pogwallet):
  [WalletHeader  128B]
  [FileEntry[]    64B each]
  [string pool        ]
  [CoordRecord[]  40B each]
  [WalletFooter   16B]

Usage (build):
    w = WalletBuilder(mode=WALLET_MODE_PATH)
    fid = w.add_file("/data/bigfile.bin", file_size, mtime, head_tail_hash)
    for chunk_offset, seed, cksum, coord in scan_results:
        w.add_coord(fid, chunk_offset, seed, cksum, coord)
    blob = w.seal()
    open("out.pogwallet","wb").write(blob)

Usage (read + reconstruct):
    wr = WalletReader(blob)
    data = wr.reconstruct_file(file_idx=0, src=open(path,"rb").read())
"""

from __future__ import annotations
import struct
import os
import mmap
from dataclasses import dataclass, field
from typing import Callable, Optional

# ── constants (mirrors C header) ────────────────────────────────────

WALLET_MAGIC         = 0x574C5447   # "WLTG"
WALLET_VERSION       = 1
WALLET_FOOTER_MAGIC  = 0x454E4457   # "WDNE"

WALLET_MODE_PATH     = 0   # prod: mmap real files
WALLET_MODE_BUFFER   = 1   # test/API: caller buf

WALLET_FLAG_SEALED   = 0x01
WALLET_FLAG_VERIFIED = 0x02
WALLET_FLAG_DRIFT_OK = 0x04

DIAMOND_BLOCK_SIZE   = 64  # frozen

# ── result codes ─────────────────────────────────────────────────────

WALLET_OK              =  0
WALLET_WARN_META_DRIFT =  1
WALLET_ERR_SEED_FAIL   = -3
WALLET_ERR_CKSUM_FAIL  = -4
WALLET_ERR_BAD_MAGIC   = -5
WALLET_ERR_BAD_VERSION = -6
WALLET_ERR_GUARD_FAIL  = -7
WALLET_ERR_FILE_IDX    = -8
WALLET_ERR_IO          = -9
WALLET_ERR_DRIFT_FAIL  = -10

# ── struct formats (all little-endian) ───────────────────────────────
# WalletHeader: 128B
# magic(4) ver(1) mode(1) flags(1) _p(1) wallet_id(8)
# file_count(4) coord_count(4)
# file_entry_offset(8) string_pool_offset(8) string_pool_size(4)
# coord_offset(8) hdr_guard(4) _pad(72)
_HDR_FMT  = "<IBBBxQIIQQIQI72x"
_HDR_SIZE = 128

# FileEntry: 64B
# path_offset(4) path_len(2) file_idx(2) file_size(8) mtime(8)
# head_tail_hash(8) chunk_start(4) chunk_count(4) entry_guard(4) _pad(20)
_FE_FMT   = "<IHHQqQIII 20x"
_FE_SIZE  = 64

# CoordRecord: 40B
# file_idx(2) _p(2) checksum(4) scan_offset(8) seed(8)
# fast_sig(4) coord_packed(4) drift_window(1) _reserved(7)
_CR_FMT   = "<HHIQQIIb 7x"
_CR_SIZE  = 40

# WalletFooter: 16B
# magic(4) coord_count_check(4) xxh64_digest(8)
_FT_FMT   = "<IIQ"
_FT_SIZE  = 16

# sanity
assert struct.calcsize(_HDR_FMT) == _HDR_SIZE, f"HDR {struct.calcsize(_HDR_FMT)}"
assert struct.calcsize(_FE_FMT)  == _FE_SIZE,  f"FE {struct.calcsize(_FE_FMT)}"
assert struct.calcsize(_CR_FMT)  == _CR_SIZE,  f"CR {struct.calcsize(_CR_FMT)}"
assert struct.calcsize(_FT_FMT)  == _FT_SIZE,  f"FT {struct.calcsize(_FT_FMT)}"

# ── integer hash (mirrors C wallet_hash_buf) ─────────────────────────

_H1 = 0x9e3779b97f4a7c15
_H2 = 0x6c62272e07bb0142
_M64 = (1 << 64) - 1

def _rotl64(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & _M64

def _hash_update(acc: int, word: int) -> int:
    acc = (acc ^ ((word * _H1) & _M64)) & _M64
    acc = _rotl64(acc, 27)
    acc = (acc * _H2 + 0x94d049bb133111eb) & _M64
    return acc

def wallet_hash_buf(data: bytes) -> int:
    """Integer xxh64-style digest — matches C wallet_hash_buf."""
    acc = (_H1 ^ len(data)) & _M64
    i = 0
    while i + 8 <= len(data):
        w, = struct.unpack_from("<Q", data, i)
        acc = _hash_update(acc, w)
        i += 8
    if i < len(data):
        tail = data[i:] + b"\x00" * (8 - len(data[i:]))
        w, = struct.unpack("<Q", tail)
        acc = _hash_update(acc, w)
    # finalizer
    acc = (acc ^ (acc >> 33)) & _M64
    acc = (acc * _H1) & _M64
    acc = (acc ^ (acc >> 29)) & _M64
    acc = (acc * _H2) & _M64
    acc = (acc ^ (acc >> 32)) & _M64
    return acc

# ── QRPN guard (mirrors qrpn_phi_scatter_hex) ────────────────────────

def _qrpn_guard(v: int) -> int:
    M = _M64
    v = (v ^ (v >> 33)) & M
    v = (v * 0xff51afd7ed558ccd) & M
    v = (v ^ (v >> 29)) & M
    v = (v * 0xc4ceb9fe1a85ec53) & M
    v = (v ^ (v >> 32)) & M
    return int((v ^ (v >> 32)) & 0xFFFFFFFF)

def _hdr_guard(wallet_id, file_count, coord_count, coord_offset,
               string_pool_offset, mode) -> int:
    mix = (wallet_id
           ^ ((file_count  << 16) & _M64)
           ^ ((coord_count << 32) & _M64)
           ^ (coord_offset & _M64)
           ^ (string_pool_offset & _M64)
           ^ ((mode << 56) & _M64)) & _M64
    return _qrpn_guard(mix)

def _fe_guard(file_size, mtime, head_tail_hash,
              file_idx, path_offset, chunk_count) -> int:
    mix = (file_size
           ^ (mtime & _M64)
           ^ (head_tail_hash & _M64)
           ^ ((file_idx   << 48) & _M64)
           ^ ((path_offset << 32) & _M64)
           ^ ((chunk_count << 16) & _M64)) & _M64
    return _qrpn_guard(mix)

# ── chunk fingerprint (mirrors C scanner) ────────────────────────────

_MIX_A = 0xff51afd7ed558ccd
_MIX_B = 0xc4ceb9fe1a85ec53

def chunk_seed(data64: bytes) -> int:
    """XOR-fold 64B → seed (mirrors _scan_chunk_seed / _wallet_chunk_seed)."""
    assert len(data64) == 64
    words = struct.unpack("<8Q", data64)
    s = 0
    for w in words:
        s ^= w
    s = (s ^ (s >> 33)) & _M64
    s = (s * _MIX_A) & _M64
    s = (s ^ (s >> 33)) & _M64
    s = (s * _MIX_B) & _M64
    s = (s ^ (s >> 33)) & _M64
    return s

def chunk_checksum(data64: bytes) -> int:
    """XOR-fold 64B → uint32 checksum (mirrors _scan_chunk_checksum)."""
    assert len(data64) == 64
    acc = 0
    for i in range(16):
        w, = struct.unpack_from("<I", data64, i * 4)
        acc ^= w
    return acc & 0xFFFFFFFF

def file_identity_hash(head64: bytes, tail64: bytes) -> int:
    """head+tail hash for file identity check."""
    h, = struct.unpack_from("<Q", head64, 0)
    t, = struct.unpack_from("<Q", tail64, 0)
    combined = (h ^ t) & _M64
    combined = (combined ^ (combined >> 33)) & _M64
    combined = (combined * _MIX_A) & _M64
    combined = (combined ^ (combined >> 33)) & _M64
    combined = (combined * _MIX_B) & _M64
    combined = (combined ^ (combined >> 33)) & _M64
    return combined

def coord_pack(face: int, edge: int, z: int) -> int:
    return ((face & 0xFF) << 24) | ((edge & 0xFF) << 16) | ((z & 0xFF) << 8)

def coord_unpack(packed: int) -> tuple[int, int, int]:
    return (packed >> 24) & 0xFF, (packed >> 16) & 0xFF, (packed >> 8) & 0xFF

# ── verify chunk (HARD FAIL policy) ─────────────────────────────────

def verify_chunk(data64: bytes, seed: int, checksum: int) -> int:
    """
    Returns WALLET_OK, WALLET_ERR_SEED_FAIL, or WALLET_ERR_CKSUM_FAIL.
    No exceptions — caller checks return code.
    """
    if chunk_seed(data64) != seed:
        return WALLET_ERR_SEED_FAIL
    if chunk_checksum(data64) != checksum:
        return WALLET_ERR_CKSUM_FAIL
    return WALLET_OK

# ── data classes (in-memory representation) ─────────────────────────

@dataclass
class _PendingCoord:
    file_idx:     int
    scan_offset:  int
    seed:         int
    checksum:     int
    face:         int
    edge:         int
    z:            int
    drift_window: int = 0

@dataclass
class _PendingFile:
    path:           bytes
    file_size:      int
    mtime:          int
    head_tail_hash: int
    chunk_start:    int = 0   # set when sealed
    chunk_count:    int = 0

# ══════════════════════════════════════════════════════════════════════
# WalletBuilder
# ══════════════════════════════════════════════════════════════════════

class WalletBuilder:
    """
    Build a .pogwallet binary blob.

    Contract: add_file → add_coords for that file → add_file → ...
    (chunk_start is set at add_file time based on current coord_count)
    """

    def __init__(self, mode: int = WALLET_MODE_PATH, wallet_id: int = 0):
        self.mode       = mode
        self.wallet_id  = wallet_id or int.from_bytes(os.urandom(8), "little")
        self._files:  list[_PendingFile]  = []
        self._coords: list[_PendingCoord] = []
        self._sealed  = False

    def add_file(self,
                 path: str | bytes,
                 file_size: int,
                 mtime: int,
                 head_tail_hash: int) -> int:
        """Register a source file. Returns file_idx."""
        if self._sealed:
            raise RuntimeError("wallet already sealed")
        if isinstance(path, str):
            path = path.encode()
        pf = _PendingFile(
            path=path,
            file_size=file_size,
            mtime=mtime,
            head_tail_hash=head_tail_hash,
            chunk_start=len(self._coords),
            chunk_count=0,
        )
        idx = len(self._files)
        self._files.append(pf)
        return idx

    def add_coord(self,
                  file_idx: int,
                  scan_offset: int,
                  seed: int,
                  checksum: int,
                  face: int = 0,
                  edge: int = 0,
                  z: int = 0,
                  drift_window: int = 0) -> int:
        """Add one CoordRecord. Returns coord_id."""
        if self._sealed:
            raise RuntimeError("wallet already sealed")
        if file_idx >= len(self._files):
            raise ValueError(f"file_idx {file_idx} out of range")
        cr = _PendingCoord(
            file_idx=file_idx,
            scan_offset=scan_offset,
            seed=seed,
            checksum=checksum,
            face=face,
            edge=edge,
            z=z,
            drift_window=drift_window,
        )
        coord_id = len(self._coords)
        self._coords.append(cr)
        self._files[file_idx].chunk_count += 1
        if drift_window > 0:
            self._has_drift = True
        return coord_id

    def seal(self) -> bytes:
        """Serialise to .pogwallet binary. Returns bytes."""
        if self._sealed:
            raise RuntimeError("already sealed")
        self._sealed = True

        # build string pool
        pool_parts = []
        pool_offsets = []
        off = 0
        for pf in self._files:
            pool_offsets.append(off)
            pool_parts.append(pf.path)
            off += len(pf.path)
        pool = b"".join(pool_parts)
        pool_size = len(pool)

        file_count  = len(self._files)
        coord_count = len(self._coords)

        # compute offsets in final blob
        hdr_size        = _HDR_SIZE
        fe_block_size   = file_count * _FE_SIZE
        pool_start      = hdr_size + fe_block_size
        coord_start     = pool_start + pool_size
        footer_start    = coord_start + coord_count * _CR_SIZE
        total_size      = footer_start + _FT_SIZE

        # build flags
        flags = WALLET_FLAG_SEALED
        if any(cr.drift_window > 0 for cr in self._coords):
            flags |= WALLET_FLAG_DRIFT_OK

        # header guard
        hg = _hdr_guard(
            self.wallet_id, file_count, coord_count,
            coord_start, pool_start, self.mode)

        hdr = struct.pack(
            _HDR_FMT,
            WALLET_MAGIC, WALLET_VERSION, self.mode, flags, 0,   # _pad0=0
            self.wallet_id,
            file_count, coord_count,
            hdr_size,        # file_entry_offset
            pool_start,      # string_pool_offset
            pool_size,       # string_pool_size
            coord_start,     # coord_offset
            hg,              # hdr_guard
        )
        assert len(hdr) == _HDR_SIZE, f"hdr {len(hdr)}"

        # file entries
        fe_parts = []
        for i, pf in enumerate(self._files):
            eg = _fe_guard(pf.file_size, pf.mtime, pf.head_tail_hash,
                           i, pool_offsets[i], pf.chunk_count)
            fe = struct.pack(
                _FE_FMT,
                pool_offsets[i],    # path_offset
                len(pf.path),       # path_len
                i,                  # file_idx
                pf.file_size,
                pf.mtime,
                pf.head_tail_hash,
                pf.chunk_start,
                pf.chunk_count,
                eg,                 # entry_guard
            )
            assert len(fe) == _FE_SIZE
            fe_parts.append(fe)

        # coord records
        cr_parts = []
        for cr in self._coords:
            packed = coord_pack(cr.face, cr.edge, cr.z)
            fast_sig = cr.seed & 0xFFFFFFFF
            rec = struct.pack(
                _CR_FMT,
                cr.file_idx,
                0,                  # _pad0
                cr.checksum,
                cr.scan_offset,
                cr.seed,
                fast_sig,
                packed,
                cr.drift_window,    # signed byte in format, value 0..127
            )
            assert len(rec) == _CR_SIZE
            cr_parts.append(rec)

        # assemble content (everything before footer)
        content = hdr + b"".join(fe_parts) + pool + b"".join(cr_parts)
        assert len(content) == footer_start

        digest = wallet_hash_buf(content)
        footer = struct.pack(_FT_FMT, WALLET_FOOTER_MAGIC, coord_count, digest)
        assert len(footer) == _FT_SIZE

        return content + footer

    @staticmethod
    def from_file_scan(path: str,
                       mode: int = WALLET_MODE_PATH,
                       drift_window: int = 0) -> tuple["WalletBuilder", int]:
        """
        Scan a real file → build wallet in one call.
        Returns (builder, file_idx).
        Zero-copy: mmap the file, walk 64B chunks.

        For large files (100GB+): mmap handles it transparently.
        """
        stat    = os.stat(path)
        fsize   = stat.st_size
        fmtime  = int(stat.st_mtime)

        wb = WalletBuilder(mode=mode)

        with open(path, "rb") as f:
            with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
                # compute head+tail hash
                head64 = bytes(mm[:64])        if fsize >= 64 else bytes(mm).ljust(64, b"\x00")
                tail64 = bytes(mm[-64:])       if fsize >= 64 else bytes(mm).ljust(64, b"\x00")
                hth = file_identity_hash(head64, tail64)

                fid = wb.add_file(path, fsize, fmtime, hth)

                offset = 0
                while offset < fsize:
                    end  = min(offset + DIAMOND_BLOCK_SIZE, fsize)
                    raw  = bytes(mm[offset:end])
                    # pad tail chunk
                    if len(raw) < DIAMOND_BLOCK_SIZE:
                        raw = raw.ljust(DIAMOND_BLOCK_SIZE, b"\x00")

                    s = chunk_seed(raw)
                    c = chunk_checksum(raw)
                    wb.add_coord(fid, offset, s, c, drift_window=drift_window)
                    offset += DIAMOND_BLOCK_SIZE

        return wb, fid


# ══════════════════════════════════════════════════════════════════════
# WalletReader
# ══════════════════════════════════════════════════════════════════════

@dataclass
class CoordInfo:
    file_idx:     int
    scan_offset:  int
    seed:         int
    checksum:     int
    face:         int
    edge:         int
    z:            int
    fast_sig:     int
    drift_window: int
    coord_packed: int

@dataclass
class FileInfo:
    path:           bytes
    file_size:      int
    mtime:          int
    head_tail_hash: int
    chunk_start:    int
    chunk_count:    int
    file_idx:       int


class WalletReader:
    """
    Read a .pogwallet blob and reconstruct chunks.

    Verify-on-open: checks header + footer + all FileEntry guards.
    Reconstruct: O(1) per chunk — seek(offset) + verify.
    """

    def __init__(self, blob: bytes, skip_verify: bool = False):
        self._blob = blob
        self._parse()
        if not skip_verify:
            self._verify_structure()

    def _parse(self):
        if len(self._blob) < _HDR_SIZE + _FT_SIZE:
            raise ValueError("blob too short to be a wallet")

        # header
        vals = struct.unpack_from(_HDR_FMT, self._blob, 0)
        (self.magic, self.version, self.mode, self.flags, _pad,
         self.wallet_id,
         self.file_count, self.coord_count,
         self.file_entry_offset, self.string_pool_offset, self.string_pool_size,
         self.coord_offset, self.hdr_guard) = vals

        # footer (last 16B)
        ft_off = len(self._blob) - _FT_SIZE
        self._footer_offset = ft_off
        self._ft_magic, self._ft_count, self._ft_digest = struct.unpack_from(
            _FT_FMT, self._blob, ft_off)

    def _verify_structure(self):
        if self.magic != WALLET_MAGIC:
            raise ValueError(f"bad magic: 0x{self.magic:08X}")
        if self.version != WALLET_VERSION:
            raise ValueError(f"bad version: {self.version}")

        expected_guard = _hdr_guard(
            self.wallet_id, self.file_count, self.coord_count,
            self.coord_offset, self.string_pool_offset, self.mode)
        if expected_guard != self.hdr_guard:
            raise ValueError("header guard mismatch")

        if self._ft_magic != WALLET_FOOTER_MAGIC:
            raise ValueError("footer magic mismatch")
        if self._ft_count != self.coord_count:
            raise ValueError("footer coord_count mismatch")

        # verify digest over content (everything except footer)
        content = self._blob[:self._footer_offset]
        digest  = wallet_hash_buf(content)
        if digest != self._ft_digest:
            raise ValueError("footer digest mismatch — wallet corrupted")

        # verify all FileEntry guards
        for i in range(self.file_count):
            fe = self._read_file_entry(i)
            eg = _fe_guard(fe.file_size, fe.mtime, fe.head_tail_hash,
                           fe.file_idx, 0, fe.chunk_count)  # path_offset recomputed
            # Note: guard uses path_offset from record
            off = self.file_entry_offset + i * _FE_SIZE
            po, pl, fi, fs, mt, hth, cs, cc, entry_guard = struct.unpack_from(
                _FE_FMT, self._blob, off)
            eg_check = _fe_guard(fs, mt, hth, fi, po, cc)
            if eg_check != entry_guard:
                raise ValueError(f"FileEntry[{i}] guard mismatch")

    def _read_file_entry(self, idx: int) -> FileInfo:
        off = self.file_entry_offset + idx * _FE_SIZE
        po, pl, fi, fs, mt, hth, cs, cc, eg = struct.unpack_from(
            _FE_FMT, self._blob, off)
        path = self._blob[self.string_pool_offset + po:
                          self.string_pool_offset + po + pl]
        return FileInfo(path=path, file_size=fs, mtime=mt,
                        head_tail_hash=hth, chunk_start=cs,
                        chunk_count=cc, file_idx=fi)

    def _read_coord(self, coord_id: int) -> CoordInfo:
        off = self.coord_offset + coord_id * _CR_SIZE
        fi, _p, ck, so, sd, fs, cp, dw = struct.unpack_from(
            _CR_FMT, self._blob, off)
        face, edge, z = coord_unpack(cp)
        return CoordInfo(file_idx=fi, scan_offset=so, seed=sd,
                         checksum=ck, face=face, edge=edge, z=z,
                         fast_sig=fs, drift_window=dw, coord_packed=cp)

    def file_info(self, file_idx: int) -> FileInfo:
        if file_idx >= self.file_count:
            raise IndexError(f"file_idx {file_idx} >= {self.file_count}")
        return self._read_file_entry(file_idx)

    def coord_info(self, coord_id: int) -> CoordInfo:
        if coord_id >= self.coord_count:
            raise IndexError(f"coord_id {coord_id} >= {self.coord_count}")
        return self._read_coord(coord_id)

    def lookup_by_offset(self, file_idx: int, byte_offset: int) -> int:
        """O(1) coord_id lookup by file + byte offset."""
        fe = self._read_file_entry(file_idx)
        local_id = byte_offset // DIAMOND_BLOCK_SIZE
        if local_id >= fe.chunk_count:
            return self.coord_count  # sentinel = not found
        return fe.chunk_start + local_id

    def fetch_chunk(self,
                    coord_id: int,
                    src: bytes) -> bytes:
        """
        O(1) fetch + verify single chunk from src buffer.
        Returns 64B chunk on success.
        Raises WalletError on any mismatch (HARD FAIL).
        """
        cr   = self._read_coord(coord_id)
        data = self._fetch_with_drift(cr, src)
        return data

    def _fetch_with_drift(self, cr: CoordInfo, src: bytes) -> bytes:
        off = cr.scan_offset
        if off + DIAMOND_BLOCK_SIZE <= len(src):
            chunk = src[off: off + DIAMOND_BLOCK_SIZE]
            rv = verify_chunk(chunk, cr.seed, cr.checksum)
            if rv == WALLET_OK:
                return chunk
        else:
            rv = WALLET_ERR_SEED_FAIL

        # drift search
        if cr.drift_window > 0:
            max_d = cr.drift_window * DIAMOND_BLOCK_SIZE
            d = -DIAMOND_BLOCK_SIZE
            while True:
                try_off = cr.scan_offset + d
                if 0 <= try_off and try_off + DIAMOND_BLOCK_SIZE <= len(src):
                    chunk = src[try_off: try_off + DIAMOND_BLOCK_SIZE]
                    if verify_chunk(chunk, cr.seed, cr.checksum) == WALLET_OK:
                        return chunk
                d = -d if d < 0 else -(d + DIAMOND_BLOCK_SIZE)
                if abs(d) > max_d:
                    break
            raise WalletError(WALLET_ERR_DRIFT_FAIL,
                              f"seed not found within drift_window={cr.drift_window}")

        if rv == WALLET_ERR_SEED_FAIL:
            raise WalletError(WALLET_ERR_SEED_FAIL,
                              f"seed mismatch at offset {cr.scan_offset}")
        raise WalletError(WALLET_ERR_CKSUM_FAIL,
                          f"checksum mismatch at offset {cr.scan_offset}")

    def reconstruct_file(self,
                         file_idx: int,
                         src: bytes,
                         on_meta_drift: Optional[Callable] = None) -> bytes:
        """
        Reconstruct all chunks of a file from src buffer.

        Meta mismatch (file_size) → calls on_meta_drift(fe) if set,
        then continues (SOFT WARN).
        Chunk mismatch → raises WalletError immediately (HARD FAIL).

        Returns reconstructed bytes (trimmed to original file_size).
        """
        fe = self._read_file_entry(file_idx)

        if len(src) != fe.file_size:
            if on_meta_drift:
                on_meta_drift(fe)

        parts = []
        for i in range(fe.chunk_count):
            cr = self._read_coord(fe.chunk_start + i)
            chunk = self._fetch_with_drift(cr, src)
            parts.append(chunk)

        raw = b"".join(parts)
        # trim to original size (last chunk may be zero-padded)
        return raw[:fe.file_size]

    def reconstruct_path(self,
                         file_idx: int,
                         path: Optional[str] = None,
                         on_meta_drift: Optional[Callable] = None) -> bytes:
        """
        Mode 0 (path-based): open real file, mmap, reconstruct.
        If path is None, uses the path stored in the wallet.
        Zero-copy: only reads 64B per chunk seek.
        """
        fe = self._read_file_entry(file_idx)
        if path is None:
            path = fe.path.decode(errors="replace")

        stat = os.stat(path)
        if stat.st_size != fe.file_size:
            if on_meta_drift:
                on_meta_drift(fe)

        parts = []
        with open(path, "rb") as f:
            with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
                for i in range(fe.chunk_count):
                    cr = self._read_coord(fe.chunk_start + i)
                    off = cr.scan_offset
                    end = off + DIAMOND_BLOCK_SIZE

                    if end <= len(mm):
                        raw = bytes(mm[off:end])
                    elif cr.drift_window > 0:
                        # read full file into buffer for drift search
                        raw = None
                        buf = bytes(mm)
                        chunk = self._fetch_with_drift(cr, buf)
                        parts.append(chunk)
                        continue
                    else:
                        raise WalletError(WALLET_ERR_IO,
                                          f"offset {off} beyond file end")

                    rv = verify_chunk(raw, cr.seed, cr.checksum)
                    if rv != WALLET_OK:
                        raise WalletError(rv,
                                          f"verify failed at offset {off}")
                    parts.append(raw)

        result = b"".join(parts)
        return result[:fe.file_size]

    def stats(self) -> dict:
        drift_records = sum(
            1 for i in range(self.coord_count)
            if self._read_coord(i).drift_window > 0)
        total_source = sum(
            self._read_file_entry(i).file_size
            for i in range(self.file_count))
        wallet_bytes = len(self._blob)
        return {
            "file_count":         self.file_count,
            "coord_count":        self.coord_count,
            "total_source_bytes": total_source,
            "wallet_bytes":       wallet_bytes,
            "drift_records":      drift_records,
            "mode":               self.mode,
            "flags":              self.flags,
            "ratio_wallet_to_source": (
                round(wallet_bytes / total_source, 4)
                if total_source else 0),
        }


class WalletError(Exception):
    def __init__(self, code: int, msg: str = ""):
        super().__init__(msg or f"wallet error {code}")
        self.code = code
