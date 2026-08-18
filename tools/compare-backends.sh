#!/bin/sh
# Render the same document through every compiled-in backend and compare the
# results pixel by pixel.
#
# Exiting zero is a weak signal for a renderer: a backend that draws nothing
# still exits zero. Comparing the captured frames catches that, and catches the
# subtler failures too -- a wrong winding order, a flipped Y axis, an atlas
# layer addressed incorrectly all change the image without changing the exit
# code.
#
# The GL and GLES backends must agree exactly. Vulkan is allowed a small
# tolerance because it rasterises through a different pipeline; the limit is
# tight enough that anything beyond edge rounding fails.
set -eu

SAMPLE="${1:-tests/samples/quick_brown_fox.txt}"
BIN="${BIN:-build/gleditor}"
OUT="${OUT:-$(mktemp -d)}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"

# Percentage of differing bytes tolerated against the OpenGL reference. OpenGL
# ES runs the same pipeline through the same driver, so it is held to exact
# equality; Vulkan rasterises through a different one and is allowed a margin
# tight enough that anything beyond edge rounding still fails.
GL_TOLERANCE_PCT=0
VK_TOLERANCE_PCT=1

# Driver errors are notifications by default, which is right for an editor and
# wrong for a comparison: a frame produced while the driver was objecting has
# proved nothing. --strict-diagnostics puts that back to fatal.
STRICT="--strict-diagnostics"

backends="opengl opengles"
# A binary built without Vulkan says so and exits non-zero, which is the only
# reliable way to ask: --help returns before the backend is ever looked at.
if "$BIN" --backend vulkan --profile $STRICT "$SAMPLE" >"$OUT/vkprobe.log" 2>&1; then
  backends="$backends vulkan"
elif grep -q 'was not compiled into this binary' "$OUT/vkprobe.log"; then
  echo "vulkan backend not compiled in, skipping"
else
  echo "FAIL: vulkan backend is compiled in but did not run"
  tail -20 "$OUT/vkprobe.log"
  exit 1
fi

# Pixels the picking query is compared at. The first is page background, the
# rest land on glyphs, so agreement covers both tag kinds.
PICK_PIXELS="200,240 300,265 420,265 250,305 400,345"

# The notification overlay is drawn in window pixels through an orthographic
# projection rather than the document camera, so it exercises a transform the
# document frames never take. Comparing it separately is what catches a backend
# getting the overlay wrong while drawing documents correctly.
# Pixel the overlay run clicks, to place the caret inside a word.
CARET_CLICK="300,265"
# A selection whose end falls inside a cluster, so the compared frame includes
# a partially highlighted quad rather than only whole ones.
SELECT_SPAN="20,34"
TOAST_ONE="error:GL_INVALID_ENUM in glEnable(0xdead)"
TOAST_TWO="warning:overlay parity check"

for backend in $backends; do
  echo "rendering with $backend"
  "$BIN" --backend "$backend" --profile $STRICT \
    --screenshot "$OUT/$backend.ppm" "$SAMPLE" >"$OUT/$backend.log" 2>&1 ||
    {
      echo "FAIL: $backend exited non-zero"
      tail -20 "$OUT/$backend.log"
      exit 1
    }
  [ -s "$OUT/$backend.ppm" ] ||
    {
      echo "FAIL: $backend produced no screenshot"
      exit 1
    }

  # The same document again, with notifications showing over it and the caret
  # placed by a click. Both are drawn through pipelines that do not depth test
  # and in coordinate spaces the plain document frame never exercises.
  "$BIN" --backend "$backend" --profile $STRICT \
    --toast "$TOAST_ONE" --toast "$TOAST_TWO" --click "$CARET_CLICK" \
    --select "$SELECT_SPAN" \
    --screenshot "$OUT/$backend.toast.ppm" "$SAMPLE" \
    >"$OUT/$backend.toastlog" 2>&1 ||
    {
      echo "FAIL: $backend overlay run exited non-zero"
      tail -20 "$OUT/$backend.toastlog"
      exit 1
    }
  [ -s "$OUT/$backend.toast.ppm" ] ||
    {
      echo "FAIL: $backend produced no overlay screenshot"
      exit 1
    }

  # All the pick pixels are answered by one run: --pick is repeatable, and
  # re-rendering a large document once per pixel would dominate the runtime.
  pickArgs=""
  for pixel in $PICK_PIXELS; do
    pickArgs="$pickArgs --pick $pixel"
  done
  # shellcheck disable=SC2086
  "$BIN" --backend "$backend" --profile $STRICT $pickArgs "$SAMPLE" \
    >"$OUT/$backend.picklog" 2>&1 ||
    {
      echo "FAIL: $backend picking run exited non-zero"
      tail -20 "$OUT/$backend.picklog"
      exit 1
    }
  sed -n 's/^pick //p' "$OUT/$backend.picklog" >"$OUT/$backend.picks"

  expected=$(echo "$PICK_PIXELS" | wc -w)
  actual=$(wc -l <"$OUT/$backend.picks")
  [ "$expected" -eq "$actual" ] ||
    {
      echo "FAIL: $backend answered $actual of $expected picking queries"
      exit 1
    }
