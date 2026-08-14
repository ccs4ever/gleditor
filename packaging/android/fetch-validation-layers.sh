#!/bin/sh
# Fetches the Khronos Vulkan validation layer's prebuilt Android binaries and
# unpacks them into app/src/debug/jniLibs/<abi>/, the source set app/build.gradle
# only merges into debug builds. Nothing here downloads a version pinned in
# this repo: the latest release is asked for every time, since the layer is a
# diagnostic tool rather than a dependency the build's correctness rests on --
# a newer one catching more than an older one is a feature, not a break.
#
# device_vk.cpp already looks for VK_LAYER_KHRONOS_validation at
# vkEnumerateInstanceLayerProperties time and enables it whenever it is
# present (the same thing that happens on desktop, when
# vulkan-validationlayers is installed); the only thing missing on Android is
# getting the .so where the loader will find it, which is what this script is
# for.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
JNI_LIBS_DIR="$SCRIPT_DIR/app/src/debug/jniLibs"

API_URL="https://api.github.com/repos/KhronosGroup/Vulkan-ValidationLayers/releases/latest"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

echo "fetch-validation-layers: asking $API_URL for the latest release"
curl -sSL -H "Accept: application/vnd.github+json" "$API_URL" -o "$WORK_DIR/release.json"

ASSET_URL=$(jq -r '.assets[] | select(.name | test("android"; "i")) | .browser_download_url' "$WORK_DIR/release.json" | head -n1)
if [ -z "$ASSET_URL" ] || [ "$ASSET_URL" = "null" ]; then
  echo "fetch-validation-layers: no Android asset found on the latest release; see $WORK_DIR/release.json" >&2
  cat "$WORK_DIR/release.json" >&2
  exit 1
fi

ASSET_NAME=${ASSET_URL##*/}
echo "fetch-validation-layers: downloading $ASSET_URL"
curl -sSL "$ASSET_URL" -o "$WORK_DIR/$ASSET_NAME"

rm -rf "$JNI_LIBS_DIR"
mkdir -p "$JNI_LIBS_DIR" "$WORK_DIR/extracted"
# .zip in some releases, .tar.gz in others (1.4.357.0's is
# android-binaries-1.4.357.0.tar.gz) -- gone by the asset's own name rather
# than assumed, since which one Khronos publishes has changed before.
case "$ASSET_NAME" in
  *.tar.gz | *.tgz)
    tar -xzf "$WORK_DIR/$ASSET_NAME" -C "$WORK_DIR/extracted"
    ;;
  *.zip)
    unzip -q "$WORK_DIR/$ASSET_NAME" -d "$WORK_DIR/extracted"
    ;;
  *)
    echo "fetch-validation-layers: don't know how to extract $ASSET_NAME" >&2
    exit 1
    ;;
esac

# The archive's internal layout has varied by release (a flat set of <abi>/
# directories in some, one more directory level in others), so this looks for
# libVkLayer_khronos_validation.so wherever it landed rather than assuming a
# fixed depth, and reconstructs only the <abi>/ part of the path that
# jniLibs/ actually needs.
find "$WORK_DIR/extracted" -name "libVkLayer_khronos_validation.so" | while read -r lib; do
  abi=$(basename "$(dirname "$lib")")
  mkdir -p "$JNI_LIBS_DIR/$abi"
  cp "$lib" "$JNI_LIBS_DIR/$abi/"
  echo "fetch-validation-layers: $abi/libVkLayer_khronos_validation.so"
done

if [ -z "$(find "$JNI_LIBS_DIR" -name '*.so' -print -quit)" ]; then
  echo "fetch-validation-layers: extracted archive but found no libVkLayer_khronos_validation.so anywhere in it" >&2
  exit 1
fi
