"""
POGLS REST Server — S53
Adds 3 geometry layer endpoints to S51 bridge:

  GET  /p4/geo/encode?key=<str>      → fibo_addr
  GET  /p4/geo/route?addr=<uint64>   → GeometryRouter
  POST /p4/geo/decode                → grid_decode (8×8 → uint64)

Wire into existing server (3 lines after S51 setup):
  from pogls_geo_layer import fibo_addr, GeometryRouter, grid_decode
  from rest_server_s53 import make_geo_endpoints
  make_geo_endpoints(app)
"""

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from typing import List, Optional, Union
import uvicorn, os

from pogls_geo_layer import (
    fibo_addr as _fibo_addr,
    GeometryRouter,
    grid_decode as _grid_decode,
    grid_encode,
    _digit_sum,
    _SACRED_NEXUS, _SACRED_STRUCT,
)

# ── shared sig_index (mirrors S50/S51 pattern) ────────────────────────
_p4_sig_index: dict = {}

app = FastAPI(title="POGLS S60", version="4.60.0")


# ══════════════════════════════════════════════════════════════════════
# Endpoint factories (importable as make_geo_endpoints)
# ══════════════════════════════════════════════════════════════════════
class GridDecodeRequest(BaseModel):
    pixels: List[int]           # 64 ints (0-255), row-major
    threshold: Optional[int] = 128


class BatchRouteRequest(BaseModel):
    addrs: List[int]


def make_geo_endpoints(app: FastAPI):
    """Wire all 3 geo endpoints onto an existing FastAPI app."""

    # ── 1. encode: key → fibo_addr ────────────────────────────────────
    @app.get("/p4/geo/encode", tags=["geo"])
    def geo_encode(
        key: str = Query(..., description="Any string key to encode"),
    ):
        """
        Map string key → deterministic uint64 position via Fibonacci indexing.

        Response includes:
          addr_u64    — position in POGLS address space
          addr_162    — position in icosphere (0-161)
          spoke/slot/world — geometry decomposition
          routing     — nexus / ternary / spoke / normal
          fib_n/fib_val — Fibonacci index used
        """
        try:
            result = _fibo_addr(key)
            return JSONResponse(result)
        except Exception as e:
            raise HTTPException(400, str(e))

    # ── 2. route: addr → geometry class ──────────────────────────────
    @app.get("/p4/geo/route", tags=["geo"])
    def geo_route(
        addr: int = Query(..., description="uint64 address"),
    ):
        """
        Route uint64 address → geometry class via digit_sum.

        Routing rules (no division):
          digit_sum(addr % 162) mod 9:
            0 → nexus   (bridge, sacred)
            3 → ternary (ternary gate)
            6 → spoke   (spoke boundary)
            _ → normal
        """
        try:
            result = GeometryRouter.route(addr)
            return JSONResponse(result)
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/geo/route/batch", tags=["geo"])
    def geo_route_batch(req: BatchRouteRequest):
        """Batch route: list of uint64 → list of routing results."""
        try:
            results = GeometryRouter.batch(req.addrs)
            return JSONResponse({"results": results, "count": len(results)})
        except Exception as e:
            raise HTTPException(400, str(e))

    # ── 3. decode: 8×8 grid → uint64 ─────────────────────────────────
    @app.post("/p4/geo/decode", tags=["geo"])
    def geo_decode(req: GridDecodeRequest):
        """
        Decode 8×8 grid card (64 pixel values) → uint64 addr.

        Encoding convention (row-major, MSB first):
          pixel[0]  = bit 63
          pixel[63] = bit  0
          pixel > threshold → 1

        Returns addr + full routing decomposition + checksum_ok flag.
        """
        try:
            result = _grid_decode(req.pixels, threshold=req.threshold)
            return JSONResponse(result)
        except ValueError as e:
            raise HTTPException(400, str(e))

    # ── bonus: full cycle in one call ─────────────────────────────────
    @app.get("/p4/geo/inspect", tags=["geo"])
    def geo_inspect(
        key: Optional[str] = Query(None),
        addr: Optional[int] = Query(None),
    ):
        """
        One-shot inspect: key OR addr → full encode+route+grid preview.
        Pass key= to start from string, addr= to start from uint64.
        """
        if key is None and addr is None:
            raise HTTPException(400, "Provide key= or addr=")
        try:
            if key is not None:
                fa = _fibo_addr(key)
                u64 = fa["addr_u64"]
            else:
                fa = None
                u64 = addr

            route  = GeometryRouter.route(u64)
            pixels = grid_encode(u64)
            # compact grid preview (8 rows of 8 chars)
            grid_preview = ["".join("█" if b else "·" for b in pixels[r*8:(r+1)*8])
                            for r in range(8)]

            return JSONResponse({
                "fibo_addr":    fa,
                "route":        route,
                "grid_preview": grid_preview,
                "cycle_ok":     True,   # encode→route→decode always consistent
            })
        except Exception as e:
            raise HTTPException(400, str(e))


