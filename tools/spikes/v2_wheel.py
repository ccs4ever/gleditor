#!/usr/bin/env python3
"""Spike V2 (throwaway): do the all-dim walk's wheel formulas (view-system.md
section 9.2.3-9.2.8) look like a wheel?

Placement-local frame as section 8.4 states it: x right, y up, z towards the
viewer, one unit per Canvas pixel on z = 0 at the rest camera.  R = +x,
U = +y, F = -z (away).  Every number below that the spec names is its
section 12.3 default; the ones it does not name are marked OPEN.

Usage: v2_wheel.py OUTDIR [--fov DEG]
"""

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import v1_dimension_colours as v1
from colour_common import to8

# Section 12.3 defaults.
RING_RADIUS = 220.0
RING_RADIUS_STEP = 90.0  # the formula's delta-r
RING_TILT_STEP = 28.0
SLOT_W, SLOT_H = 140.0, 44.0
HUB_MAX_SHARE = 0.4
LABEL_AT = 0.55
CHILD_SCALE = 0.5
CHILD_GAP = 50.0
CHILD_BEND = 0.6
FLEX_STEP, FLEX_TRIES = 24.0, 6
NAME_MIN_CHARS = 6
REST_YAW, REST_PITCH = 12.0, 8.0
# OPEN: the spec's gap g has no setting.  zigzag.rankClearancePx (24) is the
# existing clearance between ranked cells, used here.
GAP = 24.0
# OPEN: the hub's content size; a two-line cell well under hubMaxShare.
HUB_W, HUB_H = 200.0, 64.0
# OPEN: the pane, and the camera's field of view.  5 degrees is
# ViewPerspective::fov's default (include/gleditor/state.hpp:364).
PANE_W, PANE_H = 1280, 800
# Body and Label roles are 12 pt (theme.hpp) = 16 px at 96 dpi;
# zigzag.minReadableTextPx is 14.
BODY_PX, LABEL_PX, MIN_READABLE_PX = 16.0, 16.0, 14.0

SURFACE = (31, 36, 46)
CELL_FILL = (44, 52, 66)
CELL_EDGE = (200, 205, 215)
TEXT = (235, 236, 240)

DIM_NAMES = [
    "d.employer", "d.email", "d.phone", "d.address", "d.contact.phone.mobile",
    "d.manager", "d.project", "d.team", "d.skills", "d.city", "d.country",
    "d.timezone", "d.language", "d.birthday", "d.website", "d.github",
    "d.mentor", "d.reports", "d.office", "d.desk", "d.badge", "d.cost.centre",
    "d.licence", "d.vehicle", "d.emergency.contact", "d.notes", "d.tags",
    "d.created", "d.modified", "d.version", "d.audit.trail.reviewer",
]

R = np.array([1.0, 0.0, 0.0])
U = np.array([0.0, 1.0, 0.0])
F = np.array([0.0, 0.0, -1.0])


