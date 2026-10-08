#!/usr/bin/env python3
"""Spike V1 (throwaway): does the dimension colour rule of the view plan
section 5.2 give distinguishable colours?

hue(n) = arcs(frac(n * 0.6180339887)), arcs = the open OKLCH hue arcs left
after the five reserved bands, concatenated by arc length in hue order.
band(n) = vivid for the first pass of PASS dimensions, then deep, then pale,
then round again.  dash(n) = n mod 5, glyph(n) = n mod 7 (co-prime periods,
so the pair repeats only every 35).

Usage: v1_dimension_colours.py OUTDIR
"""

import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from colour_common import (de2000_rgb, deok_rgb, hexrgb, oklch_to_srgb,
                           simulate, srgb_to_oklch, to8)

PHI_CONJ = 0.6180339887498949
COUNT = 40
PASS = 13  # Fibonacci, so the first pass of hues is as even as golden gets

# OKLCH hue bands (degrees), from the hues of the colours the tree already
# spends, widened to the colour family the plan names.  Anchors measured:
#   red      Disagreement FF7A6B 28.3
#   gold     FFD700 95.3, amber F59E0B 70.1, focus 88.5, Illustration 76.0
#   green    Quotation 7FE0A8 157.1 (pure sRGB green is 142)
#   cyan/blue theme accent 237.6, 38BDF8 232.7, Comment 7FB2FF 258.4
#             (pure sRGB cyan is 195)
#   purple/violet Authorship B98CFF 300.4, Dimension AA00FF 307.0,
#             xanalink magenta FF00FF 328.4
RESERVED = {
    "red": (12.0, 44.0),
    "gold/amber": (58.0, 108.0),
    "green": (130.0, 168.0),
    "cyan/blue": (192.0, 270.0),
    "purple/violet": (288.0, 342.0),
}
# The complement, in the order u in [0,1) walks them.
OPEN_ARCS = [(44.0, 58.0), (108.0, 130.0), (168.0, 192.0), (270.0, 288.0),
             (342.0, 372.0)]

# (name, L, C): constant C chosen at or under the least in-gamut chroma over
# every open hue at that L (0.128 at L .75, 0.103 at .60, 0.058 at .88), so
# no hue is gamut-clipped and a band is one lightness and one chroma.
BANDS = [("vivid", 0.75, 0.125), ("deep", 0.60, 0.100), ("pale", 0.88, 0.055)]

DASHES = [("solid", []), ("dash", [10, 6]), ("dot", [2, 5]),
          ("dash-dot", [10, 4, 2, 4]), ("long-short", [16, 4, 5, 4])]
GLYPHS = ["circle", "triangle", "square", "diamond", "plus", "cross", "star"]

SURFACE = np.array([0.12, 0.14, 0.18])  # theme.hpp ThemeColours::surface


def hue_of(u):
    total = sum(b - a for a, b in OPEN_ARCS)
    x = u * total
    for a, b in OPEN_ARCS:
        if x < b - a:
            return (a + x) % 360
        x -= b - a
    return OPEN_ARCS[-1][1] % 360


