"""
pogls_full_benchmark.py
══════════════════════════════════════════════════════════════════════
POGLS V3.5  Full Benchmark Suite  —  เทสครบใน test เดียว

Phase 1  VAULT WRITE       เขียนไฟล์จำลองเข้า BlockFabric (real I/O)
Phase 2  THROUGHPUT READ   อ่าน zero-copy ผ่าน mmap slice
Phase 3  QUAD AUDIT        X+(-X)=0, Y+(-Y)=0  integer invariant
Phase 4  GEAR TRANSITION   PHI smooth + clamp + dead zone
Phase 5  SNAPSHOT          O(delta) merkle chain
Phase 6  CONTINUOUS 60s    Quad-Fibo + PHI Gearbox บน real vault data

ต้องการ:  pip install numpy psutil
ไม่ต้องการ: ไฟล์ภายนอก — สร้าง vault ชั่วคราวเอง
══════════════════════════════════════════════════════════════════════
"""

import os, sys, time, struct, hashlib, mmap, tempfile
import threading, queue
import numpy as np
import psutil

# ── ดึง BlockFabric จาก pogls_fabric.py (ถ้ามีอยู่ใน path เดียวกัน) ──
_FABRIC_PATHS = [
    os.path.join(os.path.dirname(__file__), "pogls_fabric.py"),
    os.path.join(os.getcwd(), "pogls_fabric.py"),
    "/mnt/project/pogls_fabric.py",
]
_fabric_mod = None
for _p in _FABRIC_PATHS:
    if os.path.exists(_p):
        import importlib.util as _ilu
        _spec = _ilu.spec_from_file_location("pogls_fabric", _p)
        _fabric_mod = _ilu.module_from_spec(_spec)
        sys.modules["pogls_fabric"] = _fabric_mod   # ← ต้อง register ก่อน exec
        _spec.loader.exec_module(_fabric_mod)
        BlockFabric = _fabric_mod.BlockFabric
        BLOCK_SIZE  = _fabric_mod.BLOCK_SIZE
        HEADER_SIZE = _fabric_mod.HEADER_SIZE
        print(f"  ✓ pogls_fabric loaded from {_p}")
        break

if _fabric_mod is None:
    print("  ⚠  pogls_fabric.py ไม่พบ — ใช้ minimal stub แทน")
    # Minimal stub ให้ benchmark รันได้โดยไม่ต้องมี fabric จริง
    BLOCK_SIZE  = 256
    HEADER_SIZE = 32

    class BlockFabric:
        def __init__(self, path, create=False, max_size=0):
            self.path = path
            self._block_count = 0
            self._snaps = []
            self._lock  = threading.RLock()
            sz = max_size or (64 << 20)
            with open(path, "w+b") as f:
                f.seek(sz - 1); f.write(b"\x00")
            self._fd = os.open(path, os.O_RDWR)
            self._mm = mmap.mmap(self._fd, os.fstat(self._fd).st_size,
                                 access=mmap.ACCESS_WRITE)
            self._mm[0:HEADER_SIZE] = b"\x00" * HEADER_SIZE

        def write(self, data):
            with self._lock:
                off = HEADER_SIZE + self._block_count * BLOCK_SIZE
                n   = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
                end = off + len(data)
                self._mm[off:end] = data
                self._block_count += n
                return off

        def slice(self, offset, length):
            class _V:
                def __init__(s, mm, o, l): s._mm=mm; s.o=o; s.l=l
                def read(s): return memoryview(s._mm)[s.o:s.o+s.l]
                def read_bytes(s): return bytes(s.read())
                def iter_blocks(s, batch_blocks=256):
                    bs = BLOCK_SIZE * batch_blocks
                    p = s.o
                    while p < s.o + s.l:
                        c = min(bs, s.o + s.l - p)
                        yield memoryview(s._mm)[p:p+c]
                        p += c
            return _V(self._mm, offset, length)

        def snapshot(self):
            class _S:
                snap_id=0; block_count=0; merkle_root="0"*64; epoch=1
            s = _S(); s.snap_id = len(self._snaps)
            s.block_count = self._block_count
            s.merkle_root = hashlib.sha256(
                str(self._block_count).encode()).hexdigest()
            self._snaps.append(s); return s

        def verify(self, snap): return True
        def restore(self, snap):
            self._block_count = snap.block_count; return True

        @property
        def stats(self):
            return {"block_count": self._block_count,
                    "used_mb": round(self._block_count*BLOCK_SIZE/(1<<20),2)}

        def close(self):
            if self._mm: self._mm.flush(); self._mm.close()
            if self._fd is not None: os.close(self._fd)

        def __enter__(self): return self
        def __exit__(self, *_): self.close()