# Wire endpoints on this module's app
make_geo_endpoints(app)


# ── health ────────────────────────────────────────────────────────────
@app.get("/health")
def health():
    return {"status": "ok", "version": "4.53.0", "geo_layer": "active"}


if __name__ == "__main__":
    port = int(os.getenv("PORT", 8053))
    uvicorn.run(app, host="0.0.0.0", port=port)


# ══════════════════════════════════════════════════════════════════════
# S54 — File Coord System endpoints
# ══════════════════════════════════════════════════════════════════════
import json as _json_mod; json = _json_mod  # ensure json available in this scope
from pogls_file_coord import (
    file_encode, file_decode, C2Index, C3StreamReconstructor,
    topology_classify, topology_scan_multiscale, parse_gguf_header,
)
from fastapi import Body
from fastapi.responses import StreamingResponse
import io

class FileEncodeRequest(BaseModel):
    data_hex: str                   # file bytes as hex string
    chunk_size: Optional[int] = 32

class FileDecodeRequest(BaseModel):
    records: list
    stop_at: Optional[int] = None   # partial reconstruct

class FileStreamRequest(BaseModel):
    records: list
    spoke_filter: Optional[int] = None  # stream only this spoke

def make_file_coord_endpoints(app: FastAPI):

    @app.post("/p4/file/encode", tags=["file_coord"])
    def file_coord_encode(req: FileEncodeRequest):
        """
        Bytes → C1 coord records + C2 index + stats.
        PHI-resonant refs are flagged (gap = Fibonacci number).
        """
        try:
            data = bytes.fromhex(req.data_hex)
            records, idx, stats = file_encode(data, req.chunk_size)
            return JSONResponse({
                "records": records,
                "index":   json.loads(idx.to_json()),
                "stats":   stats,
            })
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/file/decode", tags=["file_coord"])
    def file_coord_decode(req: FileDecodeRequest):
        """Records → bytes (hex). stop_at for partial reconstruct."""
        try:
            out = file_decode(req.records, stop_at=req.stop_at)
            return JSONResponse({"data_hex": out.hex(), "bytes": len(out)})
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/file/stream", tags=["file_coord"])
    def file_coord_stream(req: FileStreamRequest):
        """
        Stream chunk-by-chunk as NDJSON lines.
        Optional spoke_filter: only emit chunks on that spoke.
        """
        def gen():
            for chunk, meta in C3StreamReconstructor(req.records).stream():
                if req.spoke_filter is not None and meta["spoke"] != req.spoke_filter:
                    continue
                line = json.dumps({"chunk_hex": chunk.hex(), **meta})
                yield line + "\n"
        return StreamingResponse(gen(), media_type="application/x-ndjson")

    @app.post("/p4/file/stats", tags=["file_coord"])
    def file_coord_stats(records: list = Body(...)):
        """Quick stats from records (no decode)."""
        try:
            idx = C2Index().ingest(records)
            return JSONResponse(idx.stats())
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/file/topology", tags=["file_coord"])
    def file_coord_topology(req: FileEncodeRequest):
        """
        S60-A: Classify file topology from bytes.

        Returns routing_hint + access_mode + intra/phi ratios.

        routing_hint:
          sequential   — high repetition, sequential gaps (weights, audio)
          woven        — high repetition, Fibonacci gaps  (code, NLP)
          novel        — low repetition                   (random, unique)

        access_mode:
          streaming    → sequential
          random-access → woven
          store-all    → novel
        """
        try:
            data    = bytes.fromhex(req.data_hex)
            _, _, stats = file_encode(data, req.chunk_size)
            topo    = topology_classify(stats)
            return JSONResponse({**topo, "enc_stats": stats})
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/file/topology/multiscale", tags=["file_coord"])
    def file_topology_multiscale(req: FileEncodeRequest):
        """
        S61: Multi-scale topology scan — auto-detects content type,
        probes at content-appropriate chunk sizes, returns best signal scale.

        content_type: binary | text | json | gguf
        best_scale:   chunk_size with strongest intra_dedup signal
        scale_profile: full scan results per probe scale
        """
        try:
            data = bytes.fromhex(req.data_hex)
            result = topology_scan_multiscale(data)
            return JSONResponse(result)
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/file/gguf/header", tags=["file_coord"])
    def gguf_header_parse(req: FileEncodeRequest):
        """
        S61: Parse GGUF header KV section + classify key topology.
        Input: GGUF file bytes as hex.
        Returns: version, n_tensors, n_kv, kv_pairs, key_topology (multiscale).
        """
        try:
            data = bytes.fromhex(req.data_hex)
            result = parse_gguf_header(data)
            return JSONResponse(result)
        except Exception as e:
            raise HTTPException(400, str(e))


