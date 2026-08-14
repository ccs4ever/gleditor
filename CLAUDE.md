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
make test/all     # the same, nothing skipped — this is what CI (PR checks) runs
make test TEST_FILTER='SwarmTest.*'   # run/override a specific gtest filter
```

- `gleditor_test` links the real shared library (catches export-boundary
  bugs); `xudu_test` links only the xanalogical engine, with no graphics
  device, on purpose — that's the boundary being tested.
- `make test/swarm` runs the network-namespace swarm tests; needs root, not
  part of `make test`.
- Before pushing non-trivial changes, prefer `make test/all` over
  `make test` since that's what CI actually gates on.
- `./tools/compare-backends.sh` renders a sample through every compiled-in
  backend and diffs the output — the real check that a backend still draws
  correctly, since a backend that draws nothing still exits 0.

Tests live in `tests/lib/` (library, GoogleTest/GoogleMock) and `tests/xudu/`
(engine). Add new unit tests next to the existing files there, matching the
GoogleTest style already in use.

## Code style

- `.editorconfig` sets each extension's indent style/size, charset, line
  ending and trailing-whitespace handling for editors that read it, matching
  the settings the real formatters below enforce (`.clang-format`'s
  `IndentWidth: 2`, shfmt's `-i 2`, `.yamlfmt`'s `indent: 2`, ...). It's a
  hint for editors, not a gate — nothing runs `editorconfig-checker` in CI;
  see the note at the end of the coverage table below for why.
- Formatting is governed by `.clang-format` (LLVM-based, 2-space access
  modifier offset, aligned consecutive assignments, etc.), and covers `.glsl`
  shader sources as well as C++: GLSL is close enough to C that clang-format
  reformats it correctly rather than needing a separate tool. Run
  `make format` on touched files' languages before committing, or
  `clang-format -i --style=file <file>` for a single file.
- **CI enforces formatting** (`.github/workflows/c-cpp.yml`, job `format`):
  `make format-check` fails the build on the first unformatted C++/GLSL file
  or shell script; `make lint` runs the linters below. Both run without the
  graphics/build dependencies installed — see "Gotchas" below for how that
  works.
- Every `.cpp`/`.hpp`/`.glsl` file ends with a vim modeline,
  `// vi: set sw=2 sts=2 ts=2 et:`, matching `.clang-format`'s
  `IndentWidth: 2`. Carry it over verbatim into new files; a formatter change
  to the indent width should update both.
- `.clangd` enables `modernize-*`, `bugprone-*`, `cppcoreguidelines-*`,
  `performance-*`, `readability-*`, and `portability-*` clang-tidy checks
  (minus a few disabled ones — see `.clangd`) and builds with
  `-Wall -Wextra -std=c++2c`. Treat clangd/clang-tidy warnings on lines you
  touch as worth fixing, not noise. Not yet wired into CI: doing that
  meaningfully means triaging the existing warning backlog first, which is
  future work rather than something this pass attempted.
- Match the prevailing comment style in this codebase: comments explain
  *why* a non-obvious choice was made (a constraint, a workaround, a
  tradeoff), not what the code does. Don't add narrating comments.
- `compile_commands.json` is regenerated by `make` automatically; don't hand
  edit it.

### Formatting and linting coverage, by language

| Language                                                 | Formatter                                            | Linter                                                                                                                                   | CI gate                                                                                             |
| -------------------------------------------------------- | ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| C++ (`.cpp`/`.hpp`/`.h`)                                 | clang-format (`make format`)                         | clang-tidy (`.clangd`, editor-only)                                                                                                      | blocking                                                                                            |
| GLSL (`.glsl`)                                           | clang-format, same as C++                            | `glslangValidator` via `make shaders` (compiles every shader)                                                                            | blocking (both)                                                                                     |
| Shell (`.sh`, `PKGBUILD`)                                | shfmt (`make format`)                                | shellcheck (`make lint`)                                                                                                                 | blocking                                                                                            |
| YAML (workflows, dependabot)                             | yamlfmt (`make format`), config in `.yamlfmt`        | yamllint (`make lint`), config in `.yamllint`                                                                                            | blocking                                                                                            |
| Markdown (README, CLAUDE.md, design/)                    | mdformat `--wrap keep` (`make format`)               | mdl (`make lint`), config in `.mdlrc`/`.mdl_style.rb`                                                                                    | blocking                                                                                            |
| Nix (`flake.nix`, `packaging/nix/*.nix`)                 | nixfmt-rfc-style, the flake's own `formatter` output | nix-linter                                                                                                                               | blocking (job `nix`), separate from `format` because it needs Nix installed                         |
| Ruby (`packaging/macos/gleditor.rb`, a Homebrew formula) | `brew style` (needs Homebrew; not run in CI)         | `ruby -c` (syntax only)                                                                                                                  | non-blocking, see `packaging.yml`'s `macos` job                                                     |
| RPM spec (`packaging/fedora/gleditor.spec`)              | —                                                    | rpmlint (needs `rpm`'s native Python binding, so only runs inside the `fedora:latest` container the `fedora` packaging job already uses) | non-blocking until its output has been triaged                                                      |
| Debian (`debian/rules`, `debian/control`, ...)           | —                                                    | —                                                                                                                                        | not covered: `debian/rules` is a `dh`-sequenced Makefile, not a shell script shellcheck understands |

yamlfmt and mdformat are Go/Python tools rather than apt packages
(`go install`/`pip install` in the CI `format` job); they were picked over
the better-known Node ones (`prettier`, `markdownlint-cli2 --fix`) to avoid
adding a Node toolchain to an otherwise zero-Node tree, the same reasoning
`.yamllint`/`.mdl_style.rb` already used for the linters.

**Formatter and linter settings are kept in one place per language on
purpose**, so running the formatter never leaves a file the linter still
rejects:

- `.yamlfmt`'s `indentless_arrays: true` matches `.yamllint`'s
  `indent-sequences: false`; both encode the same "`- uses:` sits level with
  `steps:`" convention. `retain_line_breaks: true` is load-bearing but has a
  known bug with folded (`>`) block scalars that leaks a placeholder into the
  string (google/yamlfmt#84) — this repo has none, on purpose; see the
  comment in `.yamlfmt` before adding one back.
- mdformat's `--wrap keep` never reflows a paragraph, so it can't fight
  `.mdl_style.rb`'s `MD013` line-length allowance; the only content it
  rewrites is table cell padding, which mdl doesn't check at all. The one
  failure mode is an inline code span (`` `...` ``) split across a hand-wrapped
  line: mdformat canonicalizes that onto one line regardless of `--wrap`,
  which can push it over the line-length limit — keep code spans on one
  physical line rather than wrapping through them.
- nixfmt-rfc-style and nix-linter check disjoint things (whitespace/layout
  vs. semantic style like unused arguments), so there is nothing for them to
  disagree about.

A repo-wide `editorconfig-checker` run (not wired into CI) is noisy on this
tree for reasons that are the checker's limits, not real problems: it
doesn't understand clang-format's alignment-based continuation indents,
treats Markdown table cell padding as "wrong" indentation, and applies
`max_line_length` to LICENSE/Doxyfile text nobody intends to rewrap. Use it
to sanity-check `.editorconfig` itself, not as a gate.

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

## License

GPL-3.0 (see `LICENSE`). The README notes the GPL-3.0-only vs.
GPL-3.0-or-later distinction is still unconfirmed — don't resolve that
ambiguity unilaterally in code headers or packaging metadata.
