# gleditor

GPU text editor experiment, with OpenGL, OpenGL ES and Vulkan backends

[![C/C++ CI](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml)

Still a work in progress.

## Overview

`gleditor` is an experimental text editor rendered on the GPU. It uses SDL3 for windowing/input and Pango/Cairo for text shaping and rasterization. The goal is to explore fast, flexible text rendering in a 2D/3D scene.

- Entry point: `src/main.cpp`
- Rendering pipeline and glyph cache live under `src/` (see `src/glyphcache/*`, `src/renderer.cpp`).

## Rendering backends

The graphics API is chosen at run time with `--backend`:

| Backend | Requires | Notes |
| ------- | -------- | ----- |
| `opengl` (default) | OpenGL 3.3 core | |
| `opengles` | OpenGL ES 3.0 | |
| `vulkan` | Vulkan 1.0 | Only when built with `GLEDITOR_ENABLE_VULKAN=1` |

Everything above the backend -- documents, pages, the glyph cache, the buffer
allocator -- is written against `render::RenderDevice` (`include/gleditor/render/`)
and names no graphics API. Adding a backend means implementing that interface;
it does not mean touching the document code.

### How one pipeline serves all three

- **Glyphs are instanced quads.** One instance per glyph, with the four corners
  derived from the vertex index. Expanding points into quads in a geometry
  shader would rule out OpenGL ES 3.0, which has no geometry stage.
- **The draw offset is a buffer binding offset**, not a base-instance draw:
  OpenGL ES has no `glDrawArraysInstancedBaseInstance`.
- **The glyph atlas is single-channel coverage**, narrowed from Cairo's ARGB32
  on the CPU. Uploading BGRA and letting the driver keep one channel is an
  OpenGL convenience with no Vulkan equivalent.
- **The shaders have one source.** `assets/shaders/*.glsl` are written in the
  common subset of GLSL 3.30, GLSL ES 3.00 and Vulkan GLSL; the version
  directive, precision qualifiers, varying locations and uniform declarations
  come from a preamble generated per backend in `src/render/shader_source.cpp`.
  The SPIR-V the Vulkan backend loads is produced at build time from those same
  bodies through that same generator, so the two forms cannot drift apart.
- **Clip space differs and the backend absorbs it.** Vulkan's +Y points down;
  `DeviceVK` negates the projection's Y scale so callers hand every backend the
  same conventional matrix.

## Tech stack

- Language: C++23
- Build system: GNU Make (no CMake)
- Compiler: clang++ by default (gcc should work)
- Package discovery: pkg-config
- Libraries (via pkg-config):
  - pangomm-2.48 (Pango) and cairomm
  - SDL3, SDL3_image
  - Vulkan (only with `GLEDITOR_ENABLE_VULKAN=1`)
  - GLM (headers)
  - The OpenGL and OpenGL ES entry points are resolved at run time through
    `SDL_GL_GetProcAddress`, so no GL library is linked. `GL/glcorearb.h` is
    still needed for its typedefs and enum values.
- Testing: GoogleTest + GoogleMock
- Vendored/third-party: `thirdparty/argparse`, `thirdparty/Choreograph`, `thirdparty/cosmopolitan` toolchain support (optional)

## Requirements

You’ll need a C++23 toolchain, `make`, and the libraries above.

The vendored dependencies are git submodules, so clone with them:

```
git clone --recurse-submodules https://github.com/ccs4ever/gleditor
# or, in an existing clone:
git submodule update --init --recursive
```

On Ubuntu/Debian, for example:

```
sudo apt-get update && sudo apt-get install \
  clang libclang-rt-dev make pkg-config doxygen \
  libglm-dev libpangomm-2.48-dev \
  libsdl3-dev libsdl3-image-dev \
  libgl-dev libgl1-mesa-dev libglu1-mesa-dev \
  libgtest-dev libgmock-dev
```

For the Vulkan backend, additionally:

```
sudo apt-get install libvulkan-dev glslang-tools
# a driver, plus a software one for headless testing:
sudo apt-get install mesa-vulkan-drivers vulkan-validationlayers
```

Notes:
- SDL3 is not yet in the Ubuntu archive; the CI workflow pulls it from the
  `ppa:hrzhu/sdl3-backport` PPA. Building SDL3 and SDL3_image from source works
  equally well.
- The default `LDFLAGS` include `-rtlib=compiler-rt`, which needs the LLVM
  runtime package (`libclang-rt-dev` on Debian/Ubuntu).
- spdlog is not used at present (it was removed due to libc++ linking issues).
- For coverage (`make profile`), install `llvm-profdata` and `llvm-cov` (e.g., `llvm-14-tools` or similar on Ubuntu).  
- Headless testing works with Mesa's software drivers: `llvmpipe` for OpenGL and
  OpenGL ES, `lavapipe` for Vulkan. Both come from `mesa-vulkan-drivers` and the
  usual Mesa packages.

## Build

Common targets (see `Makefile`):

- Build everything (app, tests, compile commands):
  - `make`  → builds `build/gleditor`, `build/gleditor_test`, and `build/compile_commands.json`
- Build with the Vulkan backend:
  - `make GLEDITOR_ENABLE_VULKAN=1`  → also compiles `assets/shaders/vulkan/*.spv`
- Compile the SPIR-V modules only:
  - `make shaders`  (needs `glslangValidator`)
- Build the app only:
  - `make gleditor`  → `build/gleditor`
- Build tests only:
  - `make gleditor_test`  → `build/gleditor_test`
- Clean:
  - `make clean`  → removes `build/` artifacts

Compile commands database (for clangd, etc.):
- Generated automatically by `make` at `build/compile_commands.json`.
- You can also run: `make build/compile_commands.json`.

Optional Make variables:
- `DEBUG=1` enables debug flags and sanitizer flag sets.
- `GLEDITOR_ENABLE_VULKAN=1` compiles the Vulkan backend.
- `STATIC=--static` attempts static linking for libs resolved via pkg-config.

Objects are rebuilt when the compile flags change, so toggling either of the
first two does not leave a binary that disagrees with what was asked for.

## Run

- After building, run either:
  - `make run`  (runs `build/gleditor`), or
  - `./build/gleditor [options] [files...]`

Command-line options (from `argparse` in `src/main.cpp`):
- `--font <name>`     default: `"Monospace 16"`
- `--backend <name>`  `opengl` (default), `opengles` or `vulkan`
- `--profile`         open any provided files and then exit (useful for profiling)
- `--screenshot <path>` write the first settled frame to `<path>` as a binary PPM
- `files...`          one or more input files to open at startup

Help:
- `./build/gleditor --help`

## Running with sanitizers

Sanitizers are wired via Make targets. Set `DEBUG=1` to activate sanitizer flags:

- AddressSanitizer:
  - `make clean && make DEBUG=1 sanitize/address`
  - Run with env defaults: `./build/gleditor`
  - Or use the helper: `make DEBUG=1 sanitize/address/run`
- ThreadSanitizer:
  - `make clean && make DEBUG=1 sanitize/thread`
  - Or: `make DEBUG=1 sanitize/thread/run`
- MemorySanitizer: (requires the entire dependency chain to be MSAN-instrumented)
  - `make clean && make DEBUG=1 sanitize/memory`
  - Or: `make DEBUG=1 sanitize/memory/run`

Note: the `/run` targets set recommended `ASAN_OPTIONS`, `TSAN_OPTIONS`, or `MSAN_OPTIONS` and then execute the binary.

Known sanitizer noise: the sanitizer flag sets include `-fsanitize=integer`,
whose `implicit-integer-sign-change` check fires inside libstdc++'s `<format>`
whenever a negative integer is formatted (`format:3510` and `format:1016` in
GCC 13). Those reports come from the standard library's own modular arithmetic,
not from `gleditor`.

## Usage: Movement and Commands

Movement keys:

```
            Keys        How they move
             e               up
          s d/D f   left  zoomout/in  right
             c              down
```

Other actions:

| Key | Action |
| --- | ------ |
| n   | Create a new page |
| r   | Reset view back to start |
| q   | Quit the application |
| g   | Increment fov by 1 (max 360); use Shift+g to decrement (min 1) |

## Tests

- Build and run tests:
  - `make test`  → builds and runs `build/gleditor_test`
- Compare the backends against each other:
  - `./tools/compare-backends.sh [file]`

  Renders the same document through every compiled-in backend and diffs the
  captured frames. Exiting zero proves very little about a renderer -- a
  backend that draws nothing still exits zero -- so this comparison is what
  actually demonstrates a backend works. OpenGL and OpenGL ES are required to
  match exactly; Vulkan is allowed a small tolerance for edge rounding, since
  it rasterises through a different pipeline.

  Headless, with software drivers:

  ```
  xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh
  ```
- Coverage from tests:
  - `make profile`  → generates `gleditor_test.prof` and `coverage.lcov`
    - Requires `llvm-profdata` and `llvm-cov` on PATH.

## Documentation

- Generate API docs with Doxygen:
  - `make doc`
- Output is written to `docs/` (configured in `Doxyfile`).

## Project structure

- `src/`        application sources (document model, glyph cache, SDL wrappers, etc.)
- `src/render/` the device abstraction and its backends (`gl/`, `vulkan/`)
- `include/`    public headers under `gleditor/`
- `assets/shaders/` portable GLSL bodies, plus generated SPIR-V under `vulkan/`
- `tools/`      build-time and verification helpers
- `tests/`      unit tests (GoogleTest/GoogleMock)
- `thirdparty/` vendored dependencies (argparse, Choreograph, cosmopolitan, etc.)
- `assets/`     assets like `logo.png`
- `docs/`       Doxygen output directory
- `Makefile`    build orchestration
- `Doxyfile`    Doxygen configuration
- `LICENSE`     project license

## Environment and Make variables

- `DEBUG=1`  enable debug flags and sanitizer options in builds.
- `GLEDITOR_ENABLE_VULKAN=1`  compile the Vulkan backend and its SPIR-V.
- `STATIC=--static`  attempt static linking (where supported by your system/libs).
- `ASAN_OPTIONS`, `TSAN_OPTIONS`, `MSAN_OPTIONS`  fine-tune sanitizer behavior (the `/run` targets set sensible defaults).
- LandlockMake: if `LANDLOCKMAKE_VERSION` is set, the Makefile enables a sandbox for builds (optional/developer setup).

## License

This project is licensed under the GPL-3.0. See `LICENSE` for details.  
TODO: Confirm whether the intent is GPL-3.0-only or GPL-3.0-or-later.

## TODOs / Notes

- Cross-platform support (Windows/macOS) has not been documented or tested yet.
- Packaging/distribution not defined.
- The app resolves `assets/glsl` and `logo.png` relative to the working
  directory, so it must be started from the repository root (`make run` does).

