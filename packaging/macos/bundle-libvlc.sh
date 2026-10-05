#!/usr/bin/env bash
set -euo pipefail

prefix=${1:?usage: bundle-libvlc.sh <installed prefix> <LibVLC SDK>}
sdk=${2:?usage: bundle-libvlc.sh <installed prefix> <LibVLC SDK>}
cp -P "$sdk"/lib/libvlc*.dylib "$prefix/lib/"
cp -R "$sdk/lib/vlc" "$prefix/lib/vlc"
licenses="$prefix/share/licenses/gleditor/libvlc"
mkdir -p "$licenses"
cp "$sdk"/licenses/* "$licenses/"

for binary in "$prefix/lib/libgleditor.0.dylib" \
  "$prefix/bin/gleditor" "$prefix/bin/xuzz"; do
  if [ "$binary" = "$prefix/lib/libgleditor.0.dylib" ]; then
    relative='@loader_path/libvlc.dylib'
  else
    relative='@loader_path/../lib/libvlc.dylib'
  fi
  install_name_tool -change '@rpath/libvlc.dylib' "$relative" "$binary"
  if otool -l "$binary" | grep -Fq "path $sdk/lib (offset"; then
    install_name_tool -delete_rpath "$sdk/lib" "$binary"
  fi
  codesign --force --sign - "$binary"
done
