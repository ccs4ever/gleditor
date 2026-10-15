#!/usr/bin/env bash
set -euo pipefail

root=/tmp/spatial-local-validation.W4mSRr/gles/native-enlarged/retry5
bin=/data/git/gleditor-restore/.worktrees/spatial-ui-overhaul/build
profile=$root/profile
evidence=$root/evidence
raw=$root/raw
mkdir -p "$evidence" "$raw"

export PATH="/tmp/spatial-xvfb/usr/bin:$PATH"
export SDL_VIDEODRIVER=x11
export SDL_AUDIODRIVER=dummy
export LIBGL_ALWAYS_SOFTWARE=1
export XDG_DATA_HOME="$profile/data"
export XDG_CONFIG_HOME="$profile/config"
export XDG_CACHE_HOME="$profile/cache"
export LD_LIBRARY_PATH="$bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SPDLOG_LEVEL='xudu.settings=debug'

source_text=$(
  python3 - <<'PY'
for index in range(1, 13):
    print(f"Paragraph {index:02d}: text grows clearly and stays readable.")
PY
)
printf '%s\n' "$source_text" >"$evidence/source-fixture.txt"
{
  printf 'xuzz_sha256='
  sha256sum "$bin/xuzz" | cut -d ' ' -f1
  printf 'library_sha256='
  sha256sum "$bin/libgleditor.so.0" | cut -d ' ' -f1
  printf 'xudu_dump_sha256='
  sha256sum "$bin/xudu-dump" | cut -d ' ' -f1
} >"$evidence/binary-hashes.txt"
printf 'binary_dir=%s\nbackend=opengles\nviewport=800x600\n' "$bin" >"$evidence/run-metadata.txt"
printf 'profile_data=%s\nprofile_config=%s\nprofile_cache=%s\n' "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" >>"$evidence/run-metadata.txt"
printf 'route=Ctrl+O,Tab,type system://layout,Enter,F6,F2,type semantic readableTextPx query,Enter,Escape,E,type 24.0,Return,Ctrl+1\n' >>"$evidence/run-metadata.txt"

snapshot() {
  local phase=$1
  local rootdir="$XDG_DATA_HOME/xudu/xanadocs"
  mapfile -t stores < <(find "$rootdir" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort)
  printf 'count=%s\n' "${#stores[@]}" >"$evidence/profile-stores-$phase.txt"
  for i in "${!stores[@]}"; do
    printf '%s=%s\n' "$i" "${stores[$i]}" >>"$evidence/profile-stores-$phase.txt"
    "$bin/xudu-dump" --section=ops --permascroll="$XDG_DATA_HOME/xuzz/permascroll/default" "${stores[$i]}" >"$evidence/store-$phase-$i.ops.txt" 2>&1 || true
  done
  if [[ -d "$XDG_CONFIG_HOME/xuzz/system/layout" ]]; then
    "$bin/xudu-dump" --section=ops --permascroll="$XDG_DATA_HOME/xuzz/permascroll/default" "$XDG_CONFIG_HOME/xuzz/system/layout" >"$evidence/layout-$phase.ops.txt" 2>&1 || true
  fi
}

# Commas are intentional values for the xuzz CLI options.
# shellcheck disable=SC2054
common=(--backend opengles --resize 800,600)
# shellcheck disable=SC2054
app=("$bin/xuzz" "${common[@]}" --type "$source_text" --wait-ms 2500 --dump-a11y --capture "$raw/01-before-setting.ppm"
  --chord Ctrl+O --wait-ms 1000 --key tab --type system://layout --key enter --wait-ms 2500
  --dump-a11y --capture "$raw/02-layout-open.ppm"
  --chord F6 --chord F2 --type '##/d.vars[. = "readableTextPx"]/d.values' --key enter --wait-ms 2500
  --dump-a11y --capture "$raw/03-semantic-value.ppm" --wait-ms 5000
  --key escape --chord E --type 24.0 --key enter --wait-ms 5000
  --dump-a11y --capture "$raw/04-layout-updated.ppm"
  --chord Ctrl+1 --await-a11y 10000,default=selected --wait-ms 1000 --dump-a11y --capture "$raw/05-source-live-after.ppm"
  --chord Ctrl+Q)
{
  printf 'argv='
  printf '%q ' "${app[@]}" | sed 's/[[:space:]]\+$//'
  printf '\n'
} >>"$evidence/run-metadata.txt"

xvfb-run -a -s '-noreset -screen 0 1440x1000x24' stdbuf -oL -eL timeout --signal=TERM --kill-after=5s 120s "${app[@]}" >"$evidence/session-first.log" 2>&1 &
pid=$!
for _ in $(seq 1 100); do
  if grep -q '01-before-setting.ppm' "$evidence/session-first.log" 2>/dev/null; then
    snapshot before
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then break; fi
  sleep 1
done
for _ in $(seq 1 100); do
  if [[ -f "$raw/03-semantic-value.ppm" ]]; then
    stat -c '%y' "$raw/03-semantic-value.ppm" >"$evidence/semantic-frame-time.txt"
    date -u +'%Y-%m-%dT%H:%M:%S.%NZ' >"$evidence/pre-edit-snapshot-started.txt"
    snapshot pre-edit
    date -u +'%Y-%m-%dT%H:%M:%S.%NZ' >"$evidence/pre-edit-snapshot-completed.txt"
    printf 'yes\n' >"$evidence/pre-edit-snapshot-marker.txt"
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then break; fi
  sleep 1
done
for _ in $(seq 1 100); do
  if grep -q '04-layout-updated.ppm' "$evidence/session-first.log" 2>/dev/null; then
    snapshot after
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then break; fi
  sleep 1
done
if wait "$pid"; then first_status=0; else first_status=$?; fi
printf '%s\n' "$first_status" >"$evidence/first-exit-status.txt"
snapshot after-exit

# Reopen the same isolated profile and verify the applied setting is live.
# shellcheck disable=SC2054
reopen=("$bin/xuzz" "${common[@]}" --chord Ctrl+1 --await-a11y 10000,default=selected --wait-ms 1000 --dump-a11y --capture "$raw/06-source-after-reopen.ppm" --chord Ctrl+Q)
{
  printf 'reopen_argv='
  printf '%q ' "${reopen[@]}" | sed 's/[[:space:]]\+$//'
  printf '\n'
} >>"$evidence/run-metadata.txt"
if xvfb-run -a -s '-noreset -screen 0 1440x1000x24' stdbuf -oL -eL timeout --signal=TERM --kill-after=5s 60s "${reopen[@]}" >"$evidence/session-reopen.log" 2>&1; then reopen_status=0; else reopen_status=$?; fi
printf '%s\n' "$reopen_status" >"$evidence/reopen-exit-status.txt"
snapshot after-reopen

# Identify unchanged source store dump by byte-for-byte comparisons.
for before in "$evidence"/store-before-*.ops.txt; do
  [[ -f "$before" ]] || continue
  suffix=${before#*store-before-}
  suffix=${suffix%.ops.txt}
  if cmp -s "$before" "$evidence/store-after-$suffix.ops.txt" && cmp -s "$before" "$evidence/store-after-reopen-$suffix.ops.txt"; then
    printf '%s\n' "$suffix" >>"$evidence/unchanged-source-store-indexes.txt"
  fi
done
sha256sum "$bin/xuzz" "$bin/libgleditor.so.0" "$bin/xudu-dump" >"$evidence/binary-hashes-after.txt"
