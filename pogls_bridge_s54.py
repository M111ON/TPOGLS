"""
POGLS Bridge — S54
Connects codec/visual layer ↔ S50 engine

4 components:
  1. B62Middleware   — FastAPI middleware: b62 ↔ uint64 on /p4/* endpoints
  2. snapshot_to_svg — dump JSON → blueprint scroll SVG (print-ready)
  3. addr_inspect    — one call: addr → full decompose + sig_index lookup
  4. svg_decode      — S53: parse blueprint SVG → addr rows (path or inline)

S54 changes:
  • POST /p4/bridge/svg_decode added to make_bridge_endpoints()
      body: {"path": "..."}  OR  {"svg": "<raw svg>"}
      → {rows: [{addr,b62,hex,sig,sig_b62,spoke,world_lbl,slot,layer,...}], n, source}

Usage (in rest_server_s5x.py):
  from pogls_bridge import B62Middleware, make_bridge_endpoints
  app.add_middleware(B62Middleware)
  make_bridge_endpoints(app, lambda: dict(_p4_sig_index))
"""

import json, os, math, time
from typing import Any

# ── codec (inline — no import chain dependency) ───────────────────────
_ALPHA    = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
_BASE     = 62
_B62_LEN  = 11
_U64_MAX  = (1 << 64) - 1
_DEC      = {c: i for i, c in enumerate(_ALPHA)}

def _b62enc(n: int) -> str:
    out, x = [], n
    for _ in range(_B62_LEN):
        out.append(_ALPHA[x % _BASE])
        x //= _BASE
    return "".join(reversed(out))

def _b62dec(s: str) -> int:
    if len(s) != _B62_LEN:
        raise ValueError(f"b62 must be {_B62_LEN} chars, got {len(s)}: {s!r}")
    n = 0
    for c in s:
        if c not in _DEC:
            raise ValueError(f"invalid b62 char: {c!r}")
        n = n * _BASE + _DEC[c]
    if n > _U64_MAX:
        raise OverflowError(f"b62 value exceeds uint64: {n}")
    return n

def _is_b62(s: Any) -> bool:
    return isinstance(s, str) and len(s) == _B62_LEN and all(c in _DEC for c in s)

def _to_hex(n: int) -> str:
    return f"{n:016X}"

# ── decompose (mirrors pogls_visual.py) ──────────────────────────────
_SPOKE_COLORS = ["#E74C3C","#E67E22","#F1C40F","#2ECC71","#3498DB","#9B59B6"]
_WORLD_LABEL  = ["4n","5n","6n"]
_WORLD_SHAPE  = ["circle","triangle","square"]

def _decompose(addr: int) -> dict:
    spoke     = addr % 6
    slot      = (addr >> 6)  % 27
    world     = (addr >> 11) % 3
    layer     = (addr >> 13) % 4
    intensity = int(slot / 26 * 255) if slot else 0
    bits      = [(addr >> (63 - i)) & 1 for i in range(64)]
    return {
        "addr":      addr,
        "spoke":     spoke,
        "slot":      slot,
        "world":     world,
        "layer":     layer,
        "intensity": intensity,
        "world_lbl": _WORLD_LABEL[world],
        "shape":     _WORLD_SHAPE[world],
        "color":     _SPOKE_COLORS[spoke],
        "b62":       _b62enc(addr),
        "hex":       _to_hex(addr),
        "bits":      bits,
    }

# ═══════════════════════════════════════════════════════════════════════
# 1. B62 MIDDLEWARE
# ═══════════════════════════════════════════════════════════════════════
# Intercepts /p4/* requests:
#   request body:  addrs=[b62, ...]  → decode → addrs=[uint64, ...]
#   response body: sigs=[uint64,...] → encode → sigs=[b62, ...]
#                  addrs=[uint64,..] → encode → addrs=[b62, ...]
#
# Transparent — endpoints never change.
# Toggle: set env P4_B62_MIDDLEWARE=0 to disable.

_B62_MW_ENABLED = os.environ.get("P4_B62_MIDDLEWARE", "1") != "0"

