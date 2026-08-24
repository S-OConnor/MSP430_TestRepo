#!/usr/bin/env python3
"""Generate docs/img/config-read-timing.svg — the two accesses of the
CONFIG-readback path (adc168_config_cycle(): the init link check, and the
idle phase's once-a-second probe). Run from the repo root."""

CLK_W    = 13.0          # px per SCLK cycle (8 per byte)
AMP      = 20.0
PITCH    = 44.0
LABEL_W  = 132.0
LEFT     = 14.0
X0       = LEFT + LABEL_W

# ---- columns = the actual code steps, in order --------------------------
BYTE_W = 8 * CLK_W + 6
COLS = [
    dict(code="ADC_CS_LOW()",   kind="cs",   w=96.0),
    dict(code="RD \u2191", kind="rd",   w=64.0),
    dict(code="tx 0x10",  kind="byte", w=BYTE_W, mosi=0x10),
    dict(code="tx 0x41",  kind="byte", w=BYTE_W, mosi=0x41),
    dict(code="tx 0x00",  kind="byte", w=BYTE_W, mosi=0x00),
    dict(code="RD \u2191", kind="rd",   w=64.0),
    dict(code="tx 0x00",  kind="byte", w=BYTE_W, mosi=0x00, miso=0x04),
    dict(code="tx 0x00",  kind="byte", w=BYTE_W, mosi=0x00, miso=0x10),
    dict(code="tx 0x00",  kind="byte", w=BYTE_W, mosi=0x00, miso=0x40,
         miso_care=4),
]
TAIL = 26.0

xs = [X0]
for c in COLS:
    xs.append(xs[-1] + c["w"])
def cx0(i): return xs[i]
def cx1(i): return xs[i + 1]
RIGHT = xs[-1] + TAIL

# ---- lanes ---------------------------------------------------------------
Y_TITLE, Y_SUB = 32.0, 53.0
Y_FUNC, Y_FUNCLN, Y_CODE = 84.0, 92.0, 114.0
LANE0 = 134.0

LANES = [
    ("CLOCK",  "#0f766e", "P2.2 / UCB0CLK"),
    ("~CS",    "#64748b", "P1.4"),
    ("CONVST", "#94a3b8", "P2.6"),
    ("RD",     "#c2410c", "P4.2"),
    ("BUSY",   "#94a3b8", "P1.5"),
    ("MISO",   "#1d4ed8", "P1.7 / SDOA"),
    ("MOSI",   "#be123c", "P1.6 / SDI"),
]
IDX = {n: i for i, (n, _, _) in enumerate(LANES)}
COLOR = {n: c for n, c, _ in LANES}
def hi(n): return LANE0 + IDX[n] * PITCH
def lo(n): return hi(n) + AMP

Y_BOT  = lo("MOSI")
Y_NOTE = Y_BOT + 42
HEIGHT = Y_NOTE + 3 * 17 + 30
WIDTH  = RIGHT + 16

BG, GRID, DOT, INK, DIM = "#ffffff", "#eef2f5", "#8fa0b0", "#101418", "#67727e"
SANS = "'DejaVu Sans','Helvetica Neue',Arial,sans-serif"
MONO = "'DejaVu Sans Mono','SFMono-Regular',Consolas,monospace"

o = []
def add(s): o.append(s)
def esc(s): return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def text(xx, yy, s, size=11, fill=INK, anchor="middle", weight="400",
         family=MONO, halo=False):
    e = (' stroke="#ffffff" stroke-width="3.2" paint-order="stroke"'
         ' stroke-linejoin="round"') if halo else ""
    add(f'<text x="{xx:.1f}" y="{yy:.1f}" font-family="{family}" '
        f'font-size="{size}" font-weight="{weight}" fill="{fill}" '
        f'text-anchor="{anchor}"{e}>{esc(s)}</text>')

def line(x1, y1, x2, y2, stroke=INK, w=1.0, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
        f'stroke="{stroke}" stroke-width="{w}"{d}/>')

def poly(pts, stroke, w=2.0):
    p = " ".join(f"{a:.1f},{b:.1f}" for a, b in pts)
    add(f'<polyline points="{p}" fill="none" stroke="{stroke}" stroke-width="{w}" '
        f'stroke-linejoin="miter" stroke-linecap="butt"/>')

def hatch(xa, xb, name):
    add(f'<rect x="{xa:.1f}" y="{hi(name):.1f}" width="{xb-xa:.1f}" '
        f'height="{AMP:.1f}" fill="url(#xc)" stroke="#b9c4cf" stroke-width="0.8"/>')