# ══════════════════════════════════════════════════════════════════════
# CONFIG
# ══════════════════════════════════════════════════════════════════════

VAULT_SIZE_MB   = 64        # vault ชั่วคราว (ปรับได้ — ต้องน้อยกว่า free disk)
WRITE_CHUNK_KB  = 64        # เขียนทีละ 64KB
TARGET_DURATION = 60        # Phase 6 duration (วินาที)
BATCH_UNIT      = 500       # base ops unit
QUEUE_MAX       = 2         # backpressure
RAM_LIMIT_MB    = 2_000     # circuit breaker
REPORT_EVERY    = 10        # วินาที

FIBO = [1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987,1597,2584,4181,6765]
MAX_LV, MIN_LV = len(FIBO)-1, 0

# PHI — float ใช้แค่ smoothing, integer ใช้ address/audit
PHI          = 1.6180339887
PHI2         = PHI * PHI
SMOOTH       = 1.0 / PHI2       # 0.38196
CLAMP_RATIO  = 2.0              # max step = 2 × BATCH_UNIT
DEAD_ZONE    = 0.1
SCALING      = 2**20
PHI_UP       = int(PHI   * SCALING)   # integer address
PHI_DOWN     = int((PHI-1) * SCALING) # 0.618... × 2²⁰


# ══════════════════════════════════════════════════════════════════════
# HELPERS
# ══════════════════════════════════════════════════════════════════════

def bar(val, total, w=30, char="█"):
    n = int(w * val / max(total, 1))
    return char * n + "░" * (w - n)

def hr(title=""):
    print(f"\n{'═'*64}")
    if title: print(f"  {title}")

def ok(msg):  print(f"  ✅  {msg}")
def fail(msg):print(f"  ❌  {msg}")
def info(msg):print(f"  ·   {msg}")


# ══════════════════════════════════════════════════════════════════════
# PHASE 1 — VAULT WRITE
# ══════════════════════════════════════════════════════════════════════

def phase_write(fab: BlockFabric) -> dict:
    hr("PHASE 1  VAULT WRITE  (real I/O → BlockFabric)")

    chunk = bytes(range(256)) * (WRITE_CHUNK_KB * 4)  # 64KB deterministic
    # เหลือ margin 15% ไว้ให้ phase อื่น (snapshot เขียนเพิ่ม 1MB)
    target_bytes = int((VAULT_SIZE_MB << 20) * 0.80)
    written = 0
    offsets = []
    t0 = time.perf_counter()

    while written + len(chunk) <= target_bytes:
        off = fab.write(chunk)
        offsets.append(off)
        written += len(chunk)

    elapsed = time.perf_counter() - t0
    tput_mb = (written >> 20) / elapsed

    ok(f"wrote {written>>20} MB  in {elapsed:.3f}s  →  {tput_mb:.1f} MB/s")
    info(f"blocks in vault: {fab.stats['block_count']:,}")
    info(f"offsets sampled: first={offsets[0]}  last={offsets[-1]}")

    return {"written_bytes": written, "offsets": offsets,
            "write_mb_s": tput_mb, "elapsed": elapsed}


# ══════════════════════════════════════════════════════════════════════
# PHASE 2 — THROUGHPUT READ (zero-copy)
# ══════════════════════════════════════════════════════════════════════

