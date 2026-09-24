# gleditor

A GPU-rendered text editor library (`gleditor`) and the programs built on it. C++23, GNU Make +
pkg-config, **no CMake**. Backends: OpenGL, OpenGL ES, and Vulkan when available, from one rendering
pipeline.

| program                   | what it is                                                                                                                                                |
| ------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `apps/gleditor`           | the plain editor; shares no code with anything Xanadu-related and keeps its own YAML config                                                               |
| `apps/xudu`               | the xanadoc (xanalogical hypertext) editor                                                                                                                |
| `apps/zigzag`             | the Project Xanadu ZigZag multidimensional visualizer                                                                                                     |
| `apps/xuzz`               | xudu and zigzag fused: `apps/xuzz/main.cpp` includes xudu's `main.cpp` under `XUZZ_BUILD`, which enables the Zigzag presentation boundary at compile time |
| `apps/vquery` / `vqueryc` | VQL query REPL/runner over one or many stores, and the standalone VQL-to-Vortex-bytecode compiler                                                         |
| `apps/vpl` / `vplc`       | VPL array-language REPL/runner (APL/J syntax, lattice grids), and its compiler                                                                            |
| `apps/vprolog`            | Prolog REPL over the Vortex/Vlog logic runtime                                                                                                            |
| `build/xudu-dump`         | reads a store directory or a single `ops.nodes` without the loader; the format-change diffing tool                                                        |
| `build/xudu-swarm-peer`   | the peer the network-namespace swarm tests drive                                                                                                          |

The README is the source of truth for anything not covered here (rendering architecture,
accessibility, xudu's data model, zigzag's space, SDL2/SDL3 differences); read the relevant section
before a non-trivial change in that area.

## Xuzz navigation workflow

For changes to Xuzz link selection, many-to-many endpoint browsing, activation, or movement between
xanadoc text and ZigZag cell content, use
[`xuzz-link-navigation`](.claude/skills/xuzz-link-navigation/SKILL.md). Its interaction contract and
acceptance criteria are in
[`design/ui_workflow_xuzz_navigation.md`](design/ui_workflow_xuzz_navigation.md). Keep one link
identity and both endsets in view while the reader explores either side. Completed reader
transitions form branching walks in the private `system://activity` store; live view movement
appends no operations to visited documents or slices. See the R8 activity-store extension in
[`store-slice-convergence.md`](design/store-slice-convergence.md).

## Setup: submodules

`thirdparty/argparse`, `Choreograph`, `merklecpp`, `SDL`, `zstd`, `nontype_functional`,
`beman_optional`, and `beman_inplace_vector` are git submodules; the tree does not build without
them. Run `git submodule update --init --recursive` first in any fresh clone or worktree, and re-run
it before investigating any build failure with missing headers under `thirdparty/`. Never vendor or
hand-edit under `thirdparty/`; bump the submodule pointer and commit the gitlink.

## Build

Always `make -j$(nproc)`.

```sh
make -j$(nproc)              # lib, every program above, the four test binaries, shaders, compile_commands.json
make -j$(nproc) lib|gleditor|xudu|xuzz|zigzag|xudu-dump|vquery|vqueryc|vpl|vplc|vprolog|xudu-swarm-peer
make -j$(nproc) shaders      # glslangValidator over every shader; SPIR-V when Vulkan is on
make -j$(nproc) fuzz         # fuzz_binary_ops, fuzz_link_package, fuzz_identity_wire (sanitizer build of the engine)
make profile                 # llvm-cov coverage report for gleditor_test
make clean | dist | doc
make format | format-check   # clang-format + shfmt + yamlfmt + mdformat; -check is what CI runs
make lint                    # shellcheck + yamllint + mdl + check-config-harmony
make tidy | scan-build | cppcheck | analyze (= tidy + scan-build) | check (= format-check + lint + analyze)
make tidy TIDY_FILES=... TIDY_CHECKS=...
```

Variables, all optional:

- `DEBUG=1` — debug flags and sanitizer flag sets.
- `GLEDITOR_DISABLE_VULKAN=1` — no Vulkan backend, no SPIR-V. **Flips the flags of every object**,
  so toggling it rebuilds the tree; `ccache` (automatic when installed; `GLEDITOR_NO_CCACHE=1` to
  disable) is what makes that cheap.