def band_of(n, scheme):
    if scheme == "pass":
        return (n // PASS) % 3
    return n % 3  # the review's interleave, for comparison


def colours(scheme="pass"):
    out = []
    for n in range(COUNT):
        u = (n * PHI_CONJ) % 1.0
        h = hue_of(u)
        bname, L, C = BANDS[band_of(n, scheme)]
        rgb, ok = oklch_to_srgb(L, C, h)
        out.append(dict(n=n, u=u, h=h, band=bname, L=L, C=C, rgb=rgb,
                        gamut=ok, dash=n % len(DASHES),
                        glyph=n % len(GLYPHS)))
    return out


def in_reserved(h):
    for name, (a, b) in RESERVED.items():
        if a <= h < b:
            return name
    return None


def edge_gap(c):
    h = srgb_to_oklch(np.array(to8(c["rgb"])) / 255.0)[2]
    return min(min(abs(h - a), abs(h - b), 360 - abs(h - a), 360 - abs(h - b))
               for a, b in RESERVED.values())


def font(size):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",):
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            pass
    return ImageFont.load_default()


def strip(cols, path, title, transform=None):
    sw, sh, pad = 64, 64, 4
    per_row = 20
    rows = math.ceil(len(cols) / per_row)
    W = per_row * (sw + pad) + pad
    H = 40 + rows * (sh + 22 + pad) + pad
    img = Image.new("RGB", (W, H), to8(SURFACE))
    d = ImageDraw.Draw(img)
    d.text((pad, 8), title, fill=(240, 240, 240), font=font(16))
    f = font(12)
    for c in cols:
        r, k = divmod(c["n"], per_row)
        x = pad + k * (sw + pad)
        y = 40 + r * (sh + 22 + pad)
        rgb = c["rgb"] if transform is None else transform(c["rgb"])
        d.rectangle([x, y, x + sw, y + sh], fill=to8(rgb))
        d.text((x + 2, y + sh + 3), f"{c['n']} {c['band'][0]}",
               fill=(230, 230, 230), font=f)
    img.save(path)


def dashed_line(d, x0, x1, y, pattern, colour, width):
    if not pattern:
        d.line([x0, y, x1, y], fill=colour, width=width)
        return
    x, i, on = x0, 0, True
    while x < x1:
        seg = pattern[i % len(pattern)]
        if on:
            d.line([x, y, min(x + seg, x1), y], fill=colour, width=width)
        x += seg
        i += 1
        on = not on


def glyph(d, kind, cx, cy, r, colour):
    if kind == "circle":
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=colour)
    elif kind == "triangle":
        d.polygon([(cx, cy - r), (cx + r, cy + r), (cx - r, cy + r)],
                  fill=colour)
    elif kind == "square":
        d.rectangle([cx - r, cy - r, cx + r, cy + r], fill=colour)
    elif kind == "diamond":
        d.polygon([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)],
                  fill=colour)
    elif kind == "plus":
        d.rectangle([cx - r, cy - r / 3, cx + r, cy + r / 3], fill=colour)
        d.rectangle([cx - r / 3, cy - r, cx + r / 3, cy + r], fill=colour)
    elif kind == "cross":
        d.line([cx - r, cy - r, cx + r, cy + r], fill=colour, width=3)
        d.line([cx - r, cy + r, cx + r, cy - r], fill=colour, width=3)
    elif kind == "star":
        pts = []
        for i in range(10):
            a = -math.pi / 2 + i * math.pi / 5
            rr = r if i % 2 == 0 else r * 0.45
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        d.polygon(pts, fill=colour)


def cue_strip(cols, path):
    rowh, W = 26, 520
    img = Image.new("RGB", (W, 40 + rowh * len(cols)), to8(SURFACE))
    d = ImageDraw.Draw(img)
    f = font(13)
    d.text((8, 10), "V1 second cue: dash = n mod 5, glyph = n mod 7",
           fill=(240, 240, 240), font=f)
    for c in cols:
        y = 40 + c["n"] * rowh + rowh // 2
        col = to8(c["rgb"])
        d.text((8, y - 8), f"{c['n']:2d}", fill=(230, 230, 230), font=f)
        glyph(d, GLYPHS[c["glyph"]], 48, y, 7, col)
        dashed_line(d, 64, 330, y, DASHES[c["dash"]][1], col, 3)
        d.text((340, y - 8),
               f"{DASHES[c['dash']][0]} / {GLYPHS[c['glyph']]}",
               fill=(230, 230, 230), font=f)
    img.save(path)


def table(cols, metric, transform=None):
    rgbs = [c["rgb"] if transform is None else transform(c["rgb"])
            for c in cols]
    rows = []
    for i, c in enumerate(cols):
        prev = metric(rgbs[i], rgbs[i - 1]) if i > 0 else None
        nxt = metric(rgbs[i], rgbs[i + 1]) if i + 1 < len(cols) else None
        best, arg = math.inf, -1
        for j in range(len(cols)):
            if j != i:
                v = metric(rgbs[i], rgbs[j])
                if v < best:
                    best, arg = v, j
        rows.append((prev, nxt, best, arg))
    return rows


def summary(cols, label):
    lines = [f"[{label}]"]
    for name, tf in (("normal", None),
                     ("deuteranopia", lambda c: simulate(c, "deuteranopia")),
                     ("protanopia", lambda c: simulate(c, "protanopia")),
                     ("tritanopia", lambda c: simulate(c, "tritanopia"))):
        t = table(cols, de2000_rgb, tf)
        neigh = [v for r in t for v in r[:2] if v is not None]
        nearest = [r[2] for r in t]
        worst = min(range(len(t)), key=lambda i: t[i][2])
        weak = [f"{i - 1}-{i}:{t[i][0]:.1f}" for i in range(1, len(t))
                if t[i][0] < 10]
        lines.append(
            f"  {name:13s} min neighbour dE00 {min(neigh):6.2f}  "
            f"min nearest dE00 {min(nearest):6.2f} "
            f"(#{worst}~#{t[worst][3]})  pairs nearest<5: "
            f"{sum(1 for i, r in enumerate(t) if r[2] < 5 and i < r[3])}  "
            f"<10: {sum(1 for i, r in enumerate(t) if r[2] < 10 and i < r[3])}")
        lines.append(f"    neighbours in order under dE00 10: "
                     f"{', '.join(weak) or 'none'}")
    return lines