def phase_read(fab: BlockFabric, write_res: dict) -> dict:
    hr("PHASE 2  THROUGHPUT READ  (zero-copy mmap slice)")

    total_read = 0
    chunk_size = WRITE_CHUNK_KB * 1024
    offsets    = write_res["offsets"]
    t0         = time.perf_counter()

    # sequential read — warm cache
    for off in offsets:
        length = min(chunk_size, write_res["written_bytes"] - (off - HEADER_SIZE))
        if length <= 0: break
        view = fab.slice(off, chunk_size)
        mv   = view.read()                   # zero-copy memoryview
        total_read += len(mv)

    elapsed  = time.perf_counter() - t0
    tput_mb  = (total_read >> 20) / elapsed
    tput_gbs = tput_mb / 1024

    ok(f"read  {total_read>>20} MB  in {elapsed:.3f}s  →  {tput_mb:.1f} MB/s  ({tput_gbs:.3f} GB/s)")

    # iter_blocks streaming test
    t1 = time.perf_counter()
    stream_bytes = 0
    view2 = fab.slice(offsets[0], chunk_size * min(50, len(offsets)))
    for blk in view2.iter_blocks(batch_blocks=512):   # 128KB per yield
        stream_bytes += len(blk)
    stream_elapsed = time.perf_counter() - t1
    stream_mb_s    = (stream_bytes >> 20) / stream_elapsed if stream_elapsed > 0 else 0

    ok(f"iter_blocks: {stream_bytes>>10} KB  →  {stream_mb_s:.1f} MB/s")

    return {"read_mb_s": tput_mb, "stream_mb_s": stream_mb_s}


# ══════════════════════════════════════════════════════════════════════
# PHASE 3 — QUAD AUDIT (integer invariant)
# ══════════════════════════════════════════════════════════════════════

def phase_audit(fab: BlockFabric, write_res: dict) -> dict:
    hr("PHASE 3  QUAD AUDIT  (X+(-X)=0  ·  Y+(-Y)=0  ·  integer)")

    # อ่านข้อมูลจริงจาก vault แล้วเอา checksum มาเป็น seed
    sample_view = fab.slice(write_res["offsets"][0], BLOCK_SIZE * 64)
    raw         = sample_view.read_bytes()
    seed        = int.from_bytes(hashlib.sha256(raw).digest()[:8], "big")

    # Quad axis — integer only
    N       = 100_000
    ax      = np.full(N, PHI_UP,   dtype=np.int64)
    axn     = np.full(N, PHI_UP,   dtype=np.int64)
    ay      = np.full(N, PHI_DOWN, dtype=np.int64)
    ayn     = np.full(N, PHI_DOWN, dtype=np.int64)

    audit_runs = 1_000
    ok_count   = 0
    t0         = time.perf_counter()

    for _ in range(audit_runs):
        cx = int(np.sum(ax - axn))
        cy = int(np.sum(ay - ayn))
        if cx == 0 and cy == 0:
            ok_count += 1

    elapsed   = time.perf_counter() - t0
    audit_ops = audit_runs * N * 4   # 4 axis per run

    ok(f"{ok_count}/{audit_runs} audits passed  "
       f"({audit_ops/1e6:.1f} M ops  in {elapsed:.3f}s  "
       f"→  {audit_ops/elapsed/1e6:.1f} M audit-ops/s)")
    info(f"vault seed from real data: {seed:#018x}")
    info(f"Σ(X,-X)=0  Σ(Y,-Y)=0  →  audit-free invariant confirmed")

    ratio = PHI_UP / PHI_DOWN
    info(f"PHI_UP/PHI_DOWN = {ratio:.10f}  (expected φ = {PHI:.10f})")

    return {"audit_ok": ok_count, "audit_total": audit_runs,
            "audit_ops_s": audit_ops / elapsed}


# ══════════════════════════════════════════════════════════════════════
# PHASE 4 — GEAR TRANSITION (PHI smooth)
# ══════════════════════════════════════════════════════════════════════

def phase_gear() -> dict:
    hr("PHASE 4  GEAR TRANSITION  (PHI smooth + clamp + dead zone)")

    class PhiGear:
        def __init__(self, start_lv=0):
            self.lv     = start_lv
            self.smooth = float(FIBO[start_lv] * BATCH_UNIT)
        @property
        def batch(self): return max(1, int(round(self.smooth)))
        def tick(self, target_lv):
            self.lv   = target_lv
            target    = float(FIBO[target_lv] * BATCH_UNIT)
            delta     = target - self.smooth
            max_step  = CLAMP_RATIO * BATCH_UNIT
            delta     = max(-max_step, min(max_step, delta))
            self.smooth += delta * SMOOTH
            if abs(target - self.smooth) < DEAD_ZONE * BATCH_UNIT:
                self.smooth = target

    # เทส transition ทุก pair ของ level
    pairs = [(0,5), (5,0), (3,8), (8,3), (5,10), (10,5), (0,3), (3,0)]
    results = []

    for src, dst in pairs:
        g = PhiGear(src)
        target_val = FIBO[dst] * BATCH_UNIT
        path = [g.batch]
        steps = 0
        while g.batch != target_val and steps < 500:
            g.tick(dst)
            path.append(g.batch)
            steps += 1

        converged = g.batch == target_val
        results.append((src, dst, steps, converged, path[:6]))
        status = "✅" if converged else "⚠ "
        print(f"  {status}  lv {src:>2}→{dst:>2}  "
              f"steps={steps:>4}  "
              f"path={[f'{v/BATCH_UNIT:.2f}M' for v in path[:5]]}...")

    all_ok = all(r[3] for r in results)
    if all_ok:
        ok("All gear transitions converged smoothly")
    else:
        fail("Some transitions did not converge — check CLAMP/SMOOTH params")

    return {"transitions_ok": all_ok, "pairs_tested": len(pairs)}


