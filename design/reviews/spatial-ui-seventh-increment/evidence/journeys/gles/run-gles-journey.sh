#!/usr/bin/env bash
set -u

# Usage: run-gles-journey.sh BINARY_DIR EVIDENCE_DIR RAW_DIR WIDTH HEIGHT MODE
# MODE is normal or forced. The supplied binary directory contains xuzz,
# libgleditor.so.0, and xudu-dump. This runs inside spatial-native-session.
# This runner is prepared for the frozen seventh candidate; it has not yet run.
bin=$1 evidence=$2 raw=$3 width=$4 height=$5 mode=$6
expected_xuzz=66089e427a2ab6876c31675cfe6e6c0818294c6ba156a9c264caa8ef3c5d7739
expected_lib=419d2142fb3aa9f0181120fb084a05deae6a17935754eae61fe9cd662abf680d
[[ "$(sha256sum "$bin/xuzz" | cut -d ' ' -f1)" == "$expected_xuzz" ]] || {
  echo "unexpected xuzz binary hash" >&2
  exit 2
}
[[ "$(sha256sum "$bin/libgleditor.so.0" | cut -d ' ' -f1)" == "$expected_lib" ]] || {
  echo "unexpected libgleditor.so.0 hash" >&2
  exit 2
}
mkdir -p "$evidence" "$raw"

