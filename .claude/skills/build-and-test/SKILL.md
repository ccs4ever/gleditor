---
name: build-and-test
description: >-
  Makefile targets/variables, submodules, headless execution, XDG sandboxing, test binaries, netns
  swarm tests, compare-backends.sh, and sample fixtures. Use when compiling, running tests, debugging
  build failures, or regenerating fixtures.
---

# Build and test guide

## Setup: submodules

`thirdparty/argparse`, `Choreograph`, `merklecpp`, `SDL`, `zstd`, `nontype_functional`,
`beman_optional`, and `beman_inplace_vector` are git submodules; the tree does not build without
them. Run `git submodule update --init --recursive` first in any fresh clone or worktree, and re-run
it before investigating any build failure with missing headers under `thirdparty/`. Never vendor or
hand-edit under `thirdparty/`; bump the submodule pointer and commit the gitlink.

## Build commands and targets

Always `make -j$(nproc)`.

```sh
make -j$(nproc)              # lib, every program, test binaries, shaders, compile_commands.json
make -j$(nproc) lib|gleditor|xudu|xuzz|zigzag|store-dump|xudu-dump|vquery|vqueryc|vpl|vplc|vprolog|vlace|vlacec|vicon|viconc|xudu-swarm-peer
make -j$(nproc) shaders      # glslangValidator over every shader; SPIR-V when Vulkan is on
make -j$(nproc) fuzz         # fuzz_binary_ops, fuzz_link_package, fuzz_identity_wire, fuzz_session_package, fuzz_x509_delegation
make profile                 # llvm-cov coverage report for gleditor_test
make clean | dist | doc      # dist bundles repo + submodules into tarball; packaging/ holds distro recipes
make format | format-check   # clang-format + shfmt + yamlfmt + mdformat; -check is what CI runs
make lint                    # shellcheck + yamllint + mdl + check-config-harmony + UI text policy
make tidy | scan-build | cppcheck | analyze (= tidy + scan-build) | check (= format-check + lint + analyze)
make tidy TIDY_FILES=... TIDY_CHECKS=...
```

Optional Make variables:

- `DEBUG=1` — debug flags and sanitizer flag sets.
- `GLEDITOR_DISABLE_VULKAN=1` — no Vulkan backend, no SPIR-V. **Flips the flags of every object**,
  so toggling it rebuilds the tree; `ccache` (automatic when installed; `GLEDITOR_NO_CCACHE=1` to
  disable) makes that cheap.
- `GLEDITOR_SDL=2|3` — pick the SDL major; unset probes pkg-config (SDL3 if present, else SDL2).
- `GLEDITOR_ENABLE_A11Y=1|0` — require/disable AccessKit; unset uses it if found.
- `GLEDITOR_MAIL=1|0` — require/disable GMime 3 + libetpan for mail evidence; unset probes each and
  compiles refusing stubs (`GLEDITOR_HAVE_GMIME`/`GLEDITOR_HAVE_ETPAN`/`GLEDITOR_HAVE_MAIL`) for
  whatever is missing, the `pg_backend.cpp` pattern. Toggling flips flags on every object.
- `CXX=g++` — override the compiler (default `clang++`).

Do not hand-pick `-std=` or hardcode a compiler in new build logic; the Makefile probes.

### Makefile gotchas

- Toggling `GLEDITOR_DISABLE_VULKAN` or `DEBUG` changes every object's flags;
  `$(OBJDIR)/.buildflags` records a signature so stale objects rebuild. Preserve it if touching flag
  handling.
- SDL major is probed, not assumed; GL/GLES entry points come from `SDL_GL_GetProcAddress` — never
  add `-lGL`.
- `format`, `format-check`, `lint`, `doc` probe tools with `command -v` and echo a notice rather
  than fail when missing.
- `NO_SDL_GOALS` (`format`, `format-check`, `lint`, `swarm-image`) are exempt from pkg-config checks
  and `include $(DEPS)` (like `clean`/`dist`) so they run with zero build deps. Never add a
  compiling goal to `NO_SDL_GOALS`.

## Everything runs headless

