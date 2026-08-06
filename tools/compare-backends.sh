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
TOAST_ONE="error:GL_INVALID_ENUM in glEnable(0xdead)"
TOAST_TWO="warning:overlay parity check"


for backend in $backends; do
  echo "rendering with $backend"
  "$BIN" --backend "$backend" --profile $STRICT \
         --screenshot "$OUT/$backend.ppm" "$SAMPLE" >"$OUT/$backend.log" 2>&1 ||
    { echo "FAIL: $backend exited non-zero"; tail -20 "$OUT/$backend.log"; exit 1; }
  [ -s "$OUT/$backend.ppm" ] ||
    { echo "FAIL: $backend produced no screenshot"; exit 1; }

  # The same document again, with notifications showing over it.
  "$BIN" --backend "$backend" --profile $STRICT \
      --toast "$TOAST_ONE" --toast "$TOAST_TWO" \
      --screenshot "$OUT/$backend.toast.ppm" "$SAMPLE" \
      >"$OUT/$backend.toastlog" 2>&1 ||
    { echo "FAIL: $backend overlay run exited non-zero"
      tail -20 "$OUT/$backend.toastlog"; exit 1; }
  [ -s "$OUT/$backend.toast.ppm" ] ||
    { echo "FAIL: $backend produced no overlay screenshot"; exit 1; }

  # All the pick pixels are answered by one run: --pick is repeatable, and
  # re-rendering a large document once per pixel would dominate the runtime.
  pickArgs=""
  for pixel in $PICK_PIXELS; do
    pickArgs="$pickArgs --pick $pixel"
  done
  # shellcheck disable=SC2086
  "$BIN" --backend "$backend" --profile $STRICT $pickArgs "$SAMPLE" \
      >"$OUT/$backend.picklog" 2>&1 ||
    { echo "FAIL: $backend picking run exited non-zero"
      tail -20 "$OUT/$backend.picklog"; exit 1; }
  sed -n 's/^pick //p' "$OUT/$backend.picklog" >"$OUT/$backend.picks"

  expected=$(echo "$PICK_PIXELS" | wc -w)
  actual=$(wc -l <"$OUT/$backend.picks")
  [ "$expected" -eq "$actual" ] ||
    { echo "FAIL: $backend answered $actual of $expected picking queries"; exit 1; }
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
if ! grep -qv 'kind 0 index 0' "$OUT/opengl.picks"; then
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