passage=$(
  python3 - <<'PY'
for index in range(1, 81):
    print(f"P{index:03d} The selected passage continues across this ordinary page line with its own exact words.", end="")
    if index != 80:
        print()
PY
)
source_bytes=$(printf '%s' "$passage" | wc -c | tr -d ' ')
first=$(printf '%s\n' "$passage" | head -n 40)
second=$(printf '%s\n' "$passage" | tail -n 40)
target='Distant target passage!'
[[ "$source_bytes" == 7439 && ${#target} == 23 ]] || exit 2

if [[ "$mode" == forced ]]; then
  actual_source="${first}${second}"
else
  actual_source=$passage
fi
actual_source_bytes=$(printf '%s' "$actual_source" | wc -c | tr -d ' ')
[[ "$actual_source_bytes" == "$([[ "$mode" == forced ]] && echo 7438 || echo 7439)" ]] || exit 2

data="$XDG_DATA_HOME/seventh-$mode-$width"
config="$XDG_CONFIG_HOME/seventh-$mode-$width"
cache="$XDG_CACHE_HOME/seventh-$mode-$width"
mkdir -p "$data" "$config" "$cache"
printf '%s' "$actual_source" >"$evidence/source-fixture.txt"
if [[ "$mode" == forced ]]; then entry_name='Untitled · a'; else entry_name='Doc 2'; fi
viewport="$width,$height"
source_range="0,$actual_source_bytes"
target_range="0,${#target}"
await_control="45000,$entry_name=selected"
app=("$bin/xuzz" --backend opengles --resize "$viewport" --chord Ctrl+Alt+Shift+F --key backspace --type 1.5 --key enter --chord Ctrl+N)
if [[ "$mode" == forced ]]; then
  app+=(--type "$first" --chord Ctrl+Return --type "$second")
else
  app+=(--type "$passage")
fi
app+=(--select "$source_range"
  --dump-a11y --capture "$raw/00a-source-selection.ppm"
  --chord 'Ctrl+Alt+[' --chord Ctrl+N --type "$target"
  --select "$target_range" --chord 'Ctrl+Alt+]' --chord Ctrl+Alt+L
  --chord Alt+Shift+N --wait-ms 5000 --dump-a11y --capture "$raw/00-link-preview.ppm" --wait-ms 20000
  --click-label Enter
  --await-a11y "$await_control"
  --dump-a11y --capture "$raw/03-enter-selected.ppm" --wait-ms 5000
  --mouse-move "400,300" --wheel "0,-1" --wait-ms 1500 --dump-a11y --capture "$raw/04-pan-1.ppm"
  --wheel "0,-1" --wait-ms 1500 --dump-a11y --capture "$raw/05-pan-2.ppm"
  --wheel "0,-1" --wait-ms 1500 --dump-a11y --capture "$raw/06-pan-3.ppm"
  --wheel "0,-1" --wait-ms 1500 --dump-a11y --capture "$raw/07-pan-4.ppm"
  --wheel "0,-1" --wait-ms 1500 --dump-a11y --capture "$raw/08-pan-5.ppm"
  --wheel "0,-1" --wait-ms 1500 --dump-a11y --capture "$raw/09-pan-6.ppm"
  --dump-a11y --capture "$raw/10-continuation-wheel.ppm"
  --chord Alt+Shift+B --wait-ms 3000 --dump-a11y --capture "$raw/11-back.ppm"
  --chord Alt+Shift+O --wait-ms 3000 --dump-a11y --capture "$raw/12-origin.ppm"
  --chord Ctrl+Q)
{
  printf 'binary_sha256='
  sha256sum "$bin/xuzz" | cut -d ' ' -f1
  printf 'binary_dir=%s\nmode=%s\nsource_bytes=%s\nsource_lines=80\ntarget=%s\ntarget_bytes=%s\n' \
    "$bin" "$mode" "$actual_source_bytes" "$target" "${#target}"
  [[ "$mode" == forced ]] && printf 'page_break=Ctrl+Return at byte 3719\n' || printf 'page_break=none\n'
  printf 'backend=opengles\nviewport=%sx%s\nprofile_data=%s\nprofile_config=%s\nprofile_cache=%s\n' \
    "$width" "$height" "$data" "$config" "$cache"
  printf 'argv='
  printf '%q ' "${app[@]}" | sed 's/[[:space:]]\+$//'
  printf '\n'
} >"$evidence/argv.txt"

XDG_DATA_HOME="$data" XDG_CONFIG_HOME="$config" XDG_CACHE_HOME="$cache" \
  LD_LIBRARY_PATH="$bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  SPDLOG_LEVEL='text.layout=debug,xudu.links=debug,xuzz.keymap=debug' \
  stdbuf -oL -eL timeout --signal=TERM --kill-after=5s 100s xvfb-run -a -s '-noreset -screen 0 1440x1000x24' \
  "${app[@]}" >"$evidence/session.log" 2>&1 &
app_pid=$!

snapshot_stores() {
  local label=$1 root="$data/xudu/xanadocs"
  mapfile -t stores < <(find "$root" -mindepth 1 -maxdepth 1 -type d ! -name default 2>/dev/null | sort)
  {
    printf 'count=%s\n' "${#stores[@]}"
    for i in "${!stores[@]}"; do printf '%s=%s\n' "$i" "${stores[$i]}"; done
  } >"$evidence/profile-stores-$label.txt"
  for i in "${!stores[@]}"; do
    "$bin/xudu-dump" --section=ops --permascroll="$data/xuzz/permascroll/default" "${stores[$i]}" \
      >"$evidence/source-$label-$i.txt" 2>&1 || true
  done
  if [[ -e "$data/xudu/activity" ]]; then
    "$bin/xudu-dump" --section=ops --permascroll="$data/xuzz/permascroll/default" "$data/xudu/activity" \
      >"$evidence/activity-$label.txt" 2>&1 || true
  fi
}

# Snapshot after preview capture and during the explicit 20s wait before Enter.
marker_seen=no
for _ in $(seq 1 70); do
  if grep -q '00-link-preview.ppm' "$evidence/session.log" 2>/dev/null; then
    marker_seen=yes
    break
  fi
  if ! kill -0 "$app_pid" 2>/dev/null; then break; fi
  sleep 1
done
printf '%s\n' "$marker_seen" >"$evidence/pre-navigation-marker.txt"
snapshot_stores before

enter_seen=no
for _ in $(seq 1 70); do
  if grep -q '03-enter-selected.ppm' "$evidence/session.log" 2>/dev/null; then
    enter_seen=yes
    snapshot_stores enter
    break
  fi
  if ! kill -0 "$app_pid" 2>/dev/null; then break; fi
  sleep 1
done
printf '%s\n' "$enter_seen" >"$evidence/enter-snapshot-marker.txt"
wait "$app_pid"
status=$?
printf '%s\n' "$status" >"$evidence/exit-status.txt"
snapshot_stores after

sha256sum "$evidence"/* "$raw"/*.ppm 2>/dev/null >"$evidence/SHA256SUMS" || true
exit "$status"