# =========================================================================
add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH:.0f}" height="{HEIGHT:.0f}" '
    f'viewBox="0 0 {WIDTH:.0f} {HEIGHT:.0f}" role="img" '
    f'aria-label="Bus timing for the ADC168M102 CONFIG readback">')
add('<defs><pattern id="xc" width="6" height="6" patternUnits="userSpaceOnUse" '
    'patternTransform="rotate(45)"><rect width="6" height="6" fill="#dfe5ec"/>'
    '<line x1="0" y1="0" x2="0" y2="6" stroke="#c3ccd6" stroke-width="1.4"/>'
    '</pattern></defs>')
add(f'<rect width="{WIDTH:.0f}" height="{HEIGHT:.0f}" fill="{BG}"/>')

text(LEFT, Y_TITLE, "Reading the ADC CONFIG register — RD strobed against the burst",
     size=18, weight="700", anchor="start", family=SANS)
text(LEFT, Y_SUB, "adc168_config_cycle(): write CONFIG 0x1041, then clock the "
     "reply off SDOA — the init link check, and the idle phase once a second.  "
     "SCLK 0.5 MHz, CPOL=0/CPHA=1, MSB first.",
     size=11.5, fill=DIM, anchor="start", family=SANS)

# ---- function brackets + per-column code labels -------------------------
def fbracket(i0, i1, label):
    xa, xb = cx0(i0), cx1(i1)
    add(f'<rect x="{xa:.1f}" y="{LANE0-16:.1f}" width="{xb-xa:.1f}" '
        f'height="{Y_BOT-LANE0+34:.1f}" fill="#f6f8fa"/>')
    line(xa, Y_FUNCLN, xb, Y_FUNCLN, DIM, 1.0)
    line(xa, Y_FUNCLN - 5, xa, Y_FUNCLN, DIM, 1.0)
    line(xb, Y_FUNCLN - 5, xb, Y_FUNCLN, DIM, 1.0)
    text((xa + xb) / 2, Y_FUNC, label, size=12, weight="700", family=SANS)

fbracket(1, 4, "write_word(0x1041) \u2192 spi_burst_strobe(\u2026, RD)")
fbracket(5, 8, "read_word() \u2192 spi_burst_strobe(\u2026, RD)")

for i, c in enumerate(COLS):
    line(cx0(i), Y_CODE + 6, cx0(i), Y_BOT + 16, DOT, 1.0, dash="3 3")
    text((cx0(i) + cx1(i)) / 2, Y_CODE, c["code"], size=9.5, fill=INK)
line(cx1(len(COLS) - 1), Y_CODE + 6, cx1(len(COLS) - 1), Y_BOT + 16, DOT, 1.0, dash="3 3")

# ---- lane labels & baselines --------------------------------------------
for name, colr, pin in LANES:
    line(X0 - 4, lo(name), RIGHT, lo(name), GRID, 1.0)
    text(X0 - 12, hi(name) + 8, name, size=12.5, weight="700", fill=colr,
         anchor="end", family=SANS)
    text(X0 - 12, hi(name) + 20, pin, size=8.5, fill=DIM, anchor="end", family=SANS)

# ---- CLOCK: 8 gated pulses per spi_xfer() -------------------------------
poly([(X0, lo("CLOCK")), (RIGHT, lo("CLOCK"))], COLOR["CLOCK"])
for i, c in enumerate(COLS):
    if c["kind"] != "byte":
        continue
    pts = []
    for k in range(8):
        xa = cx0(i) + 3 + k * CLK_W
        pts += [(xa, lo("CLOCK")), (xa, hi("CLOCK")),
                (xa + CLK_W / 2, hi("CLOCK")), (xa + CLK_W / 2, lo("CLOCK"))]
    pts.append((cx0(i) + 3 + 8 * CLK_W, lo("CLOCK")))
    poly(pts, COLOR["CLOCK"], 1.7)

# ---- ~CS: falls in column 0, stays low ----------------------------------
xm = (cx0(0) + cx1(0)) / 2
poly([(X0, hi("~CS")), (xm - 5, hi("~CS")), (xm + 5, lo("~CS")), (RIGHT, lo("~CS"))],
     COLOR["~CS"])

