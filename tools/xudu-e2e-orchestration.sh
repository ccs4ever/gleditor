#!/usr/bin/env bash
#
# xudu-e2e-orchestration.sh
#
# Standalone end-to-end integration test runner that orchestrates the xudu
# binary through:
#   1. Loading 2 source media torrents.
#   2. Loading 2 published XanaDocs side-by-side.
#   3. Linking between the XanaDocs.
#   4. Transcluding content between the XanaDocs.
#   5. Adopting 2 LinkPackages (including 3rd source materialization).
#
# Produces step-by-step visual verification screenshots in PPM and PNG formats.
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="${ROOT}/build/xudu"
TEST_BIN="${ROOT}/build/xudu_test"
SCREENSHOT_DIR="${ROOT}/build/integration_screenshots"

if [ ! -x "$BIN" ] || [ ! -x "$TEST_BIN" ]; then
  echo "==> Building xudu and xudu_test..."
  make -C "$ROOT" -j"$(nproc)" xudu xudu_test
fi

echo "==> Running E2E Binary Orchestration Test Suite..."
"$TEST_BIN" --gtest_filter='*E2EBinaryOrchestration*'

echo "==> Verifying generated visual screenshots in ${SCREENSHOT_DIR}:"
SCENARIOS=(
  "step1_source_torrents"
  "step2_xanadocs_loaded"
  "step3_cross_linking"
  "step4_transclusion"
  "full_page_transclusion_lifecycle"
  "full_page_many_to_many_hypermesh"
  "full_page_one_to_many_fan"
  "full_page_multi_type_links"
  "full_page_three_doc_depth_routing"
  "extreme_framing_3x3_pages"
  "extreme_framing_5x5_pages"
  "extreme_framing_8x8_pages"
  "extreme_framing_10x10_pages"
  "extreme_framing_3x8_asymmetric"
  "extreme_framing_5x10_asymmetric"
  "large_multipage_background_flyin"
)

for step in "${SCENARIOS[@]}"; do
  ppm="${SCREENSHOT_DIR}/${step}.ppm"
  png="${SCREENSHOT_DIR}/${step}.png"
  if [ -f "$ppm" ]; then
    echo "  [OK] $ppm ($(stat -c%s "$ppm" 2>/dev/null || wc -c < "$ppm") bytes)"
  else
    echo "  [FAIL] $ppm missing" >&2
    exit 1
  fi
  if [ -f "$png" ]; then
    echo "  [OK] $png ($(stat -c%s "$png" 2>/dev/null || wc -c < "$png") bytes)"
  fi
done

echo "==> All E2E binary orchestration test scenarios and screenshots verified successfully!"