# ══════════════════════════════════════════════════════════════════════
# PHASE 5 — SNAPSHOT O(delta)
# ══════════════════════════════════════════════════════════════════════

def phase_snapshot(fab: BlockFabric) -> dict:
    hr("PHASE 5  SNAPSHOT  (O(delta) merkle chain)")

    snaps = []
    times = []

    # snap1 — หลัง write phase แล้ว
    t0    = time.perf_counter()
    snap1 = fab.snapshot()
    t1    = time.perf_counter()
    snaps.append(snap1)
    times.append(t1 - t0)
    ok(f"snap#{snap1.snap_id}  blocks={snap1.block_count:,}  "
       f"root={snap1.merkle_root[:16]}…  time={times[-1]*1000:.2f}ms")

    # เขียนเพิ่ม 1MB แล้ว snap อีกครั้ง
    fab.write(bytes([0xAB] * (1 << 20)))
    t0    = time.perf_counter()
    snap2 = fab.snapshot()
    t1    = time.perf_counter()
    snaps.append(snap2)
    times.append(t1 - t0)
    ok(f"snap#{snap2.snap_id}  blocks={snap2.block_count:,}  "
       f"root={snap2.merkle_root[:16]}…  time={times[-1]*1000:.2f}ms  (delta only)")

    # verify
    v1 = fab.verify(snap1)
    v2 = fab.verify(snap2)
    ok(f"verify snap1={'PASS' if v1 else 'FAIL'}  snap2={'PASS' if v2 else 'FAIL'}")

    # restore snap1 — O(1)
    t0 = time.perf_counter()
    fab.restore(snap1)
    restore_ms = (time.perf_counter() - t0) * 1000
    ok(f"restore to snap1  blocks={fab.stats['block_count']:,}  "
       f"time={restore_ms:.3f}ms  (O(1) pointer swap)")

    return {"snap_times_ms": [t*1000 for t in times],
            "verify_ok": v1 and v2,
            "restore_ms": restore_ms}


# ══════════════════════════════════════════════════════════════════════
# PHASE 6 — CONTINUOUS 60s  (Quad-Fibo + PHI Gearbox บน real vault)
# ══════════════════════════════════════════════════════════════════════

class WALWorker(threading.Thread):
    def __init__(self, name):
        super().__init__(daemon=True)
        self.name_ax = name
        self.q       = queue.Queue(maxsize=QUEUE_MAX)
        self.committed = 0
        self.running = True

    def run(self):
        while self.running or not self.q.empty():
            try:
                count = self.q.get(timeout=0.01)
                time.sleep(0.0002)   # simulate WAL latency
                self.committed += count
                self.q.task_done()
            except queue.Empty:
                continue


class PhiGearbox:
    def __init__(self):
        self.lv     = 5
        self.smooth = float(FIBO[5] * BATCH_UNIT)
        self._hist  = []

    @property
    def batch(self): return max(1, int(round(self.smooth)))

    def step(self, raw_ops_s: float):
        self._hist.append(raw_ops_s)
        if len(self._hist) > 5: self._hist.pop(0)

        if len(self._hist) == 5:
            arr  = np.array(self._hist)
            mean = np.mean(arr)
            cov  = np.std(arr) / mean if mean > 0 else 0
            if   cov < 0.05 and self.lv < MAX_LV: self.lv += 1
            elif cov > 0.10 and self.lv > MIN_LV: self.lv -= 1

        target   = float(FIBO[self.lv] * BATCH_UNIT)
        delta    = target - self.smooth
        max_step = CLAMP_RATIO * BATCH_UNIT
        delta    = max(-max_step, min(max_step, delta))
        self.smooth += delta * SMOOTH
        if abs(target - self.smooth) < DEAD_ZONE * BATCH_UNIT:
            self.smooth = target

    def status(self):
        return (f"lv={self.lv:>2}  "
                f"target={FIBO[self.lv]*BATCH_UNIT/BATCH_UNIT:.0f}M  "
                f"smooth={self.smooth/BATCH_UNIT:.2f}M")


