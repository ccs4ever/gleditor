# gleditor

> Refer to [`CLAUDE.md`](CLAUDE.md) for full developer guidance, build/test workflows, coding style, and architecture references.

GPU-rendered text editor library (`gleditor`) plus three programs built on it:
`apps/gleditor` (plain editor), `apps/xudu` (a xanadoc/xanalogical editor), and
`apps/zigzag` (a Project Xanadu Zigzag multidimensional slice visualizer).
Backends: OpenGL, OpenGL ES, and optionally Vulkan, all driven from one
rendering pipeline. C++23, built with GNU Make + pkg-config — **no CMake**.

The README is the source of truth for anything not covered below (rendering
architecture, accessibility, xudu's data model, zigzag's multidimensional space,
SDL2/SDL3 differences, etc.) — read the relevant section there before making
non-trivial changes in that area.

## Setup: submodules

Vendored deps (`thirdparty/argparse`, `thirdparty/Choreograph`,
`thirdparty/merklecpp`) are git submodules and the tree will not build
without them:

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
make                              # library, all three programs, test binaries, compile_commands.json (includes Vulkan if available)
make lib                          # library only
make gleditor                     # apps/gleditor only
make xudu                         # apps/xudu only
make zigzag                       # apps/zigzag only
make GLEDITOR_DISABLE_VULKAN=1    # disables the Vulkan backend and SPIR-V compilation
make GLEDITOR_SDL=2               # force SDL2 instead of the SDL3/SDL2 auto-probe
make clean
make format                       # clang-format + shfmt, in place; no build deps needed
make format-check                 # same, --dry-run; what CI runs
make lint                         # shellcheck + yamllint + mdl; what CI runs
```

Key variables:

- `DEBUG=1` — debug flags + sanitizer flag sets available.
- `GLEDITOR_DISABLE_VULKAN=1` — disable the Vulkan backend (Vulkan is built by
  default when available via pkg-config). **This flips the compile flags for
  every object file**, so toggling it rebuilds the whole tree. `ccache` (used
  automatically when installed; disable with `GLEDITOR_NO_CCACHE=1`) is what
  makes flipping it back and forth cheap.
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
make -j$(nproc) test        # builds + runs gleditor_test, xudu_test, zigzag_test, and rootless swarm tests
make test TEST_FILTER='MediaTest.*'   # run/override a specific gtest filter
```

- **Parallelism**: Always run `make -j$(nproc) test` to utilize all available cores.
- `gleditor_test` links the real shared library (catches export-boundary
  bugs); `xudu_test` links only the xanalogical engine, with no graphics
  device, on purpose — that's the boundary being tested.
- `make test` automatically runs the isolated network-namespace swarm tests
  (`tools/swarm-netns-test.sh`) rootlessly via unprivileged user namespaces
  (`unshare -Urnm`).
- `test/all` is aliased directly to `test`.
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
- Indentation and coding style are defined in `.editorconfig` at the root
  of the project and strictly aligned with `.clang-format` (`IndentWidth: 2`,
  `UseTab: Never`, `ColumnLimit: 80`). Vim modelines have been removed across
  the codebase in favor of `.editorconfig`.
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
- `tools/check-config-harmony.sh` validates that `.editorconfig`,
  `.clang-format`, `.yamlfmt`, `.yamllint`, and `.mdl_style.rb` remain in
  strict harmony across all languages so configuration rules never drift. It
  is wired into both `make format-check` and `make lint`.
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
- `src/text/` — native text layout engine: `FontManager`, `FontFace`,
  `TextLayout` (HarfBuzz, FreeType, libunibreak, FriBidi)