def main():
    global PASS
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    cols = colours("pass")

    strip(cols, out / "v1_strip.png",
          f"V1 dimension colours n=0..39, OKLCH, pass={PASS} "
          "(v vivid, d deep, p pale)")
    for kind in ("deuteranopia", "protanopia", "tritanopia"):
        strip(cols, out / f"v1_strip_{kind}.png",
              f"V1 through {kind} (Machado 2009, severity 1.0)",
              lambda c, k=kind: simulate(c, k))
    cue_strip(cols, out / "v1_cues.png")

    report = []
    report.append("Reserved OKLCH hue bands: " + ", ".join(
        f"{k} [{a:g},{b:g})" for k, (a, b) in RESERVED.items()))
    report.append("Open arcs: " + ", ".join(
        f"[{a:g},{b % 360 if b > 360 else b:g})" for a, b in OPEN_ARCS)
        + f"  total {sum(b - a for a, b in OPEN_ARCS):g} deg")
    report.append("Bands: " + ", ".join(
        f"{n} L={L} C={C}" for n, L, C in BANDS))
    t = table(cols, de2000_rgb)
    tok = table(cols, deok_rgb)
    report.append("")
    report.append(" n    u      hue  band  hex      gamut reserved?  "
                  "dE00prev dE00next nearest(dE00) dOKnearest dash/glyph")
    bad = 0
    for c, r, ro in zip(cols, t, tok):
        # Measured on the 8-bit colour as drawn: n = 0 lands on u = 0, the
        # arc's closed end, which in floating point is 43.99999 -- inside
        # the red band by rounding alone.
        L, C, h = srgb_to_oklch(np.array(to8(c["rgb"])) / 255.0)
        res = in_reserved(h)
        bad += res is not None
        hexs = "#%02X%02X%02X" % to8(c["rgb"])
        fmt = lambda v: "   -  " if v is None else f"{v:6.2f}"
        report.append(
            f"{c['n']:2d} {c['u']:.4f} {c['h']:6.1f} {c['band']:5s} {hexs} "
            f"{'yes' if c['gamut'] else 'NO':5s} {res or '-':10s} "
            f"{fmt(r[0])}   {fmt(r[1])}   {r[2]:6.2f} (#{r[3]:2d})  "
            f"{ro[2]:.4f}    {DASHES[c['dash']][0]}/{GLYPHS[c['glyph']]}")
    close = [(i, j) for i in range(COUNT) for j in range(i + 1, COUNT)
             if de2000_rgb(cols[i]["rgb"], cols[j]["rgb"]) < 5]
    report.append(
        f"Pairs under dE00 5 (normal vision): {len(close)}; sharing dash "
        f"{sum(cols[i]['dash'] == cols[j]['dash'] for i, j in close)}, "
        f"sharing glyph "
        f"{sum(cols[i]['glyph'] == cols[j]['glyph'] for i, j in close)}, "
        f"sharing both "
        f"{sum(cols[i]['dash'] == cols[j]['dash'] and cols[i]['glyph'] == cols[j]['glyph'] for i, j in close)}"
        f"; index gaps {sorted(set(j - i for i, j in close))}")
    report.append(f"Colours whose measured OKLCH hue (8-bit, as drawn) falls "
                  f"in a reserved band: {bad}; nearest approach to a band "
                  f"edge: {min(edge_gap(c) for c in cols):.2f} deg")
    report.append("")
    report += summary(cols, f"pass scheme, PASS={PASS}")
    report.append("")
    report.append("Comparison schemes (not rendered):")
    keep = PASS
    for p in (8, 21):
        PASS = p
        report += summary(colours("pass"), f"pass scheme, PASS={p}")
    PASS = keep
    report += summary(colours("mod3"), "band = n mod 3 (review 04 §2.3)")
    text = "\n".join(report)
    (out / "v1_report.txt").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
