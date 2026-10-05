#!/usr/bin/env bash
# Keep the provider glue identical to the crate used by the native release.
set -euo pipefail

android_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
dependency_dir=${1:-"$android_dir/.dependencies"}
mkdir -p "$dependency_dir"
"$android_dir/../accesskit/fetch.sh" "$dependency_dir"

crate_version=0.7.5
crate_checksum=8e27dedd5e1932e1f52fedcde681bc2ae4213df53bad5b295103aa36b1d0ffb6
crate_archive="$dependency_dir/accesskit_android-$crate_version.crate"
if [[ ! -f $crate_archive ]]; then
  curl --fail --location --retry 3 \
    "https://crates.io/api/v1/crates/accesskit_android/$crate_version/download" \
    --output "$crate_archive.tmp"
  mv "$crate_archive.tmp" "$crate_archive"
fi
if command -v sha256sum >/dev/null; then
  actual_checksum=$(sha256sum "$crate_archive")
else
  actual_checksum=$(shasum -a 256 "$crate_archive")
fi
if [[ ${actual_checksum%% *} != "$crate_checksum" ]]; then
  echo "Android AccessKit delegate checksum mismatch: $crate_archive" >&2
  exit 1
fi
tar -xf "$crate_archive" -C "$dependency_dir"
test -f "$dependency_dir/accesskit_android-$crate_version/java/dev/accesskit/android/Delegate.java"
