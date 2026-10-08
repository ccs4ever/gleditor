#!/usr/bin/env python3
"""Spike V1b (throwaway): the dimension colour rule of the view plan section
5.2, re-run with the frame inspector's corrections.

hue(n)  = arcs(frac(n * 0.6180339887)); arcs = the open OKLCH hue arcs left
          after the reserved families (measured from the tree, plus a guard
          margin), concatenated by arc length in hue order.
band(n) = n mod 3   (vivid, deep, pale; review 04 section 2.3)
dash(n) = n mod 6   (not a Fibonacci number)
glyph(n)= n mod 7   (coprime with 6, so the pair repeats every 42)

Usage: v1_dimension_colours.py OUTDIR
Writes OUTDIR/ (core-tier reserved bands, the headline run) and
OUTDIR/all_tiers/ (every measured colour reserved, for comparison).
"""

import colorsys
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from colour_common import (de2000_rgb, deok_rgb, hexrgb, max_chroma,
                           oklch_to_srgb, simulate, srgb_to_oklch, to8)
from reserved_colours import RESERVED_COLOURS

PHI_CONJ = 0.6180339887498949
COUNT = 40
GUARD = 7.0  # degrees each side of a family's outermost member (brief: 6-8)

# (name, L, C).  C is chosen after the gamut check in choose_chroma().
BAND_L = [("vivid", 0.75), ("deep", 0.60), ("pale", 0.80)]
PALE_C_WANTED = 0.08

DASHES = [("solid", []), ("dash", [10, 6]), ("dot", [2, 5]),
          ("dash-dot", [10, 4, 2, 4]), ("long-short", [16, 4, 5, 4]),
          ("dash-dot-dot", [10, 4, 2, 4, 2, 4])]
GLYPHS = ["circle", "triangle", "square", "diamond", "plus", "hexagon",
          "ring"]
assert math.gcd(len(DASHES), len(GLYPHS)) == 1

SURFACE = np.array([0.12, 0.14, 0.18])  # theme.hpp ThemeColours::surface

# Review 04 section 2.2 bands, in sRGB hue degrees as the review states them.
REVIEW_BANDS_SRGB = [(350, 20), (28, 58), (140, 162), (190, 222),
                     (258, 322)]


def colour_rgb(c):
    return np.array(c) if isinstance(c, tuple) else hexrgb(c)


def measured(tier):
    """[(family, name, source, hue, L, C)] for the chosen tier."""
    out = []
    for fam, t, name, src, c in RESERVED_COLOURS:
        if tier == "core" and t != "core":
            continue
        L, C, h = srgb_to_oklch(colour_rgb(c))
        out.append((fam, name, src, h, L, C, c))
    return out


def family_bands(members):
    """Per family: (lo, hi) in unwrapped degrees, hi may exceed 360."""
    fams = {}
    for fam, _n, _s, h, *_ in members:
        fams.setdefault(fam, []).append(h)
    bands = {}
    for fam, hs in fams.items():
        # A family that straddles 0 (pink 0.2 with magenta 354) is unwrapped
        # by lifting the small hues, only when that makes the span shorter.
        lifted = [h + 360 if h < 90 and max(hs) > 270 else h for h in hs]
        bands[fam] = (min(lifted), max(lifted))
    return bands


def open_arcs_from(bands, guard):
    """Complement of the guarded bands on the circle, as (a, b) with b > a,
    a in [0,360), b may exceed 360 when the arc wraps."""
    ivs = sorted(((lo - guard) % 360, (lo - guard) % 360 + (hi - lo) + 2 * guard)
                 for lo, hi in bands.values())
    # Merge, including wrap-around.
    merged = []
    for a, b in ivs:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    if len(merged) > 1 and merged[-1][1] - 360 >= merged[0][0]:
        merged[0] = (merged[-1][0] - 360, max(merged[0][1],
                                              merged[-1][1] - 360))
        merged.pop()
    arcs = []
    for i, (a, b) in enumerate(merged):
        na = merged[(i + 1) % len(merged)][0]
        if na <= b:
            na += 360
        arcs.append((b % 360, b % 360 + (na - b)))
    return arcs, merged


def review_arcs():
    """Review 04's bands converted to OKLCH hue (HSV S .8 V .95), then
    complemented with no extra guard (the review has none)."""
    def conv(h):
        r, g, b = colorsys.hsv_to_rgb(h / 360, 0.8, 0.95)
        return srgb_to_oklch(np.array([r, g, b]))[2]
    bands = {}
    for i, (a, b) in enumerate(REVIEW_BANDS_SRGB):
        lo, hi = conv(a), conv(b)
        if hi < lo:
            hi += 360
        bands[f"review{i}"] = (lo, hi)
    arcs, merged = open_arcs_from(bands, 0.0)
    return arcs, merged


