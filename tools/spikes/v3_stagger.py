#!/usr/bin/env python3
"""Spike V3 (throwaway): does the stacked vanishing view's stagger search
(view-system.md section 10.4.2-10.4.3) choose well?

Local units are Canvas pixels; x right, y up, z towards the viewer.  Page
t+i is centred at b + i s u, page t-i at b' + (i-1) s u, with
u = (sin psi cos phi, sin psi sin phi, -cos psi).  Pages stay parallel to the
pane, so every projection is an axis-aligned rectangle.

Usage: v3_stagger.py OUTDIR [--fov DEG]
"""

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# Section 12.3 defaults.
SPACING, GUTTER, TUCK_STRIP = 60.0, 48.0, 40.0
NEAR_PAGES, NEAR_FALLOFF = 6, 0.7
LINE_START_W, LINE_END_W = 0.5, 0.2
AZ_STEP, REC_STEP = 15, 5
MIN_REC, MAX_REC = 10, 60
HYSTERESIS = 0.05
FADE_PAGES, FADE_FLOOR = 12.0, 0.08
MIN_PAGE_PX = 8.0
# zigzag.minReadableTextPx (system_docs.hpp: 14).
MIN_READABLE_PX = 14.0
# Letter at 139.7 ppi plus a 24 px margin each side (layout_box.hpp
# letterPage), the only fixed page size in the tree.
LETTER_W = 139.70 * 8.5 + 48.0
LETTER_H = 139.70 * 11.0 + 48.0
MARGIN = 24.0
# xuzz's default font is "Monospace 16" (src/app.cpp --font): 16 pt at
# FontFace's 96 dpi is 21.33 px; DejaVu Sans Mono's ascent+descent is
# 1.164 em.
LINE_H = 16.0 * 96.0 / 72.0 * 1.164
# OPEN: the pane.  A full-HD window.
PANE_W, PANE_H = 1920, 1080
COLUMNS = 96  # samples across a line for the visible fraction

DOC_PAGES, CURRENT = 60, 20
MARKED = {4, 13, 23, 27, 31, 44, 52}


