#!/usr/bin/env bash
set -euo pipefail

android_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
dependency_dir=${1:-"$android_dir/.dependencies"}
mkdir -p "$dependency_dir/libvlc"

fetch_checked() {
  local url=$1 archive=$2 expected=$3 actual
  if [[ ! -f $archive ]]; then
    curl --fail --location --retry 3 "$url" --output "$archive.tmp"
    mv "$archive.tmp" "$archive"
  fi
  if command -v sha256sum >/dev/null; then
    actual=$(sha256sum "$archive")
  else
    actual=$(shasum -a 256 "$archive")
  fi
  if [[ ${actual%% *} != "$expected" ]]; then
    echo "LibVLC dependency checksum mismatch: $archive" >&2
    exit 1
  fi
}

aar="$dependency_dir/libvlc-all-3.7.7.aar"
headers="$dependency_dir/vlc-3.0.21.tar.xz"
fetch_checked \
  https://repo.maven.apache.org/maven2/org/videolan/android/libvlc-all/3.7.7/libvlc-all-3.7.7.aar \
  "$aar" b48dab96e0e90e34cce3861963500c144a5aadb1b249d501d6a4b104d849e61b
fetch_checked \
  https://download.videolan.org/pub/videolan/vlc/3.0.21/vlc-3.0.21.tar.xz \
  "$headers" 24dbbe1d7dfaeea0994d5def0bbde200177347136dbfe573f5b6a4cee25afbb0

# The public 3.0 C ABI is backward compatible with the AAR's newer 3.0 runtime.
tar -xf "$headers" -C "$dependency_dir/libvlc" --strip-components=1 \
  vlc-3.0.21/include/vlc vlc-3.0.21/COPYING.LIB
for abi in arm64-v8a x86_64; do
  mkdir -p "$dependency_dir/libvlc/jni/$abi"
  unzip -p "$aar" "jni/$abi/libvlc.so" >"$dependency_dir/libvlc/jni/$abi/libvlc.so"
done
