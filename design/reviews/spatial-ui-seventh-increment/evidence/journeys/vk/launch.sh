#!/usr/bin/env bash
set -u
[[ "${SIXTH_RUN_NATIVE_GO:-}" == GO ]] || {
  echo 'launch blocked until parent sets SIXTH_RUN_NATIVE_GO=GO' >&2
  exit 64
}
repo=/workspace/gleditor
base=/workspace/spatial-build
profile="$base/seventh-journey-vk-xdg"
evidence="$repo/design/reviews/spatial-ui-seventh-increment/evidence/journeys/vk"
raw="$base/seventh-journey-vk-raw"
mkdir -p "$profile/data" "$profile/config" "$profile/cache" "$raw" "$evidence"
sha256sum "$repo/build/xuzz" "$repo/build/libgleditor.so.0" >"$evidence/binaries-before.sha256"
"$base/run.sh" env SDL_VIDEODRIVER=x11 \
  XDG_DATA_HOME="$profile/data" XDG_CONFIG_HOME="$profile/config" XDG_CACHE_HOME="$profile/cache" \
  bash "$evidence/run-native-journey.sh" "$repo/build" "$evidence" "$raw" 800 600 forced \
  >"$raw/launcher.log" 2>&1
status=$?
printf '%s\n' "$status" >"$evidence/launcher-exit.txt"
sha256sum "$repo/build/xuzz" "$repo/build/libgleditor.so.0" >"$evidence/binaries-after.sha256"
exit "$status"
