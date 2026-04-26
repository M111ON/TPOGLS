"""
POGLS S56 — Compress Benchmark
Validates geometry dedup vs gzip on real codebase.

Metrics:
  1. intra_dedup_%      = ref_chunks / total_chunks  (within-file dedup)
  2. cross_file_dedup_% = cross-file shared hashes / total_chunks
  3. phi_resonant_%     = phi-resonant records / total_chunks
     NOTE: phi_resonant lives on 'ref' records — ref IS the phi signal
  4. coord_overhead     = coord JSON size vs raw bytes
  5. gzip_ratio         = gzip(raw) / raw

Record types (from file_encode):
  literal  -> { type, idx, spoke, slot, world, hash, data, phi_resonant }
  ref      -> { type, idx, spoke, slot, world, ref_idx, phi_resonant }
             ref = dedup pointer; phi_resonant=True means gap is Fibonacci
"""

import os, gzip, json, math
from pathlib import Path

# ── CONFIG ─────────────────────────────────────────────────────────────
SCAN_ROOT   = "/tmp"
CHUNK_SIZE  = 32
EXTENSIONS  = [".py"]
RECURSIVE   = False
TOP_N_FILES = 10
# ───────────────────────────────────────────────────────────────────────

from pogls_dir_scanner import scan_dir
from pogls_file_coord   import file_encode


def gzip_size(data: bytes) -> int:
    return len(gzip.compress(data, compresslevel=9))


def spoke_entropy(dist: list) -> float:
    total = sum(dist)
    if not total:
        return 0.0
    return -sum((v/total) * math.log2(v/total) for v in dist if v > 0)


def benchmark_file(path: str) -> dict | None:
    try:
        raw = Path(path).read_bytes()
        if not raw:
            return None
        result   = file_encode(raw, chunk_size=CHUNK_SIZE)
        records  = result[0]
        literals = [r for r in records if r.get("type") == "literal"]
        refs     = [r for r in records if r.get("type") == "ref"]
        total    = len(records)
        intra_dedup_ratio = len(refs) / total if total else 0.0
        phi_count = sum(1 for r in records if r.get("phi_resonant", False))
        phi_ratio = phi_count / total if total else 0.0
        coord_json = json.dumps(records).encode()
        gz = gzip_size(raw)
        return {
            "path"             : path,
            "raw_bytes"        : len(raw),
            "gzip_bytes"       : gz,
            "coord_bytes"      : len(coord_json),
            "total_chunks"     : total,
            "literal_chunks"   : len(literals),
            "ref_chunks"       : len(refs),
            "intra_dedup_ratio": round(intra_dedup_ratio, 4),
            "phi_resonant_pct" : round(phi_ratio * 100, 2),
            "gzip_ratio"       : round(gz / len(raw), 4),
            "coord_vs_raw"     : round(len(coord_json) / len(raw), 4),
        }
    except Exception as e:
        return {"path": path, "error": str(e)}