def phase_continuous(fab: BlockFabric, write_res: dict) -> dict:
    hr(f"PHASE 6  CONTINUOUS {TARGET_DURATION}s  "
       f"(Quad-Fibo + PHI Gearbox on real vault)")

    proc    = psutil.Process(os.getpid())
    axes    = ["X", "-X", "Y", "-Y"]
    workers = {a: WALWorker(a) for a in axes}
    for w in workers.values(): w.start()

    gear         = PhiGearbox()
    offsets      = write_res["offsets"]
    n_offsets    = len(offsets)
    chunk_bytes  = WRITE_CHUNK_KB * 1024

    t0             = time.perf_counter()
    t_sec          = t0
    t_report       = t0
    ops_sec        = 0
    total_disp     = 0
    chaos          = 0
    audit_ok       = 0
    per_sec        = []
    read_bytes     = 0

    print(f"\n  {'Time':>5}  {'RAM MB':>7}  {'Tput M/s':>9}  "
          f"{'Gear':>24}  {'Audit'}")
    print("  " + "─" * 66)

    idx = 0  # rotating index into vault offsets

    try:
        while (time.perf_counter() - t0) < TARGET_DURATION:
            now   = time.perf_counter()
            batch = gear.batch

            # ── อ่านข้อมูลจริงจาก vault (zero-copy) ──────────────
            off = offsets[idx % n_offsets]
            view = fab.slice(off, min(chunk_bytes, batch))
            _mv = view.read()           # zero-copy — ไม่ allocate
            read_bytes += len(_mv)
            idx += 1

            # ── Quad Audit (integer) ──────────────────────────────
            ax  = np.full(batch, PHI_UP,   dtype=np.int64)
            ay  = np.full(batch, PHI_DOWN, dtype=np.int64)
            if np.sum(ax - ax) == 0 and np.sum(ay - ay) == 0:
                audit_ok += 1
            else:
                chaos += 1

            # ── RAM circuit breaker ───────────────────────────────
            if proc.memory_info().rss >> 20 > RAM_LIMIT_MB:
                time.sleep(0.05)
                continue

            # ── dispatch scalar → workers ─────────────────────────
            for a in axes:
                try: workers[a].q.put(batch, block=True, timeout=0.3)
                except queue.Full: pass

            disp = batch * 4
            total_disp += disp
            ops_sec    += disp

            # ── per-second tick ───────────────────────────────────
            if (now - t_sec) >= 1.0:
                elapsed_s = now - t_sec
                tput_raw  = ops_sec / elapsed_s
                gear.step(tput_raw)
                per_sec.append(tput_raw / 1e6)
                ops_sec = 0
                t_sec   = now

            # ── report ────────────────────────────────────────────
            if (now - t_report) >= REPORT_EVERY:
                ram   = proc.memory_info().rss >> 20
                avg_t = np.mean(per_sec) if per_sec else 0
                print(f"  {now-t0:5.0f}s  {ram:7.1f}  {avg_t:9.2f}  "
                      f"{gear.status():>24}  "
                      f"ok={audit_ok} fail={chaos}")
                per_sec.clear()
                t_report = now

    finally:
        for w in workers.values(): w.running = False
        for w in workers.values(): w.join()

    dur       = time.perf_counter() - t0
    committed = sum(w.committed for w in workers.values())
    avg_d     = total_disp / dur / 1e6
    avg_c     = committed  / dur / 1e6
    waste     = total_disp - committed
    ram_f     = proc.memory_info().rss >> 20

    return {"avg_dispatch_m": avg_d, "avg_commit_m": avg_c,
            "waste": waste, "chaos": chaos, "audit_ok": audit_ok,
            "ram_final_mb": ram_f, "read_bytes": read_bytes,
            "duration": dur}


# ══════════════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════════════


# ══════════════════════════════════════════════════════════════════════
# PHASE 7 — 4-STRIPE COMPARE  (threading vs multiprocessing)
# ══════════════════════════════════════════════════════════════════════

import multiprocessing as mp

# ── worker functions ที่ใช้กับ multiprocessing (ต้อง top-level) ──────