done

# Picking reads a different attachment through a different path than the colour
# capture, so it is checked separately: a backend can render correctly and still
# report the wrong identity for what it drew.
echo "comparing picking results"
for backend in $backends; do
  [ "$backend" = "opengl" ] && continue
  if diff -u "$OUT/opengl.picks" "$OUT/$backend.picks" >"$OUT/$backend.pickdiff"; then
    echo "ok: $backend picking matches opengl"
  else
    echo "FAIL: $backend picking differs from opengl"
    cat "$OUT/$backend.pickdiff"
    exit 1
  fi
done

# A run where every pixel reports nothing would "match" trivially.
if ! grep -qv '^[0-9,]*: kind 0 ' "$OUT/opengl.picks"; then
  echo "FAIL: every picking query came back empty"
  cat "$OUT/opengl.picks"
  exit 1
fi
echo "picking results:"
sed 's/^/  /' "$OUT/opengl.picks"

OUT="$OUT" GL_TOLERANCE_PCT="$GL_TOLERANCE_PCT" \
  VK_TOLERANCE_PCT="$VK_TOLERANCE_PCT" \
  BACKENDS="$backends" python3 - <<'PYEOF'
import os, sys

out = os.environ["OUT"]
backends = os.environ["BACKENDS"].split()
vk_tol = float(os.environ["VK_TOLERANCE_PCT"]) / 100.0
gl_tol = float(os.environ["GL_TOLERANCE_PCT"]) / 100.0

def load(path):
    data = open(path, "rb").read()
    return data[data.index(b"255\n") + 4:]

ref = load(f"{out}/opengl.ppm")

# A frame that is entirely one colour means nothing was drawn.
if len(set(ref[i:i+3] for i in range(0, len(ref), 3))) < 2:
    sys.exit("FAIL: opengl reference frame is a single flat colour")

failed = False
for backend in backends:
    if backend == "opengl":
        continue
    other = load(f"{out}/{backend}.ppm")
    if len(other) != len(ref):
        print(f"FAIL: {backend} frame is a different size")
        failed = True
        continue
    diffs = [abs(a - b) for a, b in zip(ref, other)]
    differing = sum(1 for d in diffs if d)
    fraction = differing / len(ref)
    limit = vk_tol if backend == "vulkan" else gl_tol
    status = "ok" if fraction <= limit else "FAIL"
    print(f"{status}: {backend} vs opengl: {differing}/{len(ref)} bytes differ "
          f"({fraction*100:.4f}%), max delta {max(diffs)}, limit {limit*100:.2f}%")
    if fraction > limit:
        failed = True

# The overlay frames must agree with each other, and must differ from the
# frames without an overlay -- otherwise every backend agreeing would only mean
# every backend drew nothing.
toastRef = load(f"{out}/opengl.toast.ppm")
overlaid = sum(1 for a, b in zip(ref, toastRef) if a != b)
if overlaid == 0:
    print("FAIL: the notification overlay changed no pixels")
    failed = True
else:
    print(f"ok: the notification overlay changed {overlaid} bytes of the frame")