# ---- CONVST / BUSY: never move ------------------------------------------
poly([(X0, lo("CONVST")), (RIGHT, lo("CONVST"))], COLOR["CONVST"])
text(cx0(1) + 8, hi("CONVST") + 11,
     "CONVST never moves here — a register access starts no conversion",
     size=9.5, fill=DIM, anchor="start", family=SANS, halo=True)
poly([(X0, lo("BUSY")), (RIGHT, lo("BUSY"))], COLOR["BUSY"])
text(cx0(1) + 8, hi("BUSY") + 11, "so BUSY never rises", size=9.5, fill=DIM,
     anchor="start", family=SANS, halo=True)

# ---- RD: rises just before the burst, released on the burst's 2nd rising SCLK
pts = [(X0, lo("RD"))]
for i, c in enumerate(COLS):
    if c["kind"] != "rd":
        continue
    x_up   = cx1(i) - 10                      # ~t1 before the first clock
    x_down = cx0(i + 1) + 3 + 2 * CLK_W       # 2nd rising CLOCK edge
    pts += [(x_up, lo("RD")), (x_up, hi("RD")),
            (x_down, hi("RD")), (x_down, lo("RD"))]
    text(x_down + 3, hi("RD") - 7, "\u25bc released on rising edge 2", size=9,
         fill=COLOR["RD"], anchor="start", family=SANS, halo=True)
pts.append((RIGHT, lo("RD")))
poly(pts, COLOR["RD"])

# ---- data lanes: real bit levels, hex value labelled above ---------------
def draw_byte(i, name, val, care=8):
    yh, yl, c = hi(name), lo(name), COLOR[name]
    x_start = cx0(i) + 3
    pts = []
    for k in range(care):
        b = (val >> (7 - k)) & 1
        yv = yh if b else yl
        pts += [(x_start + k * CLK_W, yv), (x_start + (k + 1) * CLK_W, yv)]
    poly(pts, c, 1.7)
    if care < 8:
        hatch(x_start + care * CLK_W, x_start + 8 * CLK_W, name)
    text(x_start + 4 * CLK_W, yh - 7, f"0x{val:02X}", size=10, weight="700",
         fill=c, family=MONO)

# MOSI: idle low everywhere except the six transferred bytes
poly([(X0, lo("MOSI")), (RIGHT, lo("MOSI"))], COLOR["MOSI"], 1.7)
for i, c in enumerate(COLS):
    if c["kind"] == "byte":
        draw_byte(i, "MOSI", c["mosi"])

# MISO: undriven / not decoded until the read access
hatch(X0, cx0(6) + 3, "MISO")
for i, c in enumerate(COLS):
    if c.get("miso") is not None:
        draw_byte(i, "MISO", c["miso"], c.get("miso_care", 8))
hatch(cx0(8) + 3 + 8 * CLK_W, RIGHT, "MISO")
text((cx0(0) + cx0(6)) / 2, hi("MISO") + 13,
     "return value discarded", size=9.5, fill=DIM, family=SANS, halo=True)

# ---- notes ---------------------------------------------------------------
notes = [
 "Dotted verticals are byte boundaries inside ONE burst: the three bytes of an access are clocked back to "
 "back, 24 unbroken SCLK cycles. Between accesses the clock is parked low, which is why RD can be moved at all.",
 "RD idles low. spi_burst_strobe() raises it a few hundred ns before the burst's first rising CLOCK edge (t1), holds it "
 "across that edge, and releases it on the SECOND rising edge — a high time of about one CLOCK period (t3).",
 "read_word() strips the frame's 2 leading indicator bits and trailing zeros: "
 "0x04 0x10 0x40 → 0x1041. The last 4 clocks are padding past the 20-bit frame.",
]
for i, n in enumerate(notes):
    text(LEFT, Y_NOTE + i * 17, "• " + n, size=10.5, fill=DIM, anchor="start",
         family=SANS)
ky = Y_NOTE + len(notes) * 17
add(f'<rect x="{LEFT+6:.1f}" y="{ky-9:.1f}" width="22" height="11" fill="url(#xc)" '
    f'stroke="#b9c4cf" stroke-width="0.8"/>')
text(LEFT + 34, ky, "line is undriven or its value is not used", size=10.5, fill=DIM,
     anchor="start", family=SANS)

add("</svg>")

import os
os.makedirs("docs/img", exist_ok=True)
open("docs/img/config-read-timing.svg", "w").write("\n".join(o))
print("wrote docs/img/config-read-timing.svg", WIDTH, HEIGHT)
