# gleditor

> Refer to [`CLAUDE.md`](CLAUDE.md) for full developer guidance, build/test workflows, coding style,
> and architecture references.

GPU-rendered text editor library (`gleditor`) plus three programs built on it: `apps/gleditor`
(plain editor), `apps/xudu` (a xanadoc/xanalogical editor), and `apps/zigzag` (a Project Xanadu
Zigzag multidimensional slice visualizer). Backends: OpenGL, OpenGL ES, and optionally Vulkan, all
driven from one rendering pipeline. C++23, built with GNU Make + pkg-config — **no CMake**.

The README is the source of truth for anything not covered below (rendering architecture,
accessibility, xudu's data model, zigzag's multidimensional space, SDL2/SDL3 differences, etc.) —
read the relevant section there before making non-trivial changes in that area.

## Setup: submodules

Vendored deps (`thirdparty/argparse`, `thirdparty/Choreograph`, `thirdparty/merklecpp`,
`thirdparty/SDL`, `thirdparty/zstd`) are git submodules and the tree will not build without them:

```sh
git submodule update --init --recursive
```

Run this first in any fresh clone or worktree. If a submodule appears empty or a build fails with
missing headers under `thirdparty/`, re-run the above before investigating anything else. Do not
vendor these sources directly into the tree or hand-edit files under `thirdparty/` — bump the
submodule pointer (`cd thirdparty/<name> && git checkout <ref>`) instead, and commit the resulting
gitlink change in the parent repo.

## Build

```sh
make -j$(nproc)                   # library, all three programs, test binaries, compile_commands.json (includes Vulkan if available)
make -j$(nproc) lib               # library only
make -j$(nproc) gleditor          # apps/gleditor only
make -j$(nproc) xudu              # apps/xudu only
make -j$(nproc) zigzag            # apps/zigzag only
make -j$(nproc) GLEDITOR_DISABLE_VULKAN=1 # disables the Vulkan backend and SPIR-V compilation
make -j$(nproc) GLEDITOR_SDL=2    # force SDL2 instead of the SDL3/SDL2 auto-probe
make clean
make format                       # clang-format + shfmt, in place; no build deps needed
make format-check                 # same, --dry-run; what CI runs
make lint                         # shellcheck + yamllint + mdl; what CI runs
```

Key variables and guidelines:

- **Parallelism**: Always use `make -j$(nproc)` when invoking `make` to maximize build throughput
  across all available CPU cores.
- `DEBUG=1` — debug flags + sanitizer flag sets available.
- `GLEDITOR_DISABLE_VULKAN=1` — disable the Vulkan backend (Vulkan is built by default when
  available via pkg-config). **This flips the compile flags for every object file**, so toggling it
  rebuilds the whole tree. `ccache` (used automatically when installed; disable with
  `GLEDITOR_NO_CCACHE=1`) is what makes flipping it back and forth cheap.
- `GLEDITOR_SDL=2` / `=3` — pick SDL major version explicitly; unset probes via pkg-config (SDL3 if
  present, else SDL2).
- `GLEDITOR_ENABLE_A11Y=1` / `=0` — require/disable AccessKit; unset uses it if found.
- `CXX=g++ make` (or `make CXX=g++`) — override the compiler; only used when no `CXX` is otherwise
  set (`clang++` is the default).

Don't hand-pick `-std=` or hardcode a compiler in new build logic — the Makefile already probes for
what the toolchain supports.

## Tests

### Everything runs headless. No exceptions without being asked.

**Any command that could open a window or touch a display, GPU or audio device — `make test`, any
test binary, `gleditor`, `xudu`, `zigzag`, `tools/compare-backends.sh`, an ad-hoc harness — MUST be
run headless, unless the user has explicitly asked for visual confirmation.** This is not a
preference about tidiness: a window stealing focus interrupts whoever is at the keyboard, and a run
that silently depends on a real display is a run that cannot be reproduced in CI or over SSH.

Use whatever gets there. In rough order of preference:

```sh
# 1. The environment variables, which are enough for almost everything here.
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 <command>

# 2. A virtual X server, when something insists on a real display connection.
xvfb-run -s "-screen 0 1024x768x24" <command>

# 3. Both, which is what tools/compare-backends.sh wants.
xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh
```

