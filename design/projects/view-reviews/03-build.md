# Build, relocation and test-infrastructure review of `design/view-system.md`

Scope: read-only review of the repository at `/data/git/gleditor-restore`. No files were modified,
no target was built, and no test binary or program was run. One `make -n xuzz` dry run was attempted
and is disclosed in §1.6 — it is not fully dry on this Makefile (GNU Make regenerates included
`.dep` files even under `-n`), so it executed several real `clang++ -MM` dependency-scan
subprocesses before being interrupted by its own timeout. No object file, binary, or tracked file
resulted; `build/` is git-ignored (`.gitignore:52`) and pre-existed with unrelated binaries from
earlier work. No further `make` invocations (dry-run or otherwise) were made after that; everything
else below comes from `grep`/`find`/`wc`/`git log`/`git grep` and reading source.

______________________________________________________________________

## 1. Relocation procedure (spec §17 step 1)

### 1.1 Headline correction: the spec's two "no graphics header" exceptions are both wrong

`design/view-system.md:2880-2889` carves two files out of the otherwise-universal "everything in
`apps/xudu/`/`apps/zigzag/` goes to `apps/common/ui/`" rule, asserting each has "no graphics header
... included" and sending it to `apps/common/xanadu/` instead:

- `apps/xudu/link_context.*`
- `apps/zigzag/unified_transclusion_engine.*`

Both claims are false as the code stands today, and both are load-bearing for the Makefile's
device-isolation guarantee, not just a style preference.

**`unified_transclusion_engine.hpp` is the single most GPU-coupled file in either directory.** It
includes `"gleditor/doc.hpp"`, `"gleditor/glyphcache/cache.hpp"`, and
`"gleditor/render/stream_buffer.hpp"` directly
(`apps/zigzag/unified_transclusion_engine.hpp:28-30`). Its own section header says what it is:
"Zero-Copy GPU Render Staging" (`apps/zigzag/unified_transclusion_engine.hpp:241`). Its public API
is:

```cpp
[[nodiscard]] RenderInstanceBatch
stageVisibleCells(const RenderSliceRequest &req,
                   const gleditor::text::FontFacePtr &font,
                   gleditor::GlyphCache &glyphCache);                 // :262-265
[[nodiscard]] std::size_t stageIntoStreamBuffer(
    const RenderSliceRequest &req, const gleditor::text::FontFacePtr &font,
    gleditor::GlyphCache &glyphCache, render::IStreamBuffer &streamBuffer); // :272-274
```

`GlyphCache` is not a value type that merely mentions rendering: `src/glyphcache/cache.cpp:18`
includes `<gleditor/render/device.hpp>` "for RenderDevice" and uploads glyph bitmaps to it. Its
test, `tests/zigzag/test_unified_transclusion_engine.cpp:255`, constructs
`gleditor::GlyphCache glyphCache(&device)` against a real `RenderDevice` — which is exactly why that
test lives in `tests/zigzag/` (the binary that links `apps/common/ui/` and the library,
`design/view-system.md:2826`) and not in `tests/xudu/` (engine-only, no device,
`design/view-system.md:2825`, Makefile comment at `Makefile:888-892`).

**`link_context.hpp` has a hard-wired dependency on `Session`.** Its only non-trivial constructor
argument is `Session &session` (`apps/xudu/link_context.hpp:67-70`), and it `#include`s
`"xudu/session.hpp"` directly (`apps/xudu/link_context.hpp:31`) — the very file
`design/view-system.md:2883` sends to `apps/common/ui/xanadoc/` because `Session` pulls
`<gleditor/doc.hpp>`, `<gleditor/canvas.hpp>`, `<gleditor/svg_cache.hpp>`,
`<gleditor/image_cache.hpp>` (`apps/xudu/session.hpp:32-42`). `LinkContext::execute()` and friends
call `session.store()`, `session.views()`, `session.store(idx)`
(`apps/xudu/link_context.cpp:45,50,56-57,65,75-80,95,220-223,263`) — real, load-bearing calls, not
an unused include. `LinkContext`'s only existing exerciser is
`tests/zigzag/link_panel_overlay_test.cpp` (confirmed by `grep -rln LinkContext tests/` returning
only that one file) — i.e. it is already tested exclusively from the device-linked binary, never
from `tests/xudu/`.