for backend in backends:
    if backend == "opengl":
        continue
    other = load(f"{out}/{backend}.toast.ppm")
    if len(other) != len(toastRef):
        print(f"FAIL: {backend} overlay frame is a different size")
        failed = True
        continue
    diffs = [abs(a - b) for a, b in zip(toastRef, other)]
    differing = sum(1 for d in diffs if d)
    fraction = differing / len(toastRef)
    limit = vk_tol if backend == "vulkan" else gl_tol
    status = "ok" if fraction <= limit else "FAIL"
    print(f"{status}: {backend} vs opengl with overlay: "
          f"{differing}/{len(toastRef)} bytes differ ({fraction*100:.4f}%), "
          f"max delta {max(diffs)}, limit {limit*100:.2f}%")
    if fraction > limit:
        failed = True

sys.exit(1 if failed else 0)
PYEOF

echo "all backends agree"

# Vulkan is the only backend that can record one frame from several threads, so
# it is the only one where the same frame has two ways of being produced. They
# have to produce the same image: chunks are recorded out of order and executed
# in order, and an ordering mistake would show up here and nowhere else.
#
# The device normally decides for itself whether splitting is worth it, and on
# a machine where it is not, the threaded path would never run. Naming a thread
# count takes the decision away from it, which is what makes both paths
# reachable on demand.
# Culling decides not to draw things. The only way to be sure it decided
# correctly is to draw them anyway and compare: the two frames must be identical
# to the byte, on every backend, because everything skipped was outside the view.
echo "comparing culled frames against unculled ones"
for backend in $backends; do
  "$BIN" --backend "$backend" --profile $STRICT --no-cull \
    --screenshot "$OUT/$backend.nocull.ppm" "$SAMPLE" \
    >"$OUT/$backend.nocull.log" 2>&1 ||
    {
      echo "FAIL: $backend unculled run exited non-zero"
      tail -20 "$OUT/$backend.nocull.log"
      exit 1
    }
  if cmp -s "$OUT/$backend.ppm" "$OUT/$backend.nocull.ppm"; then
    echo "ok: $backend culling changed no pixels"
  else
    echo "FAIL: $backend culling changed the frame"
    exit 1
  fi
done

# The atlas grows when a glyph will not fit, which means a new texture object --
# an array texture cannot gain layers and a 2D texture cannot gain pixels -- so
# every glyph already packed has to be written into it again, at the texel it
# was already on. The coordinates naming those texels are already in a page's
# vertex buffer, on the device, and cannot be revised.
#
# No document provokes it: the whole King James Bible packs into one 512x512
# layer. GLEDITOR_ATLAS_SIZE starts the atlas small enough that ordinary text
# overflows it twice over.
#
# Not a byte comparison, and not because the tolerance is convenient. A smaller
# atlas packs glyphs at different texels, and a mip texel averages a block
# aligned to level zero's grid rather than to the glyph, so the same glyph at an
# odd offset genuinely has a different mip chain from the same glyph at an even
# one. That shows up as a sub-level nudge over the page -- an atlas opened at
# 256, which never grows at all, differs from one opened at 512 by the same
# amount as one opened at 64, which grows twice.
#
# What a failed re-upload looks like instead is glyphs missing outright, and the
# two are easy to tell apart. The limits were set by breaking it on purpose --
# dropping the re-upload from reallocate() -- and measuring both:
#
#           | mean change | >32 levels | >64 | >100
#   working |        0.64 |      0.38% | 0.02% | 0.000%
#   broken  |        2.24 |      1.92% | 1.19% | 0.739%
#
# The phase shift nudges pixels; a missing glyph flips them from ink to paper.
# So the limit that carries the check is the >100 one, which a working atlas
# does not reach at all.
echo "comparing a grown atlas against one that never grew"
for backend in $backends; do
  GLEDITOR_ATLAS_SIZE=64 "$BIN" --backend "$backend" --profile $STRICT \
    --screenshot "$OUT/$backend.grown.ppm" "$SAMPLE" \
    >"$OUT/$backend.grown.log" 2>&1 ||
    {
      echo "FAIL: $backend run with a small atlas exited non-zero"
      tail -20 "$OUT/$backend.grown.log"
      exit 1
    }
  growths=$(grep -c -- '->' "$OUT/$backend.grown.log" || true)
  if [ "$growths" -lt 1 ]; then
    echo "FAIL: $backend atlas never grew, so nothing was compared"
    exit 1
  fi
  echo "  $backend grew the atlas $growths times"
