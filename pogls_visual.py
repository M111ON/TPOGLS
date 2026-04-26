"""
POGLS Visual Codec — v1
Grid 8x8 (lossless) + Node overlay (POGLS identity)
Output: SVG string — no external deps
"""

# ── constants ─────────────────────────────────────────────────────────
GRID_N      = 8          # 8×8 = 64 bits
CELL_PX     = 24         # pixel per cell
PAD         = 16         # outer padding
OVERLAY_R   = 6          # overlay dot radius

# spoke → hue (6 spokes = 60° apart, HSL)
_SPOKE_COLOR = [
    "#E74C3C",  # 0 = red
    "#E67E22",  # 1 = orange
    "#F1C40F",  # 2 = yellow
    "#2ECC71",  # 3 = green
    "#3498DB",  # 4 = blue
    "#9B59B6",  # 5 = purple
]

# world → shape id (rendered via SVG)
_WORLD_SHAPE = {0: "circle", 1: "triangle", 2: "square"}
_WORLD_LABEL = {0: "4n", 1: "5n", 2: "6n"}

# background / bit colors
_BG      = "#0D0D0D"
_BIT_OFF = "#1A1A2E"
_BIT_ON  = "#E8E8E8"
_BORDER  = "#2A2A4A"

# ── decompose ─────────────────────────────────────────────────────────
def decompose(addr: int) -> dict:
    """addr uint64 → POGLS geometric components"""
    spoke  = addr % 6
    slot   = (addr >> 6)  % 27
    world  = (addr >> 11) % 3
    layer  = (addr >> 13) % 4          # onion layer 0-3
    intensity = int((slot / 26) * 255) if slot > 0 else 0
    return {
        "addr":      addr,
        "spoke":     spoke,
        "slot":      slot,
        "world":     world,
        "layer":     layer,
        "intensity": intensity,
        "bits":      [(addr >> (63 - i)) & 1 for i in range(64)],
    }

# ── SVG builder ───────────────────────────────────────────────────────
def _overlay_shape(cx, cy, shape, color, opacity=0.85):
    r = OVERLAY_R
    if shape == "circle":
        return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{color}" opacity="{opacity}"/>'
    if shape == "triangle":
        pts = f"{cx},{cy-r} {cx-r},{cy+r} {cx+r},{cy+r}"
        return f'<polygon points="{pts}" fill="{color}" opacity="{opacity}"/>'
    # square
    return f'<rect x="{cx-r}" y="{cy-r}" width="{r*2}" height="{r*2}" fill="{color}" opacity="{opacity}"/>'

def render_svg(addr: int) -> str:
    d    = decompose(addr)
    bits = d["bits"]
    spoke_col = _SPOKE_COLOR[d["spoke"]]
    shape     = _WORLD_SHAPE[d["world"]]
    world_lbl = _WORLD_LABEL[d["world"]]

    W = GRID_N * CELL_PX + PAD * 2    # total width
    H = W + 48                         # extra row for label

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}">',
        f'<rect width="{W}" height="{H}" fill="{_BG}"/>',
        # spoke-color border ring
        f'<rect x="2" y="2" width="{W-4}" height="{H-4}" '
        f'rx="6" fill="none" stroke="{spoke_col}" stroke-width="2" opacity="0.6"/>',
    ]

    # ── 8×8 bit grid ─────────────────────────────────────────────────
    for row in range(GRID_N):
        for col in range(GRID_N):
            bit_idx = row * GRID_N + col
            bit     = bits[bit_idx]
            x = PAD + col * CELL_PX
            y = PAD + row * CELL_PX
            fill = _BIT_ON if bit else _BIT_OFF
            lines.append(
                f'<rect x="{x+1}" y="{y+1}" width="{CELL_PX-2}" height="{CELL_PX-2}" '
                f'rx="2" fill="{fill}"/>'
            )

    # ── node overlay: center ─────────────────────────────────────────
    cx = W // 2
    cy = PAD + GRID_N * CELL_PX // 2
    # outer glow ring
    lines.append(
        f'<circle cx="{cx}" cy="{cy}" r="{OVERLAY_R+6}" '
        f'fill="none" stroke="{spoke_col}" stroke-width="1.5" opacity="0.35"/>'
    )
    lines.append(_overlay_shape(cx, cy, shape, spoke_col))

    # intensity bar (below grid)
    bar_y  = PAD + GRID_N * CELL_PX + 8
    bar_w  = GRID_N * CELL_PX
    fill_w = int(bar_w * d["intensity"] / 255) if d["intensity"] else 0
    lines += [
        f'<rect x="{PAD}" y="{bar_y}" width="{bar_w}" height="4" rx="2" fill="{_BIT_OFF}"/>',
        f'<rect x="{PAD}" y="{bar_y}" width="{fill_w}" height="4" rx="2" fill="{spoke_col}"/>',
    ]

    # ── label row ────────────────────────────────────────────────────
    label_y = bar_y + 16
    from pogls_addr_codec import encode, to_bridge
    b62 = encode(addr)
    hex_s = to_bridge(addr)
    lines += [
        f'<text x="{PAD}" y="{label_y}" font-family="monospace" font-size="9" fill="#888">',
        f'  b62={b62}  spoke={d["spoke"]}  world={world_lbl}  slot={d["slot"]}',
        f'</text>',
        f'<text x="{PAD}" y="{label_y+11}" font-family="monospace" font-size="9" fill="#555">',
        f'  0x{hex_s}',
        f'</text>',
    ]

    lines.append('</svg>')
    return '\n'.join(lines)

