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

**`make` does this for you now.** The Makefile sets `SDL_VIDEODRIVER=offscreen`,
`SDL_AUDIODRIVER=dummy` and `LIBGL_ALWAYS_SOFTWARE=1` at the top and `export`s them, so every target
it runs — the three test binaries, and the shell scripts they drive, and every program *those* start
— inherits it. `make test` on a machine with no display at all passes. It did not always: the test
target launched the binaries bare, so on any developer's machine the suites that construct a window
got a real one, and nobody noticed because a window that opens and closes during a build looks like
a build. Exported rather than written onto each recipe line precisely so there is no list of run
sites to keep up to date.

`?=`, so an explicit request survives — `SDL_VIDEODRIVER=wayland make test` still means it.

**It exports `XDG_DATA_HOME` and `XDG_CONFIG_HOME` into `build/xdg/` for the same reason.** Since
step 13 a store holds no primedia: what the tests type goes into the author's permascroll, which
`xudu` resolves under `$XDG_DATA_HOME` — so without this a test run appends every byte it invents to
the real permascroll of whoever ran it, and leaves it there. `$XDG_CONFIG_HOME` is the same argument
for the system xanadocs (keymap, settings, layout, ui), which `xudu` writes for itself on first run
and a test would otherwise overwrite with whatever it was demonstrating. Both `?=`, both survive
being pointed elsewhere.

For anything **outside** `make`, it is still yours to set. In rough order of preference:

```sh
# 1. The environment variables, which are enough for almost everything here.
#    The XDG pair keeps a run out of your real permascroll and system xanadocs.
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config <command>

# 2. A virtual X server, when something insists on a real display connection.
xvfb-run -s "-screen 0 1024x768x24" <command>

# 3. Both, which is what tools/compare-backends.sh wants.
xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh
```

That covers running a test binary directly (`./build/xudu_test --gtest_filter=...`), which is the
common case and does **not** go through make.

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

These are real on-disk stores — `ops.nodes`, `store.tables`, and `ops.export` where `--export-osmic`
was used — checked in and loaded by `SampleXanadocsTest`. **A change to `CompactOpNode`'s layout
invalidates every one of them.** Since migration step 8 they say so: `ops.nodes` opens with an
`OpsSegmentHeader` recording `sizeof(CompactOpNode)`, so a stale fixture is refused with
`OpsSegmentUnreadable` naming the two sizes rather than loading and meaning something else.
Regenerate in the same commit anyway — a refused fixture is a red test, not a working one:

```sh
make -j$(nproc) xudu
./tools/create-sample-xanadocs.sh          # core_hypertext, multimedia, beams, permascroll/
./tools/create-floating-image-sample.sh    # 11_floating_image, which the above deletes
```

**`tests/samples/xudu/permascroll/` is part of the fixture, not a stray directory.** None of these
stores holds any primedia: they are edit decision lists whose local spans are addresses in the one
permascroll all of them were generated against, so opening one without it gives a document whose
every version renders empty. `SampleXanadocsTest` constructs a `UserPermascroll` pointed at it and
passes it to every `Store`, and `TheSharedPermascrollExistsAndIsNonEmpty` checks it first so that a
missing one fails by name instead of as seventeen documents that mysteriously say nothing.
`11_floating_image` has its own alongside it, for the reason its generator script explains.

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

