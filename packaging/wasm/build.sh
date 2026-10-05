#!/usr/bin/env bash
# Build gleditor and xuzz as WebAssembly / WebGL2 applications using Emscripten.
#
# Usage:
#   packaging/wasm/build.sh [--a11y-test] [output-dir]
#
# Prerequisites:
#   Emscripten SDK installed and active (e.g. `source /path/to/emsdk/emsdk_env.sh`).
#   Full builds require VCPKG_ROOT and browser ports for native media/identity dependencies.
#   --a11y-test builds the platform adapter independently of those dependencies.

set -eu

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
A11Y_TEST=0
if [[ ${1:-} == --a11y-test ]]; then
  A11Y_TEST=1
  shift
fi
OUTPUT_DIR=${1:-build/wasm}
WASM_CXX=${EMXX:-em++}

if ! command -v "$WASM_CXX" >/dev/null 2>&1; then
  echo "ERROR: em++ not found in PATH." >&2
  echo "Please install and activate Emscripten SDK (e.g. 'emsdk activate latest && source emsdk_env.sh')." >&2
  exit 1
fi

echo "==> Building gleditor WebAssembly artifacts to $OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

cd "$ROOT_DIR"

STD_FLAG=$(make -s -j"$(nproc)" print-std-flag CXX="$WASM_CXX" GLEDITOR_ENABLE_A11Y=0 GLEDITOR_DISABLE_VULKAN=1 GLEDITOR_SDL=2)
COMMON_FLAGS=(
  "$STD_FLAG"
  -O3
  -Iinclude
  -Isrc
  -Iapps
  -Ithirdparty/Choreograph/src
  -Ithirdparty/argparse/include
  -Ithirdparty/nontype_functional/include
  -Ithirdparty/beman_optional/include
  -Ithirdparty/beman_inplace_vector/include
  -Ithirdparty/opengl-registry
  -DGLEDITOR_HAVE_A11Y=1
  --shell-file packaging/wasm/shell.html
  --pre-js packaging/wasm/accessibility.js
)

if [[ $A11Y_TEST == 1 ]]; then
  "$WASM_CXX" "${COMMON_FLAGS[@]}" \
    src/a11y/platform_web.cpp packaging/wasm/test-accessibility.cpp \
    -sNO_EXIT_RUNTIME=1 -o "$OUTPUT_DIR/test-accessibility.html"
  exit 0
fi

if [[ ! -x ${VCPKG_ROOT:-}/vcpkg ]]; then
  echo "ERROR: full WebAssembly builds require VCPKG_ROOT pointing to the pinned vcpkg checkout." >&2
  exit 1
fi
WASM_DEPENDENCIES=${WASM_DEPENDENCIES:-$ROOT_DIR/build/wasm-dependencies}
python3 packaging/wasm/prepare-dependencies.py "$VCPKG_ROOT" "$WASM_DEPENDENCIES/overlays"
"$VCPKG_ROOT/vcpkg" install --triplet=wasm32-gleditor \
  --overlay-triplets="$ROOT_DIR/packaging/wasm/triplets" \
  --overlay-ports="$WASM_DEPENDENCIES/overlays" \
  --x-manifest-root="$ROOT_DIR/packaging/wasm" --x-install-root="$WASM_DEPENDENCIES"
export PKG_CONFIG_LIBDIR="$WASM_DEPENDENCIES/wasm32-gleditor/lib/pkgconfig"
export PKG_CONFIG_PATH=
DEP_CFLAGS_RAW=$(pkg-config --cflags freetype2 harfbuzz fribidi fontconfig poppler-cpp poppler libmagic openssl spdlog)
DEP_LIBS_RAW=$(pkg-config --static --libs freetype2 harfbuzz fribidi fontconfig poppler-cpp poppler libmagic openssl spdlog)
read -r -a DEP_CFLAGS <<<"$DEP_CFLAGS_RAW"
read -r -a DEP_LIBS <<<"$DEP_LIBS_RAW"
mkdir -p "$OUTPUT_DIR/generated"
GLEDITOR_WASM_VERSION=$(cat VERSION)
sed "s/@@VERS@@/$GLEDITOR_WASM_VERSION/" src/config.h.in >"$OUTPUT_DIR/generated/config.h"

