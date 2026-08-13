#!/usr/bin/env bash
#
# Build gleditor under MSYS2 and bundle it into a zip that runs on a Windows
# machine with no MSYS2 on it.
#
# Run from an MSYS2 UCRT64 (or MINGW64) shell, from the repository root:
#
#   ./packaging/windows/build-msys2.sh
#
# The dependencies are the GTK stack -- pangomm, cairomm, glibmm -- which MSYS2
# packages and vcpkg largely does not. That is the reason this is an MSYS2
# build rather than an MSVC one.
#
# What comes out is a directory tree, not an installer:
#
#   gleditor/
#     gleditor.exe
#     *.dll            every non-system library the exe needs, found by ldd
#     assets/          shaders, SPIR-V and the icon
#
# assets/ sits beside the executable on purpose. That is the first place
# gleditor::assetDir() looks, so the tree is relocatable: unzip it anywhere and
# it finds its own data.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cd "$root"

: "${MSYSTEM:?run this from an MSYS2 UCRT64 or MINGW64 shell}"
: "${MINGW_PREFIX:?MINGW_PREFIX is not set; is this an MSYS2 mingw shell?}"

version=${GLEDITOR_VERSION:-$(cat VERSION 2>/dev/null || echo 0.0.0)}
outdir=${OUTDIR:-$root/build/windows}
stage=$outdir/gleditor

echo "==> building gleditor $version for $MSYSTEM"
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" \
  GLEDITOR_SDL=3 \
  GLEDITOR_ENABLE_VULKAN=1 \
  GLEDITOR_VERSION="$version" \
  lib gleditor xudu shaders

echo "==> staging into $stage"
rm -rf "$stage"
mkdir -p "$stage/assets/shaders/vulkan"
cp build/gleditor.exe "$stage/gleditor.exe" 2>/dev/null || cp build/gleditor "$stage/gleditor.exe"
cp build/xudu.exe "$stage/xudu.exe" 2>/dev/null || cp build/xudu "$stage/xudu.exe"
# The library both programs are linked against. The loader looks in the
# executable's own directory first, so it belongs beside them and needs no
# further arrangement -- but nothing runs without it.
cp build/libgleditor.dll "$stage/"
cp assets/shaders/*.glsl "$stage/assets/shaders/"
cp assets/shaders/vulkan/*.spv "$stage/assets/shaders/vulkan/"
cp logo.png "$stage/assets/logo.png"
cp LICENSE README.md "$stage/"

# AccessKit, when this was built with it. Copied by name rather than left to
# the collector below: the collector takes what is under $MINGW_PREFIX, and
# this came from wherever ACCESSKIT_DIR pointed. Windows has no run path -- the
# loader looks beside the executable -- so the DLL has to be in the bundle or
# nothing starts.
if [ -n "${ACCESSKIT_DIR:-}" ]; then
  akarch=$(uname -m | sed 's/^amd64$/x86_64/;s/^aarch64$/arm64/;s/^i.86$/x86/')
  akdll="$ACCESSKIT_DIR/lib/windows/$akarch/mingw/shared/accesskit.dll"
  if [ -f "$akdll" ]; then
    cp "$akdll" "$stage/"
    echo "==> bundled accesskit.dll"
  else
    echo "ACCESSKIT_DIR is set but $akdll is not there" >&2
    exit 1
  fi
fi

# Every DLL the executable needs that is not part of Windows itself. ldd under
# MSYS2 reports both; the ones under $MINGW_PREFIX are ours to ship and the
# ones in /c/WINDOWS are already on the target machine. Repeated until it
# settles, because the DLLs have DLLs.
echo "==> collecting dependent DLLs"
collect() {
  local changed=1
  while [ "$changed" -eq 1 ]; do
    changed=0
    while read -r dll; do
      local base
      base=$(basename "$dll")
      if [ ! -e "$stage/$base" ]; then
        cp "$dll" "$stage/"
        changed=1
      fi
    done < <(
      ldd "$stage"/*.exe "$stage"/*.dll 2>/dev/null |
        awk '{print $3}' |
        grep -i "^${MINGW_PREFIX}/" || true
    )
  done
}
collect
echo "    bundled $(find "$stage" -maxdepth 1 -name '*.dll' | wc -l) DLLs"

# Pango picks its font backend by loading a module at run time, and on Windows
# that is fontconfig's business. Shipping the etc/fonts tree keeps the editor
# from starting with no fonts at all on a machine that has never seen MSYS2.
if [ -d "$MINGW_PREFIX/etc/fonts" ]; then
  mkdir -p "$stage/etc"
  cp -r "$MINGW_PREFIX/etc/fonts" "$stage/etc/"
fi

echo "==> zipping"
zipfile="$outdir/gleditor-$version-windows-${MSYSTEM,,}.zip"
rm -f "$zipfile"
(cd "$outdir" && zip -qr "$(basename "$zipfile")" gleditor)
echo "wrote $zipfile"