`--headless` is also a flag on `xudu` and `zigzag` themselves, and it is what
`tools/create-sample-xanadocs.sh` uses. Prefer it when driving those programs; it is stronger than
the environment variables because it skips window creation rather than redirecting it.

"By any means necessary" is meant literally: if none of the above works for some new tool, find a
way (a dummy driver, a `Xvfb` you start yourself, a `--no-window` flag you add) rather than falling
back to opening a window. **When visual confirmation genuinely is the point — a screenshot, a
rendering regression, a layout check — the user will say so.** Then, and only then, run it against a
real display and look at the result.

### Running them

```sh
make -j$(nproc) test        # builds + runs gleditor_test, xudu_test, zigzag_test, and rootless swarm tests
make test TEST_FILTER='MediaTest.*'   # run/override a specific gtest filter
```

- **Parallelism**: Always run `make -j$(nproc) test` to utilize all available cores.
- `gleditor_test` links the real shared library (catches export-boundary bugs); `xudu_test` links
  only the xanalogical engine, with no graphics device, on purpose — that's the boundary being
  tested. `zigzag_test` covers the slice model and the transclusion engine.
- `make test` finishes by running the isolated network-namespace swarm tests
  (`tools/swarm-netns-test.sh`) rootlessly via unprivileged user namespaces (`unshare -Urnm`). That
  step needs the `veth` kernel module loaded; without it the run ends in
  `Error: Unknown device type.` and a non-zero exit **after all three gtest binaries have passed**.
  An unprivileged user namespace cannot autoload a module, so `sudo modprobe veth` is the fix and it
  is the user's to run. Read the three `[  PASSED  ]` lines before concluding anything is broken.
- `test/all` is aliased directly to `test`.
- `./tools/compare-backends.sh` renders a sample through every compiled-in backend and diffs the
  output — the real check that a backend still draws correctly, since a backend that draws nothing
  still exits 0.
- **Some tests shell out to `build/xudu`** (the `E2EBinaryOrchestration` and `AnimationTransclusion`
  suites). They fail with `xudu binary not found` if the app did not link, which is a different
  problem from a test regression — check `make -j$(nproc)` succeeded before investigating them.

Tests live in `tests/lib/` (library, GoogleTest/GoogleMock), `tests/xudu/` (engine) and
`tests/zigzag/`. Add new unit tests next to the existing files there, matching the GoogleTest style
already in use.

### Binary fixtures under `tests/samples/xudu/`

These are real on-disk stores — `ops.nodes`, `primedia.spool`, `scrolls.spool`, `links.spool`,
`current.yaml` — checked in and loaded by `SampleXanadocsTest`. **A change to `CompactOpNode`'s
layout invalidates every one of them.** Since migration step 8 they say so: `ops.nodes` opens with
an `OpsSegmentHeader` recording `sizeof(CompactOpNode)`, so a stale fixture is refused with
`OpsSegmentUnreadable` naming the two sizes rather than loading and meaning something else.
Regenerate in the same commit anyway — a refused fixture is a red test, not a working one:

```sh
make -j$(nproc) xudu
./tools/create-sample-xanadocs.sh          # core_hypertext, multimedia, beams, 000.scroll
./tools/create-floating-image-sample.sh    # 11_floating_image, which the above deletes
```

The second script exists because `create-sample-xanadocs.sh` opens by removing the whole
`multimedia` directory, and `11_floating_image` is built by hand with its own scroll rather than the
shared `000.scroll`. Run both, or that fixture silently disappears.

## Code style