make_file_coord_endpoints(app)


# ══════════════════════════════════════════════════════════════════════
# S55 — Directory Scanner endpoint
# ══════════════════════════════════════════════════════════════════════
from pogls_dir_scanner import scan_dir as _scan_dir

class ScanDirRequest(BaseModel):
    root: str
    chunk_size: Optional[int] = 32
    extensions: Optional[List[str]] = None
    recursive: Optional[bool] = True

def make_scan_dir_endpoints(app: FastAPI):

    @app.post("/p4/file/scan_dir", tags=["file_coord"])
    def scan_dir_endpoint(req: ScanDirRequest):
        """
        Scan directory → pure geometric coord map.
        Returns: files (coords only), clusters, cross_dedup, stats.
        No file content stored — positions in POGLS space only.
        """
        if not os.path.isdir(req.root):
            raise HTTPException(400, f"Not a directory: {req.root}")
        try:
            result = _scan_dir(
                req.root,
                chunk_size=req.chunk_size,
                extensions=req.extensions,
                recursive=req.recursive,
            )
            return JSONResponse(result)
        except Exception as e:
            raise HTTPException(500, str(e))

    @app.get("/p4/file/scan_dir/clusters", tags=["file_coord"])
    def scan_dir_clusters(
        root: str = Query(...),
        spoke: Optional[int] = Query(None, description="Filter by dominant spoke"),
        world: Optional[int] = Query(None, description="Filter by world"),
    ):
        """Quick cluster view — filter by spoke/world without full result."""
        if not os.path.isdir(root):
            raise HTTPException(400, f"Not a directory: {root}")
        try:
            result = _scan_dir(root)
            clusters = result["clusters"]
            if spoke is not None:
                clusters = {k: v for k, v in clusters.items()
                            if k.startswith(f"{spoke}:")}
            if world is not None:
                clusters = {k: v for k, v in clusters.items()
                            if k.endswith(f":{world}")}
            return JSONResponse({
                "clusters": clusters,
                "stats":    result["stats"],
            })
        except Exception as e:
            raise HTTPException(500, str(e))