Never open a window or touch a display, GPU, or audio device unless the user explicitly asks for
visual confirmation (prevents focus stealing and keeps runs reproducible in CI/SSH). `make` exports
`SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy`, `LIBGL_ALWAYS_SOFTWARE=1`, and
`XDG_{DATA,CONFIG,CACHE}_HOME` under `build/xdg/` so tests do not write to your real permascroll
(`$XDG_DATA_HOME`), system xanadocs (`$XDG_CONFIG_HOME`), or LMDB cache (`$XDG_CACHE_HOME`). All six
use `?=`, so explicit overrides (`SDL_VIDEODRIVER=wayland make test`) still work.

Outside `make` (e.g., `./build/xuzz_test --gtest_filter=...`), set them in order of preference:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
  XDG_CACHE_HOME=$PWD/build/xdg/cache <command>                                        # 1. almost always enough
xvfb-run -s "-screen 0 1024x768x24" <command>                                          # 2. when something insists on a display
xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh                        # 3. both; what compare-backends wants
```

`xuzz` (and `xudu`/`zigzag` symlinks) also accept `--headless`, skipping window creation entirely
(used by `tools/create-sample-xanadocs.sh`); prefer it when driving them. For new tools, add a dummy
driver, `Xvfb`, or `--no-window` rather than opening a window.

## Running tests

```sh
make -j$(nproc) test                  # gleditor_test, xuzz_test, zigzag_test, then the rootless swarm tests
make test TEST_FILTER='MediaTest.*'   # gtest filter applied to all three
make test/e2e-orchestration           # tools/xudu-e2e-orchestration.sh against build/xuzz (and build/xudu symlink)
```

- `gleditor_test` links the real shared library (catches export-boundary bugs). `xuzz_test` (which
  integrates the previous `xudu_test` suite; `xudu_test` and `xanadu_test` are symlinks to it) links
  only the xanalogical engine, no graphics device, on purpose. `zigzag_test` covers the slice model
  and the transclusion engine.
- **A test sits with its code, once.** Generic library behaviour is tested in `tests/lib/` only;
  `xuzz_test` does not repeat library sweeps (font, scale, backend).
- `make test` ends with `tools/swarm-netns-test.sh` under `unshare -Urnm`. That needs the `veth`
  kernel module; without it the run ends in `Error: Unknown device type.` and a non-zero exit
  **after all three gtest binaries passed**. `sudo modprobe veth` is the fix and is the user's to
  run. Read the three `[  PASSED  ]` lines before concluding anything is broken.
- `E2EBinaryOrchestrationTest` and `AnimationTransclusionTest` shell out to `build/xuzz` (or the
  `build/xudu` compatibility symlink); binary not found means the app did not link.
- `./tools/compare-backends.sh` renders a sample through every compiled-in backend and diffs the
  PNGs — the real check that a backend still draws, since one that draws nothing exits 0.

Unit tests live in `tests/lib/`, `tests/xuzz/` (incorporating `tests/xudu/`), and `tests/zigzag/`
(GoogleTest/GoogleMock); fuzzers live in `tests/fuzz/`.

## Binary fixtures under `tests/samples/`

`tests/samples/xudu/` — `core_hypertext/` (`unified_store`, `xanadoc_a`, `xanadoc_b`),
`multimedia/01…11`, `beams/01…03`, their generator inputs in `sources/`, and **`permascroll/`, which
is part of the fixture**: every store is an edit decision list whose spans address that one shared
permascroll, so without it every document renders empty. `SampleXanadocsTest` loads them and checks
the permascroll first, by name. `tests/samples/xuzz/slice_then_xanadoc/` is the combined
Xanadu/ZigZag store (`XuzzConvergenceSample`), from `sources/slice_then_xanadoc.xuzz`.

**Any change to `CompactOpNode`'s layout invalidates all of them.** `ops.nodes` records
`sizeof(CompactOpNode)` in its header, so a stale fixture is refused with `OpsSegmentUnreadable`
naming both sizes. Regenerate in the same commit:

```sh
make -j$(nproc) xuzz
./tools/create-sample-xanadocs.sh        # core_hypertext, multimedia, beams, permascroll/, the xuzz sample
./tools/create-floating-image-sample.sh  # multimedia/11_floating_image, which the script above deletes
```

Run both: the first removes the whole `multimedia/` directory, and `11_floating_image` is built by
hand with its own `.permascroll` beside it.
