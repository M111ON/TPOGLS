#!/usr/bin/env python3
"""
pogls — POGLS v4 CLI
════════════════════
Usage:
  pogls encode <file>            → <file>.gpkt
  pogls decode <file>.gpkt       → original file
  pogls verify <file>            → encode→decode→diff
  pogls status [--ctx id]        → engine status (needs server)
  pogls llm    [--ctx id]        → LLM advisor report (needs server)
  pogls snap   [--ctx id]        → admin snapshot (needs server)

Env:
  POGLS_HOST   default: localhost
  POGLS_PORT   default: 8765
  POGLS_CTX    default: default
  POGLS_LIB    default: /mnt/c/TPOGLS/libpogls_v4.so
"""

import argparse, base64, hashlib, json, os, struct, sys, time
from pathlib import Path

try:
    import requests
except ImportError:
    print("ERROR: pip install requests", file=sys.stderr)
    sys.exit(1)

HOST    = os.environ.get("POGLS_HOST", "localhost")
PORT    = int(os.environ.get("POGLS_PORT", 8765))
CTX     = os.environ.get("POGLS_CTX",  "default")
BASE    = f"http://{HOST}:{PORT}"
TIMEOUT = 10
MAGIC   = b"GPKT"
VERSION = 1

def _die(msg, code=1):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(code)

def _ok(msg):   print(f"✓  {msg}")
def _warn(msg): print(f"⚠  {msg}")

def _req(method, path, **kw):
    url = f"{BASE}/{path.lstrip('/')}"
    try:
        return getattr(requests, method)(url, timeout=TIMEOUT, **kw)
    except requests.ConnectionError:
        _die(f"Cannot connect to server at {BASE}\n   Start: python3 rest_server_llm_patch.py")
    except requests.Timeout:
        _die(f"Server timeout ({TIMEOUT}s)")

def _try_engine():
    try:
        from pogls_engine import PoglsEngine
        return PoglsEngine
    except Exception:
        return None

# ── encode ───────────────────────────────────────────────────────────

def cmd_encode(args):
    src = Path(args.file)
    if not src.exists(): _die(f"File not found: {src}")

    data     = src.read_bytes()
    orig_sha = hashlib.sha256(data).digest()
    ctx_id   = args.ctx or CTX

    print(f"→  encoding {src.name}  ({len(data):,} bytes)  ctx={ctx_id}")

    engine_mode = False
    EngineClass = _try_engine()
    if EngineClass:
        try:
            with EngineClass() as eng:
                chunks = eng.encode(data)
                check  = eng.decode()
            if check != data:
                _die("Engine internal round-trip failed — aborting")
            engine_mode = True
            print(f"   engine: {chunks} chunks  (real geometric encode)")
        except Exception as e:
            _warn(f"Engine error ({e}) — falling back to wrapper mode")

    if not engine_mode:
        _warn("Engine not available — wrapper mode (no geometric encode)")

    out = Path(args.output) if args.output else src.with_suffix(".gpkt")
    payload = json.dumps({
        "ctx_id":      ctx_id,
        "source":      str(src),
        "orig_name":   src.name,
        "timestamp":   time.time(),
        "engine_mode": engine_mode,
        "data_b64":    base64.b64encode(data).decode(),
    }).encode()

    header = (MAGIC
              + struct.pack("BB", VERSION, 1 if engine_mode else 0)
              + struct.pack(">Q", len(data))
              + orig_sha)
    out.write_bytes(header + payload)

    _ok(f"Encoded → {out}  ({out.stat().st_size:,} bytes)")
    _ok(f"SHA256  = {orig_sha.hex()[:16]}…")
    _ok(f"Mode    = {'engine (real)' if engine_mode else 'wrapper'}")

# ── decode ───────────────────────────────────────────────────────────

def cmd_decode(args):
    src = Path(args.file)
    if not src.exists(): _die(f"File not found: {src}")
    if src.suffix != ".gpkt":
        _warn(f"Expected .gpkt, got {src.suffix!r} — proceeding anyway")

    raw = src.read_bytes()
    if raw[:4] != MAGIC: _die("Not a valid .gpkt file (bad magic)")

    version, flags = struct.unpack("BB", raw[4:6])
    orig_size,     = struct.unpack(">Q", raw[6:14])
    stored_sha     = raw[14:46]
    meta           = json.loads(raw[46:].decode())
    data           = base64.b64decode(meta["data_b64"])

    if len(data) != orig_size:
        _die(f"Size mismatch: expected {orig_size}, got {len(data)}")
    if hashlib.sha256(data).digest() != stored_sha:
        _die("Integrity check FAILED — SHA256 mismatch")

    out = Path(args.output or meta.get("orig_name", src.stem + ".out"))
    out.write_bytes(data)

    _ok(f"Decoded → {out}  ({len(data):,} bytes)")
    _ok(f"Integrity OK — SHA256 matches")
    mode = "engine" if meta.get("engine_mode") else "wrapper"
    print(f"   encoded with: {mode}")
    print(f"   source:       {meta.get('source','?')}")
    print(f"   timestamp:    {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(meta.get('timestamp',0)))}")

# ── verify ───────────────────────────────────────────────────────────