done

OUT="$OUT" BACKENDS="$backends" python3 - <<'PYEOF'
import os, sys

out      = os.environ["OUT"]
backends = os.environ["BACKENDS"].split()

def load(path):
    data = open(path, "rb").read()
    return data[data.index(b"255\n") + 4:]

meanLimit = 1.2
lostLevel = 100
lostPct   = 0.05
failed = False
for backend in backends:
    grew = load(f"{out}/{backend}.grown.ppm")
    kept = load(f"{out}/{backend}.ppm")
    paper = [(a, b) for a, b in zip(kept, grew) if a or b]
    if not paper:
        sys.exit(f"FAIL: {backend} drew nothing to compare")
    mean = sum(abs(a - b) for a, b in paper) / len(paper)
    lost = 100.0 * sum(1 for a, b in paper
                       if abs(a - b) > lostLevel) / len(paper)
    ok = mean <= meanLimit and lost <= lostPct
    print(f"{'ok' if ok else 'FAIL'}: {backend} across atlas growths, mean "
          f"change {mean:.2f} (limit {meanLimit}), {lost:.3f}% of page pixels "
          f"changed by >{lostLevel} levels (limit {lostPct}%)")
    if not ok:
        failed = True

sys.exit(1 if failed else 0)
PYEOF

# The coarse path replaces a page's glyphs with one solid bar per line, so the
# frames are meant to differ -- what must not differ is how bright the page is,
# since a bar too dark or too light makes the switch visible as the camera
# crosses it. Compared over the pixels the pages cover: most of a zoomed-out
# frame is empty space, which would dilute the difference into invisibility.
#
# Measured just either side of the switch, at a field of view that puts a layout
# pixel a little under the threshold, rather than as far out as the coarse path
# ever runs. Further out the comparison stops meaning anything: the detailed
# path has no mipmaps, so under heavy minification it point-samples the atlas
# and its brightness becomes an artefact of where the samples land. Swept across
# fields of view on the short sample, the difference between the two paths ran
# +8.4 levels at 0.03 screen pixels per layout pixel, +1.0 at 0.06, +1.9 at 0.13
# and -0.4 at 0.24 -- converging as the detailed path becomes trustworthy, which
# is what says the coarse path is the one telling the truth out there.
COARSE_FOV=15
echo "comparing the coarse text path against the detailed one"
for detail in 0 999; do
  "$BIN" --backend opengl --profile $STRICT --fov "$COARSE_FOV" \
    --coarse-below "$detail" --screenshot "$OUT/coarse$detail.ppm" "$SAMPLE" \
    >"$OUT/coarse$detail.log" 2>&1 ||
    {
      echo "FAIL: coarse-below $detail run exited non-zero"
      tail -20 "$OUT/coarse$detail.log"
      exit 1
    }
done

OUT="$OUT" python3 - <<'PYEOF'
import os, sys

out = os.environ["OUT"]

def load(path):
    data = open(path, "rb").read()
    return data[data.index(b"255\n") + 4:]

fine   = load(f"{out}/coarse0.ppm")
coarse = load(f"{out}/coarse999.ppm")
if len(fine) != len(coarse):
    sys.exit("FAIL: the two coarse-path frames are different sizes")

# The scene clears to black, so the pages are exactly the non-black pixels.
paper = [(a, b) for a, b in zip(fine, coarse) if a or b]
if not paper:
    sys.exit("FAIL: neither frame drew anything")