# Fields to decode (request) and encode (response)
_REQ_ADDR_FIELDS = {"addrs", "sigs"}
_RES_ADDR_FIELDS = {"sigs", "addrs", "results"}  # results = sig_index range/batch

def _decode_req_body(body: dict) -> tuple[dict, bool]:
    """Decode b62 addr fields in request body. Returns (new_body, was_b62)."""
    changed = False
    out = dict(body)
    for field in _REQ_ADDR_FIELDS:
        if field not in out:
            continue
        val = out[field]
        if isinstance(val, list) and val and _is_b62(val[0]):
            out[field] = [_b62dec(v) if _is_b62(v) else int(v) for v in val]
            changed = True
    return out, changed

def _encode_res_body(body: dict) -> dict:
    """Encode uint64 addr fields in response body to b62."""
    out = dict(body)
    for field in _RES_ADDR_FIELDS:
        if field not in out:
            continue
        val = out[field]
        if isinstance(val, list) and val and isinstance(val[0], int):
            out[field] = [_b62enc(v) if 0 <= v <= _U64_MAX else v for v in val]
        elif isinstance(val, dict):
            # sig_index results: {sig: [addrs]} or {b62: [addrs]}
            out[field] = {
                k: [_b62enc(a) for a in v] if isinstance(v, list) else v
                for k, v in val.items()
            }
    return out


try:
    from starlette.middleware.base import BaseHTTPMiddleware
    from starlette.requests import Request
    from starlette.responses import Response
    import json as _json

    class B62Middleware(BaseHTTPMiddleware):
        """
        Transparent b62 ↔ uint64 translation on /p4/* routes.
        Disable: P4_B62_MIDDLEWARE=0
        """
        async def dispatch(self, request: Request, call_next):
            if not _B62_MW_ENABLED or not request.url.path.startswith("/p4"):
                return await call_next(request)

            # ── decode request ──────────────────────────────────────
            body_bytes = await request.body()
            was_b62    = False
            if body_bytes:
                try:
                    body_json = _json.loads(body_bytes)
                    if isinstance(body_json, dict):
                        body_json, was_b62 = _decode_req_body(body_json)
                        if was_b62:
                            body_bytes = _json.dumps(body_json).encode()
                except Exception:
                    pass  # non-JSON body — pass through

            # rebuild request with (possibly) modified body
            async def receive():
                return {"type": "http.request", "body": body_bytes, "more_body": False}

            request._receive = receive

            response = await call_next(request)

            # ── encode response (only if request had b62) ───────────
            if not was_b62:
                return response

            # buffer response body
            chunks = []
            async for chunk in response.body_iterator:
                chunks.append(chunk if isinstance(chunk, bytes) else chunk.encode())
            raw = b"".join(chunks)
            try:
                res_json = _json.loads(raw)
                if isinstance(res_json, dict):
                    res_json = _encode_res_body(res_json)
                    raw = _json.dumps(res_json).encode()
            except Exception:
                pass  # non-JSON response — return as-is

            return Response(
                content=raw,
                status_code=response.status_code,
                headers=dict(response.headers),
                media_type=response.media_type,
            )

except ImportError:
    # starlette not available (standalone use)
    class B62Middleware:  # type: ignore
        def __init__(self, *a, **kw): pass


# ═══════════════════════════════════════════════════════════════════════
# 2. SNAPSHOT → SVG
# ═══════════════════════════════════════════════════════════════════════

