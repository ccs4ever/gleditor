# gleditor

GPU-rendered text editor library (`gleditor`) plus two programs built on it:
`apps/gleditor` (plain editor) and `apps/xudu` (a xanadoc/xanalogical editor).
Backends: OpenGL, OpenGL ES, and optionally Vulkan, all driven from one
rendering pipeline. C++23, built with GNU Make + pkg-config — **no CMake**.

The README is the source of truth for anything not covered below (rendering
architecture, accessibility, xudu's data model, SDL2/SDL3 differences, etc.)
— read the relevant section there before making non-trivial changes in that
area.

## Setup: submodules

Vendored deps (`thirdparty/argparse`, `thirdparty/Choreograph`) are git
submodules and the tree will not build without them:

```sh
git submodule update --init --recursive
```

Run this first in any fresh clone or worktree. If a submodule appears empty
or a build fails with missing headers under `thirdparty/`, re-run the above
before investigating anything else. Do not vendor these sources directly
into the tree or hand-edit files under `thirdparty/` — bump the submodule
pointer (`cd thirdparty/<name> && git checkout <ref>`) instead, and commit
the resulting gitlink change in the parent repo.

## Build

```sh
make                              # library, both programs, both test binaries, compile_commands.json
make lib                          # library only
make gleditor                     # apps/gleditor only
make xudu                         # apps/xudu only
make GLEDITOR_ENABLE_VULKAN=1     # also compiles the Vulkan backend + SPIR-V (needs glslangValidator)
make GLEDITOR_SDL=2               # force SDL2 instead of the SDL3/SDL2 auto-probe
make clean
make format                       # clang-format + shfmt, in place; no build deps needed
make format-check                 # same, --dry-run; what CI runs
make lint                         # shellcheck + yamllint + mdl; what CI runs
```

Key variables:

- `DEBUG=1` — debug flags + sanitizer flag sets available.
- `GLEDITOR_ENABLE_VULKAN=1` — compile the Vulkan backend. **This flips the
  compile flags for every object file**, so toggling it rebuilds the whole
  tree. `ccache` (used automatically when installed; disable with
  `GLEDITOR_NO_CCACHE=1`) is what makes flipping it back and forth cheap.
- `GLEDITOR_SDL=2` / `=3` — pick SDL major version explicitly; unset probes
  via pkg-config (SDL3 if present, else SDL2).
- `GLEDITOR_ENABLE_A11Y=1` / `=0` — require/disable AccessKit; unset uses it
  if found.
- `CXX=g++ make` (or `make CXX=g++`) — override the compiler; only used when
  no `CXX` is otherwise set (`clang++` is the default).

Don't hand-pick `-std=` or hardcode a compiler in new build logic — the
Makefile already probes for what the toolchain supports.

## Tests

```sh
make test        # builds + runs gleditor_test and xudu_test, skips slow/network suites
make test/all     # the same, nothing skipped — what CI gates on; prefer this before pushing
make test TEST_FILTER='SwarmTest.*'   # run/override a specific gtest filter
```

- `gleditor_test` links the real shared library (catches export-boundary
  bugs); `xudu_test` links only the xanalogical engine, with no graphics
  device, on purpose — that's the boundary being tested.
- `make test/swarm` runs the network-namespace swarm tests; needs root, not
  part of `make test`.
- `./tools/compare-backends.sh` renders a sample through every compiled-in
  backend and diffs the output — the real check that a backend still draws
  correctly, since a backend that draws nothing still exits 0.

Tests live in `tests/lib/` (library, GoogleTest/GoogleMock) and `tests/xudu/`
(engine). Add new unit tests next to the existing files there, matching the
GoogleTest style already in use.

## Code style

- Every language here has one formatter and one linter (`make format` /
  `make format-check` — CI's `format` job — / `make lint`); for a single C++
  or GLSL file, `clang-format -i --style=file <file>`. See
  `design/formatting-and-linting.md` for the full per-language matrix,
  `.editorconfig`'s role, and the reasoning behind each tool choice — read it
  before adding a new language's tooling or changing an existing one.
- Every `.cpp`/`.hpp`/`.glsl` file ends with a vim modeline,
  `// vi: set sw=2 sts=2 ts=2 et:`, matching `.clang-format`'s
  `IndentWidth: 2`. Carry it over verbatim into new files; a formatter change
  to the indent width should update both.
- `.clangd` enables `modernize-*`, `bugprone-*`, `cppcoreguidelines-*`,
  `performance-*`, `readability-*`, and `portability-*` clang-tidy checks
  (minus a few disabled ones — see `.clangd`) and builds with
  `-Wall -Wextra -std=c++2c`. Treat clangd/clang-tidy warnings on lines you
  touch as worth fixing, not noise. Not yet wired into CI: that means
  triaging the existing warning backlog first.
- Match the prevailing comment style in this codebase: comments explain
  *why* a non-obvious choice was made (a constraint, a workaround, a
  tradeoff), not what the code does. Don't add narrating comments.
- `compile_commands.json` is regenerated by `make` automatically; don't hand
  edit it.

## Project structure

- `src/` — the library: document model, glyph cache, SDL wrappers, render loop
- `src/render/` — device abstraction and backends (`gl/`, `vulkan/`)
- `src/a11y/` — accessibility tree / AccessKit integration
- `include/gleditor/` — the library's public headers
- `apps/gleditor/` — the plain editor program
- `apps/xudu/` — the xanadoc editor; `apps/xudu/core/` is its engine (no graphics dependency)
- `assets/shaders/` — portable GLSL bodies; `vulkan/` holds generated SPIR-V
- `tests/lib/`, `tests/xudu/` — unit tests for the library and the engine, respectively
- `tools/` — build-time and verification helpers (`compare-backends.sh`, `shader_assemble.cpp`, ...)
- `packaging/` — distro packaging (arch, debian, fedora, macos, windows, nix)
- `design/` — design notes and the reasoning behind non-obvious decisions
- `thirdparty/` — vendored dependencies (git submodules; see above)

## Gotchas worth knowing before editing the Makefile

- Toggling `GLEDITOR_ENABLE_VULKAN` or `DEBUG` changes flags for *every*
  object; the build records a flags signature (`$(OBJDIR)/.buildflags`) so
  stale objects get rebuilt rather than silently linked in — preserve that
  mechanism if you touch flag handling.
- SDL major version is auto-probed via pkg-config, not assumed — don't
  hardcode SDL2 or SDL3 assumptions in new code without checking
  `GLEDITOR_SDL` handling.
- GL/GLES entry points are resolved at runtime via `SDL_GL_GetProcAddress`,
  not linked — don't add `-lGL` or equivalent link flags.
- `format`, `format-check` and `lint` are the one place `NO_SDL_GOALS` is
  checked: they're exempted from the top-of-Makefile pkg-config checks *and*
  from `include $(DEPS)` (the same exemption `clean`/`dist` already had, for
  the same reason — see the comment above that `include`), because they
  compile nothing and are meant to run on a checkout that never installed
  SDL, pangomm, or anything else the build needs. A goal that does need a
  compiler must not be added to `NO_SDL_GOALS`.

## Gotchas worth knowing before editing xudu's spool persistence

- `apps/xudu/core/store.cpp`'s `ops.spool` is binary records
  (`encodeOpRecord()`/`decodeOpRecord()` in `ops.hpp`), not the
  one-line-per-operation text older builds wrote. `load()` still reads that
  text shape when it finds no binary magic header at the start of the file —
  don't remove that fallback without a migration story for anyone still
  holding a store written before this existed.
- `Store::save()` is incremental for both spools: `opsFlushed` /
  `flushedOpsDirectory` and `primediaFlushed` / `flushedPrimediaDirectory`
  track how much of each is already on disk, and a save to the same
  directory as the last one appends only what changed rather than rewriting
  the whole spool. Touching either save block without keeping its cursor in
  step turns an O(new) save back into an O(total history) one, or worse,
  duplicates bytes already on disk.
- `sealLocalSpool()` (`publication.cpp`) folds the operations history into
  the sealed torrent too, as a fourth file addressed by its own
  `SealedScroll::opsScroll` — a separate `Scroll` identity from the
  content's `scroll`, since the two are different byte streams that both
  start at offset zero. See `design/ops-spool-format.md` for the full
  reasoning behind the binary format, the incremental-save cursors, and why
  the ops log gets its own scroll rather than a further segment of the
  content's.

## Known gaps / TODO

- **Touch and gesture support.** The Android build (`packaging/android/`)
  currently gets input only through whatever SDL synthesizes from touch
  events as mouse/keyboard equivalents — there is no tap-to-activate,
  drag-to-scroll, or pinch-to-zoom handling. That work belongs in
  `src/app.cpp` alongside the existing SDL event handling. Not started; do
  not begin it without being asked, since it hasn't been scoped yet (gesture
  thresholds, how it interacts with the existing mouse/keyboard input model,
  accessibility implications).

## License

GPL-3.0 (see `LICENSE`). The README notes the GPL-3.0-only vs.
GPL-3.0-or-later distinction is still unconfirmed — don't resolve that
ambiguity unilaterally in code headers or packaging metadata.
