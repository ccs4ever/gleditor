#!/usr/bin/env bash
set -u
repo=/workspace/gleditor
base=/workspace/spatial-build
profile="$base/seventh-journey-gl-xdg"
evidence="$repo/design/reviews/spatial-ui-seventh-increment/evidence/journeys/gl"
raw="$base/seventh-journey-gl-raw"
mkdir -p "$profile/data" "$profile/config" "$profile/cache" "$raw" "$evidence"
sha256sum "$repo/build/xuzz" "$repo/build/libgleditor.so.0" >"$evidence/binaries-before.sha256"
"$base/run.sh" env SDL_VIDEODRIVER=x11 \
  XDG_DATA_HOME="$profile/data" XDG_CONFIG_HOME="$profile/config" XDG_CACHE_HOME="$profile/cache" \
  bash /workspace/spatial-build/seventh-journey-gl-runner.sh \
  "$repo/build" "$evidence" "$raw" 800 600 forced \
  >"$raw/launcher.log" 2>&1
status=$?
printf '%s\n' "$status" >"$evidence/launcher-exit.txt"
sha256sum "$repo/build/xuzz" "$repo/build/libgleditor.so.0" >"$evidence/binaries-after.sha256"
exit "$status"
