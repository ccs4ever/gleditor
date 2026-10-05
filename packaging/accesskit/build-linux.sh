#!/bin/sh
set -eu

# Building the static binding supports the package's native architecture and
# avoids depending on a shared library that the distribution does not ship.
source_dir=${1:?usage: build-linux.sh <accesskit-source> <destination-directory>}
destination=${2:?usage: build-linux.sh <accesskit-source> <destination-directory>}
case $(uname -m) in
  aarch64) architecture=arm64 ;;
  i?86) architecture=x86 ;;
  *) architecture=$(uname -m) ;;
esac
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
# Ubuntu installs its supported Rust backport beside the older default toolchain.
cargo_command=${CARGO:-$(command -v cargo-1.85 || command -v cargo)}
export RUSTC="${RUSTC:-$(command -v rustc-1.85 || command -v rustc)}"
"$cargo_command" build --locked --release --manifest-path "$source_dir/Cargo.toml" \
  --target-dir "$destination/cargo"
mkdir -p "$destination/include" "$destination/lib/linux/$architecture/static"
cp "$source_dir/include/accesskit.h" "$destination/include/"
cp "$destination/cargo/release/libaccesskit.a" "$destination/lib/linux/$architecture/static/"
cp "$source_dir/LICENSE-MIT" "$source_dir/LICENSE-APACHE" "$destination/"