EM_FLAGS=(
  "${COMMON_FLAGS[@]}"
  "${DEP_CFLAGS[@]}"
  "-I$WASM_DEPENDENCIES/wasm32-gleditor/include"
  "-I$OUTPUT_DIR/generated"
  -DGLEDITOR_SDL_MAJOR=2
  -DGLM_ENABLE_EXPERIMENTAL
  -DGLEDITOR_DISABLE_VULKAN=1
  -DGLEDITOR_WASM=1
  '-DGLEDITOR_DEFAULT_BACKEND="opengles"'
  -pthread
  -sPROXY_TO_PTHREAD=1
  -sOFFSCREENCANVAS_SUPPORT=1
  -sPTHREAD_POOL_SIZE=2
  -fexceptions
  -sUSE_SDL=2
  -sMAX_WEBGL_VERSION=2
  -sMIN_WEBGL_VERSION=2
  -sFULL_ES3=1
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=67108864
  # Quoted because the brackets are emcc's list syntax, not the shell's: bare,
  # this is a glob that silently rewrites the flag if anything in the working
  # directory happens to match it, and shfmt refuses to parse it at all.
  "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS']"
  --preload-file assets@assets
)

# Common library source files
mapfile -t LIB_SRCS < <(find src thirdparty/Choreograph/src -name '*.cpp' ! -path 'src/render/vulkan/*' ! -name 'platform_accesskit.cpp' ! -name 'platform_none.cpp' ! -name 'platform_android.cpp' | sort)

echo "==> Compiling gleditor WebAssembly target..."
"$WASM_CXX" "${EM_FLAGS[@]}" \
  "${LIB_SRCS[@]}" \
  apps/gleditor/main.cpp apps/gleditor/editor_config.cpp \
  "${DEP_LIBS[@]}" "$WASM_DEPENDENCIES/wasm32-gleditor/lib/libunibreak.a" \
  -o "$OUTPUT_DIR/gleditor.html"

echo "==> Compiling xuzz WebAssembly target..."
mapfile -t COMMON_XANADU_SRCS < <(find apps/common/xanadu -name '*.cpp')
mapfile -t XUDU_SRCS < <(find apps/xudu -maxdepth 1 -name '*.cpp')
mapfile -t ZIGZAG_SRCS < <(find apps/zigzag -name '*.cpp')
mapfile -t XUZZ_SRCS < <(find apps/xuzz -name '*.cpp')
"$WASM_CXX" "${EM_FLAGS[@]}" \
  "${LIB_SRCS[@]}" \
  "${COMMON_XANADU_SRCS[@]}" \
  "${XUDU_SRCS[@]}" \
  "${ZIGZAG_SRCS[@]}" \
  "${XUZZ_SRCS[@]}" \
  "${DEP_LIBS[@]}" "$WASM_DEPENDENCIES/wasm32-gleditor/lib/libunibreak.a" \
  -o "$OUTPUT_DIR/xuzz.html"

# Generate index page
cat >"$OUTPUT_DIR/index.html" <<'EOF'
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>gleditor - WebAssembly Suite</title>
  <style>
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      background: #121214;
      color: #e0e0e0;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      min-height: 100vh;
      margin: 0;
    }
    h1 { color: #ffffff; margin-bottom: 8px; }
    p { color: #9aa0a6; margin-top: 0; margin-bottom: 32px; }
    .apps { display: flex; gap: 24px; flex-wrap: wrap; justify-content: center; }
    .card {
      background: #1e1e24;
      border: 1px solid #2a2a34;
      border-radius: 12px;
      padding: 24px;
      width: 280px;
      text-decoration: none;
      color: inherit;
      transition: transform 0.2s, border-color 0.2s;
    }
    .card:hover {
      transform: translateY(-4px);
      border-color: #0096ff;
    }
    .card h2 { color: #0096ff; margin-top: 0; }
    .card p { color: #b0b0b8; margin: 0; font-size: 14px; line-height: 1.5; }
  </style>
</head>
<body>
  <h1>gleditor WebAssembly Suite</h1>
  <p>GPU-rendered document library and hypertext research suite in WebAssembly & WebGL2</p>
  <div class="apps">
    <a class="card" href="gleditor.html">
      <h2>gleditor</h2>
      <p>Plain text GPU editor with HarfBuzz shaping, multi-file navigation, and subpixel quad rendering.</p>
    </a>
    <a class="card" href="xuzz.html">
      <h2>xuzz</h2>
      <p>Project Xanadu hypertext and Zigzag multidimensional hypergrid visualizer and editor.</p>
    </a>
  </div>
</body>
</html>
EOF

echo "==> WebAssembly build complete! Output files in $OUTPUT_DIR:"
ls -lh "$OUTPUT_DIR"
