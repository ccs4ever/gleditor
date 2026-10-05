#!/usr/bin/env bash
set -euo pipefail

contents=${1:?usage: prepare-libvlc.sh <VLC.app/Contents> <VLC source> <SDK directory>}
source_dir=${2:?usage: prepare-libvlc.sh <VLC.app/Contents> <VLC source> <SDK directory>}
sdk=${3:?usage: prepare-libvlc.sh <VLC.app/Contents> <VLC source> <SDK directory>}
test -f "$contents/MacOS/include/vlc/vlc.h"
test -f "$contents/MacOS/lib/libvlc.dylib"
mkdir -p "$sdk"
sdk=$(cd "$sdk" && pwd)
rm -rf "${sdk:?}/include" "${sdk:?}/lib" "${sdk:?}/licenses"
cp -R "$contents/MacOS/include" "$sdk/include"
cp -R "$contents/MacOS/lib" "$sdk/lib"
mkdir -p "$sdk/lib/vlc" "$sdk/lib/pkgconfig" "$sdk/licenses"
# libvlccore's Unix discovery uses its own directory plus /vlc/plugins and
# /vlc/share. Keep that layout so media works without a VLC.app installation.
cp -R "$contents/MacOS/plugins" "$sdk/lib/vlc/plugins"
cp -R "$contents/MacOS/share" "$sdk/lib/vlc/share"
cp -R "$contents/Frameworks" "$sdk/lib/vlc/Frameworks"
install -m 644 "$source_dir/COPYING" "$source_dir/COPYING.LIB" "$sdk/licenses/"
cat >"$sdk/licenses/SOURCE" <<'SOURCE'
VideoLAN VLC 3.0.23 runtime and matching public headers:
https://download.videolan.org/pub/videolan/vlc/3.0.23/macosx/
Corresponding VLC source:
https://download.videolan.org/pub/videolan/vlc/3.0.23/vlc-3.0.23.tar.xz
SOURCE

install_name_tool -change '@rpath/libvlccore.dylib' \
  '@loader_path/libvlccore.dylib' "$sdk/lib/libvlc.dylib"
codesign --force --sign - "$sdk/lib/libvlc.dylib"
while IFS= read -r -d '' plugin; do
  install_name_tool -change '@rpath/libvlccore.dylib' \
    '@loader_path/../../libvlccore.dylib' "$plugin"
  install_name_tool -change \
    '@executable_path/../Frameworks/Growl.framework/Versions/A/Growl' \
    '@loader_path/../Frameworks/Growl.framework/Versions/A/Growl' "$plugin"
  install_name_tool -change '@rpath/Sparkle.framework/Versions/A/Sparkle' \
    '@loader_path/../Frameworks/Sparkle.framework/Versions/A/Sparkle' "$plugin"
  codesign --force --sign - "$plugin"
done < <(find "$sdk/lib/vlc/plugins" -type f -name '*.dylib' -print0)

cat >"$sdk/lib/pkgconfig/libvlc.pc" <<PC
prefix=$sdk
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libvlc
Description: VideoLAN LibVLC multimedia library
Version: 3.0.23
Libs: -L\${libdir} -Wl,-rpath,\${libdir} -lvlc
Cflags: -I\${includedir}
PC
