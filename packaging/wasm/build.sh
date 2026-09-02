#!/usr/bin/env bash
# Build gleditor and zigzag as WebAssembly / WebGL2 web applications using Emscripten.
#
# Usage:
#   packaging/wasm/build.sh [output-dir]
#
# Prerequisites:
#   Emscripten SDK installed and active (e.g. `source /path/to/emsdk/emsdk_env.sh`).
#   Builds output HTML, JS, WASM, and DATA files ready to serve via any static HTTP server.

set -eu

OUTPUT_DIR=${1:-build/wasm}
ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)

if ! command -v em++ >/dev/null 2>&1; then
  echo "ERROR: em++ not found in PATH." >&2
  echo "Please install and activate Emscripten SDK (e.g. 'emsdk activate latest && source emsdk_env.sh')." >&2
  exit 1
fi

echo "==> Building gleditor WebAssembly artifacts to $OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

cd "$ROOT_DIR"

EM_FLAGS=(
  -std=c++2c
  -O3
  -Iinclude
  -Isrc
  -Iapps
  -Ithirdparty/Choreograph/src
  -Ithirdparty/argparse/include
  -DGLEDITOR_SDL_MAJOR=2
  -DGLM_ENABLE_EXPERIMENTAL
  -DGLEDITOR_DISABLE_VULKAN=1
  -DGLEDITOR_WASM=1
  -sUSE_SDL=2
  -sUSE_FREETYPE=1
  -sUSE_HARFBUZZ=1
  -sUSE_LIBPNG=1
  -sUSE_ZLIB=1
  -sMAX_WEBGL_VERSION=2
  -sMIN_WEBGL_VERSION=2
  -sFULL_ES3=1
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=67108864
  # Quoted because the brackets are emcc's list syntax, not the shell's: bare,
  # this is a glob that silently rewrites the flag if anything in the working
  # directory happens to match it, and shfmt refuses to parse it at all.
  "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS']"
  --shell-file packaging/wasm/shell.html
  --preload-file assets@assets
)

# Common library source files
LIB_SRCS=$(find src -name '*.cpp' ! -name 'device_vk*.cpp' ! -name 'platform_accesskit.cpp')

echo "==> Compiling gleditor WebAssembly target..."
em++ "${EM_FLAGS[@]}" \
  $LIB_SRCS \
  apps/gleditor/main.cpp \
  -o "$OUTPUT_DIR/gleditor.html"

echo "==> Compiling zigzag WebAssembly target..."
ZIGZAG_SRCS=$(find apps/zigzag/core -name '*.cpp' ! -name 'preflet_fetcher.cpp')
em++ "${EM_FLAGS[@]}" \
  $LIB_SRCS \
  $ZIGZAG_SRCS \
  apps/zigzag/zigzag_visualizer.cpp \
  apps/zigzag/main.cpp \
  -o "$OUTPUT_DIR/zigzag.html"

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
    <a class="card" href="zigzag.html">
      <h2>zigzag</h2>
      <p>Project Xanadu Zigzag multidimensional slice visualizer with interactive 3D rank navigation.</p>
    </a>
  </div>
</body>
</html>
EOF

echo "==> WebAssembly build complete! Output files in $OUTPUT_DIR:"
ls -lh "$OUTPUT_DIR"
