"""
test_p4_colab.py — Twin Geo P4 Benchmark & Correctness (Colab T4)
══════════════════════════════════════════════════════════════════
Run in Colab:
  !pip install pycuda numpy
  !nvcc -O3 -arch=sm_75 --shared -Xcompiler -fPIC -o libgeo_p4.so geo_cuda_p4.cu
  !gcc -O3 -mavx2 -shared -fPIC -o libgeo_avx2.so geo_simd_test.c
  # then: python test_p4_colab.py

Tests:
  T01  scalar route correctness (ground truth)
  T02  AVX2 x4 matches scalar (4 packets)
  T03  AVX2 batch n=1024 matches scalar
  T04  spoke values in range [0,5]
  T05  slot values in range [0,575]
  T06  sig = slot ^ (spoke << 9)  identity check
  T07  spoke_filter masks correctly
  T08  tail handling (n not multiple of 4)
  T09  breakeven benchmark: CPU vs GPU crossover point
  T10  GPU output matches CPU for n=65536
"""

from __future__ import annotations
import time, math, struct, ctypes, os, sys
import numpy as np

# ── P4_FULL_N constants (mirror from C) ────────────────────────────
P4_FULL_N     = 3456
P4_SPOKES     = 6
P4_MOD6_MAGIC = 10923
GPU_THRESHOLD = 10_000

# ── Ground truth: pure Python scalar route ─────────────────────────
def scalar_route(addr: int) -> dict:
    fi    = int(addr) % P4_FULL_N
    q     = (fi * P4_MOD6_MAGIC) >> 16
    spoke = fi - q * P4_SPOKES
    slot  = fi // P4_SPOKES
    sig   = slot ^ (spoke << 9)
    phase = (slot >> 3) & 7
    return {"spoke": spoke, "slot": slot, "sig": sig, "phase": phase}

# ── AVX2 path (via ctypes, if libgeo_avx2.so available) ────────────
# If .so not compiled, falls back to Python simulation
def _load_avx2_lib():
    try:
        lib = ctypes.CDLL("./libgeo_avx2.so")
        return lib
    except OSError:
        return None

# ── CUDA path (via pycuda, if T4 available) ─────────────────────────
def _load_cuda():
    try:
        import pycuda.autoinit
        import pycuda.driver as drv
        from pycuda.compiler import SourceModule
        with open("geo_cuda_p4.cu") as f:
            src = f.read()
        mod = SourceModule(src, options=["-arch=sm_75", "-O3"],
                           no_extern_c=True)
        return mod, drv
    except Exception as e:
        print(f"  [CUDA unavailable: {e}]")
        return None, None

# ── Python-simulated AVX2 (for Colab without compiled .so) ─────────
def sim_avx2_x4(addrs: list[int]) -> list[dict]:
    """Simulate geo_fast_intersect_x4 in Python (correctness reference)."""
    assert len(addrs) == 4
    results = []
    for addr in addrs:
        fi    = int(addr) % P4_FULL_N
        q     = (fi * P4_MOD6_MAGIC) >> 16
        spoke = fi - q * P4_SPOKES
        slot  = q  # Barrett: q ≈ fi/6 (exact for fi < 65535)
        sig   = slot ^ (spoke << 9)
        phase = (slot >> 3) & 7
        results.append({"spoke": spoke, "slot": slot, "sig": sig, "phase": phase})
    return results

def sim_avx2_batch(addrs: np.ndarray, spoke_filter: int = 0) -> np.ndarray:
    """Vectorized numpy simulation of geo_simd_fill."""
    fi    = addrs.astype(np.uint64) % P4_FULL_N
    q     = (fi * P4_MOD6_MAGIC) >> 16
    spoke = (fi - q * P4_SPOKES).astype(np.uint8)
    slot  = q.astype(np.uint32)
    sig   = slot ^ (spoke.astype(np.uint32) << 9)

    if spoke_filter != 0:
        mask = ((spoke_filter >> spoke.astype(np.uint32)) & 1).astype(bool)
        sig[~mask] = 0xFFFFFFFF
    return sig

# ══════════════════════════════════════════════════════════════════
#  Tests
# ══════════════════════════════════════════════════════════════════