- `GLEDITOR_SDL=2|3` — pick the SDL major; unset probes pkg-config (SDL3 if present, else SDL2).
- `GLEDITOR_ENABLE_A11Y=1|0` — require/disable AccessKit; unset uses it if found.
- `CXX=g++` — override the compiler (default `clang++`).

Do not hand-pick `-std=` or hardcode a compiler in new build logic; the Makefile probes.

## Tests

### Everything runs headless

**Anything that could open a window or touch a display, GPU or audio device — `make test`, a test
binary, any program, `tools/compare-backends.sh`, an ad-hoc harness — runs headless unless the user
has explicitly asked for visual confirmation.** A window stealing focus interrupts whoever is at the
keyboard, and a run that depends on a real display cannot be reproduced in CI or over SSH.

`make` handles it: the Makefile exports `SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy`,
`LIBGL_ALWAYS_SOFTWARE=1`, plus `XDG_DATA_HOME`/`XDG_CONFIG_HOME` pointed into `build/xdg/`, so
every target, script and child process inherits them. The XDG pair matters: a store holds no
primedia, so what tests type goes into the author's permascroll under `$XDG_DATA_HOME`, and the
system xanadocs live under `$XDG_CONFIG_HOME` — without the redirect a test run pollutes your real
permascroll and overwrites your real settings. All five are `?=`, so an explicit override survives
(`SDL_VIDEODRIVER=wayland make test` still means it).