meanFine   = sum(a for a, _ in paper) / len(paper)
meanCoarse = sum(b for _, b in paper) / len(paper)
delta = abs(meanCoarse - meanFine)
# Two and a half levels out of 255. Tight enough to catch a bar shade that
# drifted with a font change, loose enough to absorb what is left of the
# detailed path's aliasing at the switch point.
limit = 2.5
status = "ok" if delta <= limit else "FAIL"
print(f"{status}: coarse page brightness {meanCoarse:.1f} against detailed "
      f"{meanFine:.1f}, delta {delta:.2f} (limit {limit})")
sys.exit(0 if delta <= limit else 1)
PYEOF

# Minified text must not crawl. A tiny zoom moves every sample point a fraction
# of a texel, which a filtered render answers with a slight change everywhere
# and an unfiltered one answers by flipping pixels between stroke and paper --
# the shimmer you see when panning. Measured as how many page pixels change
# violently rather than how many change at all: good filtering makes *more*
# pixels move, each by very little.
#
# Without mipmaps this ran at 4.3% of page pixels changing by more than 32
# levels, with a mean change of 5.41; the limits below are set an order of
# magnitude under that, which no amount of rounding will drift into.
echo "checking that minified text does not crawl"
for backend in $backends; do
  for zoom in 15 15.05; do
    "$BIN" --backend "$backend" --profile $STRICT --fov "$zoom" \
      --coarse-below 0 --screenshot "$OUT/$backend.zoom$zoom.ppm" "$SAMPLE" \
      >"$OUT/$backend.zoom$zoom.log" 2>&1 ||
      {
        echo "FAIL: $backend zoom run exited non-zero"
        tail -20 "$OUT/$backend.zoom$zoom.log"
        exit 1
      }
  done
done

OUT="$OUT" BACKENDS="$backends" python3 - <<'PYEOF'
import os, sys

out = os.environ["OUT"]
backends = os.environ["BACKENDS"].split()

def load(path):
    data = open(path, "rb").read()
    return data[data.index(b"255\n") + 4:]

meanLimit  = 2.0
violentPct = 1.0
failed = False
for backend in backends:
    before = load(f"{out}/{backend}.zoom15.ppm")
    after  = load(f"{out}/{backend}.zoom15.05.ppm")
    paper  = [(a, b) for a, b in zip(before, after) if a or b]
    if not paper:
        sys.exit(f"FAIL: {backend} drew nothing to compare")
    mean    = sum(abs(a - b) for a, b in paper) / len(paper)
    violent = 100.0 * sum(1 for a, b in paper if abs(a - b) > 32) / len(paper)
    ok = mean <= meanLimit and violent <= violentPct
    print(f"{'ok' if ok else 'FAIL'}: {backend} under a 0.33% zoom, mean change "
          f"{mean:.2f} (limit {meanLimit}), {violent:.1f}% of page pixels "
          f"changed by >32 levels (limit {violentPct}%)")
    if not ok:
        failed = True

sys.exit(1 if failed else 0)
PYEOF

if echo "$backends" | grep -q vulkan; then
  echo "comparing threaded against single-threaded recording"
  for threads in 4 1; do
    GLEDITOR_RECORD_THREADS="$threads" "$BIN" --backend vulkan --profile \
      $STRICT --screenshot "$OUT/vk.threads$threads.ppm" "$SAMPLE" \
      >"$OUT/vk.threads$threads.log" 2>&1 ||
      {
        echo "FAIL: vulkan run with $threads recording thread(s) exited non-zero"
        tail -20 "$OUT/vk.threads$threads.log"
        exit 1
      }
  done

  if ! cmp -s "$OUT/vk.threads4.ppm" "$OUT/vk.threads1.ppm"; then
    echo "FAIL: recording on four threads drew a different frame than one"
    exit 1
  fi

  # A document small enough to be recorded in one piece whatever was asked for
  # compares two identical code paths, which proves nothing. Say so rather than
  # reporting a pass: the split is only reached above the device's own
  # threshold, and the device announces when it takes it.
  if grep -q 'recording .* draws on [2-9]' "$OUT/vk.threads4.log"; then
    echo "ok: threaded recording matches single-threaded exactly"
  else
    echo "ok: frames match, but $SAMPLE has too few page draws to split;"
    echo "    pass a larger sample to exercise threaded recording"
  fi
fi