| Language                                                 | Formatter                                                      | Linter                                                                                                                                   | CI gate                                                                                             |
| -------------------------------------------------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| C++ (`.cpp`/`.hpp`/`.h`)                                 | clang-format (`make format`)                                   | clang-tidy (`.clangd`, editor-only)                                                                                                      | blocking                                                                                            |
| GLSL (`.glsl`)                                           | clang-format, same as C++                                      | `glslangValidator` via `make shaders` (compiles every shader)                                                                            | blocking (both)                                                                                     |
| Shell (`.sh`, `PKGBUILD`)                                | shfmt (`make format`)                                          | shellcheck (`make lint`)                                                                                                                 | blocking                                                                                            |
| YAML (workflows, dependabot)                             | yamlfmt (`make format`), config in `.yamlfmt`                  | yamllint (`make lint`), config in `.yamllint`                                                                                            | blocking                                                                                            |
| Markdown (every `*.md` outside `thirdparty/`)            | mdformat `--wrap 100` + three required plugins (`make format`) | mdl `-i` (`make lint`), config in `.mdlrc`/`.mdl_style.rb`                                                                               | blocking                                                                                            |
| Nix (`flake.nix`, `packaging/nix/*.nix`)                 | nixfmt-rfc-style, the flake's own `formatter` output           | nix-linter                                                                                                                               | blocking (job `nix`), separate from `format` because it needs Nix installed                         |
| Ruby (`packaging/macos/gleditor.rb`, a Homebrew formula) | `brew style` (needs Homebrew; not run in CI)                   | `ruby -c` (syntax only)                                                                                                                  | non-blocking, see `packaging.yml`'s `macos` job                                                     |
| RPM spec (`packaging/fedora/gleditor.spec`)              | —                                                              | rpmlint (needs `rpm`'s native Python binding, so only runs inside the `fedora:latest` container the `fedora` packaging job already uses) | non-blocking until its output has been triaged                                                      |
| Debian (`debian/rules`, `debian/control`, ...)           | —                                                              | —                                                                                                                                        | not covered: `debian/rules` is a `dh`-sequenced Makefile, not a shell script shellcheck understands |

yamlfmt and mdformat are Go/Python tools rather than apt packages (`go install`/`pip install` in the
CI `format` job); they were picked over the better-known Node ones (`prettier`,
`markdownlint-cli2 --fix`) to avoid adding a Node toolchain to an otherwise zero-Node tree, the same
reasoning `.yamllint`/`.mdl_style.rb` already used for the linters. **mdformat needs three plugins
here and refuses to run without them** — see the `--extensions` note below before running
`make format` on a machine you have not set up.

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
  spans as opaque, so it formats around the math instead of through it; required by name, so an
  mdformat that could do that damage refuses to start. A `$$...$$` block sitting directly under a
  list item with no blank line before/after it will still make mdformat refuse to write the file
  (`Formatted Markdown renders to different HTML than input Markdown`) — give it a blank line on
  both sides, same as a fenced code block would need.

- **The Makefile *requires* mdformat's plugins rather than hoping for them.** Both `make format` and
  `make format-check` pass `--extensions gfm --extensions dollarmath --extensions frontmatter`, and
  `--extensions` requires a plugin rather than merely enabling it, so an mdformat without them
  refuses:

  ```text
  Error: Invalid extension required.
  The required 'gfm' extension is not available. Please install a plugin that adds
  the extension, or remove it from required extensions.
  ```

  Nothing is written and the target fails. Install what it names:

  ```sh
  pip install --user mdformat-gfm mdformat-dollarmath mdformat-frontmatter
  ```

  This used to be the sharpest edge in the whole toolchain, because it failed *quietly*. The
  Makefile probed for the `mdformat` binary alone, so a machine with the binary and none of the
  plugins ran `make format` happily and corrupted every `design/` file it touched — tables reflowed
  into prose, `\times` double-escaped to `\\times` — while `make format-check` reported around
  thirty files as unformatted that CI considered perfectly fine. Both halves of that are gone: the
  failure is now a refusal that names the missing plugin, and a green `format-check` means the same
  thing everywhere. If you are wondering whether your markdown is really formatted, the answer is
  now whatever the target says.

- `.agents/skills/*.md` open with YAML front matter (`name:`/`description:` consumed by the skill
  loader); `mdformat-frontmatter` is what stops mdformat parsing the closing `---` as a second
  thematic break and mangling everything after it into a heading, and `mdl -i` is the matching half
  on the lint side (see the comment above the `mdl` invocation in the Makefile). Required by name
  too, for the same reason as the other two.

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
  - `ops.hpp`: the six hyperops. `OpKind::Structure` is OSMIC's sixth, MAKE/CHANGE STRUCTURE MAP,
    added in migration step 12; `Store::replay()` treats it as a text no-op because a slice's
    structure is a *second* replay product of the same spool — `Store::rebuildManifold()` is the
    fold that builds it. Its verb, link direction and value type live in `CompactOpNode::flags`
    (`StructureVerb`, `ValueKind`), not in sibling `OpKind`s. **A `SetLink`'s subject is not in a
    field of its own**: `sourceOpIndex` names the previous operation on the same cell, and that
    chain's far end is the `MakeCell` whose index *is* the `CellRef`
  - `compact_op.hpp`: the 64-byte `CompactOpNode`. Cache-line aligned, and **immutable once stored**
    — the child/sibling tree edges live in `SegmentedOpsSpool::tree` beside the nodes, because a
    sealed segment is mapped `PROT_READ` and writing a parent's child pointer took SIGSEGV. Three
    `static_assert`s hold the line: size, alignment, and `offsetof(value) == 56` (`alignas(64)`
    would pad a shrunken struct back to 64 on its own, so the size assertion cannot catch a field
    going missing)
  - `segmented_ops_spool.hpp/.cpp`: the memory-mapped ops tree; sealed segments, the id hash, and
    the `TreeLinks` side array
  - `user_permascroll.hpp/.cpp`: sovereign user permascroll stream and registry. Since step 13 this
    is the *only* place primedia is stored — `segmented_primedia_spool.cpp`'s active segment is what
    makes it durable, and a `Store` is handed one rather than making its own
  - `merkle_ledger.hpp/.cpp`: append-only Merkle ledger for identity consensus
  - `managed_torrent.hpp/.cpp`: system-managed torrent swarms coordinator
  - `publication.hpp/.cpp`: `publish`/`adopt`, `globalise`/`localise`, scroll keys
  - `identity/`: BEP 10 plugins, Hashcash PoW engine, and network controller
  - `zigzag/zzcore.{hpp,cpp}`, `zigzag/zzstructure.hpp`: the slice model shared with `apps/zigzag`
  - `zigzag/manifold.{hpp,cpp}`: **the slice model** (migration step 14). A second replay product of
    the ops spool, folded from `OpKind::Structure`: a `CellRef` is an ops-spool index, links are
    per-cell CSR runs keyed by a dimension *cell*, and both ends of a link are maintained by the
    fold, so asymmetry is not constructible through the API. `Store::rebuildManifold()` folds one,
    `Manifold::advance()` carries it forward one operation at a time, and
    `verifyAgainstFullRebuild()` is what says the two agree. `UnifiedTransclusionEngine` reads it
    (step 19); `ZigzagVisualizer` does not yet. Spelled `<zigzag/core/manifold.hpp>` from an app or
    a test. **A cell exists only where an operation minted one** — typing into a xanadoc mints no
    cells, which is the thing most likely to surprise you
  - `zigzag/arena_manifold.{hpp,cpp}`: the *ephemeral* half of R8's two-type split (migration step
    21\) — the same cells and CSR runs with the operations removed, so an evaluation that mutates
    structure a million times costs no ops and earns no names in hypertime. **An arena cell's ref
    carries `ephemeralBit`**, which is why `Store::setLink()` and `Manifold::applyStructure()`
    already refuse one: leakage is caught by machinery written for R12's derived cells. `mark()` is
    a choice point (seven arena lengths, no allocation), `release()` is undo-by-truncation plus a
    conditional trail, `discard()` is cut, and **`compact()` is refused while a mark is
    outstanding** because compaction moves the runs a mark's offsets name. `promote()` is the one
    road to the persistent side: it maps refs rather than being trusted with them, spools scratch
    bytes for real, and writes only what is reachable from the promoted root. Content bytes an
    evaluation constructs live under `scratchScroll` and are refused everywhere else. The design is
    `design/vlog-logic-extension.md` §5.2–§5.5. **An arena may be given a base `Manifold`** and is
    then a copy-on-write view of a document: reads fall through, the first write to a cell *shadows*
    it (whole slot, links and content copied in, keeping the base's `CellRef` — an overlaid cell is
    the same cell), and the base is never touched. Dropping a shadow is the undo, so `release()`
    needs no trail entry for one; `promote()` mints only refs carrying `ephemeralBit`, since
    anything else already has a name. **Step 21 is done** — what is not built is anything *above*
    the arena: no unification, no clause selection, no solver
  - `scalar.hpp/.cpp`: a scalar cell's two halves (migration step 15, R6) — the shortest round-trip
    `to_chars` rendering, spooled as ordinary primedia, and the canonical bits in
    `CompactOpNode::value`. Canonicalisation is for value equality only and never for addresses: two
    cells holding `3.14` are two cells at two addresses, because numeric coincidence is not
    quotation. A signalling NaN is refused rather than quieted. Minted with
    `Store::makeScalarCell()`, which is deliberately *not* an overload of `makeCell()` — a `bool`
    overload would capture `makeCell(v, "d.1")`