class Deck:
    def __init__(self, W, H, fov, pane_w=PANE_W, pane_h=PANE_H):
        self.W, self.H = W, H
        self.pane_w, self.pane_h = pane_w, pane_h
        self.lines = int((H - 2 * MARGIN) // LINE_H)
        # OPEN: the spec does not say how the deck is framed.  Both tops are
        # fitted into 90% of the pane; if that leaves the current page's
        # lines unreadable the passed stack is tucked (section 10.4.2) and
        # the current page alone is fitted.
        k_fit = min(0.9 * pane_h / H, 0.9 * pane_w / (2 * W + GUTTER))
        self.tucked = k_fit * LINE_H < MIN_READABLE_PX
        if self.tucked:
            k_fit = min(0.9 * pane_h / H, 0.9 * pane_w / (W + TUCK_STRIP))
        self.k = k_fit
        self.f = (pane_h / 2) / math.tan(math.radians(fov) / 2)
        self.D = self.f / self.k
        if self.tucked:
            # OPEN: tucked geometry.  The passed top sits one spacing behind
            # the current page, offset left so tuckStrip shows.
            self.b = (0.0, 0.0, 0.0)
            self.bp = (-TUCK_STRIP, 0.0, -SPACING)
            self.cx = -TUCK_STRIP / 2
        else:
            self.b = ((W + GUTTER) / 2, 0.0, 0.0)
            self.bp = (self.b[0] - (W + GUTTER), 0.0, 0.0)
            self.cx = 0.0

    def u(self, phi, psi):
        p, s = math.radians(phi), math.radians(psi)
        return (math.sin(s) * math.cos(p), math.sin(s) * math.sin(p),
                -math.cos(s))

    def centre(self, page, phi, psi):
        u = self.u(phi, psi)
        if page >= CURRENT:
            i = page - CURRENT
            o = self.b
        else:
            i = CURRENT - page - 1
            o = self.bp
        return tuple(o[a] + i * SPACING * u[a] for a in range(3))

    def project_rect(self, c, w, h):
        """Screen rect (x0, y0, x1, y1) and scale of a pane-parallel rect."""
        depth = self.D - c[2]
        s = self.f / depth
        x = self.pane_w / 2 + s * (c[0] - self.cx)
        y = self.pane_h / 2 - s * c[1]
        return (x - s * w / 2, y - s * h / 2, x + s * w / 2, y + s * h / 2), s

    def page_rect(self, page, phi, psi):
        return self.project_rect(self.centre(page, phi, psi), self.W, self.H)

    def score(self, phi, psi, detail=False):
        total = 0.0
        parts = []
        pages = list(range(CURRENT - 1, CURRENT + NEAR_PAGES + 1))
        pages += list(range(CURRENT - 2, CURRENT - NEAR_PAGES - 2, -1))
        rects = {p: self.page_rect(p, phi, psi) for p in pages
                 if 0 <= p < DOC_PAGES}
        depth = {p: self.centre(p, phi, psi)[2] for p in rects}
        for p in rects:
            if p in (CURRENT, CURRENT - 1):
                continue
            dist = p - CURRENT if p > CURRENT else CURRENT - 1 - p
            (x0, y0, x1, y1), s = rects[p]
            line_px = LINE_H * s
            occ = [rects[q][0] for q in rects if depth[q] > depth[p] + 1e-6]
            li = np.arange(self.lines)
            top = y0 + s * (MARGIN + li * LINE_H)
            bot = top + line_px
            lx0, lx1 = x0 + s * MARGIN, x1 - s * MARGIN
            xs = lx0 + (np.arange(COLUMNS) + 0.5) * (lx1 - lx0) / COLUMNS
            vis = ((xs >= 0) & (xs <= self.pane_w))[None, :] & \
                ((top >= 0) & (bot <= self.pane_h))[:, None]
            for o in occ:
                cx = (xs >= o[0]) & (xs <= o[2])
                cy = (o[1] < bot) & (top < o[3])
                vis &= ~(cy[:, None] & cx[None, :])
            frac = vis.mean(axis=1)
            # OPEN: the readability test is stated for whole lines only;
            # applied to partial ones too, since an unreadable fragment
            # is no more legible.
            if line_px < MIN_READABLE_PX:
                frac = np.zeros_like(frac)
            whole = frac == 1
            part = (frac > 0) & ~whole
            # OPEN: a line showing neither end scores nothing.
            sub = float(whole.sum()
                        + (frac * LINE_START_W)[part & vis[:, 0]].sum()
                        + (frac * LINE_END_W)[part & ~vis[:, 0]
                                              & vis[:, -1]].sum())
            total += NEAR_FALLOFF ** dist * sub
            parts.append((p, dist, sub))
        return (total, parts) if detail else total


def search(deck, previous=None):
    grid = {}
    for phi in range(0, 181, AZ_STEP):
        for psi in range(MIN_REC, MAX_REC + 1, REC_STEP):
            grid[(phi, psi)] = deck.score(phi, psi)
    best = max(grid, key=lambda c: (grid[c], -abs(c[0] - 90)))
    # OPEN: hysteresis read as relative to the best score.
    if previous is not None and grid[previous] >= grid[best] * (1 - HYSTERESIS):
        best = previous
    return best, grid


def font(px):
    return ImageFont.truetype(
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", max(4, int(px)))


def render(deck, phi, psi, score, title, path):
    img = Image.new("RGB", (int(deck.pane_w), deck.pane_h + 34),
                    (31, 36, 46))
    d = ImageDraw.Draw(img, "RGBA")
    pages = []
    for p in range(DOC_PAGES):
        rect, s = deck.page_rect(p, phi, psi)
        dist = p - CURRENT if p >= CURRENT else CURRENT - 1 - p
        if (rect[3] - rect[1]) < MIN_PAGE_PX and p not in MARKED:
            continue
        pages.append((deck.centre(p, phi, psi)[2], p, dist, rect, s))
    tails = 0
    for z, p, dist, rect, s in sorted(pages):
        if rect[2] < 0 or rect[0] > deck.pane_w or rect[3] < 0 \
                or rect[1] > deck.pane_h:
            tails += 1
            continue
        marked = p in MARKED
        top = dist == 0
        alpha = 1.0 if (marked or top) else max(FADE_FLOOR,
                                                math.exp(-dist / FADE_PAGES))
        a = int(255 * alpha)
        d.rectangle(rect, fill=(236, 233, 224, a), outline=(90, 90, 90, a))
        x0, y0, x1, y1 = rect
        line_px = LINE_H * s
        for li in range(deck.lines):
            ty = y0 + s * (MARGIN + li * LINE_H)
            frac = 0.55 + 0.45 * ((li * 7919 + p * 104729) % 97) / 97
            if li % 17 == 16:
                frac = 0.2
            ink = (40, 40, 48, int(a * (0.85 if line_px >= MIN_READABLE_PX
                                        else 0.35)))
            d.rectangle([x0 + s * MARGIN, ty + line_px * 0.25,
                         x0 + s * MARGIN + frac * (x1 - x0 - 2 * s * MARGIN),
                         ty + line_px * 0.75], fill=ink)
        if marked:
            d.rectangle([x1 - 4, y0 + (y1 - y0) * 0.1, x1 + 6,
                         y0 + (y1 - y0) * 0.22], fill=(185, 140, 255, 255))
        if top or dist < 3:
            d.text((x0 + 6, y1 - 22), f"p{p + 1}", fill=(120, 30, 30, a),
                   font=font(16))
    # caption below the pane, so it never hides the strips at the top
    d.rectangle([0, deck.pane_h, deck.pane_w, deck.pane_h + 34],
                fill=(12, 14, 18, 255))
    d.text((10, deck.pane_h + 8), f"{title}: phi={phi} psi={psi} score={score:.2f} "
           f"k={deck.k:.3f} line={LINE_H * deck.k:.1f}px "
           f"{'passed stack tucked' if deck.tucked else 'two tops'}; "
           f"{tails} drawn pages off-pane", fill=(235, 235, 240),
           font=font(16))
    img.save(path)


def main():
    global SPACING
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--fov", type=float, default=5.0)
    ap.add_argument("--spacing", type=float, default=60.0,
                    help="override stack.spacing for a sensitivity run")
    args = ap.parse_args()
    SPACING = args.spacing
    sfx = "" if SPACING == 60.0 else f"_spacing{int(SPACING)}"
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    cases = [("letter_portrait", LETTER_W, LETTER_H),
             ("letter_landscape", LETTER_H, LETTER_W)]
    lines = []
    for name, W, H in cases:
        deck = Deck(W, H, args.fov)
        best, grid = search(deck)
        wider = Deck(W, H, args.fov, pane_w=PANE_W * 1.01)
        keep, grid2 = search(wider, previous=best)
        fresh, _ = search(wider)
        lines.append(f"== {name} page {W:.1f}x{H:.1f}, {deck.lines} lines of "
                     f"{LINE_H:.2f}, pane {PANE_W}x{PANE_H}, fov {args.fov:g},"
                     f" k={deck.k:.3f}, line {LINE_H * deck.k:.2f}px, "
                     f"{'tucked' if deck.tucked else 'two tops'}")
        lines.append("phi\\psi " + " ".join(f"{p:7d}" for p in
                                             range(MIN_REC, MAX_REC + 1,
                                                   REC_STEP)))
        for phi in range(0, 181, AZ_STEP):
            lines.append(f"{phi:7d} " + " ".join(
                f"{grid[(phi, psi)]:7.2f}"
                for psi in range(MIN_REC, MAX_REC + 1, REC_STEP)))
        ranked = sorted(grid, key=lambda c: -grid[c])[:5]
        lines.append("top 5: " + ", ".join(
            f"(phi {c[0]}, psi {c[1]}) {grid[c]:.2f}" for c in ranked))
        total, parts = deck.score(*best, detail=True)
        lines.append(f"chosen (phi {best[0]}, psi {best[1]}) {total:.2f}; per "
                     "page (page, places behind top, line score): " +
                     ", ".join(f"p{p + 1}:{dd}:{sc:.1f}" for p, dd, sc in
                               sorted(parts)))
        lines.append(f"pane 1% wider: fresh best {fresh} "
                     f"{grid2[fresh]:.2f}; with hysteresis keeps {keep}")
        render(deck, best[0], best[1], grid[best],
               f"V3 {name} fov {args.fov:g} spacing {SPACING:g}",
               out / f"v3_deck_{name}_fov{int(args.fov)}{sfx}.png")
        lines.append(
            f"wrote {out / f'v3_deck_{name}_fov{int(args.fov)}{sfx}.png'}")
    text = "\n".join(lines)
    (out / f"v3_scores_fov{int(args.fov)}{sfx}.txt").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