def run_benchmark():
    print(f"\n{'='*64}")
    print(f"  POGLS S56 -- Compress Benchmark")
    print(f"  Root: {SCAN_ROOT}  Ext: {EXTENSIONS}  Chunk: {CHUNK_SIZE}B  Recursive: {RECURSIVE}")
    print(f"{'='*64}\n")

    dir_result = scan_dir(SCAN_ROOT, chunk_size=CHUNK_SIZE,
                          extensions=EXTENSIONS, recursive=RECURSIVE)
    ds         = dir_result["stats"]
    total_dir  = ds["total_chunks"]
    cross_refs = ds["cross_dedup_refs"]
    cross_keys = ds["cross_dedup_hashes"]
    sd         = ds["spoke_distribution"]
    wd         = ds["world_distribution"]
    s_ent      = spoke_entropy(sd)
    s_max      = math.log2(6)
    s_lbl      = "diverse" if s_ent > s_max * 0.7 else "structured"

    print("[ 1. Directory Geometry Scan ]")
    print(f"  files            : {ds['file_count']}")
    print(f"  total bytes      : {ds['total_bytes']:,}")
    print(f"  total chunks     : {total_dir:,}")
    print(f"  clusters         : {ds['cluster_count']}  (max 18 = 6 spokes x 3 worlds)")
    print(f"  cross-dedup refs : {cross_refs}  ({cross_refs/total_dir*100:.2f}% of chunks)")
    print(f"  cross-dedup keys : {cross_keys}")
    print(f"  spoke_dist       : {sd}  entropy={s_ent:.3f}/{s_max:.3f} -> {s_lbl}")
    print(f"  world_dist       : {wd}")

    print(f"\n[ 2. Per-file Benchmarks ]")
    hdr = (f"  {'file':<32} {'raw_B':>7} {'gz_B':>7} {'coord_B':>8} "
           f"{'chunks':>7} {'refs':>5} {'intra%':>7} {'phi%':>6} "
           f"{'gz_r':>6} {'c/raw':>6}")
    print(hdr)
    print("  " + "-"*90)

    files   = [os.path.join(SCAN_ROOT, str(p)) for p in dir_result["files"].keys()]
    results = []
    errors  = []
    for f in files:
        r = benchmark_file(f)
        if r is None:
            continue
        if "error" in r:
            errors.append(r)
        else:
            results.append(r)

    results.sort(key=lambda x: x["total_chunks"], reverse=True)
    totals = dict(raw=0, gz=0, coord=0, chunks=0, refs=0, phi_sum=0.0)

    for r in results[:TOP_N_FILES]:
        name = Path(r["path"]).name[:32]
        print(f"  {name:<32} {r['raw_bytes']:>7,} {r['gzip_bytes']:>7,} "
              f"{r['coord_bytes']:>8,} {r['total_chunks']:>7} "
              f"{r['ref_chunks']:>5} {r['intra_dedup_ratio']*100:>6.2f}% "
              f"{r['phi_resonant_pct']:>5.2f}% "
              f"{r['gzip_ratio']:>6.4f} {r['coord_vs_raw']:>6.4f}")
        totals["raw"]    += r["raw_bytes"]
        totals["gz"]     += r["gzip_bytes"]
        totals["coord"]  += r["coord_bytes"]
        totals["chunks"] += r["total_chunks"]
        totals["refs"]   += r["ref_chunks"]
        totals["phi_sum"]+= r["phi_resonant_pct"]

    n = len(results[:TOP_N_FILES])
    agg_intra = totals["refs"]  / totals["chunks"] * 100 if totals["chunks"] else 0
    agg_gz    = totals["gz"]    / totals["raw"]           if totals["raw"]    else 1
    agg_coord = totals["coord"] / totals["raw"]           if totals["raw"]    else 1
    phi_avg   = totals["phi_sum"] / max(n, 1)

    if n:
        print("  " + "-"*90)
        print(f"  {'TOTAL / AGG':<32} {totals['raw']:>7,} {totals['gz']:>7,} "
              f"{totals['coord']:>8,} {totals['chunks']:>7} "
              f"{totals['refs']:>5} {agg_intra:>6.2f}% "
              f"{phi_avg:>5.2f}% "
              f"{agg_gz:>6.4f} {agg_coord:>6.4f}")

    if errors:
        print(f"\n  errors ({len(errors)}): " + ", ".join(Path(e["path"]).name for e in errors))

    print(f"\n[ 3. Validation Gate ]")
    cross_pct   = cross_refs / total_dir * 100 if total_dir else 0
    phi_vs_ref  = phi_avg / agg_intra if agg_intra else 0

    v_cross = "PASS" if cross_pct  > 1.0 else "WEAK (<1%)"
    v_intra = "PASS" if agg_intra  > 5.0 else "LOW  (<5%)"
    v_phi   = "PASS" if phi_avg    > 1.0 else "SPARSE"
    v_gz    = "gzip wins" if agg_gz < agg_coord else "coord competitive"

    print(f"  cross-file dedup  : {cross_pct:6.2f}%   [{v_cross}]")
    print(f"  intra-file dedup  : {agg_intra:6.2f}%   [{v_intra}]")
    print(f"  phi_resonant avg  : {phi_avg:6.2f}%   [{v_phi}]")
    print(f"  phi / ref ratio   : {phi_vs_ref:.3f}  (1.0 = all refs are phi-resonant)")
    print(f"  gzip ratio        : {agg_gz:.4f}   [{v_gz}]")
    print(f"  coord/raw ratio   : {agg_coord:.4f}")

    if phi_vs_ref > 0.3:
        print(f"\n  * PHI insight: {phi_vs_ref*100:.1f}% of dedup refs occur at Fibonacci gaps")
        print(f"    -> geometry is structurally resonant with data patterns.")
    else:
        print(f"\n  o PHI signal weak on .py files (short, low repetition).")
        print(f"    -> Try larger files (weights, binaries) for stronger phi signal.")

    unlock = cross_pct > 1.0
    print(f"\n  -> {'PASS: unlock B/C/D' if unlock else 'HOLD: investigate signal'}")
    print(f"{'='*64}\n")


if __name__ == "__main__":
    run_benchmark()
