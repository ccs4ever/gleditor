#!/usr/bin/env bash
set -euo pipefail

: "${MSYSTEM:?run this from an MSYS2 UCRT64 or MINGW64 shell}"
: "${MINGW_PREFIX:?MINGW_PREFIX is not set}"
destination=${1:?usage: build-rnp.sh <dependency build directory>}
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
archive="$destination/rnp-v0.18.1.tar.gz"
if [ ! -f "$archive" ]; then
  curl -fL --retry 3 \
    https://github.com/rnpgp/rnp/releases/download/v0.18.1/rnp-v0.18.1.tar.gz \
    -o "$archive.tmp"
  mv "$archive.tmp" "$archive"
fi
checksum=423c8e32e1e591462f759adf8441b1c44bca96d9f5daff13b82e81a79f18ecfd
echo "$checksum  $archive" | sha256sum -c -
tar -xf "$archive" -C "$destination"
# The release archive includes its pinned sexpp dependency and version helper;
# a GitHub-generated source archive would fetch additional unpinned content.
test -f "$destination/rnp-v0.18.1/cmake/version.cmake"
test -f "$destination/rnp-v0.18.1/src/libsexpp/CMakeLists.txt"
# RNP's nested OpenSSL feature probe must use the same MinGW generator instead
# of picking a separately installed Visual Studio toolchain on the runner.
CMAKE_GENERATOR='MSYS Makefiles' cmake \
  -S "$destination/rnp-v0.18.1" -B "$destination/rnp-build" \
  -G 'MSYS Makefiles' \
  -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" \
  -DCMAKE_PREFIX_PATH="$MINGW_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DDOWNLOAD_GTEST=OFF \
  -DENABLE_DOC=OFF -DCRYPTO_BACKEND=openssl
make -j"$(nproc)" -C "$destination/rnp-build" install
mkdir -p "$MINGW_PREFIX/share/licenses/rnp"
cp "$destination/rnp-v0.18.1"/LICENSE* "$MINGW_PREFIX/share/licenses/rnp/"
cp "$destination/rnp-v0.18.1/src/libsexpp/LICENSE.md" \
  "$MINGW_PREFIX/share/licenses/rnp/LICENSE-sexpp.md"
pkg-config --modversion librnp
