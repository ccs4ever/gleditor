#!/bin/sh
#
# Prove that an installed gleditor works, which is a different question from
# whether its package built.
#
#   packaging/smoke-test.sh /usr/bin/gleditor [backend...]
#
# The test runs the binary from a scratch directory that has no relation to the
# source tree. That is the whole point: until the data files were found at run
# time rather than under ./assets, every package would have built, installed,
# and then failed to start, and nothing in a build log would have said so.
#
# Needs a display and a GL driver. Headless CI has neither, so the caller is
# expected to have already arranged xvfb and a software rasteriser; this script
# only reports what it finds.
set -eu

BIN=${1:?usage: smoke-test.sh <path to gleditor> [backend...]}
shift
# Named backends are rendered with. Naming none checks everything that does not
# need a GPU and stops there, which is what the Nix job does: a binary from the
# store uses nixpkgs' libglvnd and cannot load a non-NixOS runner's Mesa
# drivers, and that is a fact about the runner rather than about the package.
BACKENDS=$*

command -v "$BIN" >/dev/null 2>&1 || [ -x "$BIN" ] || {
  echo "FAIL: $BIN is not executable"
  exit 1
}

# --help and --version return before SDL is touched, so they answer a question
# no rendering test can: whether the binary's dynamic dependencies are all
# present. A package that is missing a shared library fails here, at the
# loader, with a clear message rather than a black frame.
echo "==> $BIN --version"
"$BIN" --version
echo "==> $BIN --help (loader check)"
"$BIN" --help >/dev/null

# Where the program decided its data files are. This is the question packaging
# gets wrong, and unlike rendering it can be asked on a machine with no usable
# GL driver at all -- so it is checked everywhere, including where the frame
# below is not drawn.
echo "==> $BIN --print-asset-dir"
assets=$("$BIN" --print-asset-dir)
echo "    resolved to: $assets"
for shader in glyph.vert.glsl glyph.frag.glsl; do
  if [ ! -f "$assets/shaders/$shader" ]; then
    echo "FAIL: $assets/shaders/$shader is not there; the search found the"
    echo "      wrong directory, which is what an installed copy failing to"
    echo "      start looks like"
    exit 1
  fi
done
echo "ok: the shaders are where the program looked for them"

if [ -z "$BACKENDS" ]; then
  echo "no backends named; skipping the render"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

cat > sample.txt <<'SAMPLE'
The quick brown fox jumped over the lazy dog.
fir floor far.
SAMPLE

failed=0
for backend in $BACKENDS; do
  echo "==> rendering with $backend from $work"
  if ! "$BIN" --backend "$backend" --profile --strict-diagnostics \
       --screenshot "$work/$backend.ppm" sample.txt > "$work/$backend.log" 2>&1; then
    echo "FAIL: $backend run exited non-zero"
    tail -20 "$work/$backend.log"
    failed=1
    continue
  fi

  # A frame of one flat colour is what a missing shader or an unfound atlas
  # looks like, and it exits zero. Counting the distinct colours is what tells
  # "drew the document" from "drew the background".
  colours=$(python3 - "$work/$backend.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
pixels = data[data.index(b'255\n') + 4:]
print(len({pixels[i:i+3] for i in range(0, len(pixels) - 2, 3)}))
PY
)
  if [ "$colours" -lt 2 ]; then
    echo "FAIL: $backend produced a single flat colour ($colours distinct)"
    tail -20 "$work/$backend.log"
    failed=1
  else
    echo "ok: $backend drew a frame with $colours distinct colours"
  fi
done

exit $failed
