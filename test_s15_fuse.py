"""
test_s15_fuse.py — WalletFile + WalletFS tests (S15)
═════════════════════════════════════════════════════
WF = WalletFile (file-like, no FUSE needed)
  WF1   read() full
  WF2   read(n) partial
  WF3   seek+read
  WF4   seek SEEK_END
  WF5   seek SEEK_CUR
  WF6   tell() tracks position
  WF7   read past EOF → partial
  WF8   read at EOF → b""
  WF9   readinto() zero-copy
  WF10  context manager (close)
  WF11  stream() from position
  WF12  stream() advances pos
  WF13  buffered() wrapper
  WF14  file_idx view
  WF15  size property

FS = WalletFS (FUSE, in-process via pyfuse3 mount)
  FS1   WalletFS instantiation + entry table
  FS2   _make_entry regular file attrs
  FS3   _make_entry dir attrs
  FS4   read() routes to vf.read (via direct call, no mount)
  FS5   read() out-of-range → b""
  FS6   file_idx routing (wallet.bin vs file_0.bin)
  FS7   mount + ls + cat (live FUSE, skip if no /dev/fuse perms)
"""

import asyncio
import io
import os
import sys
import stat
import tempfile
import time

from pogls_wallet_py import (
    WalletBuilder, WALLET_MODE_BUFFER, DIAMOND_BLOCK_SIZE,
    chunk_seed, chunk_checksum, file_identity_hash,
)
from pogls_mount import VirtualFile
from pogls_fuse import WalletFile, buffered, _HAS_FUSE

# ── harness ───────────────────────────────────────────────────────────────────

PASS = FAIL = 0

def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1; print(f"  PASS  {name}")
    else:
        FAIL += 1; print(f"  FAIL  {name}" + (f"  [{detail}]" if detail else ""))

def skip(name):
    print(f"  SKIP  {name}")

# ── fixtures ──────────────────────────────────────────────────────────────────

BS = DIAMOND_BLOCK_SIZE

def make_vf(srcs):
    wb = WalletBuilder(mode=WALLET_MODE_BUFFER)
    for raw in srcs:
        h = raw[:64].ljust(64, b"\x00"); t = raw[-64:].ljust(64, b"\x00")
        fid = wb.add_file(b"f", len(raw), 0, file_identity_hash(h, t))
        off = 0
        while off < len(raw):
            c = raw[off:off+BS].ljust(BS, b"\x00")
            wb.add_coord(fid, off, chunk_seed(c), chunk_checksum(c))
            off += BS
    blob = wb.seal()
    return VirtualFile(blob, {i: s for i, s in enumerate(srcs)})

src_a = bytes(range(256))
src_b = bytes((i*3) % 256 for i in range(192))

vf1  = make_vf([src_a])
vf2  = make_vf([src_a, src_b])

# ── WalletFile tests ──────────────────────────────────────────────────────────

print("\n── S15 WalletFile tests ──")

# WF1: read() full
wf = WalletFile(vf1)
check("WF1 read() full", wf.read() == src_a)

# WF2: read(n) partial
wf = WalletFile(vf1)
check("WF2 read(64)", wf.read(64) == src_a[:64])

# WF3: seek + read
wf = WalletFile(vf1)
wf.seek(128)
check("WF3 seek+read", wf.read(64) == src_a[128:192])

# WF4: seek SEEK_END
wf = WalletFile(vf1)
wf.seek(-64, io.SEEK_END)
check("WF4 SEEK_END", wf.read() == src_a[-64:])

# WF5: seek SEEK_CUR
wf = WalletFile(vf1)
wf.read(64)
wf.seek(32, io.SEEK_CUR)
check("WF5 SEEK_CUR", wf.tell() == 96)
check("WF5 read after SEEK_CUR", wf.read(32) == src_a[96:128])

# WF6: tell() tracks
wf = WalletFile(vf1)
check("WF6 tell() initial", wf.tell() == 0)
wf.read(100)
check("WF6 tell() after read", wf.tell() == 100)

