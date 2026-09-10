#!/usr/bin/env bash
# tools/create-floating-image-sample.sh
# Rebuild the float-layout fixture described in design/rich-media-layout-boxes.md.
#
# Separate from create-sample-xanadocs.sh on purpose: this document has its own
# scroll rather than the shared 000.scroll that generator threads through every
# other sample, because what it demonstrates is a layout decision about one
# image and one paragraph and nothing else needs to see either. It lives here
# rather than in a comment because create-sample-xanadocs.sh starts by removing
# the whole multimedia directory, which takes this fixture with it -- so
# whoever regenerates the samples needs a one-liner to put it back.

set -euo pipefail

XUDU="./build/xudu"
OUT="tests/samples/xudu/multimedia/11_floating_image"

if [[ ! -x "${XUDU}" ]]; then
  echo "Error: ${XUDU} not found or not executable. Build it first with: make -j\$(nproc) xudu" >&2
  exit 1
fi

rm -rf "${OUT}"
"${XUDU}" "${OUT}" \
  --import tests/samples/sample_image.png \
  --insert-text "0:append:tests/samples/floating_image_body.txt" \
  --headless

echo "==> ${OUT} rebuilt"