- `apps/xudu/` — the xanadoc editor's own UI: `beams.cpp`, `framing.cpp` (3D link ribbons and
  transclusion prisms), `session.cpp`, the overlays, `main.cpp`
- `apps/zigzag/` — the Xanadu Zigzag multidimensional visualizer; `apps/zigzag/core/`:
  - `unified_transclusion_engine.hpp/.cpp`: 120 FPS render staging over a `Manifold`, plus the
    shaping cache. Since step 19 it holds no cell space of its own: `syncIncremental()` *folds*
    operations rather than projecting them, `addCell()` mints an operation rather than filing a
    struct, and `cellForOp()` is gone because a `CellRef` already is an operation index. **It still
    has no production caller** — only its own tests; what zigzag actually draws is
    `ZigzagVisualizer` over `space_`
  - `compact_zzcell.hpp`: the cell layout the manifold replaced. Not the model any more, and not a
    render-side cache yet either — a type the engine no longer stores. 792 bytes, down from 960 now
    that `ephemeralText` and `Preflet` are deleted (steps 18–19), against 48 for a `CellSlot` plus
    12 per dimension. Prefer `zigzag::CellSlot`
  - `zz_xudu_projector.hpp/.cpp`: bidirectional xanadoc-to-zigzag mapping (in `apps/common/`), and
    since step 20 the YAML conversions: `sliceToStore()` mints a slice as `Structure` operations,
    `storeToSlice()` reads a `Manifold` back out. **There are no YAML slices to preserve** — sample
    and system slices are regenerated as stores, so this is a conversion and not a compatibility
    layer. A cell's `role`/`mime_type`/`media_path` live on `d.role`/`d.mime`/`d.media` ranks, since
    `CellSlot` has no field for them