- `.editorconfig` sets each extension's indent style/size, charset, line ending and
  trailing-whitespace handling for editors that read it, matching the settings the real formatters
  below enforce (`.clang-format`'s `IndentWidth: 2`, shfmt's `-i 2`, `.yamlfmt`'s `indent: 2`, ...).
  It's a hint for editors, not a gate — nothing runs `editorconfig-checker` in CI; see the note at
  the end of the coverage table below for why.
- Formatting is governed by `.clang-format` (LLVM-based, 2-space access modifier offset, aligned
  consecutive assignments, etc.), and covers `.glsl` shader sources as well as C++: GLSL is close
  enough to C that clang-format reformats it correctly rather than needing a separate tool. Run
  `make format` on touched files' languages before committing, or
  `clang-format -i --style=file <file>` for a single file.
- **CI enforces formatting** (`.github/workflows/c-cpp.yml`, job `format`): `make format-check`
  fails the build on the first unformatted C++/GLSL file or shell script; `make lint` runs the
  linters below. Both run without the graphics/build dependencies installed — see "Gotchas" below
  for how that works.
- Indentation and coding style are defined in `.editorconfig` at the root of the project and
  strictly aligned with `.clang-format` (`IndentWidth: 2`, `UseTab: Never`, `ColumnLimit: 80`). Vim
  modelines have been removed across the codebase in favor of `.editorconfig`.
- `.clangd` enables `modernize-*`, `bugprone-*`, `cppcoreguidelines-*`, `performance-*`,
  `readability-*`, and `portability-*` clang-tidy checks (minus a few disabled ones — see `.clangd`)
  and builds with `-Wall -Wextra -std=c++2c`. Treat clangd/clang-tidy warnings on lines you touch as
  worth fixing, not noise. Not yet wired into CI: doing that meaningfully means triaging the
  existing warning backlog first, which is future work rather than something this pass attempted.
- Match the prevailing comment style in this codebase: comments explain *why* a non-obvious choice
  was made (a constraint, a workaround, a tradeoff), not what the code does. Don't add narrating
  comments.
- `compile_commands.json` is regenerated by `make` automatically; don't hand edit it.

### Formatting and linting coverage, by language

| Language                                                 | Formatter                                            | Linter                                                                                                                                   | CI gate                                                                                             |
| -------------------------------------------------------- | ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| C++ (`.cpp`/`.hpp`/`.h`)                                 | clang-format (`make format`)                         | clang-tidy (`.clangd`, editor-only)                                                                                                      | blocking                                                                                            |
| GLSL (`.glsl`)                                           | clang-format, same as C++                            | `glslangValidator` via `make shaders` (compiles every shader)                                                                            | blocking (both)                                                                                     |
| Shell (`.sh`, `PKGBUILD`)                                | shfmt (`make format`)                                | shellcheck (`make lint`)                                                                                                                 | blocking                                                                                            |
| YAML (workflows, dependabot)                             | yamlfmt (`make format`), config in `.yamlfmt`        | yamllint (`make lint`), config in `.yamllint`                                                                                            | blocking                                                                                            |
| Markdown (every `*.md` outside `thirdparty/`)            | mdformat `--wrap 100` (`make format`)                | mdl `-i` (`make lint`), config in `.mdlrc`/`.mdl_style.rb`                                                                               | blocking                                                                                            |
| Nix (`flake.nix`, `packaging/nix/*.nix`)                 | nixfmt-rfc-style, the flake's own `formatter` output | nix-linter                                                                                                                               | blocking (job `nix`), separate from `format` because it needs Nix installed                         |
| Ruby (`packaging/macos/gleditor.rb`, a Homebrew formula) | `brew style` (needs Homebrew; not run in CI)         | `ruby -c` (syntax only)                                                                                                                  | non-blocking, see `packaging.yml`'s `macos` job                                                     |
| RPM spec (`packaging/fedora/gleditor.spec`)              | —                                                    | rpmlint (needs `rpm`'s native Python binding, so only runs inside the `fedora:latest` container the `fedora` packaging job already uses) | non-blocking until its output has been triaged                                                      |
| Debian (`debian/rules`, `debian/control`, ...)           | —                                                    | —                                                                                                                                        | not covered: `debian/rules` is a `dh`-sequenced Makefile, not a shell script shellcheck understands |

yamlfmt and mdformat are Go/Python tools rather than apt packages (`go install`/`pip install` in the
CI `format` job); they were picked over the better-known Node ones (`prettier`,
`markdownlint-cli2 --fix`) to avoid adding a Node toolchain to an otherwise zero-Node tree, the same
reasoning `.yamllint`/`.mdl_style.rb` already used for the linters.

**Formatter and linter settings are kept in one place per language on purpose**, so running the
formatter never leaves a file the linter still rejects:

- `.yamlfmt`'s `indentless_arrays: true` matches `.yamllint`'s `indent-sequences: false`; both
  encode the same "`- uses:` sits level with `steps:`" convention. `retain_line_breaks: true` is
  load-bearing but has a known bug with folded (`>`) block scalars that leaks a placeholder into the
  string (google/yamlfmt#84) — this repo has none, on purpose; see the comment in `.yamlfmt` before
  adding one back.

- mdformat's `--wrap 100` matches `.mdl_style.rb`'s `MD013` `line_length`, so the formatter reflows
  every paragraph to fit the same limit the linter enforces — a file `make format` just wrote is a
  file `make lint` already accepts, same as every other language pair in this table. The one failure
  mode is an inline code span (`` `...` ``) or a `$...$` math span near the 100-column boundary:
  mdformat treats each as a single atomic token and will not break inside one, so a line whose only
  long token is a code span or an equation can still land over the limit — shorten the surrounding
  prose, or promote a `$$...$$` block equation to a ```` ```math ```` fenced block (still renders on
  GitHub, and fenced code is exempt from `MD013` via `:ignore_code_blocks => true`).

- `design/` narrates a lot of geometry and layout math in `$...$`/`$$...$$` LaTeX (rendered by
  GitHub's own math support). Plain mdformat has no concept of that syntax — it parses `\text{...}`
  as literal backslash-letter text and re-escapes the backslash on write, silently turning `\text`
  into `\\text` and corrupting the math. `mdformat-dollarmath` teaches it to treat `$...$`/`$$...$$`
  spans as opaque, so it formats around the math instead of through it; installed alongside
  `mdformat-gfm` in the CI `format` job. A `$$...$$` block sitting directly under a list item with
  no blank line before/after it will still make mdformat refuse to write the file
  (`Formatted Markdown renders to different HTML than input Markdown`) — give it a blank line on
  both sides, same as a fenced code block would need.

- **The Makefile probes for the `mdformat` *binary*, not for its plugins.** A machine with mdformat
  installed but without `mdformat-gfm`, `mdformat-dollarmath` and `mdformat-frontmatter` will run
  `make format` happily and corrupt every `design/` file it touches — tables reflowed into prose,
  `\times` double-escaped to `\\times` — while `make format-check` reports files as unformatted that
  CI considers fine. Before running either against Markdown, confirm the plugins are present, or use
  a throwaway venv that has them:

  ```sh
  python3 -m venv /tmp/mdenv
  /tmp/mdenv/bin/pip install --quiet mdformat mdformat-gfm mdformat-dollarmath mdformat-frontmatter
  /tmp/mdenv/bin/mdformat --wrap 100 design/whatever.md
  ```

  Verify afterwards that table separator rows survived (`grep -c '^| *-\+'`) and that no `\\text` or
  `\\times` appeared.

- `.agents/skills/*.md` open with YAML front matter (`name:`/`description:` consumed by the skill
  loader); `mdformat-frontmatter` is what stops mdformat parsing the closing `---` as a second
  thematic break and mangling everything after it into a heading, and `mdl -i` is the matching half
  on the lint side (see the comment above the `mdl` invocation in the Makefile).

- A code sample pasted into `design/` without a fence around it is invisible to both tools: mdl
  can't tell it apart from prose (so `#include <...>` lines get flagged as malformed ATX headers)
  and `--wrap 100` will happily reflow it into unreadable run-on lines. Fence anything that looks
  like C++/GLSL/shell output, even in an informal notes file.

- `tools/check-config-harmony.sh` validates that `.editorconfig`, `.clang-format`, `.yamlfmt`,
  `.yamllint`, and `.mdl_style.rb` remain in strict harmony across all languages so configuration
  rules never drift. It is wired into both `make format-check` and `make lint`.

- nixfmt-rfc-style and nix-linter check disjoint things (whitespace/layout vs. semantic style like
  unused arguments), so there is nothing for them to disagree about.

A repo-wide `editorconfig-checker` run (not wired into CI) is noisy on this tree for reasons that
are the checker's limits, not real problems: it doesn't understand clang-format's alignment-based
continuation indents, treats Markdown table cell padding as "wrong" indentation, and applies
`max_line_length` to LICENSE/Doxyfile text nobody intends to rewrap. Use it to sanity-check
`.editorconfig` itself, not as a gate.

## Project structure

- `src/` — the library: document model, glyph cache, SDL wrappers, render loop
- `src/text/` — native text layout engine: `FontManager`, `FontFace`, `TextLayout` (HarfBuzz,
  FreeType, libunibreak, FriBidi)
- `src/render/` — device abstraction and backends (`gl/`, `vulkan/`)
- `src/a11y/` — accessibility tree / AccessKit integration
- `include/gleditor/` — the library's public headers
- `include/gleditor/text/` — text layout and font management public headers
- `apps/gleditor/` — the plain editor program
- **`apps/common/xanadu/` — where the xanalogical engine actually lives.** `apps/xudu/core/` and
  `apps/zigzag/core/` are one-line forwarding headers into it (`#include "common/xanadu/x.hpp"` plus
  `namespace xudu { using namespace ::xanadu; }`), so `<xudu/core/store.hpp>` and
  `<zigzag/core/zzcore.hpp>` are the include spellings while `apps/common/xanadu/store.cpp` is the
  file to edit. Editing a `core/` shim is almost always a mistake:
  - `store.hpp/.cpp`: OSMIC time branches, microversions, and EDL operations
  - `compact_op.hpp`: the 64-byte `CompactOpNode`. Cache-line aligned, and **immutable once stored**
    — the child/sibling tree edges live in `SegmentedOpsSpool::tree` beside the nodes, because a
    sealed segment is mapped `PROT_READ` and writing a parent's child pointer took SIGSEGV. Three
    `static_assert`s hold the line: size, alignment, and `offsetof(value) == 56` (`alignas(64)`
    would pad a shrunken struct back to 64 on its own, so the size assertion cannot catch a field
    going missing)
  - `segmented_ops_spool.hpp/.cpp`: the memory-mapped ops tree; sealed segments, the id hash, and
    the `TreeLinks` side array
  - `user_permascroll.hpp/.cpp`: sovereign user permascroll stream and registry
  - `merkle_ledger.hpp/.cpp`: append-only Merkle ledger for identity consensus
  - `managed_torrent.hpp/.cpp`: system-managed torrent swarms coordinator
  - `publication.hpp/.cpp`: `publish`/`adopt`, `globalise`/`localise`, scroll keys
  - `identity/`: BEP 10 plugins, Hashcash PoW engine, and network controller
  - `zigzag/zzcore.{hpp,cpp}`, `zigzag/zzstructure.hpp`: the slice model shared with `apps/zigzag`
- `apps/xudu/` — the xanadoc editor's own UI: `beams.cpp`, `framing.cpp` (3D link ribbons and
  transclusion prisms), `session.cpp`, the overlays, `main.cpp`
- `apps/zigzag/` — the Xanadu Zigzag multidimensional visualizer; `apps/zigzag/core/`:
  - `unified_transclusion_engine.hpp/.cpp`: 120 FPS render staging and manifold checks. Syncing is
    linear in the number of ops and there is a regression test asserting it stays that way
    (`SyncCostPerOperationDoesNotGrowWithSize`) — three separate rescans of already-synced state
    used to make it quadratic
  - `compact_zzcell.hpp`: cell layout — primedia span, per-dimension links, resolution status.
    Around 960 bytes, not the 64 it claimed for a while; a `static_assert` holds the line until the
    hot/cold split is done. `design/store-slice-convergence.md` R12/R13 account for where the bytes
    go and remove most of them
  - `zz_xudu_projector.hpp/.cpp`: bidirectional xanadoc-to-zigzag mapping (in `apps/common/`)
- `assets/shaders/` — portable GLSL bodies; `vulkan/` holds generated SPIR-V
- `assets/zigzag/` — sample slice YAML documents
- `tests/lib/`, `tests/xudu/`, `tests/zigzag/` — unit tests for the library and engines
- `tests/samples/` — source material for tests; `tests/samples/xudu/` holds checked-in binary stores
  that must be regenerated whenever the on-disk format moves (see "Tests" above)
- `tools/` — build-time and verification helpers (`compare-backends.sh`, `benchmark-kjv-load.py`,
  `layout-latency-probe.cpp`, `shader_assemble.cpp`, `swarm-netns-test.sh`,
  `create-sample-xanadocs.sh`, `create-floating-image-sample.sh`)
- `packaging/` — distro packaging (arch, debian, fedora, macos, windows, nix)
- `design/` — design notes and the reasoning behind non-obvious decisions
- `thirdparty/` — vendored dependencies (git submodules; see above)

## Xanadulogical & Identity Architecture

- **One Permascroll Per User**:
  - Primedia is never siloed per document; all typing flows into a single append-only
    `UserPermascroll` bound to the author's OpenPGP / BEP 46 identity.
  - Page-aligned 64 KiB segments preserve BitTorrent v2 Merkle piece stability and enable zero-copy
    `mmap(MAP_FIXED)` address space expansion.
  - Documents (`Store`) are lightweight Edit Decision Lists referencing slot 0 (local author's
    permascroll) or external scrolls (`ScrollId > 0`).
  - Dual-key delegation (`DeviceDelegation`) maps master GPG fingerprints to device-salted BEP 46
    keypairs for offline multi-device authoring.
  - Collaborative live editing operates with **zero raw text in live ops**, passing canonical
    48-byte descriptors and `GlobalSpan` references to eliminate local spool pollution.
- **Decentralized Merkle Identity Ledger**:
  - `MerkleLedger` maintains an append-only tree of verified GPG fingerprints and email mappings
    using `microsoft/merklecpp`.
  - BEP 10 peer wire protocol plugins (`xudu_identity_lookup`, `xudu_oracle_vote`,
    `xudu_oracle_verify`) handle challenge-response authentication and peer gating.
  - Dynamic `HashcashEngine` PoW enforcement protects swarms against Sybil/DoS.
- **Beam & Optical Rendering**:
  - Distinguishes emergent transclusion prisms (Identity Gold volumetric quads) from explicit
    xanalinks (cyan/magenta ribbons).
  - Multi-span link disambiguation spines and instance hue shifts prevent visual overlap in dense
    hypertexts.
  - Document margin anchor brackets sit flush inside page boundaries, supporting up to 4 distinct
    overlapping link anchor colors.
- **Zigzag Transclusion & Manifold Engine**:
  - `CompactZZCell` and `UnifiedTransclusionEngine` stage cells and link beams for 120 FPS
    high-throughput rendering.
  - `zz_xudu_projector` projects xanadocs and hypertime branches into Zigzag cells, mapping
    unchanged spans across revisions to clone cells.

### On-disk formats: bump the version, don't carry a shim

Nothing here is in production and every store can be regenerated from its inputs, so **a format is
free to change shape: bump the version, write the new shape, and delete the old reader.** Keeping a
field only so an old file still parses is not caution, it is a permanent tax paid to protect data
nobody has. What is *not* negotiable is the structural invariants — `sizeof(CompactOpNode) == 64`
and its cache-line alignment, 64 KiB Merkle piece alignment, append-only-ness. Layout is soft;
invariants are hard.

**`ops.nodes` has a header, as of migration step 8** — a twelve-byte PNG-style signature, a format
version, and the `nodeSize` field whose silent change caused migration step 1's damage, in a 64 KiB
block sized so the nodes after it stay `mmap`-able on 4 KiB, 16 KiB and 64 KiB page systems. So
there is something to bump, and a store written in a shape this build does not read is refused with
an `OpsSegmentUnreadable` naming what was wrong rather than loading into nonsense. Two rules follow:

- **A layout change still means regenerating every fixture in the same commit** (see "Tests"). The
  header turns silent corruption into a loud failure; it does not make the fixtures correct.
- **One writer.** `SegmentedOpsSpool::writeSegmentFile()` is the only thing that produces a segment
  file, and `Store::save()` calls it. Do not write `ops.nodes` from anywhere else — a format known
  in two places is what let step 1's change go unnoticed.

A *system* xanadoc under `~/.config/xudu/system/` is the one exception to "refused means refused":
`Session::systemStoreIndex()` moves an unreadable one aside and writes a default in its place,
because the program generates those itself and would otherwise refuse to start over its own
scaffolding. A document the user named is never treated that way.

This ruling has an expiry. It is void the first time someone outside this repository has a document
they care about; see R11 in `design/store-slice-convergence.md`.

### Where the reasoning lives

`design/` is the record of *why*, and three notes are load-bearing for current work:

- [`store-slice-convergence.md`](design/store-slice-convergence.md) — the active plan: a cell is an
  operation, `Slice` becomes a replay product of the ops spool like `Version` is. Fourteen rulings
  with their prices, a numbered migration (**steps 1–8 are done**), and the measurements behind
  each. Read this before touching `CompactOpNode`, `Manifold`, `CompactZZCell` or the zigzag
  engine's sync path.
- [`vortex-hyperstructural-runtime.md`](design/vortex-hyperstructural-runtime.md) and
  [`vql-query-language.md`](design/vql-query-language.md) — a speculative runtime and query language
  over the same manifold. Neither is built, but both constrain the cell layout, and each now carries
  a reconciliation section against the convergence.
- [`osmic-microversioning-and-dag.md`](design/osmic-microversioning-and-dag.md) — hypertime naming,
  branches, and what a microversion is.

## Text Architecture & Shaping Pipeline

- **Zero Cairo / Zero Pango**: The text stack is built directly on **FreeType 2**, **HarfBuzz**,
  **libunibreak** (UAX #14), **FriBidi**, and **Fontconfig**. Cairo, Pango, PangoFT2, and Pangomm
  are eliminated.
- **`text::FontManager` & `text::FontFace`** (`src/text/font.cpp`):
  - Resolves font descriptions (e.g. `"Monospace 16"`, `"Sans Bold 12"`) via Fontconfig
    (`FcPattern*`) and loads them into thread-safe FreeType `FT_Face` and HarfBuzz `hb_font_t`
    instances.
  - Computes typographic metrics (`ascent`, `descent`, `lineHeight`, `spaceWidth`).
- **`text::TextLayout`** (`src/text/layout.cpp`):
  - `layoutPage(text, font, options)`: Breaks lines with `libunibreak` (`set_linebreaks_utf8`),
    shapes runs with HarfBuzz (`hb_shape`), wraps at `maxWidthPx`, and paginates by `maxHeightPx`.
  - **Height-Budgeted Slicing**: When `maxHeightPx > 0`, input text is sliced to the maximum lines
    that could fit the height budget. This guarantees $O(1)$ page generation time rather than
    $O(N^2)$ whole-document shaping across thousands of pages.
  - `layoutSingleLine(text, font, options)`: Single-line fast path for toasts and canvas
    measurement.
  - Outputs `PageShaping` (`limit`, `lineCount`, `clusters`, `glyphs`, `lines`) consumed directly by
    `Doc` and `Page`.
- **`GlyphCache`** (`src/glyphcache/cache.cpp`):
  - Direct 8-bit grayscale coverage texture rasterization using FreeType (`FT_Load_Glyph`,
    `FT_Glyph_To_Bitmap`).
  - Clusters are shaped via HarfBuzz and rendered into a dynamic 2D texture array atlas (512x512
    growing up to 16384x16384 across 64 layers).
- **Baseline Alignment & Quad Geometry**:
  - `Doc` glyph quads are anchored to `line.top` with height set to the font's logical `lineHeight`.
  - `GlyphCache` cluster textures are sized to `lineHeight` with baseline fixed at
    $Y = \text{ascent}$.
  - When modifying text shaping or layout, always run `./tools/compare-backends.sh` and inspect the
    frames it captures — flat baselines, correct cluster height, sharp glyphs. This is the one place
    the work *is* visual, and it still needs no window: run the script under `xvfb-run` (see
    "Tests") and read the PNGs it writes. Looking at a captured frame is inspection; opening a live
    window on the user's desktop to look at the same thing is not.

## Gotchas worth knowing before editing the Makefile

- Toggling `GLEDITOR_DISABLE_VULKAN` or `DEBUG` changes flags for *every* object; the build records
  a flags signature (`$(OBJDIR)/.buildflags`) so stale objects get rebuilt rather than silently
  linked in — preserve that mechanism if you touch flag handling.
- SDL major version is auto-probed via pkg-config, not assumed — don't hardcode SDL2 or SDL3
  assumptions in new code without checking `GLEDITOR_SDL` handling.
- GL/GLES entry points are resolved at runtime via `SDL_GL_GetProcAddress`, not linked — don't add
  `-lGL` or equivalent link flags.
- `format`, `format-check`, `lint`, and `doc` targets probe tool existence dynamically via
  `command -v` and gracefully echo missing tool notices rather than failing when optional
  linters/formatters are not installed locally.
- `format`, `format-check` and `lint` are the one place `NO_SDL_GOALS` is checked: they're exempted
  from the top-of-Makefile pkg-config checks *and* from `include $(DEPS)` (the same exemption
  `clean`/`dist` already had, for the same reason — see the comment above that `include`), because
  they compile nothing and are meant to run on a checkout that never installed SDL, font packages,
  or anything else the build needs. A goal that does need a compiler must not be added to
  `NO_SDL_GOALS`.

## License

GPL-3.0 (see `LICENSE`). The README notes the GPL-3.0-only vs. GPL-3.0-or-later distinction is still
unconfirmed — don't resolve that ambiguity unilaterally in code headers or packaging metadata.