- `src/render/` — device abstraction and backends (`gl/`, `vulkan/`)
- `src/a11y/` — accessibility tree / AccessKit integration
- `include/gleditor/` — the library's public headers
- `include/gleditor/text/` — text layout and font management public headers
- `apps/gleditor/` — the plain editor program
- `apps/xudu/` — the xanadoc editor; `apps/xudu/core/` is its engine:
  - `user_permascroll.hpp/.cpp`: Sovereign user permascroll stream and registry
  - `merkle_ledger.hpp/.cpp`: Append-only Merkle ledger for identity consensus
  - `managed_torrent.hpp/.cpp`: System-managed torrent swarms coordinator
  - `identity/`: BEP 10 plugins, Hashcash PoW engine, and network controller
  - `store.hpp/.cpp`: OSMIC time branches, microversions, and EDL operations
  - `beams.hpp/.cpp`, `framing.hpp/.cpp`: 3D link ribbons and transclusion prisms
- `apps/zigzag/` — the Xanadu Zigzag multidimensional visualizer; `apps/zigzag/core/`:
  - `zigzag_engine.hpp/.cpp`: Multidimensional slice data model and YAML parser
  - `compact_zzcell.hpp`: cell layout — primedia span, per-dimension links,
    resolution status. Around 960 bytes, not the 64 it claimed for a while;
    a `static_assert` holds the line until the hot/cold split is done
  - `unified_transclusion_engine.hpp/.cpp`: 120 FPS render staging and manifold checks
  - `zz_xudu_projector.hpp/.cpp`: Bidirectional xanadoc-to-zigzag mapping
- `assets/shaders/` — portable GLSL bodies; `vulkan/` holds generated SPIR-V
- `assets/zigzag/` — sample slice YAML documents
- `tests/lib/`, `tests/xudu/`, `tests/zigzag/` — unit tests for the library and engines
- `tools/` — build-time and verification helpers (`compare-backends.sh`,
  `benchmark-kjv-load.py`, `layout-latency-probe.cpp`, `shader_assemble.cpp`,
  `swarm-netns-test.sh`)
- `packaging/` — distro packaging (arch, debian, fedora, macos, windows, nix)
- `design/` — design notes and the reasoning behind non-obvious decisions
- `thirdparty/` — vendored dependencies (git submodules; see above)

## Xanadulogical & Identity Architecture