def hue_of(u, arcs):
    total = sum(b - a for a, b in arcs)
    x = u * total
    for a, b in arcs:
        if x < b - a:
            return (a + x) % 360
        x -= b - a
    return arcs[-1][1] % 360


def choose_chroma(arcs):
    """Largest 3-decimal chroma that is in gamut at every open hue, per L."""
    out = []
    for name, L in BAND_L:
        lo = math.inf
        for a, b in arcs:
            for k in range(0, 101):
                lo = min(lo, max_chroma(L, a + (b - a) * k / 100))
        cap = math.floor(lo * 1000) / 1000
        C = min(cap, PALE_C_WANTED) if name == "pale" else cap
        out.append((name, L, C, cap))
    return out


def make_colours(arcs, bands):
    out = []
    for n in range(COUNT):
        u = (n * PHI_CONJ) % 1.0
        h = hue_of(u, arcs)
        bname, L, C, _cap = bands[n % 3]
        rgb, ok = oklch_to_srgb(L, C, h)
        out.append(dict(n=n, u=u, h=h, band=bname, L=L, C=C, rgb=rgb,
                        gamut=ok, dash=n % len(DASHES),
                        glyph=n % len(GLYPHS)))
    return out


def font(size):
    try:
        return ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
    except OSError:
        return ImageFont.load_default()


def ref_swatches(members):
    """Reserved colours, one per distinct family+hue, ordered by family
    then hue: (label, rgb)."""
    seen, out = set(), []
    for fam, name, _s, h, _L, _C, c in sorted(
            members, key=lambda m: (m[0], m[3])):
        key = (fam, round(h))
        if key in seen:
            continue
        seen.add(key)
        out.append((f"{fam[:3]} {h:.0f}", colour_rgb(c)))
    return out


def strip(cols, members, merged, path, title, transform=None):
    tf = (lambda c: c) if transform is None else transform
    sw, sh, pad, per_row = 64, 64, 4, 20
    W = per_row * (sw + pad) + pad
    refs = ref_swatches(members)
    rsw, rsh = 40, 28
    ref_per_row = (W - pad) // (rsw + pad)
    ref_rows = math.ceil(len(refs) / ref_per_row)
    ref_h = 18 + ref_rows * (rsh + 16 + pad)
    ruler_h = 46
    rows = math.ceil(len(cols) / per_row)
    H = 34 + ref_h + ruler_h + rows * (sh + 22 + pad) + pad
    img = Image.new("RGB", (W, H), to8(SURFACE))
    d = ImageDraw.Draw(img)
    d.text((pad, 8), title, fill=(240, 240, 240), font=font(16))
    f, fs = font(12), font(10)
    y0 = 34
    d.text((pad, y0), "Reserved colours in the tree (family hue; OKLCH "
           "degrees)", fill=(200, 200, 200), font=f)
    y0 += 16
    for i, (lab, rgb) in enumerate(refs):
        r, k = divmod(i, ref_per_row)
        x = pad + k * (rsw + pad)
        y = y0 + r * (rsh + 16 + pad)
        d.rectangle([x, y, x + rsw, y + rsh], fill=to8(tf(rgb)))
        d.text((x, y + rsh + 2), lab, fill=(220, 220, 220), font=fs)
    y0 += ref_rows * (rsh + 16 + pad)
    # Hue ruler: dimmed where reserved (with guard), tick per dimension.
    rx0, rw = pad, W - 2 * pad
    for deg in range(360):
        inside = any(((deg - a) % 360) < (b - a) for a, b in merged)
        rgb, _ = oklch_to_srgb(0.75, 0.10, deg + 0.5)
        col = np.array(tf(rgb)) * (0.18 if inside else 1.0)
        x1 = rx0 + deg * rw / 360
        d.rectangle([x1, y0 + 4, x1 + rw / 360 + 1, y0 + 24],
                    fill=to8(col))
    for c in cols:
        x1 = rx0 + c["h"] * rw / 360
        d.line([x1, y0 + 24, x1, y0 + 34], fill=(240, 240, 240), width=1)
    d.text((pad, y0 + 34), "hue ruler 0-360: dimmed = reserved + guard "
           f"{GUARD:g}; ticks = the {len(cols)} dimension hues",
           fill=(200, 200, 200), font=fs)
    y0 += ruler_h
    for c in cols:
        r, k = divmod(c["n"], per_row)
        x = pad + k * (sw + pad)
        y = y0 + r * (sh + 22 + pad)
        d.rectangle([x, y, x + sw, y + sh], fill=to8(tf(c["rgb"])))
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
    elif kind == "hexagon":
        d.polygon([(cx + r * math.cos(math.pi / 3 * i),
                    cy + r * math.sin(math.pi / 3 * i)) for i in range(6)],
                  fill=colour)
    elif kind == "ring":
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=colour, width=3)


