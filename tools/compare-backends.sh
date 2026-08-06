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

# Fraction of differing bytes tolerated against the OpenGL reference.
GL_TOLERANCE=0
VK_TOLERANCE_PCT=1

backends="opengl opengles"
# A binary built without Vulkan says so and exits non-zero, which is the only
# reliable way to ask: --help returns before the backend is ever looked at.
if "$BIN" --backend vulkan --profile "$SAMPLE" >"$OUT/vkprobe.log" 2>&1; then
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
PICK_PIXELS="10,10 400,300 500,250 600,300 700,350"

for backend in $backends; do
  echo "rendering with $backend"
  "$BIN" --backend "$backend" --profile \
         --screenshot "$OUT/$backend.ppm" "$SAMPLE" >"$OUT/$backend.log" 2>&1 ||
    { echo "FAIL: $backend exited non-zero"; tail -20 "$OUT/$backend.log"; exit 1; }
  [ -s "$OUT/$backend.ppm" ] ||
    { echo "FAIL: $backend produced no screenshot"; exit 1; }

  # All the pick pixels are answered by one run: --pick is repeatable, and
  # re-rendering a large document once per pixel would dominate the runtime.
  pickArgs=""
  for pixel in $PICK_PIXELS; do
    pickArgs="$pickArgs --pick $pixel"
  done
  # shellcheck disable=SC2086
  "$BIN" --backend "$backend" --profile $pickArgs "$SAMPLE" \
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

OUT="$OUT" GL_TOLERANCE="$GL_TOLERANCE" VK_TOLERANCE_PCT="$VK_TOLERANCE_PCT" \
  BACKENDS="$backends" python3 - <<'PYEOF'
import os, sys

out = os.environ["OUT"]
backends = os.environ["BACKENDS"].split()
vk_tol = float(os.environ["VK_TOLERANCE_PCT"]) / 100.0

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
    limit = vk_tol if backend == "vulkan" else 0.0
    status = "ok" if fraction <= limit else "FAIL"
    print(f"{status}: {backend} vs opengl: {differing}/{len(ref)} bytes differ "
          f"({fraction*100:.4f}%), max delta {max(diffs)}, limit {limit*100:.2f}%")
    if fraction > limit:
        failed = True

sys.exit(1 if failed else 0)
PYEOF

echo "all backends agree"