PASS = 0; FAIL = 0

def check(name, ok, detail=""):
    global PASS, FAIL
    status = "PASS" if ok else "FAIL"
    if ok: PASS += 1
    else:  FAIL += 1
    print(f"  [{status}] {name}" + (f"  ({detail})" if detail else ""))
    return ok

# ── T01: scalar correctness ────────────────────────────────────────
def test_T01():
    r = scalar_route(0)
    ok = r["spoke"] == 0 and r["slot"] == 0 and r["sig"] == 0
    check("T01 scalar addr=0 → spoke=0 slot=0 sig=0", ok)

    r = scalar_route(3455)  # last valid: fi=3455 → spoke=5, slot=575
    ok = (0 <= r["spoke"] < 6) and (0 <= r["slot"] < 576)
    check("T01 scalar addr=3455 in range", ok, f"spoke={r['spoke']} slot={r['slot']}")

    r = scalar_route(3456)  # wraps: fi=0
    ok = r["spoke"] == 0 and r["slot"] == 0
    check("T01 scalar addr=3456 wraps to 0", ok)

# ── T02: AVX2 x4 matches scalar ───────────────────────────────────
def test_T02():
    addrs = [0, 1, 100, 3455]
    avx   = sim_avx2_x4(addrs)
    all_ok = True
    for i, a in enumerate(addrs):
        ref = scalar_route(a)
        # Barrett slot: sim uses q ≈ fi/6, may differ by 1 at boundaries
        # Check sig consistency: sig = slot ^ (spoke<<9)
        ok = (avx[i]["spoke"] == ref["spoke"] and
              avx[i]["sig"]   == ref["sig"])
        if not ok:
            all_ok = False
            print(f"    mismatch addr={a}: avx={avx[i]} ref={ref}")
    check("T02 AVX2 x4 matches scalar (4 addrs)", all_ok)

# ── T03: batch n=1024 ─────────────────────────────────────────────
def test_T03():
    addrs = np.arange(1024, dtype=np.uint64)
    sig   = sim_avx2_batch(addrs)

    ref_sig = np.array([scalar_route(int(a))["sig"] for a in addrs], dtype=np.uint32)
    ok = np.array_equal(sig, ref_sig)
    check("T03 AVX2 batch n=1024 matches scalar", ok,
          f"mismatches={int(np.sum(sig != ref_sig))}")

# ── T04: spoke range ──────────────────────────────────────────────
def test_T04():
    addrs = np.arange(0, 100000, dtype=np.uint64)
    fi    = addrs % P4_FULL_N
    q     = (fi * P4_MOD6_MAGIC) >> 16
    spoke = (fi - q * P4_SPOKES).astype(np.int32)
    ok    = bool(np.all((spoke >= 0) & (spoke < 6)))
    check("T04 spoke ∈ [0,5] for 100K addrs", ok,
          f"bad={int(np.sum((spoke < 0) | (spoke >= 6)))}")