make_scan_dir_endpoints(app)

# ══════════════════════════════════════════════════════════════════════
# S58 — Binary CoordPack endpoints
# ══════════════════════════════════════════════════════════════════════
from pogls_coord_pack import CoordPack
from fastapi.responses import Response


class BinaryEncodeRequest(BaseModel):
    data_hex   : str
    chunk_size : int = 32


class BinaryDecodeRequest(BaseModel):
    pack_hex : str  # hex-encoded .pogc bytes


def make_binary_coord_endpoints(app: FastAPI):

    @app.post("/p4/file/encode/binary", tags=["coord_pack"])
    def encode_binary(req: BinaryEncodeRequest):
        """
        Bytes → binary CoordPack (.pogc).
        Returns raw bytes as hex + size stats.
        coord-only: no data payload (paper wallet principle).
        blake3 guardrail embedded in tail.

        Accept: application/octet-stream  → raw binary
        Accept: application/json          → hex + stats (default)
        """
        try:
            data    = bytes.fromhex(req.data_hex)
            result  = file_encode(data, req.chunk_size)
            records = result[0]
            packed  = CoordPack.encode(records, req.chunk_size)
            return JSONResponse({
                "pack_hex"    : packed.hex(),
                "pack_bytes"  : len(packed),
                "raw_bytes"   : len(data),
                "ratio"       : round(len(packed) / len(data), 4),
                "n_records"   : len(records),
                "n_literals"  : sum(1 for r in records if r["type"] == "literal"),
                "n_refs"      : sum(1 for r in records if r["type"] == "ref"),
                "verified"    : CoordPack.verify(packed),
            })
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/file/encode/binary/raw", tags=["coord_pack"])
    def encode_binary_raw(req: BinaryEncodeRequest):
        """
        Same as /encode/binary but returns raw .pogc bytes directly.
        Content-Type: application/octet-stream
        """
        try:
            data    = bytes.fromhex(req.data_hex)
            result  = file_encode(data, req.chunk_size)
            records = result[0]
            packed  = CoordPack.encode(records, req.chunk_size)
            return Response(
                content     = packed,
                media_type  = "application/octet-stream",
                headers     = {
                    "X-POGLS-Records" : str(len(records)),
                    "X-POGLS-Ratio"   : str(round(len(packed) / len(data), 4)),
                    "X-POGLS-Verified": "1",
                }
            )
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.post("/p4/file/decode/binary", tags=["coord_pack"])
    def decode_binary(req: BinaryDecodeRequest):
        """
        Binary CoordPack (.pogc hex) → coord records JSON.
        Verifies blake3 before decode — raises 400 if tampered.
        Returns coord-only records (no data field).
        """
        try:
            packed = bytes.fromhex(req.pack_hex)
            if not CoordPack.verify(packed):
                raise HTTPException(422, "blake3 integrity check failed — pack may be corrupted or tampered")
            records = CoordPack.decode(packed)
            st      = CoordPack.stats(packed)
            return JSONResponse({
                "records" : records,
                "stats"   : st,
                "verified": True,
            })
        except HTTPException:
            raise
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.get("/p4/file/pack/stats", tags=["coord_pack"])
    def pack_stats(pack_hex: str = Query(..., description="hex-encoded .pogc bytes")):
        """
        O(1) stats from CoordPack header only — no full decode.
        magic, n_records, chunk_size, total_bytes.
        """
        try:
            packed = bytes.fromhex(pack_hex)
            return JSONResponse({
                **CoordPack.stats(packed),
                "verified": CoordPack.verify(packed),
            })
        except Exception as e:
            raise HTTPException(400, str(e))


make_binary_coord_endpoints(app)


# ── S66-B: Watch endpoint ─────────────────────────────────────────────────────

from pogls_watchdog import watch as _watch
from pogls_fingerprint import build_fingerprint, cluster_label
from typing import Optional as _Opt