- **One Permascroll Per User**:
  - Primedia is never siloed per document; all typing flows into a single
    append-only `UserPermascroll` bound to the author's OpenPGP / BEP 46 identity.
  - Page-aligned 64 KiB segments preserve BitTorrent v2 Merkle piece stability
    and enable zero-copy `mmap(MAP_FIXED)` address space expansion.
  - Documents (`Store`) are lightweight Edit Decision Lists referencing slot 0
    (local author's permascroll) or external scrolls (`ScrollId > 0`).
  - Dual-key delegation (`DeviceDelegation`) maps master GPG fingerprints to
    device-salted BEP 46 keypairs for offline multi-device authoring.
  - Collaborative live editing operates with **zero raw text in live ops**,
    passing canonical 48-byte descriptors and `GlobalSpan` references to eliminate
    local spool pollution.
- **Decentralized Merkle Identity Ledger**:
  - `MerkleLedger` maintains an append-only tree of verified GPG fingerprints
    and email mappings using `microsoft/merklecpp`.
  - BEP 10 peer wire protocol plugins (`xudu_identity_lookup`, `xudu_oracle_vote`,
    `xudu_oracle_verify`) handle challenge-response authentication and peer gating.
  - Dynamic `HashcashEngine` PoW enforcement protects swarms against Sybil/DoS.
- **Beam & Optical Rendering**:
  - Distinguishes emergent transclusion prisms (Identity Gold volumetric
    quads) from explicit xanalinks (cyan/magenta ribbons).
  - Multi-span link disambiguation spines and instance hue shifts prevent visual
    overlap in dense hypertexts.
  - Document margin anchor brackets sit flush inside page boundaries, supporting
    up to 4 distinct overlapping link anchor colors.
- **Zigzag Transclusion & Manifold Engine**:
  - `CompactZZCell` and `UnifiedTransclusionEngine` stage cells and link beams
    for 120 FPS high-throughput rendering.
  - `zz_xudu_projector` projects xanadocs and hypertime branches into Zigzag
    cells, mapping unchanged spans across revisions to clone cells.

## Text Architecture & Shaping Pipeline

- **Zero Cairo / Zero Pango**: The text stack is built directly on
  **FreeType 2**, **HarfBuzz**, **libunibreak** (UAX #14), **FriBidi**, and
  **Fontconfig**. Cairo, Pango, PangoFT2, and Pangomm are eliminated.
- **`text::FontManager` & `text::FontFace`** (`src/text/font.cpp`):
  - Resolves font descriptions (e.g. `"Monospace 16"`, `"Sans Bold 12"`) via
    Fontconfig (`FcPattern*`) and loads them into thread-safe FreeType `FT_Face`
    and HarfBuzz `hb_font_t` instances.
  - Computes typographic metrics (`ascent`, `descent`, `lineHeight`, `spaceWidth`).
- **`text::TextLayout`** (`src/text/layout.cpp`):
  - `layoutPage(text, font, options)`: Breaks lines with `libunibreak`
    (`set_linebreaks_utf8`), shapes runs with HarfBuzz (`hb_shape`), wraps at
    `maxWidthPx`, and paginates by `maxHeightPx`.
  - **Height-Budgeted Slicing**: When `maxHeightPx > 0`, input text is sliced
    to the maximum lines that could fit the height budget. This guarantees
    $O(1)$ page generation time rather than $O(N^2)$ whole-document shaping
    across thousands of pages.
  - `layoutSingleLine(text, font, options)`: Single-line fast path for toasts
    and canvas measurement.
  - Outputs `PageShaping` (`limit`, `lineCount`, `clusters`, `glyphs`, `lines`)
    consumed directly by `Doc` and `Page`.
- **`GlyphCache`** (`src/glyphcache/cache.cpp`):
  - Direct 8-bit grayscale coverage texture rasterization using FreeType
    (`FT_Load_Glyph`, `FT_Glyph_To_Bitmap`).
  - Clusters are shaped via HarfBuzz and rendered into a dynamic 2D texture
    array atlas (512x512 growing up to 16384x16384 across 64 layers).
- **Baseline Alignment & Quad Geometry**:
  - `Doc` glyph quads are anchored to `line.top` with height set to the font's
    logical `lineHeight`.
  - `GlyphCache` cluster textures are sized to `lineHeight` with baseline fixed
    at $Y = \\text{ascent}$.
  - When modifying text shaping or layout, always run
    `./tools/compare-backends.sh` and visually inspect screenshots with visual
    tools (`view_file`) to check for flat baselines, correct cluster height,
    and sharp glyph rendering.

## Gotchas worth knowing before editing the Makefile

- Toggling `GLEDITOR_DISABLE_VULKAN` or `DEBUG` changes flags for *every*
  object; the build records a flags signature (`$(OBJDIR)/.buildflags`) so
  stale objects get rebuilt rather than silently linked in — preserve that
  mechanism if you touch flag handling.
- SDL major version is auto-probed via pkg-config, not assumed — don't
  hardcode SDL2 or SDL3 assumptions in new code without checking
  `GLEDITOR_SDL` handling.
- GL/GLES entry points are resolved at runtime via `SDL_GL_GetProcAddress`,
  not linked — don't add `-lGL` or equivalent link flags.
- `format`, `format-check`, `lint`, and `doc` targets probe tool existence
  dynamically via `command -v` and gracefully echo missing tool notices rather
  than failing when optional linters/formatters are not installed locally.
- `format`, `format-check` and `lint` are the one place `NO_SDL_GOALS` is
  checked: they're exempted from the top-of-Makefile pkg-config checks *and*
  from `include $(DEPS)` (the same exemption `clean`/`dist` already had, for
  the same reason — see the comment above that `include`), because they
  compile nothing and are meant to run on a checkout that never installed
  SDL, font packages, or anything else the build needs. A goal that does need a
  compiler must not be added to `NO_SDL_GOALS`.

## License

GPL-3.0 (see `LICENSE`). The README notes the GPL-3.0-only vs.
GPL-3.0-or-later distinction is still unconfirmed — don't resolve that
ambiguity unilaterally in code headers or packaging metadata.