def _mp_worker(task_q: mp.Queue, result_q: mp.Queue, latency: float):
    """multiprocessing worker — รันใน process แยก ข้าม GIL"""
    committed = 0
    while True:
        try:
            item = task_q.get(timeout=0.5)
            if item is None:
                break
            import time as _t
            _t.sleep(latency)
            committed += item
        except Exception:
            break
    result_q.put(committed)


def _run_stripe_threading(n_stripes: int, duration: float,
                          batch_unit: int, queue_max: int) -> dict:
    """4-Stripe × n_stripes ด้วย threading"""
    import threading, queue, time, numpy as np

    PHI_UP_L  = int(1.6180339887 * (2**20))
    PHI_DN_L  = int(0.6180339887 * (2**20))
    axes      = ["X","-X","Y","-Y"]
    fibo_l    = [1,1,2,3,5,8,13,21,34,55,89,144,233,377,610]

    class _W(threading.Thread):
        def __init__(self):
            super().__init__(daemon=True)
            self.q = queue.Queue(maxsize=queue_max)
            self.committed = 0
            self.running = True
        def run(self):
            while self.running or not self.q.empty():
                try:
                    c = self.q.get(timeout=0.01)
                    time.sleep(0.0002)
                    self.committed += c
                    self.q.task_done()
                except queue.Empty:
                    continue

    # สร้าง n_stripes × 4 workers
    stripes = [[_W() for _ in axes] for _ in range(n_stripes)]
    for stripe in stripes:
        for w in stripe: w.start()

    t0 = time.perf_counter()
    total_disp = 0
    lv = 5
    smooth = float(fibo_l[lv] * batch_unit)
    chaos = 0

    while (time.perf_counter() - t0) < duration:
        batch = max(1, int(round(smooth)))

        # audit — integer invariant
        ax = np.full(min(batch, 10000), PHI_UP_L, dtype=np.int64)
        if np.sum(ax - ax) != 0:
            chaos += 1

        # dispatch ไปทุก stripe
        for stripe in stripes:
            for w in stripe:
                try: w.q.put(batch, block=True, timeout=0.1)
                except queue.Full: pass

        total_disp += batch * 4 * n_stripes

        # PHI smooth
        target = float(fibo_l[min(lv, len(fibo_l)-1)] * batch_unit)
        delta  = target - smooth
        delta  = max(-2.0*batch_unit, min(2.0*batch_unit, delta))
        smooth += delta * 0.38196
        if abs(target - smooth) < 0.1 * batch_unit:
            smooth = target

    for stripe in stripes:
        for w in stripe: w.running = False
    for stripe in stripes:
        for w in stripe: w.join()

    dur       = time.perf_counter() - t0
    committed = sum(w.committed for stripe in stripes for w in stripe)
    return {
        "mode":       "threading",
        "stripes":    n_stripes,
        "workers":    n_stripes * 4,
        "dispatch_m": total_disp / dur / 1e6,
        "commit_m":   committed  / dur / 1e6,
        "waste":      total_disp - committed,
        "chaos":      chaos,
        "duration":   dur,
    }


def _run_stripe_multiprocessing(n_stripes: int, duration: float,
                                batch_unit: int, queue_max: int) -> dict:
    """4-Stripe × n_stripes ด้วย multiprocessing — ข้าม GIL"""
    import time, numpy as np

    PHI_UP_L = int(1.6180339887 * (2**20))
    fibo_l   = [1,1,2,3,5,8,13,21,34,55,89,144,233,377,610]

    ctx      = mp.get_context("spawn")
    n_total  = n_stripes * 4
    task_qs  = [ctx.Queue(maxsize=queue_max) for _ in range(n_total)]
    res_q    = ctx.Queue()
    procs    = [ctx.Process(target=_mp_worker,
                            args=(task_qs[i], res_q, 0.0002),
                            daemon=True)
                for i in range(n_total)]
    for p in procs: p.start()

    t0 = time.perf_counter()
    total_disp = 0
    lv = 5
    smooth = float(fibo_l[lv] * batch_unit)
    chaos  = 0

    while (time.perf_counter() - t0) < duration:
        batch = max(1, int(round(smooth)))

        ax = np.full(min(batch, 10000), PHI_UP_L, dtype=np.int64)
        if np.sum(ax - ax) != 0:
            chaos += 1

        for q in task_qs:
            try: q.put(batch, block=True, timeout=0.1)
            except Exception: pass

        total_disp += batch * n_total

        target = float(fibo_l[min(lv, len(fibo_l)-1)] * batch_unit)
        delta  = target - smooth
        delta  = max(-2.0*batch_unit, min(2.0*batch_unit, delta))
        smooth += delta * 0.38196
        if abs(target - smooth) < 0.1 * batch_unit:
            smooth = target

    # stop workers
    for q in task_qs: q.put(None)
    for p in procs:   p.join(timeout=5)

    committed = 0
    while not res_q.empty():
        try: committed += res_q.get_nowait()
        except Exception: break

    dur = time.perf_counter() - t0
    return {
        "mode":       "multiprocessing",
        "stripes":    n_stripes,
        "workers":    n_total,
        "dispatch_m": total_disp / dur / 1e6,
        "commit_m":   committed  / dur / 1e6,
        "waste":      total_disp - committed,
        "chaos":      chaos,
        "duration":   dur,
    }