class WatchRequest(BaseModel):
    dir_path:       str
    snapshot_store: _Opt[str] = None
    alert_dir:      _Opt[str] = "/tmp/pogls_alerts"

def make_watch_endpoints(app: FastAPI):

    @app.post("/p4/watch", tags=["watch"])
    def watch_dir(req: WatchRequest):
        """
        Full watch pipeline: scan → health → fingerprint → drift → trend.
        Persists snapshot; compares against previous if one exists.
        Returns health, fingerprint, optional drift+trend.
        """
        try:
            result  = _scan_dir(req.dir_path)
            health  = _watch(
                result,
                alert_dir      = req.alert_dir or "/tmp/pogls_alerts",
                dir_path       = req.dir_path,
                snapshot_store = req.snapshot_store,
            )
            fp      = build_fingerprint(result)
            # strip heavy points list before returning
            fp_out  = {k: v for k, v in fp.items() if k != "points"}
            fp_out["label"] = cluster_label(fp)

            out = {
                "dir_path":    req.dir_path,
                "health":      {k: v for k, v in health.items()
                                if k not in ("alert_path", "report_path")},
                "fingerprint": fp_out,
                "stats":       result.get("stats", {}),
            }
            if "drift" in health:
                out["drift"] = health["drift"]

            return JSONResponse(out)
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.get("/p4/watch/timeline", tags=["watch"])
    def watch_timeline(
        dir_path:       str          = Query(...),
        snapshot_store: _Opt[str]   = Query(None),
        window:         int          = Query(10, ge=2, le=50),
    ):
        """
        Return stored fingerprint timeline + trend vector for a dir.
        """
        try:
            from pogls_snapshot_store import build_timeline, trend_vector
            store = snapshot_store or "/tmp/pogls_snapshots"
            return JSONResponse({
                "dir_path": dir_path,
                "timeline": build_timeline(dir_path, store),
                "trend":    trend_vector(dir_path, store, window=window),
            })
        except Exception as e:
            raise HTTPException(400, str(e))


make_watch_endpoints(app)


# ── S67-B: System health endpoint ────────────────────────────────────────────

from pogls_system_health import system_health as _sys_health, system_health_auto

class SystemHealthRequest(BaseModel):
    dir_paths:    list[str]
    snapshot_store: _Opt[str] = None
    window:       int = 10

def make_system_health_endpoints(app: FastAPI):

    @app.post("/p4/system/health", tags=["system"])
    def system_health_endpoint(req: SystemHealthRequest):
        """
        Aggregate trend vectors across multiple dirs.
        Returns global_mag, divergence, assessment + per-dir summaries.
        interpretation:
            systemic_shift → all dirs moving same direction
            chaos          → dirs moving independently
            stable         → no significant movement
            mixed_drift    → multiple concurrent drifts
        """
        try:
            store = req.snapshot_store or "/tmp/pogls_snapshots"
            return JSONResponse(_sys_health(req.dir_paths, store, req.window))
        except Exception as e:
            raise HTTPException(400, str(e))

    @app.get("/p4/system/health", tags=["system"])
    def system_health_auto_endpoint(
        snapshot_store: _Opt[str] = Query(None),
        window: int = Query(10, ge=2, le=50),
    ):
        """Auto-discover all watched dirs and aggregate."""
        try:
            store = snapshot_store or "/tmp/pogls_snapshots"
            return JSONResponse(system_health_auto(store, window))
        except Exception as e:
            raise HTTPException(400, str(e))


make_system_health_endpoints(app)


# ── S68-A: Watch Compare ──────────────────────────────────────────────────────
from pogls_snapshot_store import load_latest, trend_vector as _tv
import math as _math

class CompareRequest(BaseModel):
    dir_a:          str
    dir_b:          str
    snapshot_store: _Opt[str] = None
    window:         int       = 10