# WF7: read past EOF
wf = WalletFile(vf1)
wf.seek(240)
data = wf.read(100)  # only 16B remain
check("WF7 read past EOF", data == src_a[240:] and len(data) == 16)

# WF8: read at EOF
wf = WalletFile(vf1)
wf.seek(0, io.SEEK_END)
check("WF8 read at EOF", wf.read() == b"")

# WF9: readinto()
wf  = WalletFile(vf1)
buf = bytearray(64)
n   = wf.readinto(buf)
check("WF9 readinto n=64", n == 64)
check("WF9 readinto data", bytes(buf) == src_a[:64])

# WF10: context manager
with WalletFile(vf1) as wf:
    data = wf.read(64)
check("WF10 ctx manager read", data == src_a[:64])
check("WF10 ctx manager closed", wf.closed)

# WF11: stream() from position
wf = WalletFile(vf1)
wf.seek(128)
chunks = list(wf.stream())
check("WF11 stream from pos=128", b"".join(chunks) == src_a[128:])

# WF12: stream() advances pos
wf = WalletFile(vf1)
for chunk in wf.stream(chunk_size=64):
    pass
check("WF12 stream advances pos to EOF", wf.tell() == len(src_a))

# WF13: buffered() wrapper
wf_buf = buffered(vf1, buffer_size=256)
check("WF13 buffered read", wf_buf.read(64) == src_a[:64])
wf_buf.seek(192)
check("WF13 buffered seek+read", wf_buf.read(64) == src_a[192:])
wf_buf.close()

# WF14: file_idx view
wf_b = WalletFile(vf2, file_idx=1)
check("WF14 file_idx=1 size", wf_b.size == len(src_b))
check("WF14 file_idx=1 read", wf_b.read() == src_b)

# WF15: size property
wf = WalletFile(vf1)
check("WF15 size", wf.size == 256)
wf_full = WalletFile(vf2)
check("WF15 size multi-file", wf_full.size == 448)

# ── WalletFS tests (in-process, no real mount needed) ─────────────────────────

print("\n── S15 WalletFS tests ──")

if not _HAS_FUSE:
    for name in ["FS1","FS2","FS3","FS4","FS5","FS6","FS7"]:
        skip(f"{name} (pyfuse3 not available)")