def cue_strip(cols, path):
    rowh, W = 28, 560
    img = Image.new("RGB", (W, 40 + rowh * len(cols)), to8(SURFACE))
    d = ImageDraw.Draw(img)
    f = font(13)
    d.text((8, 10), f"V1b second cue: dash = n mod {len(DASHES)}, "
           f"glyph = n mod {len(GLYPHS)}", fill=(240, 240, 240), font=f)
    for c in cols:
        y = 40 + c["n"] * rowh + rowh // 2
        col = to8(c["rgb"])
        d.text((8, y - 8), f"{c['n']:2d}", fill=(230, 230, 230), font=f)
        glyph(d, GLYPHS[c["glyph"]], 52, y, 9, col)
        dashed_line(d, 72, 350, y, DASHES[c["dash"]][1], col, 3)
        d.text((360, y - 8),
               f"{DASHES[c['dash']][0]} / {GLYPHS[c['glyph']]}",
               fill=(230, 230, 230), font=f)
    img.save(path)


VISIONS = (("normal", None),
           ("deuteranopia", lambda c: simulate(c, "deuteranopia")),
           ("protanopia", lambda c: simulate(c, "protanopia")),
           ("tritanopia", lambda c: simulate(c, "tritanopia")))


def table(cols, metric, transform=None):
    rgbs = [c["rgb"] if transform is None else transform(c["rgb"])
            for c in cols]
    rows = []
    for i in range(len(cols)):
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


def pair_counts(cols, transform=None):
    rgbs = [c["rgb"] if transform is None else transform(c["rgb"])
            for c in cols]
    under5, under10 = [], []
    for i in range(len(cols)):
        for j in range(i + 1, len(cols)):
            v = de2000_rgb(rgbs[i], rgbs[j])
            if v < 5:
                under5.append((i, j))
            if v < 10:
                under10.append((i, j))
    return under5, under10


def summary(cols, label):
    lines = [f"[{label}]"]
    for name, tf in VISIONS:
        t = table(cols, de2000_rgb, tf)
        u5, u10 = pair_counts(cols, tf)
        neigh = [v for r in t for v in r[:2] if v is not None]
        worst = min(range(len(t)), key=lambda i: t[i][2])
        shared = sum(cols[i]["dash"] == cols[j]["dash"]
                     and cols[i]["glyph"] == cols[j]["glyph"]
                     for i, j in u10)
        lines.append(
            f"  {name:13s} pairs dE00<5: {len(u5):3d}  <10: {len(u10):3d} "
            f"(of {len(cols) * (len(cols) - 1) // 2})  "
            f"min neighbour {min(neigh):5.2f}  min nearest "
            f"{min(r[2] for r in t):5.2f} (#{worst}~#{t[worst][3]})  "
            f"<10 pairs also sharing dash+glyph: {shared}")
        if name == "normal":
            lines.append("    pairs <5: " + (", ".join(
                f"{i}-{j}" for i, j in u5) or "none"))
            lines.append("    pairs <10 index gaps: " + str(sorted(
                set(j - i for i, j in u10))))
    return lines


def nearest_reserved(cols, members):
    refs = [colour_rgb(m[6]) for m in members]
    out = []
    for c in cols:
        out.append(min(de2000_rgb(c["rgb"], r) for r in refs))
    return out