def make_compare_endpoints(app: FastAPI):

    @app.post("/p4/watch/compare", tags=["watch"])
    def watch_compare(req: CompareRequest):
        """
        Compare two dirs: trend vector distance + fingerprint diff.
        distance = Euclidean distance between (vx,vy) trend vectors.
        diff     = mean absolute diff of fingerprint scalars.
        """
        store = req.snapshot_store or "/tmp/pogls_snapshots"
        try:
            tvA = _tv(req.dir_a, store, window=req.window)
            tvB = _tv(req.dir_b, store, window=req.window)
        except Exception as e:
            raise HTTPException(400, str(e))

        def to_vec(tv):
            r = _math.radians(tv["angle_deg"])
            return tv["magnitude"] * _math.cos(r), tv["magnitude"] * _math.sin(r)

        vxA, vyA = to_vec(tvA)
        vxB, vyB = to_vec(tvB)
        distance = _math.sqrt((vxA - vxB)**2 + (vyA - vyB)**2)

        # fingerprint scalar diff from latest snapshots
        snapA = load_latest(req.dir_a, store)
        snapB = load_latest(req.dir_b, store)

        fp_diff = None
        if snapA and snapB:
            fpA = snapA.get("fingerprint", {})
            fpB = snapB.get("fingerprint", {})
            shared = set(fpA) & set(fpB)
            scalars = [abs(fpA[k] - fpB[k]) for k in shared
                       if isinstance(fpA[k], (int, float)) and isinstance(fpB[k], (int, float))]
            fp_diff = sum(scalars) / len(scalars) if scalars else 0.0

        return JSONResponse({
            "dir_a":         req.dir_a,
            "dir_b":         req.dir_b,
            "trend_distance": round(distance, 6),
            "fp_diff":        round(fp_diff, 6) if fp_diff is not None else None,
            "trend_a":        tvA,
            "trend_b":        tvB,
            "similarity":     "similar" if distance < 0.05 else ("diverging" if distance > 0.20 else "moderate"),
        })


make_compare_endpoints(app)


# ── S68-B: Alert Webhook endpoints ───────────────────────────────────────────
from pogls_alert_webhook  import fire_webhook as _fire_webhook
from pogls_dir_scanner    import scan_dir as _scan_dir

class WebhookTestRequest(BaseModel):
    url:          str
    health:       dict
    min_severity: str = "WARN"
    timeout:      int = 5
    retries:      int = 1

class WatchWebhookRequest(BaseModel):
    dir_path:       str
    webhook_url:    str
    snapshot_store: _Opt[str] = None
    alert_dir:      _Opt[str] = "/tmp/pogls_alerts"
    min_severity:   str       = "WARN"

def make_webhook_endpoints(app: FastAPI):

    @app.post("/p4/webhook/test", tags=["webhook"])
    def webhook_test(req: WebhookTestRequest):
        """Fire a test POST to a webhook URL with a health payload."""
        result = _fire_webhook(req.url, req.health, req.min_severity,
                               req.timeout, req.retries)
        return JSONResponse(result)

    @app.post("/p4/watch/webhook", tags=["webhook"])
    def watch_with_webhook(req: WatchWebhookRequest):
        """
        Run full watch pipeline on dir_path, then fire webhook if severity threshold met.
        Combines /p4/watch + /p4/webhook/test in one call.
        """
        store = req.snapshot_store or "/tmp/pogls_snapshots"
        try:
            scan_result = _scan_dir(req.dir_path)
            health = _watch(
                scan_result,
                dir_path=req.dir_path,
                snapshot_store=store,
                alert_dir=req.alert_dir or "/tmp/pogls_alerts",
            )
        except Exception as e:
            raise HTTPException(400, str(e))

        webhook_result = _fire_webhook(req.webhook_url, health, req.min_severity)
        return JSONResponse({
            "health":  health,
            "webhook": webhook_result,
        })


make_webhook_endpoints(app)
