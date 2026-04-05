"""
pogls_delta_bridge.py — Python Bridge: BlockFabric ↔ Delta Lane
================================================================
เชื่อม pogls_fabric.py เข้ากับ pogls_delta.c crash recovery system

อุดมการณ์:
  • ไม่แตะไฟล์ต้นฉบับ
  • snapshot.merkle = Single Source of Truth
  • ผู้ใช้ไม่รู้ว่ามี .pogls/ อยู่

สิ่งที่ bridge นี้ทำ:
  1. wraps BlockFabric.write() → delta_append() ผ่าน ctypes
  2. wraps BlockFabric.snapshot() → delta_commit() atomic
  3. boot scan → delta_recover() ก่อน fabric open
  4. audit hook — ตรวจ X+(-X)=0 ทุก batch

Usage:
    from pogls_delta_bridge import DeltaFabric

    fabric = DeltaFabric("model.safetensors")
    fabric.write(data, lane=LANE_X, addr=angular_addr)
    fabric.write(data, lane=LANE_NX, addr=angular_addr)
    snap = fabric.commit()          # atomic: audit → merkle → rename
    view = fabric.slice(0, 4<<20)   # zero-copy read ปกติ
"""

import os
import ctypes
import struct
import hashlib
import threading
import time
import json
import shutil
from pathlib import Path
from dataclasses import dataclass
from typing import Optional, Callable
from enum import IntEnum

# ── import fabric ──────────────────────────────────────────────────
from pogls_fabric import BlockFabric, SnapshotPointer, BLOCK_SIZE

# ══════════════════════════════════════════════════════════════════════
# CONSTANTS  (ตรงกับ pogls_delta.h)
# ══════════════════════════════════════════════════════════════════════

DELTA_MAGIC        = 0x504C4400   # "PLD\0"
DELTA_VERSION      = 1
DELTA_BLOCK_SIZE   = 256
DELTA_HEADER_SIZE  = 32
DELTA_MAX_PAYLOAD  = DELTA_BLOCK_SIZE - DELTA_HEADER_SIZE  # 224B

PHI_SCALE          = 1 << 20      # 2²⁰ = 1,048,576
PHI_UP             = 1696631      # floor(φ  × 2²⁰)
PHI_DOWN           = 648055       # floor(φ⁻¹ × 2²⁰)

LANE_X  = 0
LANE_NX = 1   # -X
LANE_Y  = 2
LANE_NY = 3   # -Y
LANE_COUNT = 4

LANE_NAMES = {LANE_X: "X", LANE_NX: "-X", LANE_Y: "Y", LANE_NY: "-Y"}

POGLS_DIR          = ".pogls"
FNAME_MERKLE       = "snapshot.merkle"
FNAME_MERKLE_PEND  = "snapshot.merkle.pending"

PENDING_NAMES = {
    LANE_X:  "lane_X.pending",
    LANE_NX: "lane_nX.pending",
    LANE_Y:  "lane_Y.pending",
    LANE_NY: "lane_nY.pending",
}
DELTA_NAMES = {
    LANE_X:  "lane_X.delta",
    LANE_NX: "lane_nX.delta",
    LANE_Y:  "lane_Y.delta",
    LANE_NY: "lane_nY.delta",
}

# ── Merkle record struct (80 bytes, big-endian) ─────────────────────
# magic(4) pad(4) epoch(8) seq×4(32) root(32) crc32(4)
MERKLE_FMT  = ">II Q QQQQ 32s I"
MERKLE_SIZE = struct.calcsize(MERKLE_FMT)   # = 80

# ── Delta block header (32 bytes) ──────────────────────────────────
# magic(4) lane_id(1) version(1) pad(2) seq(8) addr(8) payload_size(4) crc32(4)
DELTA_HDR_FMT  = ">IBBHQQIi"   # 32 bytes — signed i เพื่อ CRC ชัดเจน
DELTA_HDR_SIZE = DELTA_HEADER_SIZE


# ══════════════════════════════════════════════════════════════════════
# CRC32  (IEEE 802.3 — same as pogls_delta.c)
# ══════════════════════════════════════════════════════════════════════

import zlib

def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


# ══════════════════════════════════════════════════════════════════════
# RECOVERY RESULT
# ══════════════════════════════════════════════════════════════════════

class RecoveryResult(IntEnum):
    CLEAN  =  0   # committed สมบูรณ์
    TORN   =  1   # crash ระหว่าง commit → fallback
    NEW    =  2   # ไฟล์ใหม่ ยังไม่เคย ingest
    ERROR  = -1


# ══════════════════════════════════════════════════════════════════════
# DELTA CONTEXT  (Python side)
# ══════════════════════════════════════════════════════════════════════