def run(tier, out):
    out.mkdir(parents=True, exist_ok=True)
    members = measured(tier)
    bands_fam = family_bands(members)
    arcs, merged = open_arcs_from(bands_fam, GUARD)
    bands = choose_chroma(arcs)
    cols = make_colours(arcs, bands)
    total = sum(b - a for a, b in arcs)

    tag = f"tier={tier}, guard {GUARD:g}"
    strip(cols, members, merged, out / "v1_strip.png",
          f"V1b dimension colours n=0..{COUNT - 1}, OKLCH, band = n mod 3 "
          f"({tag}; v vivid, d deep, p pale)")
    for kind in ("deuteranopia", "protanopia", "tritanopia"):
        strip(cols, members, merged, out / f"v1_strip_{kind}.png",
              f"V1b through {kind} (Machado 2009, severity 1.0; {tag})",
              lambda c, k=kind: simulate(c, k))
    cue_strip(cols, out / "v1_cues.png")

    def arc_s(a, b):
        return f"[{a % 360:.1f},{b % 360:.1f})"

    rep = []
    rep.append(f"Reserved colours: tier '{tier}', {len(members)} measured "
               f"(OKLCH hue of the 8-bit literal). Guard {GUARD:g} deg each "
               "side.")
    rep.append("Measured members (family, hue, L, C, name, source):")
    for fam, name, src, h, L, C, _c in sorted(
            members, key=lambda m: (m[0], m[3])):
        rep.append(f"  {fam:15s} {h:6.1f} L{L:.2f} C{C:.3f}  {name}  "
                   f"[{src}]")
    rep.append("Family bands (member extent -> with guard):")
    for fam, (lo, hi) in bands_fam.items():
        rep.append(f"  {fam:15s} members [{lo % 360:.1f},{hi % 360:.1f}] "
                   f"width {hi - lo:.1f} -> reserved "
                   f"[{(lo - GUARD) % 360:.1f},{(hi + GUARD) % 360:.1f}] "
                   f"width {hi - lo + 2 * GUARD:.1f}")
    rep.append("Open arcs: " + ", ".join(arc_s(a, b) for a, b in arcs)
               + f"  widths " + ", ".join(f"{b - a:.1f}" for a, b in arcs)
               + f"  TOTAL {total:.1f} deg")
    rep.append("Bands (L, chosen C, gamut cap over all open hues): "
               + ", ".join(f"{n} L={L} C={C} cap={cap}"
                           for n, L, C, cap in bands))
    rep.append("")
    rep.append("Hue arithmetic: hue(n) = arcs(frac(n*0.618034)); "
               f"band = n mod 3; dash = n mod {len(DASHES)}; "
               f"glyph = n mod {len(GLYPHS)}")
    t = table(cols, de2000_rgb)
    tok = table(cols, deok_rgb)
    nres = nearest_reserved(cols, members)
    rep.append("")
    rep.append(" n    u      hue  band  hex      gamut dE00prev dE00next "
               "nearest(dE00) dOKnearest dE00toReserved dash/glyph")
    for c, r, ro, nr in zip(cols, t, tok, nres):
        hexs = "#%02X%02X%02X" % to8(c["rgb"])
        fmt = lambda v: "   -  " if v is None else f"{v:6.2f}"
        rep.append(
            f"{c['n']:2d} {c['u']:.4f} {c['h']:6.1f} {c['band']:5s} {hexs} "
            f"{'yes' if c['gamut'] else 'NO':5s} {fmt(r[0])}  {fmt(r[1])}  "
            f"{r[2]:6.2f} (#{r[3]:2d})  {ro[2]:.4f}   {nr:6.2f}        "
            f"{DASHES[c['dash']][0]}/{GLYPHS[c['glyph']]}")
    inside_guard, margins = 0, []
    for c in cols:
        h = srgb_to_oklch(np.array(to8(c["rgb"])) / 255.0)[2]
        if any(((h - a) % 360) < (b - a) for a, b in merged):
            inside_guard += 1
        # Distance to the nearest unguarded member band edge, in degrees.
        margins.append(min(min((h - lo) % 360, (lo - h) % 360,
                               (h - hi) % 360, (hi - h) % 360)
                           for lo, hi in bands_fam.values()))
    rep.append(f"Dimension colours (8-bit hue as drawn) inside a guarded "
               f"band: {inside_guard} (a hue rounding onto the guard edge "
               f"itself counts); smallest distance to any family's "
               f"outermost member: {min(margins):.2f} deg")
    rep.append(f"Smallest dE00 from any dimension colour to any reserved "
               f"swatch: {min(nres):.2f}")
    rep.append("")
    rep += summary(cols, "band = n mod 3, dash 6, glyph 7")
    text = "\n".join(rep)
    (out / "v1_report.txt").write_text(text + "\n")
    return text, total, arcs


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    text, total, _ = run("core", out)
    text2, total2, _ = run("all", out / "all_tiers")
    rarcs, rmerged = review_arcs()
    # The review's own arcs, same rule, for the disagreement section.
    bands = choose_chroma(rarcs)
    rcols = make_colours(rarcs, bands)
    extra = ["", "Review 04 section 2.2 bands converted to OKLCH hue "
             "(HSV S .8 V .95), no guard:"]
    extra.append("  reserved: " + ", ".join(
        f"[{a % 360:.1f},{b % 360:.1f})" for a, b in rmerged))
    extra.append("  open arcs: " + ", ".join(
        f"[{a % 360:.1f},{b % 360:.1f})" for a, b in rarcs)
        + f"  TOTAL {sum(b - a for a, b in rarcs):.1f} deg")
    extra += summary(rcols, "review 04 arcs, same n mod 3 rule")
    for name, tf in VISIONS[:1]:
        pass
    (out / "v1_report.txt").write_text(text + "\n" + "\n".join(extra) + "\n")
    print(text)
    print("\n".join(extra))
    print(f"\nall-tiers variant: open total {total2:.1f}; see all_tiers/")


if __name__ == "__main__":
    main()