- `assets/shaders/` — portable GLSL bodies; `vulkan/` holds generated SPIR-V
- `assets/zigzag/` — sample slice YAML documents
- `tests/lib/`, `tests/xudu/`, `tests/zigzag/` — unit tests for the library and engines
- `tests/samples/` — source material for tests; `tests/samples/xudu/` holds checked-in binary stores
  that must be regenerated whenever the on-disk format moves, plus the `permascroll/` their spans
  address (see "Tests" above)
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

**What a store is, on disk, after migration steps 8–13:**

| file           | shape                                                                                                      |
| -------------- | ---------------------------------------------------------------------------------------------------------- |
| `ops.nodes`    | `OpsSegmentHeader` (64 KiB, sparse) then a run of `CompactOpNode`                                          |
| `store.tables` | `\x89XUDUTBL` + version 2 + bencode: scroll registry, local segments, links, current versions, annotations |
| `ops.export`   | only when `--export-osmic` was given: the operations in OSMIC text, or the compact binary wire format      |

**That is the whole list — a store holds no primedia.** Everything typed goes into the author's one
`UserPermascroll` (`$XDG_DATA_HOME/xudu/permascroll/<key>/active.primedia`, or wherever
`--permascroll` says), and a store is an edit decision list naming addresses in it. So a store
directory is **not portable on its own**: opening one needs the permascroll it was written against,
which is why every checked-in fixture has one beside it and why `Store`'s permascroll is a
constructor argument rather than something it makes for itself.

`primedia.spool`, `ops.spool`, `current.yaml`, `versions.yaml`, `scrolls.spool`, `links.spool` and
`origins.spool` are gone, and **a directory holding any of them is refused by name.** Each was the
only copy of something: loading a `primedia.spool` store against the caller's permascroll would
resolve every local span into the wrong scroll and render a document that is not the document, which
is worse than failing because it looks like it worked. `Store::save()` deletes the superseded ones
once the new shape is written.