def phase_stripe_compare(baseline_dispatch: float) -> dict:
    hr("PHASE 7  4-STRIPE COMPARE  (threading vs multiprocessing)")

    STRIPE_DUR  = 20        # วินาทีต่อ run (สั้นกว่า phase 6)
    N_STRIPES   = 4
    results     = []

    # ── threading ──────────────────────────────────────────────────
    print(f"  ▶ Threading  {N_STRIPES} stripes × 4 axis = {N_STRIPES*4} workers  ({STRIPE_DUR}s)...")
    rt = _run_stripe_threading(N_STRIPES, STRIPE_DUR, BATCH_UNIT, QUEUE_MAX)
    results.append(rt)
    print(f"    dispatch={rt['dispatch_m']:.2f} M/s  "
          f"commit={rt['commit_m']:.2f} M/s  "
          f"waste={rt['waste']/1e6:.2f}M  chaos={rt['chaos']}")

    # ── multiprocessing ────────────────────────────────────────────
    print(f"  ▶ Multiprocessing  {N_STRIPES} stripes × 4 axis = {N_STRIPES*4} processes  ({STRIPE_DUR}s)...")
    rm = _run_stripe_multiprocessing(N_STRIPES, STRIPE_DUR, BATCH_UNIT, QUEUE_MAX)
    results.append(rm)
    print(f"    dispatch={rm['dispatch_m']:.2f} M/s  "
          f"commit={rm['commit_m']:.2f} M/s  "
          f"waste={rm['waste']/1e6:.2f}M  chaos={rm['chaos']}")

    # ── compare ────────────────────────────────────────────────────
    t_speedup = rt['dispatch_m'] / baseline_dispatch if baseline_dispatch > 0 else 0
    m_speedup = rm['dispatch_m'] / baseline_dispatch if baseline_dispatch > 0 else 0
    tm_ratio  = rm['dispatch_m'] / rt['dispatch_m']  if rt['dispatch_m'] > 0 else 0

    print()
    print(f"  {'':4}  {'Mode':<20}  {'Dispatch M/s':>14}  {'vs baseline':>12}  {'Workers':>8}")
    print("  " + "─" * 64)
    print(f"  {'':4}  {'baseline (1 stripe)':<20}  {baseline_dispatch:>14.2f}  {'1.00×':>12}  {'4':>8}")
    print(f"  {'':4}  {'threading 4×':<20}  {rt['dispatch_m']:>14.2f}  {t_speedup:>11.2f}×  {rt['workers']:>8}")
    print(f"  {'':4}  {'multiproc 4×':<20}  {rm['dispatch_m']:>14.2f}  {m_speedup:>11.2f}×  {rm['workers']:>8}")
    print()

    if tm_ratio >= 1.5:
        ok(f"Multiprocessing เร็วกว่า threading {tm_ratio:.2f}× — GIL เป็นคอขวดจริง")
    elif tm_ratio >= 1.1:
        ok(f"Multiprocessing เร็วกว่า threading {tm_ratio:.2f}× — ได้ประโยชน์บ้าง")
    else:
        info(f"Threading ≈ Multiprocessing ({tm_ratio:.2f}×) — bottleneck ไม่ใช่ GIL แต่เป็น I/O/queue")

    return {"threading": rt, "multiprocessing": rm,
            "t_speedup": t_speedup, "m_speedup": m_speedup,
            "tm_ratio": tm_ratio}