class DeltaContext:
    """
    State ของ 1 source file — mirror of C Delta_Context
    ใช้ pure Python I/O แทน ctypes (portable, ไม่ต้อง compile .so)
    """

    def __init__(self, source_path: str):
        self.source_path  = Path(source_path).resolve()
        self.pogls_dir    = self._build_pogls_dir()
        self.lane_files   = {}     # lane_id → file object
        self.lane_seq     = {i: 0 for i in range(LANE_COUNT)}
        self.epoch        = 0
        self._lock        = threading.Lock()
        self._open        = False

    def _build_pogls_dir(self) -> Path:
        parent    = self.source_path.parent
        pogls_root = parent / POGLS_DIR
        return pogls_root / self.source_path.name

    # ── open ──────────────────────────────────────────────────────────

    def open(self) -> "DeltaContext":
        self.pogls_dir.mkdir(parents=True, exist_ok=True)

        # restore epoch จาก merkle ถ้ามี
        rec = self._read_merkle()
        if rec:
            self.epoch = rec["epoch"]
            for i in range(LANE_COUNT):
                self.lane_seq[i] = rec["seq"][i]

        # เปิด .pending files (append mode)
        for lane_id, fname in PENDING_NAMES.items():
            path = self.pogls_dir / fname
            self.lane_files[lane_id] = open(path, "a+b")

        self._open = True
        return self

    # ── append ────────────────────────────────────────────────────────

    def append(self, lane_id: int, addr: int, data: bytes) -> int:
        """
        append 1 delta block ลง lane
        คืน seq number ที่เขียนไป
        """
        if not self._open:
            raise RuntimeError("DeltaContext not open")
        if lane_id not in range(LANE_COUNT):
            raise ValueError(f"lane_id must be 0-3, got {lane_id}")
        if len(data) == 0 or len(data) > DELTA_MAX_PAYLOAD:
            raise ValueError(f"payload size {len(data)} out of range [1,{DELTA_MAX_PAYLOAD}]")

        with self._lock:
            self.lane_seq[lane_id] += 1
            seq = self.lane_seq[lane_id]

            # build header (32 bytes)
            # pack ก่อนโดยไม่มี crc → คำนวณ crc → pack ใหม่
            hdr_no_crc = struct.pack(
                ">IBBHQQIi",
                DELTA_MAGIC,
                lane_id,
                DELTA_VERSION,
                0,             # pad
                seq,
                addr,
                len(data),
                0,             # crc placeholder
            )
            crc = _crc32(hdr_no_crc[:-4] + data)
            hdr = hdr_no_crc[:-4] + struct.pack(">I", crc)

            f = self.lane_files[lane_id]
            f.write(hdr)
            f.write(data)
            # O_APPEND equivalent — flush buffer ทุก write
            f.flush()

        return seq

    # ── audit ─────────────────────────────────────────────────────────

    def audit(self) -> bool:
        """
        ตรวจ invariant:
          lane_X.seq  - lane_nX.seq == 0
          lane_Y.seq  - lane_nY.seq == 0
        """
        diff_x = self.lane_seq[LANE_X]  - self.lane_seq[LANE_NX]
        diff_y = self.lane_seq[LANE_Y]  - self.lane_seq[LANE_NY]
        if diff_x != 0:
            raise AuditError(f"X+(-X) = {diff_x} ≠ 0")
        if diff_y != 0:
            raise AuditError(f"Y+(-Y) = {diff_y} ≠ 0")
        return True

    # ── merkle ────────────────────────────────────────────────────────

    def _compute_merkle_root(self) -> bytes:
        """
        merkle root = SHA256(SHA256(lane_X) ‖ SHA256(lane_nX) ‖ ...)
        อ่านจาก .pending files
        """
        import hashlib
        outer = hashlib.sha256()
        for lane_id in range(LANE_COUNT):
            f = self.lane_files.get(lane_id)
            if f is None:
                inner = hashlib.sha256(b"").digest()
            else:
                f.flush()
                f.seek(0)
                inner_h = hashlib.sha256()
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    inner_h.update(chunk)
                inner = inner_h.digest()
                f.seek(0, 2)   # seek back to end
            outer.update(inner)
        return outer.digest()

    def _write_merkle_pending(self):
        root = self._compute_merkle_root()
        rec = struct.pack(
            ">II Q QQQQ 32s",
            DELTA_MAGIC, 0,
            self.epoch + 1,
            self.lane_seq[LANE_X],
            self.lane_seq[LANE_NX],
            self.lane_seq[LANE_Y],
            self.lane_seq[LANE_NY],
            root,
        )
        crc = _crc32(rec)
        rec += struct.pack(">I", crc)

        path = self.pogls_dir / FNAME_MERKLE_PEND
        path.write_bytes(rec)
        # fsync merkle.pending
        fd = os.open(str(path), os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def _read_merkle(self) -> Optional[dict]:
        path = self.pogls_dir / FNAME_MERKLE
        if not path.exists():
            return None
        raw = path.read_bytes()
        if len(raw) < MERKLE_SIZE:
            return None
        try:
            magic, pad, epoch, s0, s1, s2, s3, root, crc = \
                struct.unpack(MERKLE_FMT, raw[:MERKLE_SIZE])
        except struct.error:
            return None
        if magic != DELTA_MAGIC:
            return None
        # verify CRC
        if _crc32(raw[:MERKLE_SIZE - 4]) != crc:
            return None
        return {
            "epoch": epoch,
            "seq":   [s0, s1, s2, s3],
            "root":  root,
        }

    # ── commit ────────────────────────────────────────────────────────

    def commit(self) -> int:
        """
        Commit protocol (ลำดับห้ามสลับ):
          audit → merkle.pending → fsync lanes → rename lanes → rename merkle
        คืน epoch ใหม่
        """
        with self._lock:
            # Step 3  audit
            self.audit()

            # Step 5  merkle.pending
            self._write_merkle_pending()

            # Step 6  fsync ทุก lane
            for lane_id, f in self.lane_files.items():
                f.flush()
                os.fsync(f.fileno())

            # Step 8  rename .pending → .delta
            for lane_id in range(LANE_COUNT):
                src = self.pogls_dir / PENDING_NAMES[lane_id]
                dst = self.pogls_dir / DELTA_NAMES[lane_id]
                os.rename(src, dst)
                self.lane_files[lane_id].close()
                self.lane_files[lane_id] = None

            # Step 9  rename merkle.pending → snapshot.merkle  ← ATOMIC FINAL
            src = self.pogls_dir / FNAME_MERKLE_PEND
            dst = self.pogls_dir / FNAME_MERKLE
            os.rename(src, dst)

            # Step 10  update epoch
            self.epoch += 1

            # re-open .pending สำหรับ batch ถัดไป
            for lane_id, fname in PENDING_NAMES.items():
                path = self.pogls_dir / fname
                self.lane_files[lane_id] = open(path, "a+b")

        return self.epoch

    # ── close ─────────────────────────────────────────────────────────

    def close(self):
        for f in self.lane_files.values():
            if f:
                try:
                    f.close()
                except Exception:
                    pass
        self._open = False

    def __enter__(self):
        return self.open()

    def __exit__(self, *_):
        self.close()


# ══════════════════════════════════════════════════════════════════════
# BOOT RECOVERY SCANNER  (Python)
# ══════════════════════════════════════════════════════════════════════

def delta_recover(source_path: str) -> RecoveryResult:
    """
    Boot recovery scan สำหรับ 1 source file
    กฎ: snapshot.merkle = truth
        มี .pending ค้าง = TORN → discard → fallback
    """
    src       = Path(source_path).resolve()
    parent    = src.parent
    pogls_dir = parent / POGLS_DIR / src.name

    # กรณี C — ยังไม่เคย ingest
    if not pogls_dir.exists():
        return RecoveryResult.NEW

    # อ่าน merkle
    ctx_tmp = DeltaContext(source_path)
    rec = ctx_tmp._read_merkle()
    has_merkle = rec is not None

    # ตรวจ .pending
    has_pending = any(
        (pogls_dir / fname).exists() and
        (pogls_dir / fname).stat().st_size > 0
        for fname in PENDING_NAMES.values()
    )

    # กรณี A — CLEAN
    if has_merkle and not has_pending:
        all_delta = all(
            (pogls_dir / fname).exists()
            for fname in DELTA_NAMES.values()
        )
        if all_delta:
            return RecoveryResult.CLEAN

    # กรณี B — TORN → discard + fallback
    _discard_pending(pogls_dir)
    return RecoveryResult.TORN


def _discard_pending(pogls_dir: Path):
    """ลบ .pending + merkle.pending ทั้งหมด"""
    for fname in PENDING_NAMES.values():
        p = pogls_dir / fname
        if p.exists():
            p.unlink()
    p = pogls_dir / FNAME_MERKLE_PEND
    if p.exists():
        p.unlink()


def scan_vault(vault_path: str,
               callback: Optional[Callable] = None) -> dict:
    """
    scan vault directory ทั้งหมด
    คืน dict: {filepath: RecoveryResult}
    callback(filepath, result) ถ้ากำหนด
    """
    results = {}
    vault = Path(vault_path)

    for entry in sorted(vault.iterdir()):
        if entry.name.startswith("."):
            continue
        if entry.is_dir():
            continue
        r = delta_recover(str(entry))
        results[str(entry)] = r
        if callback:
            callback(str(entry), r)

    return results


# ══════════════════════════════════════════════════════════════════════
# DELTA FABRIC  — BlockFabric + Delta Lane ด้วยกัน
# ══════════════════════════════════════════════════════════════════════

class AuditError(Exception):
    pass


class DeltaFabric:
    """
    BlockFabric + Delta Lane crash recovery

    Usage:
        fabric = DeltaFabric("model.safetensors")

        # เขียน — ต้องเขียน X กับ -X คู่กันเสมอ
        fabric.write_pair(addr, data_x, data_nx, axis="X")
        fabric.write_pair(addr, data_y, data_ny, axis="Y")

        snap = fabric.commit()       # atomic crash-safe commit
        view = fabric.slice(0, 4<<20)  # zero-copy read ปกติ

    ถ้าอยากเขียน lane เดี่ยวๆ:
        fabric.write(data, lane=LANE_X, addr=addr)
        fabric.write(data, lane=LANE_NX, addr=addr)
    """

    def __init__(self, source_path: str,
                 auto_recover: bool = True,
                 fabric_max_size: int = 0):

        self.source_path = Path(source_path).resolve()
        self._delta      = DeltaContext(str(self.source_path))
        self._lock       = threading.Lock()

        # ── boot recovery ──────────────────────────────────────────
        if auto_recover:
            result = delta_recover(str(self.source_path))
            self._recovery = result
            if result == RecoveryResult.TORN:
                print(f"[DeltaFabric] ⚠  TORN recovery on {self.source_path.name}"
                      f" — .pending discarded, fallback N-1")
            elif result == RecoveryResult.CLEAN:
                print(f"[DeltaFabric] ✅ CLEAN  {self.source_path.name}"
                      f"  epoch={self._delta._read_merkle()['epoch']}")
            else:
                print(f"[DeltaFabric] 🆕 NEW    {self.source_path.name}")
        else:
            self._recovery = RecoveryResult.NEW

        # ── open delta context ─────────────────────────────────────
        self._delta.open()

        # ── open fabric (mmap) ─────────────────────────────────────
        fabric_path = self.source_path.parent / (self.source_path.name + ".fabric")
        self._fabric = BlockFabric(
            str(fabric_path),
            create=not fabric_path.exists(),
            max_size=fabric_max_size or (512 << 20),
        )

    # ── Write API ─────────────────────────────────────────────────────

    def write(self, data: bytes, lane: int, addr: int) -> int:
        """append delta block ลง lane เดียว"""
        return self._delta.append(lane, addr, data)

    def write_pair(self, addr: int,
                   data_pos: bytes, data_neg: bytes,
                   axis: str = "X") -> tuple:
        """
        เขียน positive + negative axis พร้อมกัน
        รับประกัน audit invariant
        axis: "X" หรือ "Y"
        """
        if axis.upper() == "X":
            s1 = self._delta.append(LANE_X,  addr, data_pos)
            s2 = self._delta.append(LANE_NX, addr, data_neg)
        elif axis.upper() == "Y":
            s1 = self._delta.append(LANE_Y,  addr, data_pos)
            s2 = self._delta.append(LANE_NY, addr, data_neg)
        else:
            raise ValueError(f"axis must be X or Y, got {axis}")

        # เขียนลง fabric ด้วย
        self._fabric.write(data_pos)
        self._fabric.write(data_neg)
        return s1, s2

    # ── Commit ────────────────────────────────────────────────────────

    def commit(self) -> SnapshotPointer:
        """
        atomic commit:
          1. delta audit
          2. delta commit (rename → merkle)
          3. fabric snapshot (merkle chain)
        คืน SnapshotPointer จาก fabric
        """
        with self._lock:
            self._delta.commit()
            snap = self._fabric.snapshot()
        return snap

    # ── Read (delegate to fabric) ─────────────────────────────────────

    def slice(self, offset: int, length: int):
        return self._fabric.slice(offset, length)

    @property
    def stats(self) -> dict:
        fs = self._fabric.stats
        return {
            **fs,
            "delta_epoch":    self._delta.epoch,
            "delta_lane_seq": dict(self._delta.lane_seq),
            "recovery":       self._recovery.name,
        }

    # ── Cleanup ───────────────────────────────────────────────────────

    def close(self):
        self._delta.close()
        if hasattr(self._fabric, "_mm") and self._fabric._mm:
            self._fabric._mm.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
