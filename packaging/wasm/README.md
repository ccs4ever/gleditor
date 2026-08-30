# WebAssembly & WebGL 2.0 Build

`gleditor` can be compiled into standalone WebAssembly (Wasm) modules with WebGL 2.0 rendering via [Emscripten](https://emscripten.org/).

## Prerequisites

1. Install the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html):
   ```sh
   git clone https://github.com/emscripten-core/emsdk.git
   cd emsdk
   ./emsdk install latest
   ./emsdk activate latest
   source ./emsdk_env.sh
   ```

2. Ensure git submodules are initialized in the `gleditor` repository:
   ```sh
   git submodule update --init --recursive
   ```

## Building

Run the build script:

```sh
./packaging/wasm/build.sh
```

This compiles `gleditor` and `zigzag` and outputs the resulting `.html`, `.js`, `.wasm`, and `.data` asset packages to `build/wasm/`.

## Running Locally

Because modern web browsers restrict loading WebAssembly and assets over `file://` URIs due to CORS policies, serve the output directory over a local HTTP server:

```sh
python3 -m http.server -d build/wasm 8080
```

Then open `http://localhost:8080` in your web browser:
- `http://localhost:8080/` — Suite landing portal
- `http://localhost:8080/gleditor.html` — Plain editor
- `http://localhost:8080/zigzag.html` — Project Xanadu Zigzag visualizer