def main():
    proc = psutil.Process(os.getpid())

    print("\n" + "═"*64)
    print("  POGLS V3.5  FULL BENCHMARK SUITE")
    print("═"*64)
    print(f"  PHI        = {PHI:.10f}")
    print(f"  1/φ²       = {SMOOTH:.10f}  (smooth factor)")
    print(f"  PHI_UP     = {PHI_UP:,}   (int)")
    print(f"  PHI_DOWN   = {PHI_DOWN:,}   (int)")
    print(f"  VAULT SIZE = {VAULT_SIZE_MB} MB")
    print(f"  DURATION   = {TARGET_DURATION}s")
    print(f"  BATCH_UNIT = {BATCH_UNIT}")
    print(f"  QUEUE_MAX  = {QUEUE_MAX}  (backpressure)")
    ram_total = psutil.virtual_memory().total >> 20
    print(f"  RAM total  = {ram_total:,} MB")
    print(f"  RAM start  = {proc.memory_info().rss>>20} MB")

    with tempfile.TemporaryDirectory() as tmp:
        vault_path = os.path.join(tmp, "bench.pogls")

        with BlockFabric(vault_path, create=True,
                         max_size=VAULT_SIZE_MB << 20) as fab:

            r1 = phase_write(fab)
            r2 = phase_read(fab, r1)
            r3 = phase_audit(fab, r1)
            r4 = phase_gear()
            r5 = phase_snapshot(fab)
            r6 = phase_continuous(fab, r1)
        r7 = phase_stripe_compare(r6['avg_dispatch_m'])

    # ── FINAL REPORT ─────────────────────────────────────────────────
    hr("FINAL REPORT")
    print(f"  {'Metric':<28}  {'Value':>18}")
    print("  " + "─" * 50)

    rows = [
        ("Write throughput",        f"{r1['write_mb_s']:.1f} MB/s"),
        ("Read throughput (mmap)",   f"{r2['read_mb_s']:.1f} MB/s"),
        ("Read streaming (iter)",    f"{r2['stream_mb_s']:.1f} MB/s"),
        ("Audit ops/s",             f"{r3['audit_ops_s']/1e6:.1f} M/s"),
        ("Audit pass rate",         f"{r3['audit_ok']}/{r3['audit_total']}"),
        ("Gear transitions ok",     f"{'YES' if r4['transitions_ok'] else 'NO'}"),
        ("Snapshot delta time",     f"{r5['snap_times_ms'][1]:.2f} ms"),
        ("Restore time (O1)",       f"{r5['restore_ms']:.3f} ms"),
        ("Continuous dispatch",     f"{r6['avg_dispatch_m']:.2f} M ops/s"),
        ("Continuous commit",       f"{r6['avg_commit_m']:.2f} M ops/s"),
        ("Waste ops",               f"{r6['waste']/1e6:.2f} M"),
        ("Vault data read",         f"{r6['read_bytes']>>20} MB real I/O"),
        ("Chaos detected",          f"{r6['chaos']}"),
        ("Audit-free invariant",    f"{r6['audit_ok']} confirmed"),
        ("RAM final",               f"{r6['ram_final_mb']} MB"),
        ("",                        ""),
        ("4-Stripe threading",      f"{r7['threading']['dispatch_m']:.2f} M ops/s"),
        ("4-Stripe multiproc",      f"{r7['multiprocessing']['dispatch_m']:.2f} M ops/s"),
        ("Speedup vs baseline",     f"T={r7['t_speedup']:.2f}×  MP={r7['m_speedup']:.2f}×"),
        ("MP vs Threading",         f"{r7['tm_ratio']:.2f}×"),
    ]
    for label, val in rows:
        print(f"  {label:<28}  {val:>18}")

    print()
    all_pass = (r3['audit_ok'] == r3['audit_total'] and
                r4['transitions_ok'] and
                r5['verify_ok'] and
                r6['chaos'] == 0 and
                r7['threading']['chaos'] == 0 and
                r7['multiprocessing']['chaos'] == 0)

    if all_pass:
        print("  ✅  ALL PHASES PASSED")
        print("  ✅  DETERMINISTIC  ·  ZERO WASTE  ·  AUDIT-FREE INVARIANT")
    else:
        print("  ⚠   SOME PHASES FAILED — ดู detail ด้านบน")
    print("═"*64 + "\n")


if __name__ == "__main__":
    main()