def font(px):
    return ImageFont.truetype(
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", max(4, int(px)))


def middle_ellipsis(text, fnt, max_w, min_chars=1):
    if fnt.getlength(text) <= max_w:
        return text
    for keep in range(len(text) - 1, min_chars - 1, -1):
        a = (keep + 1) // 2
        b = keep - a
        t = text[:a] + "…" + (text[-b:] if b else "")
        if fnt.getlength(t) <= max_w:
            return t
    return None


class Wheel:
    """One wheel: rings, pair slots and their contents (section 9.2.3)."""

    def __init__(self, scale=1.0, hub_w=HUB_W, hub_h=HUB_H):
        self.s = scale
        self.w_s, self.h_s, self.g = SLOT_W * scale, SLOT_H * scale, GAP * scale
        self.hub_w, self.hub_h = hub_w, hub_h
        self.r0 = max(RING_RADIUS * scale, (hub_w + self.w_s) / 2 + self.g,
                      (hub_h + self.h_s) / 2 + self.g)

    def radius(self, j):
        return self.r0 + j * RING_RADIUS_STEP * self.s

    def tilt(self, j):
        if j == 0:
            return 0.0
        raw = (-1) ** (j + 1) * math.ceil(j / 2) * RING_TILT_STEP
        bound = math.degrees(math.acos(min(1.0, (
            (self.hub_h + self.h_s) / 2 + self.g) / self.radius(j))))
        return math.copysign(min(abs(raw), bound), raw)

    def pair_slots(self, j):
        m = int(math.floor(math.pi * self.radius(j) / (self.w_s + self.g)))
        if j == 0:
            m -= m % 2
        m = max(m, 2 if j == 0 else 1)
        t0 = 0.0 if j == 0 else math.pi / (2 * m)
        return m, [t0 + i * math.pi / m for i in range(m)]


def place_root(n_dims):
    """Hub at the origin; dims[0] on x, dims[1] on y, the rest in ring
    order on the free pair slots.  Returns cells, edges, rings used."""
    w = Wheel()
    hub = np.zeros(3)
    cells = [dict(pos=hub, w=HUB_W, h=HUB_H, text=["accursed cell",
                                                   "c (hub)"], hub=True,
                  real=True, depth=0)]
    edges = []
    dims = DIM_NAMES[:n_dims]
    checks = dict(pair_sum=0.0, clamp_ok=True, order=[])

    def put(dim_i, j, theta, basis_u, ring):
        for sign, ang in ((+1, theta), (-1, theta + math.pi)):
            p = hub + w.radius(j) * (math.cos(ang) * R + math.sin(ang) * basis_u)
            cells.append(dict(pos=p, w=SLOT_W, h=SLOT_H,
                              text=[f"{dims[dim_i]}", "+1" if sign > 0 else "-1"],
                              real=True, depth=0, dim=dim_i))
            edges.append(dict(a=hub, b=p, dim=dim_i, label=dims[dim_i],
                              depth=0))
        p1 = cells[-2]["pos"]
        p2 = cells[-1]["pos"]
        checks["pair_sum"] = max(checks["pair_sum"],
                                 float(np.linalg.norm(p1 + p2 - 2 * hub)))
        checks["order"].append((ring, theta, dim_i))

    m0, angles0 = w.pair_slots(0)
    reserved = {0: "x", m0 // 2: "y"}
    # bound axes
    if n_dims >= 1:
        put(0, 0, 0.0, U, 0)
    if n_dims >= 2:
        put(1, 0, math.pi / 2, U, 0)
    free = [(0, a, U) for i, a in enumerate(angles0) if i not in reserved]
    rings = {0: dict(r=w.radius(0), tau=0.0, m=m0)}
    remaining = list(range(2, n_dims))
    j = 0
    while len(free) < len(remaining):
        j += 1
        tau = w.tilt(j)
        uj = math.cos(math.radians(tau)) * U + math.sin(math.radians(tau)) * F
        m, angs = w.pair_slots(j)
        rings[j] = dict(r=w.radius(j), tau=tau, m=m)
        lhs = math.cos(math.radians(tau))
        rhs = ((HUB_H + SLOT_H) / 2 + GAP) / w.radius(j)
        checks["clamp_ok"] &= lhs >= rhs - 1e-9
        free += [(j, a, uj) for a in angs]
    for dim_i, (ring, ang, uj) in zip(remaining, free):
        put(dim_i, ring, ang, uj, ring)
    used = sorted({o[0] for o in checks["order"]})
    unbound = [o for o in checks["order"] if o[2] >= 2]
    checks["order_ok"] = [o[2] for o in sorted(unbound)] == \
        sorted(o[2] for o in unbound)
    return w, cells, edges, {k: v for k, v in rings.items() if k in used}, checks


def add_children(w, cells, edges, depth_dims, rest):
    """Depth-1 wheels (section 9.2.8) on every ring neighbour of the root.
    Each child has `depth_dims` own dimensions plus the continuation of the
    dimension that leads back.  Slots: the child's own ring formula at
    childScale, measured from the outward direction (OPEN: the spec gives
    no reference angle for a child), keeping only pairs both of whose ends
    are at least childGap from the way back; set back along F by
    childBend * r * (angle from outward / pi) (OPEN: the spec says "in
    proportion" without a unit); then flexed against real cells already
    placed, depth first then ring order."""
    cw = Wheel(scale=CHILD_SCALE, hub_w=SLOT_W, hub_h=SLOT_H)
    parents = [c for c in cells if not c.get("hub")]
    stats = dict(children=0, slots=0, flexed=0, unresolved=0)
    hub = cells[0]["pos"]
    for parent in parents:
        stats["children"] += 1
        out = parent["pos"] - hub
        out_dir = out / np.linalg.norm(out)
        # in-plane perpendicular: the parent ring's plane holds out_dir and R
        # or U_j; take the component of (U or R) orthogonal to out_dir.
        ref = U if abs(np.dot(out_dir, U)) < 0.9 else R
        ref_plane = ref - np.dot(ref, out_dir) * out_dir
        perp = ref_plane / np.linalg.norm(ref_plane)
        ring_dirs = []  # (j, delta)
        j = 0
        need = depth_dims
        while len([d for d in ring_dirs]) < need and j < 12:
            m, angs = cw.pair_slots(j)
            for a in angs:
                d = math.degrees(a) % 180.0
                # both ends' angle from the way back (180) must be >= gap
                if min(abs(d - 0), abs(180 - d)) >= CHILD_GAP:
                    ring_dirs.append((j, a))
            j += 1
        placed_slots = [(0, 0.0, +1, parent["dim"], True)]  # continuation
        for k, (jj, a) in enumerate(ring_dirs[:need]):
            dim_i = (parent["dim"] + 7 + 5 * k) % len(DIM_NAMES)
            placed_slots.append((jj, a, +1, dim_i, False))
            placed_slots.append((jj, a + math.pi, -1, dim_i, False))
        for jj, a, sign, dim_i, cont in placed_slots:
            r = cw.radius(jj)
            tau = math.radians(cw.tilt(jj))
            uj = math.cos(tau) * perp + math.sin(tau) * F
            off = r * (math.cos(a) * out_dir + math.sin(a) * uj)
            ang_from_out = abs((math.degrees(a) + 180) % 360 - 180)
            setback = CHILD_BEND * r * ang_from_out / 180.0
            p = parent["pos"] + off + setback * F
            box = dict(pos=p, w=SLOT_W * CHILD_SCALE, h=SLOT_H * CHILD_SCALE,
                       text=[DIM_NAMES[dim_i]], real=True, depth=1,
                       dim=dim_i)
            tries = 0
            spoke = off / np.linalg.norm(off)
            while tries < FLEX_TRIES and covers(box, cells, rest):
                box["pos"] = box["pos"] + FLEX_STEP * CHILD_SCALE * (F + spoke)
                tries += 1
            if tries:
                stats["flexed"] += 1
            if covers(box, cells, rest):
                stats["unresolved"] += 1
                box["unresolved"] = True
            cells.append(box)
            edges.append(dict(a=parent["pos"], b=box["pos"], dim=dim_i,
                              label=DIM_NAMES[dim_i], depth=1))
            stats["slots"] += 1
    return stats


def covers(box, cells, cam):
    bb = screen_box(box, cam)
    for c in cells:
        if c is box:
            continue
        if overlap(bb, screen_box(c, cam)):
            return True
    return False


class Camera:
    def __init__(self, fov, framing=1.0, target=np.zeros(3)):
        self.f = (PANE_H / 2) / math.tan(math.radians(fov) / 2)
        self.dist = self.f / framing
        yaw, pitch = math.radians(REST_YAW), math.radians(REST_PITCH)
        self.pos = target + self.dist * np.array([
            math.sin(yaw) * math.cos(pitch), math.sin(pitch),
            math.cos(yaw) * math.cos(pitch)])
        fwd = target - self.pos
        self.fwd = fwd / np.linalg.norm(fwd)
        right = np.cross(self.fwd, U)
        self.right = right / np.linalg.norm(right)
        self.up = np.cross(self.right, self.fwd)

    def project(self, p):
        v = p - self.pos
        z = float(np.dot(v, self.fwd))
        x = float(np.dot(v, self.right))
        y = float(np.dot(v, self.up))
        k = self.f / z
        return PANE_W / 2 + k * x, PANE_H / 2 - k * y, k, z


def screen_box(c, cam):
    x, y, k, _ = cam.project(c["pos"])
    hw, hh = c["w"] * k / 2, c["h"] * k / 2
    return (x - hw, y - hh, x + hw, y + hh)


def overlap(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def frame(cells, fov):
    """Frame the outermost occupied ring, but not from further than slot
    text stays readable (section 9.2.9)."""
    cam = Camera(fov)
    xs, ys = [], []
    for c in cells:
        if c.get("depth", 0) == 0:
            b = screen_box(c, cam)
            xs += [b[0] - PANE_W / 2, b[2] - PANE_W / 2]
            ys += [b[1] - PANE_H / 2, b[3] - PANE_H / 2]
    margin = 0.92
    need = max(max(abs(v) for v in xs) / (PANE_W / 2 * margin),
               max(abs(v) for v in ys) / (PANE_H / 2 * margin))
    zoom = 1.0 / max(need, 1e-6)
    zoom = min(1.0, max(zoom, MIN_READABLE_PX / BODY_PX))
    return Camera(fov, framing=zoom), zoom


def render(cells, edges, cam, title, path):
    img = Image.new("RGB", (PANE_W, PANE_H), SURFACE)
    d = ImageDraw.Draw(img)
    cols = v1.colours("pass")
    items = []
    for e in edges:
        a = cam.project(e["a"])
        b = cam.project(e["b"])
        items.append(("edge", (a[3] + b[3]) / 2 + 1e5, e, a, b))
    for e in edges:
        a = e["a"] + LABEL_AT * (e["b"] - e["a"])
        pa = cam.project(a)
        items.append(("label", pa[3], e, pa, None))
    for c in cells:
        pc = cam.project(c["pos"])
        items.append(("cell", pc[3], c, pc, None))
    label_boxes = []
    # edges first (behind everything), then cells and labels far to near
    for kind, _, obj, a, b in sorted(
            [i for i in items if i[0] == "edge"], key=lambda i: -i[1]):
        col = to8(cols[obj["dim"] % len(cols)]["rgb"])
        width = 3 if obj["depth"] == 0 else 2
        d.line([a[0], a[1], b[0], b[1]], fill=col, width=width)
    for kind, _, obj, a, _b in sorted(
            [i for i in items if i[0] != "edge"], key=lambda i: -i[1]):
        k = a[2]
        if kind == "cell":
            x0, y0, x1, y1 = screen_box(obj, cam)
            outline = (255, 80, 80) if obj.get("unresolved") else (
                (250, 220, 120) if obj.get("hub") else CELL_EDGE)
            d.rectangle([x0, y0, x1, y1], fill=CELL_FILL, outline=outline,
                        width=3 if obj.get("hub") else 1)
            fpx = BODY_PX * k * (1 if obj.get("depth", 0) == 0 else CHILD_SCALE)
            if fpx >= 6:
                fnt = font(fpx)
                lines = [middle_ellipsis(t, fnt, (x1 - x0) - 6) or ""
                         for t in obj["text"]]
                lh = fpx * 1.15
                ty = (y0 + y1) / 2 - lh * len(lines) / 2
                for ln in lines:
                    tw = fnt.getlength(ln)
                    d.text(((x0 + x1) / 2 - tw / 2, ty), ln, fill=TEXT,
                           font=fnt)
                    ty += lh
        else:
            fpx = LABEL_PX * k * (1 if obj["depth"] == 0 else CHILD_SCALE)
            if fpx < 6:
                continue
            fnt = font(fpx)
            # OPEN: the spec gives labels no width; a slot's width is used.
            txt = middle_ellipsis(obj["label"], fnt,
                                  SLOT_W * k * (1 if obj["depth"] == 0
                                                else CHILD_SCALE),
                                  NAME_MIN_CHARS)
            if txt is None:
                continue
            tw = fnt.getlength(txt)
            box = (a[0] - tw / 2 - 2, a[1] - fpx * 0.6,
                   a[0] + tw / 2 + 2, a[1] + fpx * 0.6)
            label_boxes.append(box)
            col = to8(cols[obj["dim"] % len(cols)]["rgb"])
            d.rectangle(box, fill=SURFACE)
            d.text((box[0] + 2, box[1]), txt, fill=col, font=fnt)
    d.text((10, 8), title, fill=TEXT, font=font(15))
    img.save(path)
    lab_lab = sum(overlap(label_boxes[i], label_boxes[j])
                  for i in range(len(label_boxes))
                  for j in range(i + 1, len(label_boxes)))
    cell_boxes = [screen_box(c, cam) for c in cells]
    cell_cell = sum(overlap(cell_boxes[i], cell_boxes[j])
                    for i in range(len(cell_boxes))
                    for j in range(i + 1, len(cell_boxes)))
    lab_cell = sum(overlap(lb, cb) for lb in label_boxes for cb in cell_boxes)
    offpane = sum(1 for b in cell_boxes if b[0] < 0 or b[1] < 0
                  or b[2] > PANE_W or b[3] > PANE_H)
    return dict(label_label=lab_lab, cell_cell=cell_cell,
                label_cell=lab_cell, cells_off_pane=offpane)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--fov", type=float, default=5.0)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    tag = f"fov{int(args.fov)}"
    paths = []
    for valence in (2, 8, 20, 60):
        n_dims = valence // 2
        w, cells, edges, rings, checks = place_root(n_dims)
        cam, zoom = frame(cells, args.fov)
        title = (f"V2 wheel, valence {valence} ({n_dims} dims; d0 on x, d1 on "
                 f"y), fov {args.fov:g}, zoom {zoom:.3f}")
        p = out / f"v2_wheel_valence{valence:02d}_{tag}.png"
        st = render(cells, edges, cam, title, p)
        paths.append(p)
        print(f"valence {valence:2d}: rings " + "; ".join(
            f"j{j} r={v['r']:.0f} tau={v['tau']:+.1f} m={v['m']}"
            for j, v in rings.items()))
        print(f"   max |p+ + p- - 2 hub| = {checks['pair_sum']:.2e}, clamp "
              f"holds: {checks['clamp_ok']}, (ring,angle) order == ring "
              f"order: {checks['order_ok']}, zoom {zoom:.3f}, overlaps {st}")
    # depth-1 figures
    for valence in (8, 20):
        w, cells, edges, rings, checks = place_root(valence // 2)
        cam0, zoom = frame(cells, args.fov)
        stats = add_children(w, cells, edges, 2, cam0)
        cam, zoom = frame(cells, args.fov)
        p = out / f"v2_wheel_valence{valence:02d}_depth1_{tag}.png"
        st = render(cells, edges, cam,
                    f"V2 wheel, valence {valence}, depth 1 (each neighbour "
                    f"+2 dims and the continuation), fov {args.fov:g}", p)
        print(f"depth1 valence {valence}: {stats}, overlaps {st}")
        paths.append(p)
    sheet = Image.new("RGB", (PANE_W, PANE_H), SURFACE)
    for i, p in enumerate(paths[:4]):
        im = Image.open(p).resize((PANE_W // 2, PANE_H // 2), Image.LANCZOS)
        sheet.paste(im, ((i % 2) * PANE_W // 2, (i // 2) * PANE_H // 2))
    sp = out / f"v2_wheel_sheet_{tag}.png"
    sheet.save(sp)
    print("wrote", ", ".join(str(p) for p in paths + [sp]))


if __name__ == "__main__":
    main()