The one name worth being careful about is `ops.spool`. It meant the *binary operations spool* back
when the spool was a file; since `ops.nodes` it names the in-memory `SegmentedOpsSpool` instead, so
nothing on disk may be called that — `ops.export` is the export's name, and it holds either encoding
because `readOpsSpool()` tells them apart by magic.

**Three formats, one habit: refuse loudly, by number.** `ops.nodes` checks a signature and its
`nodeSize`; `store.tables` checks a signature and its version; the compact binary ops spool (now
`CompactBinaryV3`, four-bit kind field) refuses versions 1 and 2 with *"is version 2 and this build
reads version 3"*. Each throws a typed exception naming what was wrong. If you add a fourth format,
it does this too — a reader that returns an empty result for a file it did not understand is the
failure mode all of this exists to prevent.

Two rules follow:

- **A layout change still means regenerating every fixture in the same commit** (see "Tests"). The
  header turns silent corruption into a loud failure; it does not make the fixtures correct.
- **One writer.** `SegmentedOpsSpool::writeSegmentFile()` is the only thing that produces a segment
  file, and `Store::save()` calls it. Do not write `ops.nodes` from anywhere else — a format known
  in two places is what let step 1's change go unnoticed.
- **`build/xudu-dump` reads a store without the loader**, which is what lets a format here stop
  being human-readable at all. Point it at a store directory or at a single `ops.nodes`. Before and
  after any format change, diff `xudu-dump --section=ops <store>`: it renders what each operation
  *means*, including the text its span names, so a change that preserves meaning shows no diff.
  `--section=header` is where a version bump is supposed to show. Rendering the *text* needs
  `--permascroll=<dir>` as well, since the store does not have it; without one every other field
  still renders and only `text=` goes missing.

A *system* xanadoc under `~/.config/xudu/system/` is the one exception to "refused means refused":
`Session::systemStoreIndex()` moves an unreadable one aside and writes a default in its place,
because the program generates those itself and would otherwise refuse to start over its own
scaffolding. It catches every typed refusal a store shape can raise, listed rather than caught as a
common base — add a format and add it there too, or a config written by an older build becomes a
reason the program will not start. A document the user named is never treated that way.

This ruling has an expiry. It is void the first time someone outside this repository has a document
they care about; see R11 in `design/store-slice-convergence.md`.

### Where the reasoning lives

`design/` is the record of *why*, and three notes are load-bearing for current work:

- [`store-slice-convergence.md`](design/store-slice-convergence.md) — the active plan: a cell is an
  operation, `Slice` becomes a replay product of the ops spool like `Version` is. Fourteen rulings
  with their prices, a numbered migration (**steps 1–20 are done bar the visualizer's move onto
  `Manifold`**), and the measurements behind each. Read this before touching `CompactOpNode`,
  `Manifold`, `CompactZZCell` or the zigzag engine's sync path.
- [`vortex-hyperstructural-runtime.md`](design/vortex-hyperstructural-runtime.md) and
  [`vql-query-language.md`](design/vql-query-language.md) — a speculative runtime and query language
  over the same manifold. Neither is built, but both constrain the cell layout, and each now carries
  a reconciliation section against the convergence. Both are versioned with a change history, and
  the rule is stated in each: major for a change a conforming implementation could not ignore, minor
  for anything else that still alters the specification.
- [`vpl-array-language.md`](design/vpl-array-language.md) — an APL over the same manifold, written
  for fun and kept because it puts pressure where VQL does not: it is a language built on random
  access, which is exactly the convergence's open question U1 (a rank answers successor and
  predecessor, not "the *n*-th thing"). Read §5 before taking any of it seriously.
- [`vlog-logic-extension.md`](design/vlog-logic-extension.md) — **Vlog**, the Vortex Logic
  Extension: Prolog's unification and backtracking *inside* Vortex rather than compiled onto it,
  because `d.clone` is already a variable binding and a microversion is already a choice point. An
  extension and not a fourth language — it has no syntax of its own, and unlike VQL and VPL it is
  **the one design note here that asks the C++ core for something**. That ask is deliberately small:
  two methods and a 16-byte-per-entry vector on `ArenaManifold`, a class convergence step 21 has not
  written yet. Nothing in `ops.hpp`, `Manifold`, `Store` or any on-disk format changes. §10 lists
  what is gating, and §6.3 is U1 again from the logic side.
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
