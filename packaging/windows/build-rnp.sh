#!/usr/bin/env bash
set -euo pipefail

: "${MSYSTEM:?run this from an MSYS2 UCRT64 or MINGW64 shell}"
: "${MINGW_PREFIX:?MINGW_PREFIX is not set}"
destination=${1:?usage: build-rnp.sh <dependency build directory>}
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
script_dir=$(cd "$(dirname "$0")" && pwd)
"$script_dir/../rnp/fetch.sh" "$destination"
# RNP's nested OpenSSL probe must use the same MinGW generator.
CMAKE_GENERATOR='MSYS Makefiles' "$script_dir/../rnp/build.sh" \
  "$destination/rnp-v0.18.1" "$destination/rnp-build" "$MINGW_PREFIX" \
  -G 'MSYS Makefiles' -DCMAKE_PREFIX_PATH="$MINGW_PREFIX"
pkg-config --modversion librnp