Outside `make` it is yours to set, in order of preference:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config <command>   # 1. almost always enough
xvfb-run -s "-screen 0 1024x768x24" <command>                                          # 2. when something insists on a display
xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh                        # 3. both; what compare-backends wants
```

That covers `./build/xudu_test --gtest_filter=...`, the common case that bypasses make. `xudu` (and
therefore `xuzz`) also take `--headless`, which skips window creation entirely and is what
`tools/create-sample-xanadocs.sh` uses; prefer it when driving them. `zigzag` has no such flag — use
the environment. If nothing above works for a new tool, add a way (a dummy driver, your own `Xvfb`,
a `--no-window` flag) rather than opening a window. When visual confirmation is the point, the user
will say so.

### Running them

```sh
make -j$(nproc) test                  # gleditor_test, xudu_test, xuzz_test, zigzag_test, then the rootless swarm tests
make test TEST_FILTER='MediaTest.*'   # gtest filter applied to all four
make test/e2e-orchestration           # tools/xudu-e2e-orchestration.sh against build/xudu
```

- `gleditor_test` links the real shared library (catches export-boundary bugs). `xudu_test` and
  `xuzz_test` link only the xanalogical engine, no graphics device, on purpose. `zigzag_test` covers
  the slice model and the transclusion engine.
- `make test` ends with `tools/swarm-netns-test.sh` under `unshare -Urnm`. That needs the `veth`
  kernel module; without it the run ends in `Error: Unknown device type.` and a non-zero exit
  **after all four gtest binaries passed**. `sudo modprobe veth` is the fix and it is the user's to
  run. Read the four `[  PASSED  ]` lines before concluding anything is broken.
- `E2EBinaryOrchestrationTest` and `AnimationTransclusionTest` shell out to `build/xudu`;
  `xudu binary not found` means the app did not link, not a test regression.
- `./tools/compare-backends.sh` renders a sample through every compiled-in backend and diffs the
  PNGs — the real check that a backend still draws, since one that draws nothing exits 0.

Unit tests live in `tests/lib/`, `tests/xudu/`, `tests/xuzz/`, `tests/zigzag/` (GoogleTest/
GoogleMock; match the style in place); fuzzers in `tests/fuzz/`.

### Binary fixtures under `tests/samples/`

`tests/samples/xudu/` — `core_hypertext/` (`unified_store`, `xanadoc_a`, `xanadoc_b`),
`multimedia/01…11`, `beams/01…03`, their generator inputs in `sources/`, and **`permascroll/`, which
is part of the fixture**: every store is an edit decision list whose spans address that one shared
permascroll, so without it every document renders empty. `SampleXanadocsTest` loads them and checks
the permascroll first, by name. `tests/samples/xuzz/slice_then_xanadoc/` is the combined
Xanadu/ZigZag store (`XuzzConvergenceSample`), from `sources/slice_then_xanadoc.xuzz`.

**Any change to `CompactOpNode`'s layout invalidates all of them.** `ops.nodes` records
`sizeof(CompactOpNode)` in its header, so a stale fixture is refused with `OpsSegmentUnreadable`
naming both sizes — a red test, not a working one. Regenerate in the same commit:

```sh
make -j$(nproc) xudu
./tools/create-sample-xanadocs.sh        # core_hypertext, multimedia, beams, permascroll/, the xuzz sample
./tools/create-floating-image-sample.sh  # multimedia/11_floating_image, which the script above deletes
```

Run both: the first removes the whole `multimedia/` directory, and `11_floating_image` is built by
hand with its own `.permascroll` beside it.

## Code style

### Diagnostics and debug output

- Use the named spdlog categories in `<gleditor/logging.hpp>` for new diagnostic messages. Prefer a
  permanent `GLEDITOR_LOG_DEBUG` or `GLEDITOR_LOG_TRACE` call that can be enabled with
  `SPDLOG_LEVEL` over a temporary `std::cout`, `std::cerr`, or `printf` statement. Name the
  subsystem in the category (for example, `text.layout`, `render.scene`, `xudu.edit`, or
  `xudu.links`).

- Keep command results, REPL output, benchmark markers, and machine-readable protocol lines on their
  existing stdout streams. Do not log document text, credentials, or key material in routine
  diagnostics.

- The macro checks the category level before evaluating formatting arguments. Use that guard for
  expensive diagnostics in render and edit paths. See `design/logging-migration.md` for the
  remaining conversion plan.

- `.clang-format` (LLVM-based, `IndentWidth: 2`, `ColumnLimit: 80`, aligned consecutive assignments)
  governs C++ **and `.glsl`**. `.editorconfig` mirrors every formatter's settings for editors; it is
  a hint, not a gate. Run `make format` before committing, or `clang-format -i --style=file <file>`
  for one file.

- CI (`.github/workflows/c-cpp.yml`, job `format`) runs `make format-check` and `make lint` on a
  checkout with no build dependencies installed — see "Makefile gotchas" for how.

- `.clangd`/`.clang-tidy` enable `modernize-*`, `bugprone-*`, `cppcoreguidelines-*`,
  `performance-*`, `readability-*`, `portability-*`, `clang-analyzer-*` (minus the few disabled in
  `.clangd`). Treat warnings on lines you touch as worth fixing.

- Comments explain *why* a non-obvious choice was made — a constraint, a workaround, a tradeoff —
  never what the code does.

- `compile_commands.json` is regenerated by `make`; do not hand-edit it.

### Formatting and linting, by language

| Language                                    | Formatter                                      | Linter                                                       | CI gate                                         |
| ------------------------------------------- | ---------------------------------------------- | ------------------------------------------------------------ | ----------------------------------------------- |
| C++ (`.cpp`/`.hpp`/`.h`)                    | clang-format                                   | clang-tidy (`make tidy`), scan-build                         | blocking; `make check` runs all gates           |
| GLSL (`.glsl`)                              | clang-format                                   | `glslangValidator` via `make shaders`                        | blocking (both)                                 |
| Shell (`.sh`, `PKGBUILD`)                   | shfmt `-i 2 -ci`                               | shellcheck                                                   | blocking                                        |
| YAML (workflows, dependabot)                | yamlfmt (`.yamlfmt`)                           | yamllint (`.yamllint`)                                       | blocking                                        |
| Markdown (all `*.md` outside `thirdparty/`) | mdformat `--wrap 100` + three required plugins | mdl `-i` (`.mdlrc`, `.mdl_style.rb`)                         | blocking                                        |
| Nix (`flake.nix`, `packaging/nix/`)         | nixfmt-rfc-style (the flake's `formatter`)     | nix-linter                                                   | blocking, job `nix` (needs Nix)                 |
| Ruby (`packaging/macos/gleditor.rb`)        | `brew style` (needs Homebrew; not in CI)       | `ruby -c`                                                    | non-blocking (`packaging.yml`, `macos`)         |
| RPM spec (`packaging/fedora/`)              | —                                              | rpmlint, inside the `fedora:latest` packaging container only | non-blocking until triaged                      |
| Debian (`debian/`)                          | —                                              | —                                                            | not covered (`debian/rules` is a `dh` Makefile) |

Formatter and linter settings are kept in one place per language so `make format` never leaves a
file `make lint` rejects; `tools/check-config-harmony.sh` (run by both targets) enforces that
`.editorconfig`, `.clang-format`, `.yamlfmt`, `.yamllint`, `.mdl_style.rb`, `.clangd` and
`.clang-tidy` agree. Details worth knowing:

- yamlfmt and mdformat are Go/Python tools (`go install`/`pip install`), chosen over the Node
  equivalents to keep the tree zero-Node. `.yamlfmt`'s `indentless_arrays` matches `.yamllint`'s
  `indent-sequences: false`; its `retain_line_breaks` has a known bug with folded `>` scalars
  (google/yamlfmt#84), so this repo has none.
- **mdformat requires three plugins and refuses to start without them:**
  `pip install --user mdformat-gfm mdformat-dollarmath mdformat-frontmatter`. The Makefile passes
  them as `--extensions`, so a missing one fails loudly by name rather than silently corrupting
  `design/` (which is what an unplugged mdformat did: `\text` → `\\text`, tables reflowed to prose).
  A green `format-check` therefore means the same thing everywhere. `dollarmath` keeps `$...$` and
  `$$...$$` opaque; `frontmatter` keeps `.agents/skills/*.md`'s YAML header from being parsed as a
  heading (`mdl -i` is the lint-side half).
- `--wrap 100` matches `MD013`'s limit. An inline code span or `$...$` near column 100 is atomic to
  mdformat and can still overflow: shorten the prose, or promote `$$...$$` to a ```` ```math ````
  fence (exempt from `MD013`). A `$$...$$` block directly under a list item needs blank lines on
  both sides or mdformat refuses to write.
- Unfenced code in `design/` is invisible to both tools (mdl flags `#include` as a bad header,
  mdformat reflows it). Fence anything that looks like code.
- `editorconfig-checker` is not a gate: it misreads clang-format continuation indents and table
  padding. Use it only to sanity-check `.editorconfig` itself.

## Project structure

- `src/` — the library: document model, glyph cache, SDL wrappers, render loop. `src/text/` (native
  layout: `FontManager`, `FontFace`, `TextLayout` over HarfBuzz/FreeType/libunibreak/FriBidi),
  `src/render/` (`gl/`, `vulkan/`), `src/a11y/` (AccessKit), `src/enfilade/` (Layoutfilade).
- `include/gleditor/` — public headers, incl. `text/`, `render/`, `enfilade/`.
- **`apps/common/xanadu/` — where the xanalogical engine lives.** `apps/xudu/core/` and
  `apps/zigzag/core/` are one-line forwarding shims (`#include "common/xanadu/x.hpp"` plus
  `namespace xudu { using namespace ::xanadu; }`); `<xudu/core/store.hpp>` is the include spelling,
  `apps/common/xanadu/store.cpp` is the file to edit. Editing a `core/` shim is almost always a
  mistake. Subdirectories: `enfilade/` (Spanfilade, Chronofilade + `EdlTransform`, Holefilade,
  Arrayfilade — all ephemeral replay products that mint no operations), `identity/` (BEP 10 plugins,
  Hashcash PoW, network controller), `vortex/` (core, VM, host, stdlib), `vql/`, `vpl/`, `vprolog/`,
  `zigzag/`.
- `apps/xudu/` — the editor's own UI: `beams.cpp`, `framing.cpp`, `session.cpp`,
  `hypertime_graph.cpp`, `bridge_coordinator.cpp`, the overlays, `main.cpp`. **Xudu emits no
  Structure operations itself** (only `batch_orchestrator.cpp` does); the editor is text-ops only.
- `apps/zigzag/` — the visualizer. `ZigzagVisualizer` owns a `UnifiedTransclusionEngine` over a
  `Manifold` and draws from it; the legacy `ZZSpace` is gone from it. `adoptDocument()` takes the
  projector's `ZzStructureDocument` DTO and mints it as Structure operations via `sliceToStore()`.
- `apps/xuzz/`, `apps/vquery`, `vqueryc`, `vpl`, `vplc`, `vprolog` — see the table at the top.
- `assets/shaders/` — GLSL bodies; `vulkan/` holds generated SPIR-V. There is no `assets/zigzag/`
  any more: the YAML slice format is deleted and every slice is a store.
- `tests/lib/`, `tests/xudu/`, `tests/xuzz/`, `tests/zigzag/`, `tests/fuzz/`, `tests/samples/`.
- `tools/` — `compare-backends.sh`, `create-sample-xanadocs.sh`, `create-floating-image-sample.sh`,
  `swarm-netns-test.sh`, `xudu-e2e-orchestration.sh`, `check-config-harmony.sh`,
  `benchmark-kjv-load.{py,sh}`, `layout-latency-probe.cpp`, `shader_assemble.cpp`, `xudu-dump.cpp`,
  `xudu-swarm-peer.cpp`, `code-quality-audit.py`, and the scene/showcase generators.
- `packaging/` (arch, debian, fedora, macos, windows, nix), `design/` (the *why*), `thirdparty/`.

### The engine's load-bearing types

- `store.hpp/.cpp` — OSMIC hypertime: branches, microversions, the six hyperops as operations, and
  the Structure API (`sliceGenesis`, `makeCell`, `makeScalarCell`, `setLink`, `spliceCell`,
  `spliceCellSpan`, `setValue`, `makeDimension`, `rebuildManifold`).
- `ops.hpp` — `OpKind`. `Structure` is OSMIC's sixth hyperop (MAKE/CHANGE STRUCTURE MAP): verb
  (`MakeCell`/`SetLink`/`SetValue`/`Splice`), direction and `ValueKind` ride in
  `CompactOpNode::flags`. `Store::replay()` treats it as a text no-op because a slice's structure is
  a *second* replay product of the same spool. **A `SetLink`'s subject is not a field**:
  `sourceOpIndex` names the previous operation on the same cell, and the chain's far end is the
  `MakeCell` whose index *is* the `CellRef` (R7).
- `compact_op.hpp` — the 64-byte `CompactOpNode`, cache-line aligned, **immutable once stored**
  (sealed segments are `PROT_READ`; tree edges live in `SegmentedOpsSpool::tree`). Three
  `static_assert`s: size, alignment, `offsetof(value) == 56` — the last because `alignas(64)` would
  pad a shrunken struct back to 64 silently.
- `segmented_ops_spool.hpp/.cpp` — the mmap'd ops tree; 64 KiB Merkle-piece-aligned segments (1,024
  nodes each), sealed segments, the id hash, `TreeLinks`.
- `user_permascroll.hpp/.cpp` — the author's one permascroll, **the only place primedia is stored**;
  a `Store` is handed one, never makes its own.
- `store_tables.hpp` — `store.tables` (bencode, version 3): scroll registry, local segments, the
  `Link` records, designated current versions, version annotations. None has a hypertime name.
- `publication.hpp/.cpp` — publish/adopt, `GlobalSpan` ↔ `PrimediaSpan` (`globalise`/`localise`),
  `GlobalOpRef` ↔ op index (`opRefOf`/`localiseOpRef`), `historyFromSeal`. `provenance.hpp` — the
  GPG-signed `AUTHORSHIP.tsv` sealed into a torrent. `merkle_ledger`, `managed_torrent`,
  `transcopyright_*`, `swarm*` — identity and swarm.
- `zigzag/manifold.{hpp,cpp}` — **the slice model**: cells folded from Structure operations, a
  `CellRef` is an ops index, links are per-cell CSR runs keyed by a dimension *cell*, a cell's
  content is a run of spans. `Store::rebuildManifold()` folds, `advance()` steps,
  `verifyAgainstFullRebuild()` is the honesty check. **A cell exists only where an operation minted
  one** — typing into a xanadoc mints no cells.
- `zigzag/arena_manifold.{hpp,cpp}` — the ephemeral half of R8's two-type split: same cells and
  runs, no operations, refs carry `ephemeralBit` (which `Store::setLink` and `applyStructure`
  refuse). `mark()`/`release()`/`discard()` are choice point/undo-by-truncation/cut; `compact()` is
  refused while a mark is outstanding; `promote()` is the one road to persistence and maps refs
  rather than trusting them. Given a base `Manifold` it is a copy-on-write view (first write shadows
  the slot, keeps the `CellRef`, base untouched). Scratch bytes live under `scratchScroll` and are
  refused everywhere else. Hosts Vortex, VQL, VPL and Vlog.
- `zigzag/vlog.{hpp,cpp}` — `unify()` and the trail over the arena. `vortex/vortex_vm.hpp` carries
  the logic opcodes (`Unify`, `Choice`, `Fail`, `Cut`, `ChoicePoint`) that `vprolog` runs on.
- `zigzag/zz_xudu_projector.{hpp,cpp}` — xanadoc ↔ ZigZag: `projectXuduToZigzag` (documents and
  hypertime branches into cells on `d.doc`/`d.transclude`/`d.link`/`d.version`), `sliceToStore()`
  (mint a DTO as operations), `storeToSlice()`. `role`/`mime_type`/`media_path` live on
  `d.role`/`d.mime`/`d.media` ranks.
- `zigzag/compact_zzcell.hpp` — a dead type: the pre-manifold cell layout, referenced by nothing but
  itself, budgeted by `static_assert` at ≤ 1024 bytes. Prefer `zigzag::CellSlot` (32 bytes plus 12
  per dimension).
- `scalar.hpp/.cpp` — a scalar cell's two halves (R6): shortest-round-trip text spooled as primedia
  plus canonical bits in `value`. Canonicalisation is for equality, never addresses. Signalling NaN
  refused. `makeScalarCell()` is deliberately not a `makeCell()` overload.
- `system_docs.hpp/.cpp` — the five system xanadocs (`system://keymap|settings|layout|ui|pouches`)
  as configuration-as-structure over ten dimensions (`d.dims`, `d.vars`, `d.values`, `d.groups`,
  `d.subgroups`, `d.clone`, `d.notes`, `d.schemas`, `d.alternates`, `d.default`), with schema
  validation and defaults; the heaviest emitter of Structure operations in the tree. Adding a
  setting means adding a `defaultSettingSpecs()` entry, which is easy to skip.
- `vql/multi_store.hpp` — many stores composed in **one arena** via a `d.stores` rank; foreign cells
  are re-minted as fresh ephemeral refs. Nothing persistent crosses stores anywhere yet.
- `vortex/vortex_host.hpp` — `promoteAndAttachToStore()` persists a compiled program as real
  Structure operations; the zigzag VQL palette calls it in production.

## Xanadulogical and identity architecture

- **One permascroll per user.** All typing appends to one `UserPermascroll` bound to the author's
  OpenPGP / BEP 46 identity, in 64 KiB page-aligned segments (Merkle piece stability, zero-copy
  `mmap(MAP_FIXED)` growth). A `Store` is an edit decision list over slot 0 (local) or external
  scrolls (`ScrollId > 0`). `DeviceDelegation` maps master GPG fingerprints to device-salted BEP 46
  keys. Live collaboration carries **no raw text**: 48-byte descriptors and `GlobalSpan`s.
- **Merkle identity ledger.** `MerkleLedger` (merklecpp) holds verified fingerprints and email
  mappings; BEP 10 plugins `xudu_identity_lookup`, `xudu_oracle_vote`, `xudu_oracle_verify` do
  challenge-response and peer gating; `HashcashEngine` PoW resists Sybil/DoS.
- **Beams.** Emergent transclusion prisms (Identity Gold quads) versus explicit xanalinks
  (cyan/magenta ribbons); multi-span spines and hue shifts disambiguate dense hypertexts; margin
  anchor brackets support four overlapping anchor colours.
- **Transclusion discovery** is the Spanfilade (O(log N + K) interval stabbing over permascroll
  addresses, across documents and cells); `link_layout.cpp` still holds a duplicate linear scanner
  the bridge plan retires.

### On-disk formats: bump the version, don't carry a shim

Nothing is in production and every store regenerates from its inputs, so **a format may change
shape: bump the version, write the new shape, delete the old reader.** Non-negotiable are the
invariants: `sizeof(CompactOpNode) == 64` and its alignment, 64 KiB Merkle pieces, append-only-ness.
This ruling expires the first time someone outside this repository has a document they care about
(R11 in `design/store-slice-convergence.md`).

A store directory is exactly:

| file           | shape                                                                                                          |
| -------------- | -------------------------------------------------------------------------------------------------------------- |
| `ops.nodes`    | `OpsSegmentHeader` (64 KiB, sparse, records `nodeSize`) then a run of `CompactOpNode`                          |
| `store.tables` | `\x89XUDUTBL` + version 3 + bencode: scrolls, local segments, links, current versions, annotations             |
| `ops.export`   | only with `--export-osmic`: the operations as OSMIC text or the compact binary wire format (`CompactBinaryV3`) |

**A store holds no primedia**, so it is not portable alone: it needs the permascroll it was written
against (`$XDG_DATA_HOME/xudu/permascroll/<key>/active.primedia` or `--permascroll`). The retired
files — `primedia.spool`, `ops.spool`, `current.yaml`, `versions.yaml`, `scrolls.spool`,
`links.spool`, `origins.spool` — are **refused by name**, because loading one against the wrong
permascroll renders a document that is not the document. `ops.spool` now names only the in-memory
`SegmentedOpsSpool`; nothing on disk may use it.

Rules:

- **Refuse loudly, by number.** Each format checks a signature and version and throws a typed
  exception naming what was wrong. A reader that returns empty for a file it did not understand is
  the failure all of this exists to prevent. A new format does the same.
- **Regenerate every fixture in the same commit** as a layout change.
- **One writer.** `SegmentedOpsSpool::writeSegmentFile()` alone produces `ops.nodes`;
  `Store::save()` calls it.
- **Diff meaning, not bytes.** Before and after a format change, diff
  `xudu-dump --section=ops <store>` (add `--permascroll=<dir>` to render text); `--section=header`
  is where a version bump shows.
- **System xanadocs are the one exception to "refused means refused":**
  `Session::systemStoreIndex()` moves an unreadable one aside and writes a default, catching every
  typed refusal *by name* — add a format, add it there, or an old config stops the program starting.
  A user's document is never treated that way.

**Known defect:** the `CompactBinaryV3` writer omits `op.source` for Structure operations, so a
published `SetLink`/`SetValue`/`Splice` arrives with no R7 chain and the reader's fold refuses it;
published slices land as unlinked cells. `Splice` also lacks `at`/`length` on the wire. The fix is
`CompactBinaryV4`; see `design/structure-hyperop-vision.md` §3 before touching `binary_ops.cpp`.

### Where the reasoning lives

- [`store-slice-convergence.md`](design/store-slice-convergence.md) — the active plan: a cell is an
  operation and a slice is a replay product. Fourteen rulings (R1–R14) with prices, the migration
  (complete through step 21, `ArenaManifold`), open questions U1–U3. Read before touching
  `CompactOpNode`, `Manifold`, or the zigzag sync path. R8 (view movement does not mutate a visited
  store; a proposed separate activity store records completed reader visits), R11 and R12 are the
  ones most often needed.
- [`structure-hyperop-vision.md`](design/structure-hyperop-vision.md) — what else Structure can
  carry, grounded: the wire defect above, and a prerequisite-ordered proposal list.
- [`vortex-hyperstructural-runtime.md`](design/vortex-hyperstructural-runtime.md),
  [`vql-query-language.md`](design/vql-query-language.md),
  [`vpl-array-language.md`](design/vpl-array-language.md) — the runtime and the two front-end
  languages over the manifold. All three are now implemented under `apps/common/xanadu/`; each doc
  carries a reconciliation section against the convergence and a versioned change history.
- [`vlog-logic-extension.md`](design/vlog-logic-extension.md) — Vlog: unification and backtracking
  inside Vortex. §4–§5 (arena, choice points, trail, `unify()`) are built; §6–§7 (clause selection
  and a solver written *in Vortex*) are not and are gated on U1. `vprolog` runs on the VM's C++
  logic opcodes instead.
- [`enfilade/`](design/enfilade/) — `enfilade-discussion.md` (the survey and the 4.96 ns breakeven),
  `enfilade-rank-indexing.md` (the U1 proposal; §3 says why crums are not cells), and one note per
  built enfilade (spanfilade, chronofilade, holefilade, layoutfilade, arrayfilade).
- [`osmic-microversioning-and-dag.md`](design/osmic-microversioning-and-dag.md) — hypertime naming.
- [`system-xanadocs-customization-and-metasystem.md`](design/system-xanadocs-customization-and-metasystem.md)
  — the settings model; §6.2 is the live ten-dimension geometry, §4.1 preset sharing is unbuilt.
- [`xudu-zigzag-unified-hypermedia-bridge.md`](design/xudu-zigzag-unified-hypermedia-bridge.md) and
  [`xudu-zigzag-hypermedia-bridge-next-stage.md`](design/xudu-zigzag-hypermedia-bridge-next-stage.md)
  — the bridge (`UniversalLinkEnd`, `BridgeCoordinator`, Spanfilade as canonical discovery); the
  federated cross-store link source is explicitly deferred.

## Text architecture

Zero Cairo, zero Pango: FreeType 2, HarfBuzz, libunibreak (UAX #14), FriBidi, Fontconfig.

- `text::FontManager`/`FontFace` (`src/text/font.cpp`) resolve `"Sans Bold 12"`-style descriptions
  via Fontconfig into thread-safe `FT_Face`/`hb_font_t` pairs and compute `ascent`, `descent`,
  `lineHeight`, `spaceWidth`.
- `text::TextLayout` (`src/text/layout.cpp`): `layoutPage()` breaks with libunibreak, shapes with
  HarfBuzz, wraps at `maxWidthPx`, paginates by `maxHeightPx`. **Height-budgeted slicing** — input
  is cut to the lines that could fit — keeps page generation $O(1)$ instead of $O(N^2)$ over
  thousands of pages. `layoutSingleLine()` is the toast/measurement fast path. Output is
  `PageShaping`, consumed by `Doc` and `Page`.
- `GlyphCache` (`src/glyphcache/cache.cpp`) rasterises 8-bit coverage with FreeType into a 2D
  texture array atlas (512² growing to 16384² over 64 layers). Quads anchor to `line.top` at
  `lineHeight`; cluster textures put the baseline at $Y = \text{ascent}$.
- After any shaping or layout change run `./tools/compare-backends.sh` under `xvfb-run` and inspect
  the PNGs it writes (flat baselines, correct cluster height, sharp glyphs). Reading a captured
  frame is inspection; opening a live window is not.

## ZigZag cell representation: text and mixed media

Text that would require parsing or mixed-media cells are better represented by additional cells and
dimensions rather than crammed into a cell's content span. A cell's span is its canonical text; new
dimensions (`d.parsed`, `d.media`, `d.roles`, etc.) let structure live in the model rather than in
prose. This keeps ZigZag cells orthogonal, keeps them quotable, and avoids the grammar-lock that
arises when meaning is hidden in paragraph structure or embedded markup.

## Sovereign keymap and Vortex governance

- Every key binding in `xudu`, `zigzag` and `xuzz` is defined in `system://keymap`, never hardcoded;
  binding actions are Vortex calls or registered Vortex routines/macros. `gleditor` is exempt and
  must share no code or dependency with Xanadu, ZigZag or Xuzz.
- New C++ must justify why it is not Vortex (hardware/driver interfacing, rendering intrinsics,
  allocator primitives, raw OS events).
- New Vortex standard-library code reuses existing standard-library functions unless it cannot.
- Configuration lives in system xanadocs (xudu/xuzz), sovereign-store-backed system slices (zigzag),
  or gleditor's YAML — no naked magic numbers in algorithms; see
  `.agents/rules/architectural_governance.md`.

## Makefile gotchas

- Toggling `GLEDITOR_DISABLE_VULKAN` or `DEBUG` changes every object's flags;
  `$(OBJDIR)/.buildflags` records a signature so stale objects rebuild. Preserve it if you touch
  flag handling.
- SDL major is probed, not assumed; GL/GLES entry points come from `SDL_GL_GetProcAddress` — never
  add `-lGL`.
- `format`, `format-check`, `lint`, `doc` probe tools with `command -v` and echo a notice rather
  than fail when one is missing.
- `format`, `format-check`, `lint` are the `NO_SDL_GOALS`: exempt from the pkg-config checks and
  from `include $(DEPS)` (as `clean`/`dist` are) so they run on a checkout with no build deps. Never
  add a goal that compiles anything to that list.

## License

GPL-3.0 (see `LICENSE`). Whether it is GPL-3.0-only or -or-later is unconfirmed; do not resolve that
in headers or packaging metadata.