def cmd_verify(args):
    src = Path(args.file)
    if not src.exists(): _die(f"File not found: {src}")

    print(f"→  verifying {src.name}  (encode → decode → diff)")
    data     = src.read_bytes()
    orig_sha = hashlib.sha256(data).hexdigest()

    tmp_gpkt = src.with_suffix(".gpkt.tmp")
    tmp_out  = src.with_suffix(".out.tmp")

    class _EA:
        file=str(src); output=str(tmp_gpkt); ctx=args.ctx or CTX
    class _DA:
        file=str(tmp_gpkt); output=str(tmp_out)

    cmd_encode(_EA())
    cmd_decode(_DA())

    recovered = tmp_out.read_bytes()
    rec_sha   = hashlib.sha256(recovered).hexdigest()
    tmp_gpkt.unlink(missing_ok=True)
    tmp_out.unlink(missing_ok=True)

    print(f"\n   orig = {orig_sha[:32]}…")
    print(f"   recv = {rec_sha[:32]}…\n")

    if data == recovered:
        _ok(f"VERIFY PASS — bit-perfect round-trip ({len(data):,} bytes)")
    else:
        _die("VERIFY FAIL — data mismatch")

# ── status ───────────────────────────────────────────────────────────

def cmd_status(args):
    ctx_id = args.ctx or CTX
    r = _req("get", f"/{ctx_id}/status")
    if not r.ok: _die(f"Status {r.status_code}: {r.text[:200]}")
    s = r.json()
    print(f"\n  POGLS Status — ctx: {ctx_id}\n  {'─'*36}")
    for key, label in [
        ("epoch","Epoch"),("total_ops","Total ops"),("shat_stage","Shatter stage"),
        ("drift_active","Drift active"),("writes_frozen","Writes frozen"),
        ("shatter_count","Shatter count"),("lane_b_active","Lane B active"),
        ("qrpn_state","QRPN state"),("qrpn_fails","QRPN fails"),("gpu_fail_count","GPU fails")
    ]:
        if key in s:
            flag = "  ⚠" if key in ("writes_frozen","drift_active") and s[key] else ""
            print(f"  {label:<20} {s[key]}{flag}")
    print()

# ── llm ──────────────────────────────────────────────────────────────

def cmd_llm(args):
    ctx_id = args.ctx or CTX
    body   = {}
    if getattr(args, "snap_file", None):
        sp = Path(args.snap_file)
        if not sp.exists(): _die(f"Snap file not found: {sp}")
        body["snap"] = json.loads(sp.read_text())
    if getattr(args, "force", False):
        body["force"] = True

    r = _req("post", f"/{ctx_id}/llm_report", json=body)
    if not r.ok: _die(f"LLM {r.status_code}: {r.text[:200]}")
    d = r.json()

    conf = d.get("confidence", 0)
    bar  = "█" * int(conf * 20) + "░" * (20 - int(conf * 20))
    print(f"\n  LLM Advisor — ctx: {ctx_id}\n  {'─'*36}")
    print(f"  State key   : {d.get('state_key','?')}")
    print(f"  Action      : {d.get('action','?')}")
    print(f"  Confidence  : {conf:.2f}  [{bar}]")
    print(f"  From cache  : {d.get('from_cache','?')}")
    print(f"  Allowed     : {', '.join(d.get('allowed_actions',[]))}")

    rs = _req("get", "/llm_stats")
    if rs and rs.ok:
        st = rs.json(); c = st.get("cache", {})
        print(f"\n  LLM calls   : {st.get('llm_calls','?')}")
        print(f"  LLM errors  : {st.get('llm_errors','?')}")
        print(f"  Cache       : size={c.get('size','?')}  hits={c.get('total_hits','?')}")
    print()

# ── snap ─────────────────────────────────────────────────────────────

def cmd_snap(args):
    ctx_id = args.ctx or CTX
    r = _req("get", f"/{ctx_id}/snapshot")
    if not r.ok: _die(f"Snapshot {r.status_code}: {r.text[:200]}")
    print(json.dumps(r.json(), indent=2))

# ── main ─────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(prog="pogls", description="POGLS v4 CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
examples:
  pogls encode myfile.bin
  pogls decode myfile.gpkt
  pogls verify myfile.bin
  pogls status --ctx ctx_001
  pogls llm --force
  pogls llm --snap mock_snap.json

env:  POGLS_HOST  POGLS_PORT  POGLS_CTX  POGLS_LIB""")

    sub = p.add_subparsers(dest="cmd", required=True)

    pe = sub.add_parser("encode", help="encode file → .gpkt")
    pe.add_argument("file"); pe.add_argument("-o","--output"); pe.add_argument("--ctx")

    pd = sub.add_parser("decode", help="decode .gpkt → original")
    pd.add_argument("file"); pd.add_argument("-o","--output")

    pv = sub.add_parser("verify", help="encode→decode→diff round-trip")
    pv.add_argument("file"); pv.add_argument("--ctx")

    ps = sub.add_parser("status", help="engine status (needs server)")
    ps.add_argument("--ctx")

    pl = sub.add_parser("llm", help="LLM advisor report (needs server)")
    pl.add_argument("--ctx"); pl.add_argument("--force", action="store_true")
    pl.add_argument("--snap-file", dest="snap_file")

    pn = sub.add_parser("snap", help="admin snapshot JSON (needs server)")
    pn.add_argument("--ctx")

    args = p.parse_args()
    {"encode":cmd_encode,"decode":cmd_decode,"verify":cmd_verify,
     "status":cmd_status,"llm":cmd_llm,"snap":cmd_snap}[args.cmd](args)

if __name__ == "__main__":
    main()
