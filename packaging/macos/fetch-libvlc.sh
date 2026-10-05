#!/usr/bin/env bash
set -euo pipefail

destination=${1:?usage: fetch-libvlc.sh <dependency directory>}
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
case $(uname -m) in
  arm64)
    vlc_arch=arm64
    checksum=fc6fac08d87f538517d44aca0c5e7a244b67c8c4cb589bf478363a7315fd5e0d
    ;;
  x86_64)
    vlc_arch=intel64
    checksum=ec01530ce69d849dd057fba8876e68ac39bf279dc28de4e9c04e4aec11fc98db
    ;;
  *)
    echo "Unsupported macOS LibVLC architecture: $(uname -m)" >&2
    exit 1
    ;;
esac
fetch() {
  local url=$1 archive=$2 expected=$3
  if [ ! -f "$archive" ]; then
    curl -fL --retry 3 "$url" -o "$archive.tmp"
    mv "$archive.tmp" "$archive"
  fi
  echo "$expected  $archive" | shasum -a 256 -c -
}
dmg="$destination/vlc-3.0.23-$vlc_arch.dmg"
source_archive="$destination/vlc-3.0.23.tar.xz"
fetch "https://download.videolan.org/pub/videolan/vlc/3.0.23/macosx/$(basename "$dmg")" \
  "$dmg" "$checksum"
fetch https://download.videolan.org/pub/videolan/vlc/3.0.23/vlc-3.0.23.tar.xz \
  "$source_archive" e891cae6aa3ccda69bf94173d5105cbc55c7a7d9b1d21b9b21666e69eff3e7e0
tar -xf "$source_archive" -C "$destination" \
  vlc-3.0.23/COPYING vlc-3.0.23/COPYING.LIB
mountpoint=$(mktemp -d "$destination/mount.XXXXXX")
cleanup() {
  hdiutil detach "$mountpoint" >/dev/null 2>&1 || true
  rmdir "$mountpoint" 2>/dev/null || true
}
trap cleanup EXIT
hdiutil attach -nobrowse -readonly -mountpoint "$mountpoint" "$dmg" >/dev/null
bash "$(dirname "$0")/prepare-libvlc.sh" "$mountpoint/VLC.app/Contents" \
  "$destination/vlc-3.0.23" "$destination/sdk"
