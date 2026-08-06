# gleditor

OpenGL-based text editor experiment

[![C/C++ CI](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml)

Still a work in progress.

## Overview

`gleditor` is an experimental text editor rendered with OpenGL. It uses SDL3 for windowing/input and Pango/Cairo for text shaping and rasterization. The goal is to explore fast, flexible text rendering in a 2D/3D scene.

- Entry point: `src/main.cpp`
- Rendering pipeline and glyph cache live under `src/` (see `src/glyphcache/*`, `src/renderer.cpp`).

## Tech stack

- Language: C++23
- Build system: GNU Make (no CMake)
- Compiler: clang++ by default (gcc should work)
- Package discovery: pkg-config
- Libraries (via pkg-config):
  - pangomm-2.48 (Pango) and cairomm
  - SDL3, SDL3_image
  - OpenGL, GLU, GLEW
  - GLM (headers)
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
  libsdl3-dev libsdl3-image-dev libglew-dev \
  libgl1-mesa-dev libglu1-mesa-dev \
  libgtest-dev libgmock-dev
```

Notes:
- SDL3 is not yet in the Ubuntu archive; the CI workflow pulls it from the
  `ppa:hrzhu/sdl3-backport` PPA. Building SDL3 and SDL3_image from source works
  equally well.
- The default `LDFLAGS` include `-rtlib=compiler-rt`, which needs the LLVM
  runtime package (`libclang-rt-dev` on Debian/Ubuntu).
- spdlog is not used at present (it was removed due to libc++ linking issues).
- For coverage (`make profile`), install `llvm-profdata` and `llvm-cov` (e.g., `llvm-14-tools` or similar on Ubuntu).  
- Depending on your system, you may also need OpenGL dev headers (e.g., `libgl1-mesa-dev` and `libglu1-mesa-dev`).

## Build

Common targets (see `Makefile`):

- Build everything (app, tests, compile commands):
  - `make`  → builds `build/gleditor`, `build/gleditor_test`, and `build/compile_commands.json`
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
- `STATIC=--static` attempts static linking for libs resolved via pkg-config.

## Run

- After building, run either:
  - `make run`  (runs `build/gleditor`), or
  - `./build/gleditor [options] [files...]`

Command-line options (from `argparse` in `src/main.cpp`):
- `--font <name>`  default: `"Monospace 16"`
- `--profile`      open any provided files and then exit (useful for profiling)
- `files...`       one or more input files to open at startup

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
- Coverage from tests:
  - `make profile`  → generates `gleditor_test.prof` and `coverage.lcov`
    - Requires `llvm-profdata` and `llvm-cov` on PATH.

## Documentation

- Generate API docs with Doxygen:
  - `make doc`
- Output is written to `docs/` (configured in `Doxyfile`).

## Project structure

- `src/`        application sources (OpenGL rendering, glyph cache, SDL wrappers, etc.)
- `include/`    public headers under `gleditor/`
- `tests/`      unit tests (GoogleTest/GoogleMock)
- `thirdparty/` vendored dependencies (argparse, Choreograph, cosmopolitan, etc.)
- `assets/`     assets like `logo.png`
- `docs/`       Doxygen output directory
- `Makefile`    build orchestration
- `Doxyfile`    Doxygen configuration
- `LICENSE`     project license

## Environment and Make variables

- `DEBUG=1`  enable debug flags and sanitizer options in builds.
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

