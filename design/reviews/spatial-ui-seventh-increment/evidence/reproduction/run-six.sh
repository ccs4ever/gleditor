#!/usr/bin/env bash
set -u
repo=/workspace/gleditor
raw=/workspace/spatial-build/seventh-repro/raw
xdgroot=/workspace/spatial-build/seventh-repro/xdg
run_one() {
  local name=$1 backend=$2 overlay=$3
  local profile="$xdgroot/$name"
  mkdir -p "$profile/data" "$profile/config" "$profile/cache"
  local screenshot="$raw/$name.ppm" logfile="$raw/$name.log"
  local args=(--backend "$backend" --profile --strict-diagnostics)
  if [[ "$overlay" == yes ]]; then
    args+=(--toast 'error:GL_INVALID_ENUM in glEnable(0xdead)' --toast 'warning:overlay parity check')
  fi
  args+=(--click "300,265" --select "20,34" --screenshot "$screenshot" tests/samples/quick_brown_fox.txt)
  {
    printf 'run=%s\nbackend=%s\noverlay=%s\n' "$name" "$backend" "$overlay"
    printf 'XDG_DATA_HOME=%s/data\nXDG_CONFIG_HOME=%s/config\nXDG_CACHE_HOME=%s/cache\n' "$profile" "$profile" "$profile"
    printf 'argv='
    printf '%q ' build/gleditor "${args[@]}"
    printf '\n'
  } >"$raw/$name.argv"
  XDG_DATA_HOME="$profile/data" XDG_CONFIG_HOME="$profile/config" XDG_CACHE_HOME="$profile/cache" \
    SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH="$repo/build" \
    timeout --signal=TERM --kill-after=5s 90s xvfb-run -a -s '-noreset -screen 0 1440x1000x24' \
    "$repo/build/gleditor" "${args[@]}" >"$logfile" 2>&1
  local status=$?
  printf '%s\n' "$status" >"$raw/$name.exit"
  printf '%s status=%s screenshot=%s\n' "$name" "$status" "$([[ -f "$screenshot" ]] && echo yes || echo no)"
}
run_one opengl-overlay-1 opengl yes
run_one opengl-overlay-2 opengl yes
run_one opengles-overlay-1 opengles yes
run_one opengles-overlay-2 opengles yes
run_one opengl-plain-1 opengl no
run_one opengl-plain-2 opengl no