Putting either file under `apps/common/xanadu/` as `COMMON_XANADU_SRCS` (`Makefile:603`) would make
it part of `XUDU_CORE_OBJS` (`Makefile:605,622`), which is linked into `xudu_test`, `xuzz_test`,
`vquery`, `vpl`, `vprolog`, `vqueryc`, `vplc`, `xudu-swarm-peer`, and all three fuzz targets —
**none of which link `$(LIBLINK)`/`$(APP_LDFLAGS)`**
(`Makefile:894-895,898-899,969-990,952-953,931-934`). The Makefile's own comment at
`Makefile:888-892` says this link line *is* the architectural boundary check ("if a rule ... ever
needed a renderer, this would stop linking"). Moving either file there as specified would not "fail
the architecture review" — it would fail to **link**, across ten targets at once, the first time any
of them pulled in a symbol `GlyphCache::upload*`, `Doc::VBORow`, or `Session::store()` actually
needs resolved (which, given the methods above, is immediately).

**Corrected destination table for step 1** — everything in both directories, no exceptions:

| From                                                                        | To                        |
| --------------------------------------------------------------------------- | ------------------------- |
| every file in `apps/xudu/` (19 `.hpp`/`.cpp` pairs, incl. `link_context.*`) | `apps/common/ui/xanadoc/` |
| every file in `apps/zigzag/` (incl. `unified_transclusion_engine.*`)        | `apps/common/ui/slice/`   |

No file moves to `apps/common/xanadu/` in step 1. `apps/xuzz/view_coordinator.*` stays put, as the
spec already says (`design/view-system.md:2890`), pending step 6.

A narrower split that honours the spec's *intent* — hoisting the device-free parts of
`unified_transclusion_engine` (`syncIncremental`, `linkCells`, `addCell`, `metaDimensionsOf`,
`linked`, `isProtected`, `resolveCellText` — none of which touch `Doc`/`GlyphCache`/`IStreamBuffer`)
into a real `apps/common/xanadu/zigzag/` engine class, leaving only the "Zero-Copy GPU Render
Staging" section behind — is possible, but it is new design work (a second VU7-style split this
document doesn't mention; see §1.4), not a "moves only" step. It should not block step 1.

### 1.2 A related, smaller finding

```text
apps/xudu/satelloid.hpp
```

and `tenuous_tether.hpp` already mix layers, and a test already straddles the move

`tests/xudu/satelloid_physics_test.cpp:16-17` and `tests/xudu/tension_layout_test.cpp:15` — both in
the **engine-only** `tests/xudu/` directory, which links neither the library nor `apps/xudu`'s own
`.cpp` objects (`XUDU_SRCS` is not part of `XUDU_CORE_SRCS`, `Makefile:605-606`) — `#include`
`"xudu/satelloid.hpp"` / `"xudu/tenuous_tether.hpp"`. Those headers are themselves mixed:
`satelloid.hpp` declares a plain, inline-only `CellSatelloid` struct
(`apps/xudu/satelloid.hpp:71-126`, no out-of-line methods) **and** a device-bound
`SatelloidOverlay : FrameContributor, PickObserver, a11y::Source` class in the same file
(`apps/xudu/satelloid.hpp:133-150`). The physics test only ever instantiates `CellSatelloid`
(confirmed by grep: no `SatelloidOverlay`/`TenuousTether` symbol appears in either test file), which
is why it links today without the library. This is pre-existing, harmless as long as nobody adds an
out-of-line call from the test, but it is fragile: after the move, `tests/xudu/` will
`#include "common/ui/xanadoc/satelloid.hpp"` for a type that conceptually belongs in the engine
layer. Not a blocker; worth a line in the plan as future cleanup (extract `CellSatelloid` into
`apps/common/xanadu/` as a real engine value type, the same shape of fix as VU7 proposes for
`session`/`batch_orchestrator`).

### 1.3 Every include spelling that must change

`-Iapps` is the only app-level include root (`Makefile:463`), and quoted includes resolve against
the including file's own directory before falling back to `-I` search paths, so:

- `#include "xudu/<name>.hpp"` used from **outside** `apps/xudu/` resolves via `-Iapps` to
  `apps/xudu/<name>.hpp` today and must become `"common/ui/xanadoc/<name>.hpp"`.
- `#include "zigzag/<name>.hpp"` used from **outside** `apps/zigzag/` resolves the same way to
  `apps/zigzag/<name>.hpp` and must become `"common/ui/slice/<name>.hpp"`.
- **Caution:** `#include "zigzag/manifold.hpp"` and friends, used from inside `apps/common/xanadu/`
  (`apps/common/xanadu/format_resolver.hpp:21`, `apps/common/xanadu/store.hpp:62`,
  `apps/common/xanadu/extern_ref.{hpp,cpp}`, `apps/common/xanadu/reading_place.cpp:13`,
  `apps/common/xanadu/pouch_zone.cpp:15`) are a **different "zigzag"** — the engine's own
  `apps/common/xanadu/zigzag/` subdirectory (manifold, cell views, dim vectors), which is not moving
  at all. Do not touch these; a blind `s/zigzag\//common\/ui\/slice\//` would corrupt them. Scope
  the sed to the literal header names that exist in `apps/zigzag/` today
  (`unified_transclusion_engine.hpp`, `zigzag_commands.hpp`, `zigzag_visualizer.hpp`) or to files
  outside `apps/common/xanadu/`.
- Same-directory, unprefixed quotes (`#include "beams.hpp"`, `#include "session.hpp"`,
  `#include "world_card_presentation.hpp"`) need **no change**, because the files that use them move
  together.

Exhaustive list of files with a `"xudu/..."` or `"zigzag/..."` spelling that must be rewritten
(excludes the moving files' own self-references, which stay unprefixed once co-located; excludes the
engine's own `zigzag/manifold.hpp`-style includes noted above):

| File                                                                                        | Lines                                                                                                                                                                                              |
| ------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tools/xudu-multimodal-demo.cpp`                                                            | 42-43 (`xudu/beams.hpp`, `xudu/session.hpp`)                                                                                                                                                       |
| `tests/xudu/satelloid_physics_test.cpp`                                                     | 17-18                                                                                                                                                                                              |
| `tests/xudu/tension_layout_test.cpp`                                                        | 15                                                                                                                                                                                                 |
| `tests/zigzag/world_presentation_test.cpp`                                                  | 2-7 (4 `xudu/...`, 2 `zigzag/...`)                                                                                                                                                                 |
| `tests/zigzag/pouch_drawer_ink_test.cpp`                                                    | 2                                                                                                                                                                                                  |
| `tests/zigzag/system_ui_config_test.cpp`                                                    | 11                                                                                                                                                                                                 |
| `tests/zigzag/overview_overlay_test.cpp`                                                    | 5                                                                                                                                                                                                  |
| `tests/zigzag/page_break_overlay_test.cpp`                                                  | 8-9                                                                                                                                                                                                |
| `tests/zigzag/clasp_link_forge_overlay_test.cpp`                                            | 5                                                                                                                                                                                                  |
| `tests/zigzag/link_panel_overlay_test.cpp`                                                  | 17                                                                                                                                                                                                 |
| `tests/zigzag/swarm_telescope_overlay_test.cpp`                                             | 10                                                                                                                                                                                                 |
| `tests/zigzag/transcopyright_overlay_test.cpp`                                              | 2                                                                                                                                                                                                  |
| `tests/zigzag/collaborator_nameplate_test.cpp`                                              | 1                                                                                                                                                                                                  |
| `tests/zigzag/pouch_drawer_overlay_test.cpp`                                                | 5                                                                                                                                                                                                  |
| `tests/zigzag/test_unified_transclusion_engine.cpp`                                         | 24                                                                                                                                                                                                 |
| `tests/zigzag/test_visualizer.cpp`                                                          | 13                                                                                                                                                                                                 |
| `apps/xuzz/view_coordinator.hpp`                                                            | 16-18 (`xudu/bridge_coordinator.hpp`, `xudu/views.hpp`, `zigzag/zigzag_visualizer.hpp`)                                                                                                            |
| `apps/xuzz/cli.cpp`                                                                         | 13                                                                                                                                                                                                 |
| `apps/xuzz/xuzz_app.cpp`                                                                    | 57-71 (15 lines, every `xudu/*` plus `zigzag/zigzag_commands.hpp`)                                                                                                                                 |
| within `apps/xudu/*` itself (become same-dir, unprefixed, once co-located under `xanadoc/`) | `batch_orchestrator.hpp:20`, `link_panel_overlay.hpp:39-40`, `beams.hpp:51`, `beams.cpp:33-35`, `link_context.hpp:31`, `satelloid.cpp:6,20`, `views.hpp:37-40`, `views.cpp:46-51`, `session.cpp:1` |
| within `apps/zigzag/*` itself (same-dir once co-located under `slice/`)                     | `zigzag_visualizer.hpp:18`, `zigzag_commands.cpp:16`                                                                                                                                               |

`tools/xudu-multimodal-demo.cpp` is not wired into the Makefile under any `all`/`test` target
(checked with `grep -n xudu-multimodal-demo Makefile` — no match), so it is dead to CI either way,
but it should still be moved in the same commit so the tree keeps compiling if anyone builds it by
hand.

### 1.4 The Makefile: exact lines that must change

Source/object lists (`Makefile:600-630`):

- `Makefile:606` `XUDU_SRCS := $(shell find apps/xudu -maxdepth 1 -name '*.cpp' ...)` and
  `Makefile:607` `ZIGZAG_SRCS := $(shell find apps/zigzag -name '*.cpp' ...)` become empty and
  should be **deleted**, not left dangling — `COMMON_UI_SRCS` (`Makefile:604`, already a recursive
  `find apps/common/ui -name '*.cpp'`) picks up everything under the new `xanadoc/` and `slice/`
  subdirectories automatically, with **no Makefile edit needed** for the glob itself.
- `Makefile:623` `XUDU_OBJS` and `Makefile:624` `ZIGZAG_OBJS` likewise become empty/dead and should
  be deleted along with every reference to them (next bullet).
- Every place `$(XUDU_OBJS)`/`$(ZIGZAG_OBJS)` appears must drop the now-dead variable:
  `Makefile:702-706` (`ALL_OBJS`), `Makefile:841-843` (the `xuzz` link line — becomes
  `$(XUZZ_OBJS) $(XUDU_CORE_OBJS) $(COMMON_UI_OBJS) $(LIBLINK)` /
  `$(APP_LDFLAGS) $(LIBS) $(XUDU_LIBS)`), `Makefile:902-903` (the `zigzag_test` link line — becomes
  `$(ZIGZAG_TEST_OBJS) $(ZIGZAG_SHARED_CORE_OBJS) $(COMMON_UI_OBJS) $(LIBLINK)` /
  `$(APP_LDFLAGS) $(LIBS) $(ZIGZAG_LIBS) $(TEST_LIBS)`).
- `Makefile:846` `ZIGZAG_SHARED_CORE_OBJS := $(COMMON_XANADU_OBJS)` is unaffected (it was never
  `apps/zigzag`-sourced) and can stay as-is, though its name becomes slightly misleading once there
  is no more `apps/zigzag/`; a rename is optional polish, not required.
- `XUDU_TEST_SRCS`/`ZIGZAG_TEST_SRCS` (`Makefile:610-611`) are **not** touched by step 1 — the
  spec's own step 1 only empties `apps/xudu/`/`apps/zigzag/` (application code);
  `tests/xudu/`/`tests/zigzag/` keep their current names per VU6 (`design/view-system.md:3188-3189`,
  "settled by: the owner").

Targets that reference the now-empty variables indirectly and need no edit (confirmed by reading,
not assuming): `Makefile:740` (`all:` target-name list — `xudu`/`zigzag` are still Make-level
symlink targets, `Makefile:835-838,849-852`, untouched by the source move), `Makefile:1281`
(`SCAN_BUILD_TARGETS ?= lib gleditor xudu zigzag` — also target names), `Makefile:1212-1213`
(`CXX_FORMAT_FILES`/`SH_FORMAT_FILES`, driven by `git ls-files`, path-agnostic), `Makefile:1582`
(`compile_commands.json: $(JFILES)`, derived from `ALL_OBJS`, auto-adapts once the dead variables
are removed from it).

Fuzz and tool targets that link `XUDU_CORE_OBJS` and would have broken had
`unified_transclusion_engine` landed there as literally specified (now moot under the corrected
table, but worth knowing *why* they are not accidentally safe): `Makefile:894-899` (`xudu_test`,
`xuzz_test`), `Makefile:915,921` (`FUZZ_CORE_OBJS`), `Makefile:934` (fuzz link line),
`Makefile:952-953` (`xudu-swarm-peer`), `Makefile:969-990` (`vqueryc`, `vquery`, `vprolog`, `vplc`,
`vpl`). None of these need editing under the corrected destination table — they keep linking exactly
`$(XUDU_CORE_OBJS)` = `$(COMMON_XANADU_OBJS)`, unchanged in membership.

`Makefile:1009-1020` (`xudu-dump`) is untouched either way: it already names individual
`apps/common/xanadu/*.o` objects explicitly and never referenced `apps/xudu`/`apps/zigzag`.

Install/clean/dist (`Makefile:1427-1452,1508-1523,1531-1540`): all keyed to the `xudu`/`zigzag`
**binary symlink names**, not source paths — no change needed.

### 1.5 Non-Makefile files that name these paths

- **`packaging/wasm/build.sh:68-69`** mirrors the Makefile's dead pattern exactly —
  `mapfile -t XUDU_SRCS < <(find apps/xudu -maxdepth 1 -name '*.cpp')` /
  `mapfile -t ZIGZAG_SRCS < <(find apps/zigzag -name '*.cpp')` — and must be edited the same way
  (fold into a single `find apps/common/ui -name '*.cpp'` glob alongside the existing
  `COMMON_XANADU_SRCS` glob at line 67). **While editing this, note a pre-existing, unrelated bug
  worth surfacing to the user:** this script's `em++` invocation (`packaging/wasm/build.sh:70-75`)
  never compiles `apps/common/ui/*.cpp` at all — `COMMON_UI_SRCS` has no counterpart here — even
  though `apps/xudu/views.hpp:30` already depends on `"common/ui/hypertime_graph.hpp"`. The script
  also only ever produces `gleditor.html`/`xuzz.html` (`packaging/wasm/build.sh:61-75,118-127`), yet
  `.github/workflows/packaging.yml:717-718` unconditionally asserts `dist/wasm/zigzag.html` and
  `dist/wasm/zigzag.wasm` exist. `git log --oneline -- packaging/wasm/build.sh` shows `build.sh` was
  last touched by `b48bf09` ("fold xudu and zigzag into unified sovereign xuzz application"), which
  collapsed the separate outputs into `xuzz.html` without updating `packaging.yml`'s artifact check
  — the `wasm` job on `.github/workflows/packaging.yml` is very likely red on `main` right now,
  independent of this migration. Fixing it is not required for step 1, but the same commit that
  rewrites this script's source globs is the natural place to also delete the two stale `test -f`
  lines, and the user should be told either way.
- `packaging/xudu.1` (the man page) references `$XDG_DATA_HOME/xudu/permascroll` and the `xudu`
  binary name, not source paths — unaffected.
- `packaging/{fedora,arch,nix,windows,macos}/*` all invoke the Make **target** `xudu` (e.g.
  `packaging/fedora/gleditor.spec:82`, `packaging/arch/PKGBUILD:37`,
  `packaging/nix/gleditor.nix:101,118`, `packaging/windows/build-msys2.sh:41,47`,
  `packaging/macos/gleditor.rb:114`) or the installed binary — none reference
  `apps/xudu`/`apps/zigzag` source paths. No edits needed.
- `packaging/android/*` does not build `xudu`/`zigzag`/`xuzz` at all yet
  (`packaging/android/README.md:3,95`: "not yet `xudu`, which additionally needs
  libtorrent-rasterbar..."). Not affected.
- `debian/` has zero references to `xudu`/`zigzag` paths (confirmed by `grep -rln`); it presumably
  builds via the top-level Make targets. Not affected.
- `.clangd`/`.clang-tidy`: **no directory path filters reference `apps/xudu` or `apps/zigzag`** —
  `TIDY_FILES`/`TIDY_HEADER_FILTER` (`Makefile:1277,1280`) are generic `(src|apps)/` regexes. The
  one hit in `.clang-tidy:77` is an unrelated code-identifier comment (`zigzag::walkRank`). No
  config-file edits needed here, contrary to what one might assume going in.
- `tools/check-ui-text-policy.py`: **already scans recursively** (`main()` walks `include`, `src`,
  `apps` with `rglob("*")`, `tools/check-ui-text-policy.py:98-100`) and already classifies anything
  whose path contains `/ui/` as UI (`tools/check-ui-text-policy.py:63-65`). So
  `apps/common/ui/xanadoc/` and `apps/common/ui/slice/` are picked up with **no script edit**,
  contradicting `design/view-system.md:2860`'s claim that this file "is added" for
  `apps/common/ui/view/` — it already covers any `/ui/` path. One real consequence: files that move
  under `/ui/` and were previously *not* matched by the explicit `UI_NAMES` allowlist
  (`tools/check-ui-text-policy.py:56-60`) — e.g. `session.cpp`, `batch_orchestrator.cpp` — will
  start being checked for literal `"Sans N"` font strings (`FONT` regex, line 15) that they were not
  checked for before. Checked: `grep -rn '"Sans' apps/xudu/ apps/zigzag/` found nothing, so no new
  failures are expected, but it is cheap insurance to rerun this script over the moved files in the
  same commit.
- `tools/code-quality-audit.py` is **not wired into the Makefile or CI** (confirmed: no hits for
  `code-quality-audit` in `Makefile` or `.github/workflows/`), so it is not a gate, but its core
  checks become dead code after step 1: `zigzag_dir`/`xudu_dir` existence checks
  (`tools/code-quality-audit.py:44-63`) walk directories that will be empty, so the "Decoupling
  Violation" report silently stops finding anything — exactly the invariant this migration dissolves
  (there is no more `apps/xudu` vs `apps/zigzag` to decouple; the new concern, `apps/common/xanadu`
  must not depend on `apps/common/ui`, is not checked by this script at all). It is also already
  partially stale independent of this migration — `tools/code-quality-audit.py:81` checks
  `assets/zigzag/*.yaml` for "system slices," but `AGENTS.md` says "There is no `assets/zigzag/` any
  more: the YAML slice format is deleted and every slice is a store." Recommend updating or retiring
  this script as part of (or shortly after) step 1, and pointing its decoupling check at the real
  new boundary (`apps/common/xanadu/**` must not `#include` anything under `apps/common/ui/`) —
  which is precisely the check that would have caught §1.1's two mis-placements automatically.
- `.github/workflows/c-cpp.yml` references `xudu_test`/`zigzag_test` only as **binary/target names**
  (`:202,207-208,238,244`) — unaffected by step 1 itself. They become relevant only if/when VU6's
  rename happens (§2 below).

### 1.6 Recipe and verification checklist

**One commit, not several**, for step 1 itself: it is a pure move (git mv + sed on include spellings
\+ the Makefile/packaging-script source-list edits above), and the design explicitly calls it
behaviour-preserving (`design/view-system.md:2877`: "Moves only: no behaviour changes"). Splitting a
single mechanical rename across multiple commits only creates intermediate states where the tree
does not build (e.g. half the `#include "xudu/..."` spellings fixed) with no compensating benefit —
nothing in step 1 is independently useful or revertable on its own finer grain. (Steps 2 onward are
a different matter — see §4 for why those *should* be separate commits.)

Suggested mechanical order within that one commit:

1. `git mv apps/xudu/<each file> apps/common/ui/xanadoc/<file>`,
   `git mv apps/zigzag/<each file> apps/common/ui/slice/<file>` (preserves history per file;
   `git mv` directory-at-a-time is fine too since there are no destination-name collisions —
   verified: `comm -12` on sorted basenames of `apps/xudu`+`apps/zigzag` vs. the existing top-level
   files of `apps/common/ui`+`apps/common/xanadu` returned nothing).
1. `sed -i 's#"xudu/#"common/ui/xanadoc/#g; s#"zigzag/#"common/ui/slice/#g'` scoped to exactly the
   file list in §1.3 (not a tree-wide sed — the engine's own `"zigzag/manifold.hpp"`-style includes
   inside `apps/common/xanadu/` must not match; scoping by explicit file list sidesteps that rather
   than relying on a clever regex).
1. Within the moved files themselves, same-directory quoted includes need no change (they were
   already relative); only cross-references between what *was* `apps/xudu/` and what *was*
   `apps/zigzag/` (now both siblings under different `apps/common/ui/` subdirectories) need a path
   segment added, e.g. `apps/zigzag/zigzag_visualizer.hpp:18`'s
   `#include "zigzag/unified_transclusion_engine.hpp"` resolves same-directory already once both are
   under `apps/common/ui/slice/`, so it needs **no** change either — double-check each
   cross-reference resolves rather than assuming the sed above covers it.
1. Edit `Makefile:603-630,702-706,841-843,902-903` and `packaging/wasm/build.sh:67-75` per §1.4-1.5.
1. `find apps/xudu apps/zigzag -type f` must return nothing (catches a file accidentally left behind
   — `find`-based globs do not error on a stray leftover, they silently keep including it, so this
   is a real failure mode worth an explicit check rather than trusting a clean `git status`).
1. `rmdir apps/xudu apps/zigzag` (or let `git mv` empty them and remove afterward).

Verification checklist, cheapest-to-dearest, matching the Makefile's own cost structure (§4 has the
reasoning for why this order):

1. `find apps/xudu apps/zigzag -type f` — empty.
1. `make -j$(nproc) all` — every target in `Makefile:740` must still **link**, which is the real
   test of §1.1's correction (a wrong placement fails here, not at `make lint`).
1. `make test` — all four gtest binaries plus the netns swarm suite (needs `veth`;
   `sudo modprobe veth` first per `AGENTS.md`).
1. `make format-check lint` — every moved file gets a fresh path; `.mdl`/`clang-format`/shellcheck
   (for `build.sh`) all re-run over it. Needs `clang-format-19` specifically, not whatever
   `clang-format` resolves to by default (see the user's own memory note; this machine has 22
   installed, the tree pins 19 — `AGENTS.md`'s "Code style" section is explicit that
   `format`/`format-check` "refuse anything older by name," and a newer major is not interchangeable
   either given known indent-width disagreements between majors).
1. `xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh` — the actual proof a
   UI-touching file still draws; a silently-broken include chain in a moved overlay would otherwise
   exit 0.
1. Packaging dry run of just the one script that changed: `packaging/wasm/build.sh` under emscripten
   (or at minimum, a read of its new em++ invocation to confirm `apps/common/ui` is now included,
   which it was not before — see §1.5).

______________________________________________________________________

## 2. Where the new code and tests plug in

### 2.1 `apps/common/xanadu/view/**` and `apps/common/ui/view/**`

Both `COMMON_XANADU_SRCS := $(shell find apps/common/xanadu -name '*.cpp' ...)` (`Makefile:603`) and
`COMMON_UI_SRCS := $(shell find apps/common/ui -name '*.cpp' ...)` (`Makefile:604`) are already
unbounded recursive globs with no `-maxdepth`. A brand-new `apps/common/xanadu/view/` or
`apps/common/xanadu/view/slice/` subdirectory (per the package map at
`design/view-system.md:384-411`) is picked up automatically, with zero Makefile edits, and flows
into `XUDU_CORE_OBJS` (`Makefile:605,622`) — which is exactly what `xuzz` (`Makefile:841-843`),
`xuzz_test` (`Makefile:897-899`), and the four language tools plus their compilers
(`vquery`/`vpl`/`vprolog`/`vqueryc`/`vplc`, `Makefile:967-990`) already link. Likewise
`apps/common/ui/view/` lands in `COMMON_UI_OBJS` and is linked by `xuzz` and (today) `zigzag_test`
(`Makefile:841-843,902-903`).

The one thing the Makefile's glob cannot enforce is the dependency direction §5.3 of the spec
(`design/view-system.md:433-446`) requires: that `apps/common/xanadu/view/` never pull in a device
header. §1.1 above is a live demonstration of how that discipline is lost by hand (twice, in the
very same document that states the rule). The migration should add a cheap, mechanical guard for
this now that `tools/code-quality-audit.py` is already being revisited (§1.5): a grep-based check —
"no file under `apps/common/xanadu/` may `#include` any of `gleditor/doc.hpp`,
`gleditor/canvas.hpp`, `gleditor/renderer.hpp`, `gleditor/glyphcache/*`, `gleditor/render/*` (except
the explicitly allowed `draw_budget.hpp`/`spatial.hpp`), `gleditor/text/layout.hpp`,
`gleditor/text/shaping_cache.hpp`, `gleditor/svg_cache.hpp`, `gleditor/image_cache.hpp`" — wired
into `make lint`, so a future accidental re-introduction of this exact mistake fails loudly instead
of only failing to link ten targets at once.

### 2.2 A new test binary, or rename `zigzag_test` (VU6)

`design/view-system.md:2822-2833` already collapses "presenter, host, chrome, commands" into the
*existing* `zigzag_test` binary ("the binary that links `apps/common/ui/` and the library
(`zigzag_test` today)"), alongside the surviving overlay/visualizer tests. Nothing in the testing
table or the migration steps implies the new presentation tests need library dependencies the
current overlay tests lack (both need the full `apps/common/ui/` + `libgleditor` link set).
Introducing a *second* UI-linked binary would duplicate that already-expensive link
(`Makefile:902-903`: links `$(LIBLINK)`, `$(ZIGZAG_LIBS)` — the heaviest dependency set in the tree,
per `Makefile:537`) for no functional separation. **Recommendation: rename in place rather than add
a binary** — e.g. to something like `ui_test`, since after step 1 it genuinely is "the binary that
links `apps/common/ui/` and the library," independent of either legacy app name.

Cost of the rename, concretely, so it can be budgeted: `Makefile:611`
(`ZIGZAG_TEST_SRCS := $(shell find tests/zigzag ...)` → new dir name), `Makefile:628`
(`ZIGZAG_TEST_OBJS`), `Makefile:740` (`all:` list), `Makefile:878` (`.PHONY` line),
`Makefile:901-903` (target + link line), `Makefile:1107,1109-1111` (the `test:` target's run lines),
plus a `git mv tests/zigzag tests/<new-name>`. In CI, `.github/workflows/c-cpp.yml:202,207-208` (the
three explicit binary-name mentions in the C++26 fallback step). None of this is required by step 1
— VU6 explicitly defers it to the owner (`design/view-system.md:3188-3189`) — so it should be its
own small commit, timed for whenever step 6 (presenter/host, the first step that actually adds code
to this binary) lands, not bundled into step 1's pure move.

### 2.3 `tests/lib/`

`LIB_TEST_SRCS := $(shell find tests/lib -name '*.cpp' ...)` (`Makefile:609`) — same unbounded glob,
automatic pickup, nothing to wire up for the library-side tests of step 2 (unprojection,
`insideFrustum`, `PlaneSet`, page matrices, `PaneTree`).

______________________________________________________________________

## 3. Settings, keymap and fixtures

### 3.1 Adding a setting

`defaultSettingSpecs(SystemDocKind)` (`apps/common/xanadu/system_docs.cpp:524`) returns a
`std::vector<SettingSpec>`; `ensureAllSettings()` (`apps/common/xanadu/system_docs.cpp:2080-2093`)
walks it and calls `ensureSetting()` per entry to mint the Structure operations. Adding a new entry
to the per-kind list in `defaultSettingSpecs()` is the whole of "seeding" it — no other registration
point exists. This matches `design/view-system.md:136`'s citation of the `SettingSpec` pattern as
something to reuse rather than invent.

### 3.2 Does a new setting touch `tests/samples/`

No. `tools/create-sample-xanadocs.sh` (invoked by `make sample-xanadocs`, `Makefile:1077-1078`) only
ever writes `tests/samples/xudu/{core_hypertext,multimedia,beams,permascroll}` and
`tests/samples/xuzz/slice_then_xanadoc`, driven by explicit CLI arguments to `./build/xudu`
(confirmed: `grep -n "system\|keymap\|settings\|XDG_CONFIG_HOME" tools/create-sample-xanadocs.sh`
has zero hits). System xanadoc initialization happens under `$XDG_CONFIG_HOME` (`AGENTS.md`'s
"Everything runs headless" section), which `make`'s test/sample targets redirect into
`build/xdg/config` — outside the repo, never captured into a fixture. The only thing that
invalidates `tests/samples/` is a `CompactOpNode` layout change (`AGENTS.md`'s "Binary fixtures"
section), which a setting addition is not. This is verified mechanically (script scope + the XDG
redirect), not by actually running the generator; a one-time `git status` diff after running
`make sample-xanadocs` once a new setting lands is cheap insurance before trusting it blindly on a
real PR.

### 3.3 Tests that pin settings or chords

No test in the tree pins an **exact count** of settings or chords (checked: no
`EXPECT_EQ(<size>, <setting/chord-related symbol>)` pattern anywhere in
`tests/xudu/system_docs_test.cpp` or elsewhere). The tests that do constrain the *shape* of the
keymap, and so are the real gate a new default chord must pass, all iterate `defaultSettingSpecs()`
rather than hard-coding a list, and so scale automatically with a new entry:

- `SystemDocsTest.DefaultKeymapGivesEachChordOneActionPerScope`
  (`tests/xudu/system_docs_test.cpp:113-128`) — builds a `(scope, normalised chord)` → action map
  over every keymap spec and fails on any collision. **This is the keymap conflict detection** the
  task asks about: it is a build-time unit test, not a runtime UI feature — a colliding default
  chord is caught by `make test`, not by the keymap editor. It also asserts `spec.schemas` is
  non-empty per entry (`:119`), so a new chord's `SettingSpec` must carry a validation schema or
  this test fails.
- `SystemDocsTest.SchemaAndNotesNonEmptyAndNoMarkdown` (`tests/xudu/system_docs_test.cpp:513-525`)
  and the bidirectional-link assertion at `:567-573` check the per-**document-kind**
  Schema&Purpose/Notes pages (`architectural_governance.md`'s required supplemental metadata), not
  per-setting — adding one setting without updating that doc-kind's schema prose will not fail this
  test; it is a governance expectation, not a mechanically-enforced one. If a new view/binding
  setting is added, the schema page's prose should be hand-updated in the same commit as a matter of
  the rule's spirit, but nothing currently checks that it was.

### 3.4 Keymap conflict detection, restated plainly

It is the one test above: build every `(scope, chord)` pair across `defaultSettingSpecs(Keymap)` and
assert the map is injective. Any new default chord `view_commands` registers (per
`design/view-system.md:2936-2937`) goes through the same list and the same test with no extra
wiring.

______________________________________________________________________

## 4. Gates and CI

### 4.1 What CI actually runs

`.github/workflows/c-cpp.yml` jobs: `format` (packages, `make format-check`, `make lint` — `:16-48`,
no build deps per the `NO_SDL_GOALS` exemption in `AGENTS.md`'s Makefile-gotchas section), `nix`
(`nixfmt`/`nix-linter`, `:50-69`), `build` (matrix `sdl: [2, 3]`, fail-fast off, `:70-220` —
installs deps, `make -j$(nproc) GLEDITOR_SDL=N GLEDITOR_ENABLE_VULKAN=1`, then `make ... test/all`,
then (SDL2 only) the C++26-fallback rebuild+rerun of `gleditor_test`/`xudu_test`/`zigzag_test`, then
`xvfb-run ... ./tools/compare-backends.sh`), `swarm` (builds only `xudu_test xudu-swarm-peer`, then
`sudo tools/swarm-netns-test.sh`, `:222-250`). No job in this file sets `timeout-minutes` (confirmed
by grep), so each uses GitHub's default ceiling, not a project-chosen one.
`.github/workflows/packaging.yml` separately builds Debian/Fedora/Arch/Nix/macOS/ Windows/WASM
packages and smoke-tests the installed binary on each.

### 4.2 What dominates wall-clock (reasoned estimate, not measured — building was out of scope)

By file count, `tests/xudu/` (113 files) and `apps/common/xanadu/` (101 files) are the largest
translation-unit sets in the tree; by byte size, the heaviest single units are
`apps/common/xanadu/system_docs.cpp` (172 KB), `apps/zigzag/zigzag_visualizer.cpp` (158 KB),
`apps/common/xanadu/store.cpp` (154 KB), `apps/common/xanadu/vortex/vortex_stdlib.cpp` (150 KB),
`apps/xuzz/xuzz_app.cpp` (127 KB), `apps/xudu/session.cpp` (113 KB),
`tests/xudu/e2e_binary_orchestration_test.cpp` (116 KB, spawns `build/xudu` as a real subprocess per
`AGENTS.md`'s testing notes), `apps/xudu/beams.cpp` (93 KB), `apps/xudu/views.cpp` (80 KB). Step 1
relocates these files but does not shrink them, so compile cost is unchanged by the move itself;
steps 6-8 (deleting `ZigzagVisualizer`, slimming `LinkBeams`) are what actually reduce it. The
`build` job's cold, two-way matrix (SDL2 and SDL3, each a from-scratch `make -j$(nproc)` over
roughly 400 `.cpp` files across `src/`+`apps/`+`tests/`) is structurally the most expensive thing in
`c-cpp.yml`; `swarm-netns-test.sh` adds real subprocess/network-namespace setup cost on top of the
four gtest binaries (`AGENTS.md` flags this as the thing most likely to fail for environment reasons
— missing `veth` — rather than time); `compare-backends.sh` under `llvmpipe` renders the sample
through every compiled-in backend and diffs PNGs, adding GPU-adjacent wall-clock the unit tests
don't carry.

### 4.3 Does `make format-check` currently pass on `main`

Not run (out of scope). Circumstantial evidence from history: the most recent
C++-formatting-specific commit is `2b92584` ("Normalize C++ and shader formatting with clang-format
19"), and the ~15 commits since are all `design/view-system.md` edits (prose only, gated by
`mdformat`/`mdl` rather than `clang-format`) — i.e. no evidence of repeated format-fix churn that
would suggest the gate is currently broken. The one concrete friction point on *this machine*
specifically: the user's own memory notes that the installed `clang-format` here is v22 while the
tree pins 19 ("use a venv copy for format gates") — consistent with `AGENTS.md`'s statement that
`format`/`format-check` "prefer `clang-format-19` and refuse anything older **by name**" (nothing
stops a *newer* unpinned binary from being picked up by a bare `clang-format` on `$PATH`, which is
the actual hazard this note is about). Separately, and with higher confidence (this one *was*
checked): the `wasm` packaging job is very likely broken right now, independent of this migration —
see §1.5's `zigzag.html`/`b48bf09` finding.

### 4.4 Recommended per-step gate for the multi-commit migration

Scaled to each step's actual blast radius, using the Makefile's own cheap/expensive split
(engine-only targets vs. device-linked ones):

| Step                                        | What changed                                                      | Minimum gate                                                                                                                                                                                                                                                    |
| ------------------------------------------- | ----------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 (relocation)                              | moves only                                                        | full checklist in §1.6 (`make all`, `make test`, `make format-check lint`, `compare-backends.sh`, the empty-directory check) — this step touches both layers and every packaging script that globs by path, so it earns the expensive gate once                 |
| 2 (library world-space work)                | `src/`, `include/gleditor/` only                                  | `make -j$(nproc) lib gleditor_test` + the new `tests/lib/` cases + relevant `compare-backends.sh` scenes; no need to rebuild `xuzz`/`zigzag_test` for every iteration                                                                                           |
| 3-5 (view space, binding, slice/page views) | new files under `apps/common/xanadu/view/`, no application wiring | `make -j$(nproc) lib xuzz_test` only — these are explicitly "no application change" (`design/view-system.md:2906-2924`) and link none of the device layer; this is the cheapest possible loop in the whole migration and should be exploited for fast iteration |
| 6 (presenter/host)                          | first `apps/common/ui/view/` code, `xuzz_app` rewired             | full `make -j$(nproc) all test`, `compare-backends.sh`, plus one journey smoke test — this is where a legacy-placement wrapping mistake would show up, and it is the first step since 1 that rebuilds the expensive device-linked targets                       |
| 7 (commands/keymap)                         | `view_commands`, binding points                                   | full suite + `SystemDocsTest.DefaultKeymapGivesEachChordOneActionPerScope` explicitly called out (new chords land here)                                                                                                                                         |
| 8 (replace what was wrapped)                | deletes `ZigzagVisualizer`, slims `LinkBeams`                     | full suite + packaging dry runs (this is the highest-risk step — code deletion, not addition) + a size/diff sanity check that nothing load-bearing was deleted silently                                                                                         |
| 9 (journeys/goldens)                        | golden layouts, `tests/samples/view/` (new dir), probes           | the new goldens themselves, plus the `xuzz-ux-validation` skill's journeys, which the task's `design/ux_workflow_real_work.md` already names as the acceptance bar                                                                                              |

Each step is already stated to be independently committable and green (`design/view-system.md:2872`:
"Each step builds and keeps `make test` green, and each can be committed alone"); the table above is
only about *how much* of the gate is worth re-running at each step's price point, not about whether
to commit separately (the spec already says yes).

______________________________________________________________________

## 5. Feature flags and legacy-placement coexistence

The codebase already has the exact precedent the spec wants for "legacy placement beside the new
views": `ZigzagVisualizer::ViewMode` (`apps/zigzag/zigzag_visualizer.hpp:209-218`,
`CellContent`/`Topology`) is a plain runtime enum switched by `setViewMode()`/`toggleViewMode()` — a
method call, not a compile-time `#ifdef`, bound to a key through the ordinary keymap/command path
(`zigzag_commands.cpp` registers the action; the keymap names the chord). This is precisely the
"view kind in the registry, switchable through the sovereign keymap" shape the architectural
governance rule already requires for every other UI action
(`.claude/rules/architectural_governance.md` §4, "all key bindings ... must be defined in
`system://keymap`").

**Recommendation:** give the legacy `ZigzagVisualizer` placement and the legacy `xanadu::Views`
placement each a `PlacementKind`/view-registry entry exactly like any new, real view
(`design/view-system.md:2929-2931` already plans this: "`ZigzagVisualizer` and `xanadu::Views` are
each wrapped as a legacy placement"), switched by a `view_commands`-registered action bound through
`system://keymap`, with the *currently selected* kind persisted as an ordinary setting in
`system://layout` (via `SettingSpec`, the same pattern as everything else configurable) if the
choice should survive a restart. This needs:

- no CLI flag (keymap-bound command, following every other view action in the tree, not an
  out-of-band switch nobody else uses),
- no `#ifdef` (both placements are compiled in always; which one is active is runtime state, same as
  `ZigzagVisualizer::view_mode_` today),
- one new `ViewRegistry` entry per legacy placement plus the wrapping code step 6 already plans.

**Test coverage to keep the legacy path from rotting:** the spec's own step 6 and step 8 already say
"existing visualizer tests unchanged" (`design/view-system.md:2932,2949`) through those steps — keep
`tests/zigzag/test_visualizer.cpp` and `tests/zigzag/world_presentation_test.cpp` running unmodified
until step 8 actually deletes `ZigzagVisualizer`. Add, in step 6 or 9: one `compare-backends.sh`
scene with the legacy placement explicitly selected (so a silent rendering regression in the legacy
path is caught the same way `design/view-system.md:2861` already requires for every new view —
"`gains one scene per view`" should include the legacy one for as long as it's selectable), and one
journey in `design/ux_workflow_real_work.md` (or the `xuzz-ux-validation` skill's run against it)
that explicitly exercises switching *to* the legacy placement and back, not just confirming the new
views work. Without that, "selectable beside the new views" quietly becomes "compiles but nobody
looks at it" the moment attention moves to step 7 onward.

______________________________________________________________________

## 6. Risks and order

Ranked by how much of the tree each one can break at once, with mitigations:

1. **Highest — misplacing `unified_transclusion_engine.*` or `link_context.*` into
   `apps/common/xanadu/` exactly as `design/view-system.md:2880-2889` currently specifies.** Breaks
   **linking** (not compiling, not formatting) of `xudu_test`, `xuzz_test`, `vquery`, `vpl`,
   `vprolog`, `vqueryc`, `vplc`, `xudu-swarm-peer`, and all three fuzz targets simultaneously — ten
   targets from one wrong `git mv`. Mitigation: §1.1's corrected table (both files go to
   `apps/common/ui/`); add the grep-based device-header guard from §2.1 to `make lint` so this class
   of mistake fails fast and by name instead of as a wall of undefined-reference errors.
1. **Silent empty-glob masking.** `find`-based source lists (`Makefile:600-608`) do not error on a
   stray leftover file; a file accidentally left behind in `apps/xudu/`/`apps/zigzag/` (or dropped
   during a manual merge conflict) keeps compiling into the old location with no Makefile complaint.
   Mitigation: the explicit `find apps/xudu apps/zigzag -type f` emptiness check in §1.6's recipe,
   run as its own step rather than inferred from a clean `make all`.
1. **Pre-existing, migration-adjacent breakage that could be mistaken for a regression this
   migration caused.** The `wasm` packaging job (§1.5/§4.3) is very likely already red on `main`
   independent of this work; if nobody flags it now, whoever next touches `packaging/wasm/build.sh`
   (which step 1 must, per §1.5) will plausibly get blamed for a `zigzag.html` test failure they did
   not introduce. Mitigation: say so in the step-1 PR description, and either fix the two stale
   `test -f` lines in the same commit (cheap, since the file is already open) or file it separately
   before starting.
1. **`clang-format` version drift on the machine actually doing the work.** `AGENTS.md` pins 19 by
   name; this environment has 22 on `$PATH`. A relocation commit that touches ~20 files' worth of
   `#include` ordering is exactly the kind of change where a wrong formatter version produces a diff
   that looks plausible but fails `format-check` in CI. Mitigation: the user's own memory note (use
   a pinned venv/binary) — worth restating explicitly in whatever executes this plan, since the gate
   will not catch the mistake until CI, not locally, if the wrong binary is on `$PATH`.
1. **`tools/code-quality-audit.py` going quietly blind.** Not a CI gate today, so it cannot fail a
   build, but it is the one tool in the tree whose entire purpose is checking the invariant this
   migration changes, and it will report "no violations" for the wrong reason (nothing left to walk)
   rather than "no violations" for the right one. Mitigation: update it alongside step 1 (§1.5) so
   it starts checking the real post-migration boundary instead of going silent.
1. **Lowest — the `tests/xudu/` tests that reach into soon-to-move UI headers for a device-free
   sub-struct (§1.2).** Mechanically trivial to fix (an include path change) and already isolated by
   what the tests actually instantiate; the risk is purely that a future, unrelated edit to
   `satelloid.hpp`/`tenuous_tether.hpp` could silently turn an engine-only test into one that needs
   the device, with a confusing link error pointing at the wrong layer. Mitigation: track as a
   follow-up (extract `CellSatelloid` into `apps/common/xanadu/`), not a blocker for this migration.

**Recommended order for the build-level work:** (1) step 1's relocation exactly as corrected in §1,
landed and green per §1.6's checklist, including the `code-quality-audit.py` boundary-check update
from §2.1/§1.5 so the corrected boundary is mechanically enforced from day one rather than merely
documented; (2) steps 2 and 3-5 in parallel, as the spec already allows
(`design/view-system.md:2873`: "Step 2 is independent of steps 3 to 5 and can run beside them"),
both exploiting the cheap `lib`/`xuzz_test`-only gate from §4.4 for fast iteration since neither
touches the device layer; (3) step 6, the first step since the relocation to rebuild the expensive
device-linked targets, gated at full cost; (4) step 7, gated with explicit attention to the
keymap-collision test since this is where every new default chord actually lands; (5) the VU6 rename
(§2.2), as its own small commit, timed here (once step 6 has given the binary new content worth
naming correctly) rather than bundled into step 1; (6) step 8 (deletion), the
highest-individual-step risk, gated at full cost plus a deliberate "what got deleted and why" review
rather than trusting the test suite alone to notice a silently-dropped behaviour; (7) step 9,
closing with the journeys and goldens the earlier steps were building toward.
