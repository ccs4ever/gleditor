#!/usr/bin/env bash
set -euo pipefail

source_dir=${1:?usage: build.sh <release-source> <build-directory> <install-prefix> [cmake options]}
build_dir=${2:?usage: build.sh <release-source> <build-directory> <install-prefix> [cmake options]}
install_prefix=${3:?usage: build.sh <release-source> <build-directory> <install-prefix> [cmake options]}
shift 3
# The release archive includes sexpp and the version helper; generated source
# archives would fetch additional content without the release checksum.
test -f "$source_dir/cmake/version.cmake"
test -f "$source_dir/src/libsexpp/CMakeLists.txt"
cmake -S "$source_dir" -B "$build_dir" \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DDOWNLOAD_GTEST=OFF \
  -DENABLE_DOC=OFF -DCRYPTO_BACKEND=openssl "$@"
cmake --build "$build_dir" --parallel "$(nproc)"
cmake --install "$build_dir"
mkdir -p "$install_prefix/share/licenses/rnp"
cp "$source_dir"/LICENSE* "$install_prefix/share/licenses/rnp/"
cp "$source_dir/src/libsexpp/LICENSE.md" \
  "$install_prefix/share/licenses/rnp/LICENSE-sexpp.md"
