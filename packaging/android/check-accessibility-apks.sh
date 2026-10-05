#!/usr/bin/env bash
set -euo pipefail

android_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ndk_dir=${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must name the build NDK}
nm_tool="$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
scratch_dir=$(mktemp -d)
trap 'rm -rf "$scratch_dir"' EXIT
for flavor in arm64 x86_64; do
  abi=$flavor
  [[ $flavor != arm64 ]] || abi=arm64-v8a
  apk="$android_dir/app/build/outputs/apk/$flavor/debug/app-$flavor-debug.apk"
  unzip -p "$apk" "lib/$abi/libmain.so" >"$scratch_dir/libmain.so"
  "$nm_tool" -D "$scratch_dir/libmain.so" >"$scratch_dir/symbols.txt"
  grep -q 'accesskit_android_injecting_adapter_new' "$scratch_dir/symbols.txt"
  unzip -p "$apk" 'classes*.dex' >"$scratch_dir/classes.dex"
  strings "$scratch_dir/classes.dex" >"$scratch_dir/classes.txt"
  grep -q 'Ldev/accesskit/android/Delegate;' "$scratch_dir/classes.txt"
  unzip -Z1 "$apk" >"$scratch_dir/entries.txt"
  for source in "$android_dir/../../assets/shaders/"*.glsl; do
    name=${source##*/}
    grep -Fxq "assets/shaders/$name" "$scratch_dir/entries.txt"
    grep -Fxq "assets/shaders/vulkan/${name%.glsl}.spv" "$scratch_dir/entries.txt"
  done
  printf 'accessibility %s: native adapter and Java delegate present\n' "$abi"
done