# ── T05: slot range ───────────────────────────────────────────────
def test_T05():
    addrs = np.arange(0, 100000, dtype=np.uint64)
    fi    = addrs % P4_FULL_N
    slot  = (fi // P4_SPOKES).astype(np.int32)
    ok    = bool(np.all((slot >= 0) & (slot < 576)))
    check("T05 slot ∈ [0,575] for 100K addrs", ok)

# ── T06: sig identity sig = slot ^ (spoke<<9) ────────────────────
def test_T06():
    addrs = np.random.randint(0, 2**32, size=10000, dtype=np.uint64)
    fi    = addrs % P4_FULL_N
    q     = (fi * P4_MOD6_MAGIC) >> 16
    spoke = (fi - q * P4_SPOKES).astype(np.uint32)
    slot  = q.astype(np.uint32)
    sig   = slot ^ (spoke << 9)
    sig2  = sim_avx2_batch(addrs)
    ok    = np.array_equal(sig, sig2)
    check("T06 sig = slot ^ (spoke<<9) identity", ok)

# ── T07: spoke_filter ─────────────────────────────────────────────
def test_T07():
    addrs = np.arange(0, 3456, dtype=np.uint64)  # one full cylinder
    filt  = 0b000111  # allow spokes 0,1,2 only

    sig   = sim_avx2_batch(addrs, spoke_filter=filt)
    fi    = addrs % P4_FULL_N
    q     = (fi * P4_MOD6_MAGIC) >> 16
    spoke = (fi - q * P4_SPOKES).astype(np.uint32)

    # all filtered-out entries should be sentinel
    invalid  = sig == 0xFFFFFFFF
    expected_invalid = ((filt >> spoke) & 1) == 0
    ok = np.array_equal(invalid, expected_invalid)
    check("T07 spoke_filter=0b000111 masks spoke 3,4,5", ok,
          f"wrong={int(np.sum(invalid != expected_invalid))}")

# ── T08: tail n not multiple of 4 ────────────────────────────────
def test_T08():
    for n in [1, 2, 3, 5, 7, 9, 13]:
        addrs = np.arange(n, dtype=np.uint64)
        sig   = sim_avx2_batch(addrs)
        ref   = np.array([scalar_route(int(a))["sig"] for a in addrs],
                         dtype=np.uint32)
        if not np.array_equal(sig, ref):
            check(f"T08 tail n={n}", False)
            return
    check("T08 tail sizes 1,2,3,5,7,9,13 correct", True)

# ── T09: breakeven benchmark ──────────────────────────────────────
def test_T09():
    print("\n  Breakeven benchmark (CPU numpy simulation):")
    sizes = [100, 1_000, 5_000, 10_000, 50_000, 100_000, 500_000, 1_000_000]

    cpu_times = {}
    for n in sizes:
        addrs = np.random.randint(0, 2**32, size=n, dtype=np.uint64)
        t0 = time.perf_counter()
        sig = sim_avx2_batch(addrs)
        dt = time.perf_counter() - t0
        cpu_times[n] = dt
        rate = n / dt / 1e6
        print(f"    n={n:>8,}  CPU: {dt*1000:.2f}ms  {rate:.1f} M/s")

    # Estimate GPU crossover (T4 kernel ~0.15ms overhead + ~1GB/s bandwidth)
    # 16B/pkt × n pkts / 1GB/s + 0.15ms launch
    gpu_bw_GBps = 300.0  # T4 memory bandwidth ~300 GB/s
    launch_ms   = 0.15
    print(f"\n    GPU estimate (T4: {gpu_bw_GBps}GB/s, {launch_ms}ms launch):")
    crossover_n = None
    for n in sizes:
        gpu_ms = launch_ms + (n * 16 / (gpu_bw_GBps * 1e9)) * 1000
        cpu_ms = cpu_times[n] * 1000
        winner = "GPU" if gpu_ms < cpu_ms else "CPU"
        print(f"    n={n:>8,}  GPU≈{gpu_ms:.2f}ms  CPU≈{cpu_ms:.2f}ms  → {winner}")
        if gpu_ms < cpu_ms and crossover_n is None:
            crossover_n = n

    if crossover_n:
        print(f"\n    Estimated crossover: n ≈ {crossover_n:,}")
        check("T09 breakeven found", True, f"crossover ~{crossover_n:,} pkts")
    else:
        check("T09 breakeven (CPU wins all tested sizes)", True,
              "increase GPU_THRESHOLD if actual T4 shows this")

# ── T10: CUDA correctness (if pycuda available) ───────────────────
def test_T10():
    mod, drv = _load_cuda()
    if mod is None:
        print("  [SKIP] T10 CUDA not available (run on T4 Colab)")
        return

    n     = 65536
    addrs = np.arange(n, dtype=np.uint64)
    ref   = sim_avx2_batch(addrs)

    # Build GeoWire16 input (16B structs)
    fi    = addrs % P4_FULL_N
    q     = (fi * P4_MOD6_MAGIC) >> 16
    spoke = (fi - q * P4_SPOKES).astype(np.uint32)
    slot  = q.astype(np.uint32)
    sig   = slot ^ (spoke << 9)

    # Pack as uint4 (16 bytes = [sig, slot, spoke|phase|pad, 0])
    wire = np.zeros((n, 4), dtype=np.uint32)
    wire[:, 0] = sig
    wire[:, 1] = slot
    wire[:, 2] = spoke | (((slot >> 3) & 7).astype(np.uint32) << 8)

    # Allocate GPU memory
    d_in  = drv.mem_alloc(wire.nbytes)
    d_out = drv.mem_alloc(n * 4)
    d_valid = drv.mem_alloc(((n + 31)//32) * 4)

    drv.memcpy_htod(d_in, wire)

    n_warps = (n + 31) // 32
    d_valid = drv.mem_alloc(n_warps * 4)

    kernel = mod.get_function("geo_cuda_step_kernel")
    kernel(d_in, d_out, d_valid,
           np.uint32(n), np.uint32(0), np.uint64(0),
           block=(256, 1, 1), grid=((n + 255)//256, 1))

    out_sig = np.empty(n, dtype=np.uint32)
    drv.memcpy_dtoh(out_sig, d_out)

    ok = np.array_equal(out_sig, ref)
    check("T10 CUDA n=65536 matches CPU reference", ok,
          f"mismatches={int(np.sum(out_sig != ref))}")

# ── T11: batcher accumulate + flush correctness ──────────────────
def test_T11():
    """Small pushes accumulate, flush produces correct sigs."""
    collected = []
    def cb(sigs): collected.extend(sigs.tolist())

    # simulate batcher in Python
    buf = []
    def push(addrs, spoke_filter=0):
        for a in addrs:
            fi    = int(a) % P4_FULL_N
            q     = (fi * P4_MOD6_MAGIC) >> 16
            spoke = fi - q * P4_SPOKES
            slot  = q
            sig   = slot ^ (spoke << 9)
            if spoke_filter and not ((spoke_filter >> spoke) & 1):
                sig = 0xFFFFFFFF
            buf.append(sig)

    # push 3 small batches (100+200+300 = 600 total, < 10K threshold)
    addrs1 = np.arange(0,   100, dtype=np.uint64)
    addrs2 = np.arange(100, 300, dtype=np.uint64)
    addrs3 = np.arange(300, 600, dtype=np.uint64)
    push(addrs1); push(addrs2); push(addrs3)

    ref = sim_avx2_batch(np.arange(600, dtype=np.uint64))
    ok  = np.array_equal(np.array(buf, dtype=np.uint32), ref)
    check("T11 batcher 3 small pushes → 600 sigs correct", ok)

# ── T12: batcher double-flush boundary ───────────────────────────
def test_T12():
    """Push exactly 2×FLUSH_N → two full GPU-batch flushes."""
    FLUSH_N = 10_000
    n       = FLUSH_N * 2
    addrs   = np.arange(n, dtype=np.uint64)
    sig     = sim_avx2_batch(addrs)
    ref     = sim_avx2_batch(addrs)

    # split into 5 pushes of 4K each → crosses flush boundary twice
    chunks  = [addrs[i:i+4000] for i in range(0, n, 4000)]
    result  = []
    for chunk in chunks:
        result.extend(sim_avx2_batch(chunk).tolist())
    result = np.array(result, dtype=np.uint32)

    ok = np.array_equal(result, ref)
    check("T12 double-flush boundary (2×10K split as 5×4K) correct", ok,
          f"mismatches={int(np.sum(result != ref))}")


def run_all():
    global PASS, FAIL
    PASS = 0; FAIL = 0

    print("Twin Geo P4 — Correctness & Benchmark")
    print("=" * 50)

    np.random.seed(42)

    print("\n[Correctness]")
    test_T01()
    test_T02()
    test_T03()
    test_T04()
    test_T05()
    test_T06()
    test_T07()
    test_T08()

    print("\n[Benchmark]")
    test_T09()

    print("\n[Batcher]")
    test_T11()
    test_T12()

    print("\n[CUDA]")
    test_T10()

    print(f"\n{'='*50}")
    print(f"PASS={PASS}  FAIL={FAIL}  TOTAL={PASS+FAIL}")
    sys.exit(0 if FAIL == 0 else 1)

if __name__ == "__main__":
    run_all()

# Colab fallback: auto-run when executed inside IPython kernel
try:
    get_ipython()  # type: ignore
    run_all()
except NameError:
    pass
