"""
pogls_fuse.py — S15: WalletFile + FUSE mount
═════════════════════════════════════════════
Two layers, one module:

  Layer 1 — WalletFile (file-like, seekable, no OS dependency)
  ┌─────────────────────────────────────────────────────────────┐
  │  wf = WalletFile(vf, file_idx=None)  # None = virtual space │
  │  wf.read(n)   wf.seek(off)   wf.tell()                     │
  │  wf.readinto(buf)  → zero-copy into pre-allocated buffer    │
  │  use as context manager  (with WalletFile(...) as f: ...)   │
  └─────────────────────────────────────────────────────────────┘

  Layer 2 — WalletFS (pyfuse3 FUSE adapter)
  ┌─────────────────────────────────────────────────────────────┐
  │  Mounts .pogwallet as read-only filesystem:                 │
  │    /  (root dir)                                            │
  │    /wallet.bin       ← full virtual space (all files cat'd) │
  │    /file_0.bin       ← per-file views                       │
  │    /file_1.bin       ...                                    │
  │                                                             │
  │  Implements: lookup, getattr, open, read, readdir, release  │
  │  FUSE = thin adapter; all I/O routes through VirtualFile    │
  └─────────────────────────────────────────────────────────────┘

Usage:

    # Layer 1 — file-like (works everywhere, no FUSE needed)
    from pogls_mount import VirtualFile
    from pogls_fuse  import WalletFile

    vf = VirtualFile(blob, src_map, trusted=True)
    with WalletFile(vf) as f:
        header = f.read(128)
        f.seek(4096)
        layer  = f.read(65536)

    # feed directly into torch / safetensors / numpy
    import numpy as np
    with WalletFile(vf) as f:
        arr = np.frombuffer(f.read(512), dtype=np.float32)

    # Layer 2 — FUSE mount
    from pogls_fuse import WalletFS, mount_wallet

    mount_wallet(vf, mountpoint="/mnt/wallet", foreground=False)
    # now: cat /mnt/wallet/wallet.bin  works
    # umount /mnt/wallet  to release
"""

from __future__ import annotations

import errno
import io
import os
import stat
import sys
import time
import threading
from typing import Optional

from pogls_mount import VirtualFile
from pogls_wallet_py import DIAMOND_BLOCK_SIZE

# ══════════════════════════════════════════════════════════════════════════════
# Layer 1 — WalletFile
# ══════════════════════════════════════════════════════════════════════════════

class WalletFile(io.RawIOBase):
    """
    Seekable, readable file-like object backed by a VirtualFile.

    file_idx=None  → full virtual address space (all files concatenated)
    file_idx=N     → single file view

    Inherits io.RawIOBase → compatible with:
      io.BufferedReader(WalletFile(...))  → buffered reads
      np.frombuffer / torch.load         → direct tensor loading
      safetensors / pickle               → model weight streaming
    """

    def __init__(self, vf: VirtualFile, file_idx: Optional[int] = None):
        super().__init__()
        self._vf       = vf
        self._file_idx = file_idx
        self._size     = vf.file_size(file_idx) if file_idx is not None else vf.size
        self._pos      = 0

    # ── io.RawIOBase interface ─────────────────────────────────────────────

    def readable(self) -> bool:   return True
    def writable(self) -> bool:   return False
    def seekable(self) -> bool:   return True

    def readinto(self, b: bytearray) -> int:
        """Zero-copy into pre-allocated buffer. Returns bytes read."""
        if self._pos >= self._size:
            return 0
        n       = min(len(b), self._size - self._pos)
        chunk   = self._read_at(self._pos, n)
        nread   = len(chunk)
        b[:nread] = chunk
        self._pos += nread
        return nread

    def read(self, size: int = -1) -> bytes:
        if self.closed:
            raise ValueError("read on closed WalletFile")
        if self._pos >= self._size:
            return b""
        if size == -1 or size is None:
            size = self._size - self._pos
        n     = min(size, self._size - self._pos)
        data  = self._read_at(self._pos, n)
        self._pos += len(data)
        return data

    def readall(self) -> bytes:
        return self.read(-1)

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        if whence == io.SEEK_SET:
            new_pos = offset
        elif whence == io.SEEK_CUR:
            new_pos = self._pos + offset
        elif whence == io.SEEK_END:
            new_pos = self._size + offset
        else:
            raise ValueError(f"invalid whence={whence}")
        self._pos = max(0, min(new_pos, self._size))
        return self._pos

    def tell(self) -> int:
        return self._pos

    @property
    def size(self) -> int:
        return self._size

    def __len__(self) -> int:
        return self._size

    # ── internals ──────────────────────────────────────────────────────────

    def _read_at(self, offset: int, size: int) -> bytes:
        if self._file_idx is not None:
            return self._vf.read_file(self._file_idx, offset, size)
        return self._vf.read(offset, size)

    # ── stream adapter ────────────────────────────────────────────────────

    def stream(self, chunk_size: int = DIAMOND_BLOCK_SIZE, **kw):
        """
        Yield chunks from current position to EOF.
        Advances internal position as it yields.
        """
        remaining = self._size - self._pos
        if remaining <= 0:
            return
        if self._file_idx is not None:
            gen = self._vf.stream_file(self._file_idx, self._pos, remaining,
                                       chunk_size=chunk_size, **kw)
        else:
            gen = self._vf.stream(self._pos, remaining,
                                  chunk_size=chunk_size, **kw)
        for chunk in gen:
            self._pos += len(chunk)
            yield chunk