def snapshot_to_svg(path: str, max_entries: int = 64) -> str:
    """
    Read S49/S50 snapshot JSON → blueprint scroll SVG (print-ready A4 style).

    Supports both:
      plain dump    : {sig: [addrs, ...]}
      versioned     : {version, epoch, label, n_sigs, data: {sig: [addrs]}}
    """
    with open(path) as f:
        raw = json.load(f)

    is_versioned = "version" in raw and "data" in raw
    meta = {
        "version": raw.get("version", "legacy"),
        "epoch":   raw.get("epoch",   0),
        "label":   raw.get("label",   os.path.basename(path)),
        "n_sigs":  raw.get("n_sigs",  0),
        "path":    path,
    }
    data: dict = raw["data"] if is_versioned else raw

    # flatten: collect (sig, addr) pairs sorted by sig
    pairs: list[tuple[int, int]] = []
    for sig_str, addrs in data.items():
        sig = int(sig_str)
        for addr in (addrs if isinstance(addrs, list) else [addrs]):
            pairs.append((sig, int(addr)))
            if len(pairs) >= max_entries:
                break
        if len(pairs) >= max_entries:
            break
    pairs.sort()

    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(meta["epoch"])) if meta["epoch"] else "unknown"

    # ── SVG layout constants ──────────────────────────────────────────
    PAGE_W    = 680
    ROW_H     = 52
    HEADER_H  = 48
    FOOTER_H  = 24
    MINI_CELL = 5          # mini grid cell px
    MINI_SIZE = 8 * MINI_CELL + 7  # 8×8 grid + gaps
    RING_SIZE = 44         # mini radial diameter
    PAD       = 20
    total_h   = HEADER_H + len(pairs) * ROW_H + FOOTER_H + PAD * 2

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{PAGE_W}" height="{total_h}" '
        f'style="background:#020208;font-family:\'Courier New\',monospace">',

        # background
        f'<rect width="{PAGE_W}" height="{total_h}" fill="#020208"/>',

        # corner marks
        *_corner_marks(8, 8, PAGE_W - 8, total_h - 8, "#1a2a4a"),

        # header
        f'<text x="{PAD}" y="22" font-size="11" letter-spacing="3" fill="#4a6aaa">'
        f'POGLS · BLUEPRINT SCROLL</text>',
        f'<text x="{PAD}" y="36" font-size="8" letter-spacing="1" fill="#2a3a5a">'
        f'label={meta["label"]}  ts={ts}  n_sigs={meta["n_sigs"]}  '
        f'entries={len(pairs)}  version={meta["version"]}</text>',
        f'<line x1="{PAD}" y1="{HEADER_H}" x2="{PAGE_W-PAD}" y2="{HEADER_H}" '
        f'stroke="#1a2a4a" stroke-width="1"/>',
    ]

    # ── rows ──────────────────────────────────────────────────────────
    for idx, (sig, addr) in enumerate(pairs):
        y = HEADER_H + PAD + idx * ROW_H
        d = _decompose(addr)
        color = d["color"]

        # row bg (alternate)
        if idx % 2 == 0:
            lines.append(f'<rect x="{PAD}" y="{y}" width="{PAGE_W-PAD*2}" height="{ROW_H-2}" fill="#080812" opacity=".5"/>')

        # index
        lines.append(f'<text x="{PAD+2}" y="{y+16}" font-size="8" fill="#2a3a5a">{str(idx).zfill(3)}</text>')

        # mini 8×8 grid
        gx = PAD + 28
        for row in range(8):
            for col in range(8):
                bit = d["bits"][row * 8 + col]
                cx  = gx + col * (MINI_CELL + 1)
                cy  = y + row * (MINI_CELL + 1) + 2
                fill = color if bit else "#0e0e20"
                op   = ".85" if bit else ".4"
                lines.append(f'<rect x="{cx}" y="{cy}" width="{MINI_CELL}" height="{MINI_CELL}" fill="{fill}" opacity="{op}"/>')

        # mini radial
        rx = gx + MINI_SIZE + 10
        ry = y + ROW_H // 2
        lines.extend(_mini_radial(rx, ry, RING_SIZE // 2, d))

        # spoke accent line
        sx = rx + RING_SIZE // 2 + 6
        lines.append(f'<line x1="{sx}" y1="{y+4}" x2="{sx}" y2="{y+ROW_H-4}" stroke="{color}" stroke-width="2" opacity=".5"/>')

        # text block
        tx = sx + 12
        lines += [
            f'<text x="{tx}" y="{y+14}" font-size="12" letter-spacing="2" fill="#8ab4f8">{d["b62"]}</text>',
            f'<text x="{tx}" y="{y+26}" font-size="8" fill="#2a3a5a">0x{d["hex"]}</text>',
            f'<text x="{tx}" y="{y+38}" font-size="8" fill="#3a4a6a">'
            f'sig={sig}  spoke=<tspan fill="{color}">{d["spoke"]}</tspan>  '
            f'world=<tspan fill="{color}">{d["world_lbl"]}</tspan>  '
            f'slot={d["slot"]}  layer={d["layer"]}</text>',
        ]

        # right sig label
        lines.append(
            f'<text x="{PAGE_W-PAD}" y="{y+16}" font-size="8" fill="#1a2a4a" text-anchor="end">'
            f'sig={_b62enc(sig)}</text>'
        )

    # footer
    fy = total_h - FOOTER_H
    lines += [
        f'<line x1="{PAD}" y1="{fy}" x2="{PAGE_W-PAD}" y2="{fy}" stroke="#1a2a4a" stroke-width="1" stroke-dasharray="4 4"/>',
        f'<text x="{PAD}" y="{fy+14}" font-size="7" fill="#1a2a4a" letter-spacing="1">'
        f'POGLS · deterministic · geometry-native · base62 case-sensitive · A≠a</text>',
        f'<text x="{PAGE_W-PAD}" y="{fy+14}" font-size="7" fill="#1a2a4a" text-anchor="end">'
        f'max={_b62enc(_U64_MAX)}</text>',
    ]

    lines.append("</svg>")
    return "\n".join(lines)


def _corner_marks(x0, y0, x1, y1, color, size=10):
    s = size
    return [
        f'<path d="M{x0},{y0+s} L{x0},{y0} L{x0+s},{y0}" fill="none" stroke="{color}" stroke-width="1"/>',
        f'<path d="M{x1-s},{y0} L{x1},{y0} L{x1},{y0+s}" fill="none" stroke="{color}" stroke-width="1"/>',
        f'<path d="M{x0},{y1-s} L{x0},{y1} L{x0+s},{y1}" fill="none" stroke="{color}" stroke-width="1"/>',
        f'<path d="M{x1-s},{y1} L{x1},{y1} L{x1},{y1-s}" fill="none" stroke="{color}" stroke-width="1"/>',
    ]


def _mini_radial(cx, cy, radius, d):
    """SVG elements for a mini radial ring — 8 rings × 8 bits."""
    out = []
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{radius}" fill="#080814" stroke="{d["color"]}" stroke-width=".4" opacity=".2"/>')
    for ring in range(8):
        ri = 3 + ring * (radius - 3) // 8
        ro = ri + (radius - 3) // 8 - .5
        bits = d["bits"][ring * 8: (ring + 1) * 8]
        for i, bit in enumerate(bits):
            if not bit:
                continue
            a0 = -math.pi / 2 + i * math.pi / 4
            a1 = a0 + math.pi / 4 - 0.08
            p = (f"M{cx+ri*math.cos(a0):.1f},{cy+ri*math.sin(a0):.1f} "
                 f"L{cx+ro*math.cos(a0):.1f},{cy+ro*math.sin(a0):.1f} "
                 f"A{ro:.1f},{ro:.1f},0,0,1,{cx+ro*math.cos(a1):.1f},{cy+ro*math.sin(a1):.1f} "
                 f"L{cx+ri*math.cos(a1):.1f},{cy+ri*math.sin(a1):.1f} "
                 f"A{ri:.1f},{ri:.1f},0,0,0,{cx+ri*math.cos(a0):.1f},{cy+ri*math.sin(a0):.1f}")
            out.append(f'<path d="{p}" fill="{d["color"]}" opacity="{.4+ring*.06:.2f}"/>')
    # center dot
    out.append(f'<circle cx="{cx}" cy="{cy}" r="2.5" fill="{d["color"]}" opacity=".9"/>')
    return out


# ═══════════════════════════════════════════════════════════════════════
# 3. ADDR INSPECT
# ═══════════════════════════════════════════════════════════════════════

def addr_inspect(addr: int, sig_index: dict | None = None) -> dict:
    """
    Full inspection of a single addr:
      - decompose → spoke/slot/world/layer/intensity
      - b62 + hex representations
      - sig_index reverse lookup (optional)
      - geometry position summary
    """
    d = _decompose(addr)
    result = {
        "addr":       addr,
        "b62":        d["b62"],
        "hex":        f'0x{d["hex"]}',
        "geometry": {
            "spoke":     d["spoke"],
            "slot":      d["slot"],
            "world":     d["world_lbl"],
            "layer":     d["layer"],
            "intensity": d["intensity"],
            "color":     d["color"],
            "shape":     d["shape"],
        },
        "bits": {
            "raw":    "".join(str(b) for b in d["bits"]),
            "rows":  ["".join(str(b) for b in d["bits"][r*8:(r+1)*8]) for r in range(8)],
            "popcount": sum(d["bits"]),
        },
        "sig_index": None,
    }

    if sig_index is not None:
        # find all sigs that contain this addr
        matching = {
            str(sig): addrs
            for sig, addrs in sig_index.items()
            if addr in (addrs if isinstance(addrs, list) else [addrs])
        }
        result["sig_index"] = {
            "found":    len(matching) > 0,
            "n_sigs":   len(matching),
            "sigs_b62": {_b62enc(int(k)): v for k, v in matching.items()},
        }

    return result


# ═══════════════════════════════════════════════════════════════════════
# FastAPI endpoint helpers (mount into rest_server_s51.py)
# ═══════════════════════════════════════════════════════════════════════

def make_bridge_endpoints(app, get_sig_index_fn):
    """
    Register bridge endpoints on an existing FastAPI app.

    Usage in rest_server_s51.py:
        from pogls_bridge import make_bridge_endpoints
        make_bridge_endpoints(app, lambda: dict(_p4_sig_index))
    """
    from fastapi import Query as Q
    from fastapi.responses import JSONResponse, Response

    @app.get("/p4/bridge/inspect")
    def bridge_inspect(
        addr: int = Q(None, description="uint64 addr"),
        b62:  str = Q(None, description="base62 11-char addr"),
    ):
        """S51: Full addr decompose + sig_index reverse lookup."""
        if addr is None and b62 is None:
            from fastapi import HTTPException
            raise HTTPException(400, "provide addr= or b62=")
        try:
            a = _b62dec(b62) if b62 else addr
        except Exception as e:
            from fastapi import HTTPException
            raise HTTPException(400, str(e))
        idx = get_sig_index_fn()
        return JSONResponse(addr_inspect(a, idx))

    @app.get("/p4/bridge/snapshot_svg")
    def bridge_snapshot_svg(
        path:        str = Q(...,   description="path to snapshot JSON"),
        max_entries: int = Q(64,    description="max addr rows in SVG"),
    ):
        """S51: Render snapshot JSON → blueprint scroll SVG."""
        import os as _os
        if not _os.path.exists(path):
            from fastapi import HTTPException
            raise HTTPException(404, f"snapshot not found: {path}")
        try:
            svg = snapshot_to_svg(path, max_entries)
        except Exception as e:
            from fastapi import HTTPException
            raise HTTPException(500, str(e))
        return Response(content=svg, media_type="image/svg+xml")

    @app.post("/p4/bridge/b62_encode")
    def bridge_b62_encode(body: dict):
        """S51: Encode list of uint64 addrs → b62."""
        addrs = body.get("addrs", [])
        return JSONResponse({"b62": [_b62enc(int(a)) for a in addrs]})

    @app.post("/p4/bridge/b62_decode")
    def bridge_b62_decode(body: dict):
        """S51: Decode list of b62 strings → uint64."""
        try:
            return JSONResponse({"addrs": [_b62dec(s) for s in body.get("b62", [])]})
        except Exception as e:
            from fastapi import HTTPException
            raise HTTPException(400, str(e))

    @app.post("/p4/bridge/svg_decode")
    def bridge_svg_decode(body: dict):
        """S54: Parse blueprint SVG → addr rows with full decompose + sig.

        Two input modes (mutually exclusive, path takes priority):
          {"path": "/tmp/snapshot.svg"}          — read from filesystem
          {"svg":  "<svg ...>...</svg>"}          — raw SVG string in body

        Returns:
          {"rows": [{addr, b62, hex, sig, sig_b62, spoke, world_lbl,
                     slot, layer, intensity, world, shape, color}, ...],
           "n": <count>, "source": "path"|"inline"}
        """
        import tempfile, os as _os
        from fastapi import HTTPException

        path_in = body.get("path")
        svg_in  = body.get("svg")

        if not path_in and not svg_in:
            raise HTTPException(400, "provide 'path' or 'svg' in body")

        try:
            if path_in:
                if not _os.path.exists(path_in):
                    raise HTTPException(404, f"SVG not found: {path_in}")
                rows   = svg_decode(path_in)
                source = "path"
            else:
                # write raw svg to tmp file — svg_decode uses ElementTree which needs file
                with tempfile.NamedTemporaryFile(suffix=".svg", mode="w",
                                                 delete=False, encoding="utf-8") as tf:
                    tf.write(svg_in)
                    tmp_path = tf.name
                try:
                    rows   = svg_decode(tmp_path)
                    source = "inline"
                finally:
                    _os.unlink(tmp_path)
        except HTTPException:
            raise
        except Exception as e:
            raise HTTPException(500, str(e))

        # drop bits array — too verbose for HTTP response
        for r in rows:
            r.pop("bits", None)

        return JSONResponse({"rows": rows, "n": len(rows), "source": source})


# ═══════════════════════════════════════════════════════════════════════
# TESTS
# ═══════════════════════════════════════════════════════════════════════

def _run_tests():
    PASS = FAIL = 0
    def check(label, ok):
        nonlocal PASS, FAIL
        PASS, FAIL = (PASS+1, FAIL) if ok else (PASS, FAIL+1)
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}")

    U64MAX = (1 << 64) - 1

    # ── codec round-trip ─────────────────────────────────────────────
    for addr in [0, 1, 54, 162, 648, 640, 654, 2562, U64MAX, 0xDEADBEEFCAFEBABE]:
        b62  = _b62enc(addr)
        back = _b62dec(b62)
        check(f"b62 round-trip addr={addr}", back == addr)
        check(f"b62 len=11     addr={addr}", len(b62) == _B62_LEN)

    # ── decompose ────────────────────────────────────────────────────
    for addr in [0, 54, 162, 648, U64MAX]:
        d = _decompose(addr)
        check(f"decompose spoke 0-5   addr={addr}", 0 <= d["spoke"] <= 5)
        check(f"decompose world 0-2   addr={addr}", 0 <= d["world"] <= 2)
        check(f"decompose bits=64     addr={addr}", len(d["bits"]) == 64)
        recon = sum(b << (63-i) for i, b in enumerate(d["bits"]))
        check(f"bits round-trip       addr={addr}", recon == addr)

    # sacred numbers all spoke=0
    check("54/162/648 spoke=0", all(_decompose(n)["spoke"] == 0 for n in [54, 162, 648]))

    # ── req/res body translation ─────────────────────────────────────
    req = {"addrs": [_b62enc(54), _b62enc(162)], "spoke_filter": 0}
    out, was = _decode_req_body(req)
    check("decode req: was_b62=True",     was)
    check("decode req: addrs=[54,162]",   out["addrs"] == [54, 162])
    check("decode req: other fields kept", out["spoke_filter"] == 0)

    req2 = {"addrs": [54, 162]}  # already uint64
    out2, was2 = _decode_req_body(req2)
    check("decode req: uint64 passthrough was_b62=False", not was2)
    check("decode req: uint64 values unchanged", out2["addrs"] == [54, 162])

    res = {"sigs": [101, 202, 303], "n_sigs": 3}
    enc = _encode_res_body(res)
    check("encode res: sigs are b62", all(_is_b62(s) for s in enc["sigs"]))
    check("encode res: n_sigs unchanged", enc["n_sigs"] == 3)

    # round-trip: encode response → decode request
    sigs_b62 = enc["sigs"]
    decoded_back = [_b62dec(s) for s in sigs_b62]
    check("middleware round-trip sigs", decoded_back == [101, 202, 303])

    # ── addr_inspect ─────────────────────────────────────────────────
    ins = addr_inspect(54)
    check("inspect b62",    ins["b62"]  == _b62enc(54))
    check("inspect hex",    ins["hex"]  == f"0x{_to_hex(54)}")
    check("inspect spoke",  ins["geometry"]["spoke"]  == 0)
    check("inspect world",  ins["geometry"]["world"]  == "4n")
    check("inspect no_sig_index", ins["sig_index"] is None)

    fake_idx = {101: [54, 162], 202: [648]}
    ins2 = addr_inspect(54, fake_idx)
    check("inspect with sig_index found", ins2["sig_index"]["found"])
    check("inspect sig count=1", ins2["sig_index"]["n_sigs"] == 1)

    ins3 = addr_inspect(999, fake_idx)
    check("inspect addr not in index", not ins3["sig_index"]["found"])

    # ── snapshot_to_svg ──────────────────────────────────────────────
    import tempfile
    snap_plain = {str(sig): [sig*10, sig*20] for sig in [54, 162, 648]}
    snap_v = {"version":"S49","epoch":1700000000,"label":"test","n_sigs":3,"data":snap_plain}

    for label, snap in [("plain", snap_plain), ("versioned", snap_v)]:
        with tempfile.NamedTemporaryFile(suffix=".json", mode="w", delete=False) as f:
            json.dump(snap, f); fname = f.name
        try:
            svg = snapshot_to_svg(fname, max_entries=10)
            check(f"snapshot_to_svg {label}: returns svg", svg.strip().startswith("<svg"))
            # verify at least one b62-encoded addr appears in the SVG
            first_addr = list(snap_plain.values())[0][0]
            check(f"snapshot_to_svg {label}: has entries", _b62enc(first_addr) in svg)
            check(f"snapshot_to_svg {label}: closes svg", svg.strip().endswith("</svg>"))
        finally:
            os.unlink(fname)

    print(f"\n  PASS={PASS}  FAIL={FAIL}  TOTAL={PASS+FAIL}")
    return FAIL == 0


if __name__ == "__main__":
    print("POGLS Bridge — S51")
    print("=" * 50)
    ok = _run_tests()
    import sys; sys.exit(0 if ok else 1)


# ── S53: SVG decode ───────────────────────────────────────────────────

import re as _re
import xml.etree.ElementTree as _ET

_B62_RE      = _re.compile(r'^[0-9A-Za-z]{11}$')
_SIG_ONLY_RE = _re.compile(r'^sig=([0-9A-Za-z]{11})$')

def svg_decode(svg_path: str) -> list[dict]:
    """
    Parse a pogls_bridge blueprint SVG → list of {addr, b62, hex, sig, sig_b62, ...}.

    Scans all <text> elements:
      - bare 11-char b62  → addr
      - "sig=XXXXXXXXXXX" → sig for the matching addr row
    Rows are 1:1 ordered in SVG, so zip alignment is exact.
    """
    tree = _ET.parse(svg_path)
    texts = [el.text.strip()
             for el in tree.findall(".//{http://www.w3.org/2000/svg}text")
             if el.text]

    addr_b62s: list[str] = []
    sig_b62s:  list[str] = []
    for t in texts:
        if _B62_RE.match(t):
            addr_b62s.append(t)
        else:
            m = _SIG_ONLY_RE.match(t)
            if m:
                sig_b62s.append(m.group(1))

    results = []
    for i, b62 in enumerate(addr_b62s):
        addr    = _b62dec(b62)
        sig_b62 = sig_b62s[i] if i < len(sig_b62s) else None
        entry   = {
            "addr":    addr,
            "b62":     b62,
            "hex":     _to_hex(addr),
            "sig_b62": sig_b62,
            "sig":     _b62dec(sig_b62) if sig_b62 else None,
        }
        entry.update({k: v for k, v in _decompose(addr).items()
                       if k not in ("addr", "b62", "hex", "bits")})
        results.append(entry)
    return results