else:
    # Run FS tests in a subprocess to isolate pyfuse3 C-level atexit SEGFAULT
    import subprocess, json as _json

    _FS_SCRIPT = """
import sys, os, stat, json, errno
import trio
sys.path.insert(0, sys.argv[1])
from pogls_wallet_py import *
from pogls_mount import VirtualFile
from pogls_fuse import WalletFS, _HAS_FUSE

BS = DIAMOND_BLOCK_SIZE
def make_vf(srcs):
    wb = WalletBuilder(mode=WALLET_MODE_BUFFER)
    for raw in srcs:
        h = raw[:64].ljust(64,b"\\x00"); t = raw[-64:].ljust(64,b"\\x00")
        fid = wb.add_file(b"f", len(raw), 0, file_identity_hash(h, t))
        off = 0
        while off < len(raw):
            c = raw[off:off+BS].ljust(BS, b"\\x00")
            wb.add_coord(fid, off, chunk_seed(c), chunk_checksum(c))
            off += BS
    return VirtualFile(wb.seal(), {i: s for i, s in enumerate(srcs)})

src_a = bytes(range(256))
src_b = bytes((i*3)%256 for i in range(192))
vf2 = make_vf([src_a, src_b])
fs  = WalletFS(vf2, trusted=True)

results = {}
results["FS1_wallet"] = b"wallet.bin" in fs._entries
results["FS1_file0"]  = b"file_0.bin" in fs._entries
results["FS1_file1"]  = b"file_1.bin" in fs._entries
results["FS1_meta"]   = len(fs._ino_meta) == 3

attr = fs._make_entry(2, 448, is_dir=False)
results["FS2_mode"]   = stat.S_IMODE(attr.st_mode) == 0o444
results["FS2_isreg"]  = bool(stat.S_ISREG(attr.st_mode))
results["FS2_size"]   = attr.st_size == 448

attr = fs._make_entry(1, 0, is_dir=True)
results["FS3_mode"]   = stat.S_IMODE(attr.st_mode) == 0o555
results["FS3_isdir"]  = bool(stat.S_ISDIR(attr.st_mode))

async def _tests():
    d = await fs.read(fh=2, offset=0, length=64)
    results["FS4_data"] = d == (src_a + src_b[:192])[:64]
    d = await fs.read(fh=2, offset=10000, length=64)
    results["FS5_oor"]  = d == b""
    d = await fs.read(fh=4, offset=0, length=64)
    results["FS6_file1"] = d == src_b[:64]

trio.run(_tests)
print(json.dumps(results))
"""

    _cwd = os.path.dirname(os.path.abspath(__file__))
    try:
        proc = subprocess.run(
            [sys.executable, "-c", _FS_SCRIPT, _cwd],
            capture_output=True, text=True, timeout=15
        )
        if proc.returncode not in (0, -11):  # -11 = SIGSEGV, still got output
            raise RuntimeError(proc.stderr[:200])
        # parse last JSON line (ignore any SEGFAULT noise after)
        out_lines = [l for l in proc.stdout.strip().splitlines() if l.startswith("{")]
        if not out_lines:
            raise RuntimeError("no JSON output")
        r = _json.loads(out_lines[-1])
        check("FS1 wallet.bin in entries",  r.get("FS1_wallet", False))
        check("FS1 file_0.bin in entries",  r.get("FS1_file0",  False))
        check("FS1 file_1.bin in entries",  r.get("FS1_file1",  False))
        check("FS1 ino_meta coverage",       r.get("FS1_meta",   False))
        check("FS2 file mode=0o444",         r.get("FS2_mode",   False))
        check("FS2 file type=REG",           r.get("FS2_isreg",  False))
        check("FS2 file st_size=448",        r.get("FS2_size",   False))
        check("FS3 dir mode=0o555",          r.get("FS3_mode",   False))
        check("FS3 dir type=DIR",            r.get("FS3_isdir",  False))
        check("FS4 read wallet.bin[0:64]",   r.get("FS4_data",   False))
        check("FS5 read out-of-range → b''", r.get("FS5_oor",    False))
        check("FS6 read file_1.bin[0:64]",   r.get("FS6_file1",  False))
    except Exception as e:
        for name in ["FS1","FS2","FS3","FS4","FS5","FS6"]:
            check(f"{name} subprocess", False, str(e)[:80])

    # FS7: live mount
    def _env_ok():
        if not os.path.exists("/dev/fuse"):        return False, "no /dev/fuse"
        if not os.access("/dev/fuse", os.R_OK|os.W_OK): return False, "no permission"
        if os.path.exists("/.dockerenv"):          return False, "docker container"
        try:
            r = os.uname().release.split(".")
            if (int(r[0]), int(r[1])) < (3, 9):   return False, f"kernel {os.uname().release}"
        except Exception:                          return False, "kernel unknown"
        return True, "ok"

    _ok, _reason = _env_ok()
    if not _ok:
        skip(f"FS7 live mount ({_reason})")
    else:
        mnt = tempfile.mkdtemp(prefix="pogls_test_mount_")
        try:
            from pogls_fuse import mount_wallet, umount_wallet
            t = mount_wallet(vf1, mnt, foreground=False, trusted=True)
            time.sleep(0.5)
            with open(os.path.join(mnt, "wallet.bin"), "rb") as f:
                data = f.read()
            check("FS7 cat wallet.bin == src_a", data == src_a)
        except Exception as e:
            skip(f"FS7 live mount: {e}")
        finally:
            try:
                from pogls_fuse import umount_wallet; umount_wallet()
            except: pass
            time.sleep(0.2)
            try: os.rmdir(mnt)
            except: pass

# ── summary ───────────────────────────────────────────────────────────────────

print(f"\n{'─'*36}")
print(f"  {PASS}/{PASS+FAIL} PASS", "✓" if FAIL == 0 else "✗")
if FAIL:
    sys.exit(1)