def buffered(vf: VirtualFile, file_idx: Optional[int] = None,
             buffer_size: int = 65536) -> io.BufferedReader:
    """Return a BufferedReader wrapping WalletFile — reduces FUSE small-read overhead."""
    return io.BufferedReader(WalletFile(vf, file_idx), buffer_size=buffer_size)


# ══════════════════════════════════════════════════════════════════════════════
# Layer 2 — WalletFS (pyfuse3 FUSE adapter)
# ══════════════════════════════════════════════════════════════════════════════

try:
    import pyfuse3
    import trio
    _HAS_FUSE = True
except ImportError:
    _HAS_FUSE = False


if _HAS_FUSE:

    # inode layout:
    #   1        = root dir
    #   2        = /wallet.bin  (full virtual space)
    #   3..N+2   = /file_0.bin .. /file_{N-1}.bin
    _INO_ROOT    = pyfuse3.ROOT_INODE   # always 1
    _INO_WALLET  = 2

    def _ino_file(idx: int) -> int:
        return _INO_WALLET + 1 + idx

    class WalletFS(pyfuse3.Operations):
        """
        Read-only FUSE filesystem exposing a VirtualFile as real files.

        Mount layout:
          /wallet.bin        full virtual address space
          /file_0.bin        individual file 0
          /file_1.bin        individual file 1
          ...
        """

        def __init__(self, vf: VirtualFile, trusted: bool = True):
            super().__init__()
            self._vf      = vf
            self._trusted = trusted
            self._created = int(time.time())

            # build name → inode + size tables once
            self._entries: dict[bytes, tuple[int, int]] = {}  # name → (ino, size)
            self._entries[b"wallet.bin"] = (_INO_WALLET, vf.size)
            for i in range(vf.file_count):
                name = f"file_{i}.bin".encode()
                self._entries[name] = (_ino_file(i), vf.file_size(i))

            # reverse: ino → (name, size, file_idx)  file_idx=None for wallet.bin
            self._ino_meta: dict[int, tuple[bytes, int, Optional[int]]] = {
                _INO_WALLET: (b"wallet.bin", vf.size, None),
            }
            for i in range(vf.file_count):
                ino  = _ino_file(i)
                name = f"file_{i}.bin".encode()
                self._ino_meta[ino] = (name, vf.file_size(i), i)

        # ── FUSE ops ───────────────────────────────────────────────────

        async def lookup(self, parent_inode, name, ctx=None):
            if parent_inode != _INO_ROOT:
                raise pyfuse3.FUSEError(errno.ENOENT)
            entry = self._entries.get(name)
            if entry is None:
                raise pyfuse3.FUSEError(errno.ENOENT)
            ino, size = entry
            return self._make_entry(ino, size, is_dir=False)

        async def getattr(self, inode, ctx=None):
            if inode == _INO_ROOT:
                return self._make_entry(_INO_ROOT, 0, is_dir=True)
            meta = self._ino_meta.get(inode)
            if meta is None:
                raise pyfuse3.FUSEError(errno.ENOENT)
            _, size, _ = meta
            return self._make_entry(inode, size, is_dir=False)

        async def opendir(self, inode, ctx):
            if inode != _INO_ROOT:
                raise pyfuse3.FUSEError(errno.ENOTDIR)
            return inode

        async def readdir(self, fh, start_id, token):
            # entries: (name, attr, next_id)
            all_entries = (
                [(b".", _INO_ROOT, True), (b"..", _INO_ROOT, True)]
                + [(name, ino, False) for name, (ino, _) in self._entries.items()]
            )
            for idx, (name, ino, is_dir) in enumerate(all_entries):
                if idx < start_id:
                    continue
                attr = self._make_entry(ino, 0 if is_dir else self._ino_meta.get(ino, (None, 0, None))[1], is_dir=is_dir)
                if not pyfuse3.readdir_reply(token, name, attr, idx + 1):
                    return

        async def open(self, inode, flags, ctx):
            if inode not in self._ino_meta:
                raise pyfuse3.FUSEError(errno.ENOENT)
            # read-only filesystem
            if flags & os.O_WRONLY or flags & os.O_RDWR:
                raise pyfuse3.FUSEError(errno.EROFS)
            return pyfuse3.FileInfo(fh=inode, keep_cache=True)

        async def read(self, fh, offset, length):
            meta = self._ino_meta.get(fh)
            if meta is None:
                raise pyfuse3.FUSEError(errno.EBADF)
            _, size, file_idx = meta
            if offset >= size:
                return b""
            length = min(length, size - offset)
            try:
                if file_idx is None:
                    return self._vf.read(offset, length)
                return self._vf.read_file(file_idx, offset, length)
            except Exception:
                raise pyfuse3.FUSEError(errno.EIO)

        async def release(self, fh):
            pass  # no per-fd state to release

        async def releasedir(self, fh):
            pass

        # ── helpers ────────────────────────────────────────────────────

        def _make_entry(self, ino: int, size: int, is_dir: bool) -> pyfuse3.EntryAttributes:
            attr           = pyfuse3.EntryAttributes()
            attr.st_ino    = ino
            attr.st_size   = size
            attr.st_nlink  = 2 if is_dir else 1
            attr.st_uid    = os.getuid()
            attr.st_gid    = os.getgid()
            attr.st_atime_ns = self._created * 10**9
            attr.st_mtime_ns = self._created * 10**9
            attr.st_ctime_ns = self._created * 10**9
            attr.entry_timeout = 300
            attr.attr_timeout  = 300
            if is_dir:
                attr.st_mode = stat.S_IFDIR | 0o555
            else:
                attr.st_mode = stat.S_IFREG | 0o444
            return attr


    def mount_wallet(
        vf: VirtualFile,
        mountpoint: str,
        foreground: bool = True,
        trusted: bool = True,
        allow_other: bool = False,
    ):
        """
        Mount VirtualFile at mountpoint (blocking if foreground=True).

        foreground=True  → blocks; Ctrl-C or pyfuse3.close() to unmount
        foreground=False → runs in background thread; returns thread handle
        """
        os.makedirs(mountpoint, exist_ok=True)
        fs      = WalletFS(vf, trusted=trusted)
        options = set(pyfuse3.default_options)
        options.add("fsname=pogls_wallet")
        options.discard("default_permissions")  # we handle read-only ourselves
        if allow_other:
            options.add("allow_other")

        # kernel check before init — prevents SEGFAULT on old/container kernels
        try:
            rel = os.uname().release.split(".")
            if (int(rel[0]), int(rel[1])) < (3, 9):
                raise RuntimeError("kernel < 3.9, FUSE not supported")
        except (ValueError, IndexError):
            pass

        pyfuse3.init(fs, mountpoint, options)

        async def _run():
            try:
                await trio.lowlevel.checkpoint()
                await pyfuse3.main()
            except Exception:
                pass
            finally:
                try: pyfuse3.close()
                except: pass

        if foreground:
            trio.run(_run)
        else:
            def _thread():
                trio.run(_run)
            t = threading.Thread(target=_thread, daemon=True, name="WalletFS")
            t.start()
            return t


    def umount_wallet():
        """Signal the running WalletFS to unmount."""
        try:
            pyfuse3.close(unmount=True)
        except Exception:
            pass

else:
    # stubs when pyfuse3 not available
    class WalletFS:  # type: ignore
        def __init__(self, *a, **kw):
            raise ImportError("pyfuse3 not installed — run: pip install pyfuse3")

    def mount_wallet(*a, **kw):
        raise ImportError("pyfuse3 not installed")

    def umount_wallet():
        pass
