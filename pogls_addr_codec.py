"""
POGLS Address Codec
addr_core  : uint64
bridge     : hex 16 chars fixed
interface  : base62 11 chars fixed, zero-padded
alphabet   : "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
"""

# ── constants (never change) ──────────────────────────────────────────
ALPHABET   = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
BASE       = 62
B62_LEN    = 11          # ceil(log62(2^64)) = 10.75 → 11
HEX_LEN    = 16
UINT64_MAX = (1 << 64) - 1

# precompute decode table — O(1) lookup, built once
_DEC = {c: i for i, c in enumerate(ALPHABET)}

# ── core ──────────────────────────────────────────────────────────────
def encode(addr: int) -> str:
    """uint64 → base62 11-char fixed (zero-padded left)"""
    if not (0 <= addr <= UINT64_MAX):
        raise ValueError(f"addr out of uint64 range: {addr}")
    out = []
    n = addr
    for _ in range(B62_LEN):
        out.append(ALPHABET[n % BASE])
        n //= BASE
    return ''.join(reversed(out))          # big-endian, left-padded


def decode(s: str) -> int:
    """base62 11-char → uint64  (case-sensitive: A ≠ a)"""
    if len(s) != B62_LEN:
        raise ValueError(f"expected {B62_LEN} chars, got {len(s)}: {s!r}")
    n = 0
    for c in s:
        if c not in _DEC:
            raise ValueError(f"invalid char {c!r} — not in POGLS alphabet")
        n = n * BASE + _DEC[c]
    if n > UINT64_MAX:
        raise OverflowError(f"decoded value exceeds uint64: {n}")
    return n


def to_bridge(addr: int) -> str:
    """uint64 → hex 16-char fixed"""
    if not (0 <= addr <= UINT64_MAX):
        raise ValueError(f"addr out of range: {addr}")
    return f"{addr:016X}"


def from_bridge(h: str) -> int:
    """hex 16-char → uint64"""
    if len(h) != HEX_LEN:
        raise ValueError(f"expected {HEX_LEN} hex chars, got {len(h)}")
    return int(h, 16)


# ── round-trip helpers ────────────────────────────────────────────────
def addr_to_all(addr: int) -> dict:
    """uint64 → all 3 representations"""
    return {
        "core":      addr,
        "bridge":    to_bridge(addr),
        "interface": encode(addr),
    }


def interface_to_all(s: str) -> dict:
    addr = decode(s)
    return addr_to_all(addr)


# ── tests ─────────────────────────────────────────────────────────────
def _run_tests():
    PASS = FAIL = 0

    def check(label, ok):
        nonlocal PASS, FAIL
        PASS, FAIL = (PASS+1, FAIL) if ok else (PASS, FAIL+1)
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}")

    # boundary values
    cases = [
        0,
        1,
        UINT64_MAX,
        UINT64_MAX - 1,
        1 << 32,
        0xDEADBEEFCAFE0000,
        162,        # POGLS icosphere node count
        54,         # bridge nexus
        648,        # world 4n
        640,        # world 5n
    ]

    for addr in cases:
        b62    = encode(addr)
        back   = decode(b62)
        bridge = to_bridge(addr)
        back2  = from_bridge(bridge)

        check(f"round-trip core→b62→core  addr={addr}",  back  == addr)
        check(f"round-trip core→hex→core  addr={addr}",  back2 == addr)
        check(f"b62 length=11             addr={addr}",  len(b62)    == B62_LEN)
        check(f"hex length=16             addr={addr}",  len(bridge) == HEX_LEN)

    # case-sensitivity: A ≠ a
    b62_upper = encode(10)   # '10' encodes to some string with uppercase possible
    # manually mutate one char and confirm decode gives different result
    mangled = b62_upper[:10] + ('a' if b62_upper[-1] == 'A' else 'A')
    check("case-sensitive: mutated str decodes differently",
          decode(mangled) != decode(b62_upper))

    # sort order matches numeric order
    addrs_sorted  = sorted(cases)
    b62_sorted    = sorted(encode(a) for a in addrs_sorted)
    b62_from_sort = [encode(a) for a in addrs_sorted]
    check("sort order: base62 sort == numeric sort",
          b62_sorted == b62_from_sort)

    # zero-padding: addr=0 must be 11 zeros
    check("zero addr encodes to all-zeros padded",
          encode(0) == "00000000000")

    # addr=1 must be left-padded — 10 zeros then '1' = 11 chars total
    check("addr=1 is '00000000001'",
          encode(1) == "00000000001")

    # error handling
    try:
        decode("0000000000!")   # bad char
        check("bad char raises ValueError", False)
    except ValueError:
        check("bad char raises ValueError", True)

    try:
        decode("0000000")       # wrong length
        check("wrong length raises ValueError", False)
    except ValueError:
        check("wrong length raises ValueError", True)

    try:
        encode(-1)
        check("negative addr raises ValueError", False)
    except ValueError:
        check("negative addr raises ValueError", True)

    print(f"\n  PASS={PASS}  FAIL={FAIL}  TOTAL={PASS+FAIL}")
    return FAIL == 0


if __name__ == "__main__":
    print("POGLS Address Codec — round-trip tests")
    print("=" * 50)

    # show spec
    print(f"\nSpec:")
    print(f"  ALPHABET = {ALPHABET!r}")
    print(f"  B62_LEN  = {B62_LEN}")
    print(f"  HEX_LEN  = {HEX_LEN}")
    print(f"  UINT64MAX = {UINT64_MAX} (0x{UINT64_MAX:016X})")

    # show examples
    print(f"\nExamples:")
    for addr in [0, 1, 54, 162, 648, UINT64_MAX]:
        r = addr_to_all(addr)
        print(f"  {r['core']:>20}  hex={r['bridge']}  b62={r['interface']}")

    print(f"\nTests:")
    ok = _run_tests()
    import sys; sys.exit(0 if ok else 1)
