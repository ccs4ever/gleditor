#!/usr/bin/env bash
set -euo pipefail

libdir=${1:?usage: bundle-accesskit.sh <installed lib directory> <AccessKit release>}
accesskit_dir=${2:?usage: bundle-accesskit.sh <installed lib directory> <AccessKit release>}
arch=$(uname -m)
source_dir="$accesskit_dir/lib/macos/$arch/shared"
gleditor_lib="$libdir/libgleditor.0.dylib"

# The release's @rpath install name would otherwise resolve only through the
# temporary resource directory used while building the package.
install -m 755 "$source_dir/libaccesskit.dylib" "$libdir/libaccesskit.dylib"
install_name_tool -id '@rpath/libaccesskit.dylib' "$libdir/libaccesskit.dylib"
install_name_tool -change '@rpath/libaccesskit.dylib' \
  '@loader_path/libaccesskit.dylib' "$gleditor_lib"
if otool -l "$gleditor_lib" | grep -Fq "path $source_dir (offset"; then
  install_name_tool -delete_rpath "$source_dir" "$gleditor_lib"
fi

# Editing a Mach-O load command invalidates its existing signature, including
# the ad-hoc signature required by the arm64 loader.
codesign --force --sign - "$libdir/libaccesskit.dylib" "$gleditor_lib"

licenses="$libdir/../share/licenses/gleditor/accesskit"
mkdir -p "$licenses"
install -m 644 "$accesskit_dir"/LICENSE* "$licenses/"
otool -L "$gleditor_lib" | grep -Fq '@loader_path/libaccesskit.dylib'