# ── batch render ──────────────────────────────────────────────────────
def render_multi_svg(addrs: list, cols: int = 4) -> str:
    """render multiple addr cards in a grid layout"""
    from pogls_addr_codec import encode
    W_card = GRID_N * CELL_PX + PAD * 2
    H_card = W_card + 48
    GAP    = 8
    rows   = (len(addrs) + cols - 1) // cols
    W_total = cols * (W_card + GAP) + GAP
    H_total = rows * (H_card + GAP) + GAP

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W_total}" height="{H_total}">',
        f'<rect width="{W_total}" height="{H_total}" fill="#080808"/>',
    ]
    for i, addr in enumerate(addrs):
        row_i = i // cols
        col_i = i % cols
        tx = GAP + col_i * (W_card + GAP)
        ty = GAP + row_i * (H_card + GAP)
        inner = render_svg(addr)
        # wrap in group with translate
        inner_body = '\n'.join(inner.split('\n')[1:-1])  # strip outer svg tags
        lines.append(f'<g transform="translate({tx},{ty})">')
        lines.append(inner_body)
        lines.append('</g>')
    lines.append('</svg>')
    return '\n'.join(lines)

# ── tests ─────────────────────────────────────────────────────────────
def _run_tests():
    PASS = FAIL = 0
    def check(label, ok):
        nonlocal PASS, FAIL
        PASS, FAIL = (PASS+1, FAIL) if ok else (PASS, FAIL+1)
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}")

    UINT64_MAX = (1 << 64) - 1
    cases = [0, 1, 54, 162, 648, 640, 654, UINT64_MAX, 0xDEADBEEFCAFEBABE]

    for addr in cases:
        d = decompose(addr)
        check(f"decompose spoke in 0-5       addr={addr}", 0 <= d["spoke"] <= 5)
        check(f"decompose slot  in 0-26      addr={addr}", 0 <= d["slot"]  <= 26)
        check(f"decompose world in 0-2       addr={addr}", 0 <= d["world"] <= 2)
        check(f"decompose bits len=64        addr={addr}", len(d["bits"]) == 64)
        check(f"decompose bits are 0/1       addr={addr}", all(b in (0,1) for b in d["bits"]))

        # bit reconstruction must round-trip
        reconstructed = sum(b << (63 - i) for i, b in enumerate(d["bits"]))
        check(f"bits round-trip to addr      addr={addr}", reconstructed == addr)

        svg = render_svg(addr)
        check(f"SVG is non-empty             addr={addr}", len(svg) > 100)
        check(f"SVG has grid rects           addr={addr}", svg.count('<rect') >= 64)
        check(f"SVG has spoke color          addr={addr}",
              _SPOKE_COLOR[d["spoke"]] in svg)
        check(f"SVG valid open/close tags    addr={addr}",
              svg.strip().startswith('<svg') and svg.strip().endswith('</svg>'))

    # decompose known values
    d54 = decompose(54)
    check("addr=54 spoke = 54%6 = 0",   d54["spoke"] == 0)
    check("addr=162 spoke = 162%6 = 0", decompose(162)["spoke"] == 0)
    check("addr=648 spoke = 648%6 = 0", decompose(648)["spoke"] == 0)
    # 54, 162, 648 all divisible by 6 → all spoke 0 (nexus property)
    check("nexus numbers all spoke=0",
          all(decompose(n)["spoke"] == 0 for n in [54, 162, 648]))

    print(f"\n  PASS={PASS}  FAIL={FAIL}  TOTAL={PASS+FAIL}")
    return FAIL == 0


if __name__ == "__main__":
    import os
    print("POGLS Visual Codec — tests + render")
    print("=" * 50)

    ok = _run_tests()

    # render sample cards
    sacred = [0, 1, 54, 162, 648, 640, 654, 0xDEADBEEFCAFEBABE]
    svg_single = render_svg(54)
    svg_multi  = render_multi_svg(sacred, cols=4)

    out_dir = "/mnt/user-data/outputs"
    os.makedirs(out_dir, exist_ok=True)
    with open(f"{out_dir}/pogls_card_54.svg", "w") as f:
        f.write(svg_single)
    with open(f"{out_dir}/pogls_cards_sacred.svg", "w") as f:
        f.write(svg_multi)

    print(f"\nRendered:")
    print(f"  pogls_card_54.svg      (single card, addr=54 nexus)")
    print(f"  pogls_cards_sacred.svg (8 sacred numbers grid)")
    import sys; sys.exit(0 if ok else 1)
