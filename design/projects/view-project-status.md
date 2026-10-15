# View Project: Status

The running record of the view project that [`start-view-project.md`](start-view-project.md) starts.
Updated with every package. A session that picks the project up reads this first and continues from
**Next**.

Last updated: 2026-10-15.

## Where it stands

| Milestone                 | State                                           |
| ------------------------- | ----------------------------------------------- |
| M0, the tree in its shape | A1 and A2 done; closed                          |
| M1, spikes                | done; closed                                    |
| M2, the spine             | second gate: no-go until a second closing round |
| M3 onward                 | not started                                     |

## Done

| Package or change              | Commit                                     | Notes                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------------------ | ------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A1 moves                       | `66afa4b`                                  | Landed upstream before this project started, inside "Add private reader layers for signed link packages": every file of `apps/xudu/` to `apps/common/ui/xanadoc/` and of `apps/zigzag/` to `apps/common/ui/slice/`, include spellings and Makefile lists with them.                                                                                                                                                                                                                                                                                                                                                         |
| Build without ThorVG           | `a863fbe`                                  | `readFile` in `tests/lib/svg_cache_test.cpp` was unused, and fatal under `-Werror`, wherever ThorVG is absent.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| `compare-backends.sh` isolated | `5142e20`                                  | Writes under `build/compare-backends/`; the E2E tests take their workspace from `XUDU_WORKSPACE_DIR`. It can now run beside `make test`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| No xvfb for GL and GLES        | `0d52fa3`                                  | Offscreen SDL over llvmpipe needs no display; the Makefile exports `GALLIUM_DRIVER=llvmpipe`. Only a Vulkan capture under SDL2 still needs `xvfb-run`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| A1 remainder                   | `3e48652`                                  | See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| A2, `zigzag_test` to `ui_test` | `0270fc7`                                  | See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| M1, the spikes                 | `a1b8bf0`                                  | Results and what each changes: the plan's §3.1.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| L1, L2, L8                     | `90960b7`, `5b69a19`, `55699d7`            | Unprojection to a world ray; `insideFrustum`; `ui::PaneTree`. The pane tree's mutators return `std::expected`, and it gains `dividers()` and `resizeDivider()` (amended in the rendering plan).                                                                                                                                                                                                                                                                                                                                                                                                                             |
| E2, E1, E3, E6                 | `e5e6292`, `e2bfb3a`, `a8572c2`, `178ef87` | `ViewError` with its own `ViewMessage` keys (G8); records, `SubjectId` factories and `LayoutSink` (G13); `ViewRegistry`, refusing duplicate kinds and chords in one scope; the raster. `ViewEpoch` and `ViewAxisId` live in `view_ids.hpp`. The chord normaliser moved to `xanadu::canonicalChord()`.                                                                                                                                                                                                                                                                                                                       |
| E0, E4                         | `5d9adad`, `bff4a36`                       | The `release()` guard and `shadowCount()`; `ViewManifold` over a binding and a derived arena, the layer rule (G1), toss, counts (G3) and `verifyViewSpace`, with S4 as the first test and random sequences against a model. I6 waits for E8's cursor.                                                                                                                                                                                                                                                                                                                                                                       |
| U0                             | `282b6e9`, `74c2e05`                       | `SlicePresentation` in `apps/common/ui/view/`; `xuzz_app` and `ViewCoordinator` hold the slice through it, except `vortexHost()` and the visualizer's own command set (X0's). The drawing roles left the engine's `presentation_surface.hpp`, which now includes no library header.                                                                                                                                                                                                                                                                                                                                         |
| E14                            | `5c44044`                                  | Walk summaries kept in `system://activity` as cells and dimensions (a run cell, its ends, a cell per dimension and pair with its count), ranking by use with decay. The Markov order is deferred as §4.1 says; the pair counts it needs are recorded.                                                                                                                                                                                                                                                                                                                                                                       |
| P1, P2                         | `8cf7583`, `f866b75`                       | `PageCatalog` and the page cursor; `TensionCoalesce` per §3.1's S2 row. The engine's tie gains a side. Parity with the `docSlots` row holds when every row document is a body.                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| E5, E8                         | `cbdef9a`, `de166ff`                       | `ViewAxisSet` over the binding arena, with `d.axis-role` and `d.view-groups` added; a group resolves as a pack does. The slice cursor survives a toss (I6).                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| U1                             | `65983bd`                                  | The colour rule of §3.1 in the engine, colour maths in `<gleditor/color_space.hpp>`. Each band's open arcs are also cut where its colour comes within ΔE₀₀ 10 of a reserved colour, which the V1 rule failed for 7 of 40. The band constants are `constexpr`, not settings: the reader overrides a dimension's colour, not the rule.                                                                                                                                                                                                                                                                                        |
| P3                             | `3e228de`, `b4b6c5a`                       | The base view, with `documents` (parity) and `pages` (the default) sub-views. The tension engine no longer allocates per step.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| L5, R5, L9                     | `8a8eb1b`                                  | `PipelineDesc::depthWrite`; `TranslucentList` sorted by camera depth with beams cut at sheets (R5's scene: 0 of 480,000 pixels wrong from either side); beam width from the view ray. Off-axis head-on beams draw slightly wider, contrary to R3's "unchanged". The UI's own beams still draw after the list until their contributors adopt it.                                                                                                                                                                                                                                                                             |
| R4's test                      | `9cf6aa2`                                  | `tests/lib/doc_page_pose_test.cpp`, L7's first test, disabled until L7; forced to run, all three fail as F2 says.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| E9                             | `a71458b`, `0554121`                       | Stretch vanishing with ghosts, the two-axis rule (G5), D1 and D2, and the shared marks helper. Edge heat deferred (§4.1). Its 10⁶-cell test compares 10⁴ with 10³: minting is O(N²) (see Findings).                                                                                                                                                                                                                                                                                                                                                                                                                         |
| U6a                            | `1ca38df`                                  | `ViewAnimation`: tweens by `SubjectId`, the toss fade from a snapshot, the held-key rule as a timing rule (no repeat flag reaches commands), reduced motion, no allocation at steady state. `view.motion.*` in `system://ui`. `PlacedFrame` gains `opacity`.                                                                                                                                                                                                                                                                                                                                                                |
| Translucent list in the UI     | `8ba9428`, `9a61f9c`                       | `LinkBeams`, the visualizer, tethers and satelloids draw in `drawScene()` and hand beams and sheets to the list. Two visible changes beyond occlusion, kept: chrome draws over beams, and a visualizer beam stops at the cards it joins. Library world widgets and translucent images are not in the list yet.                                                                                                                                                                                                                                                                                                              |
| U2                             | `bf465b0`, `353d61a`, `5f63e3d`            | The second slice presentation beside the visualizer, chosen from the palette and kept across a restart; steps and home; every other action says it is not supported. Found and fixed `Views::frameForReading()` framing against the page mid-arrival, which varied run to run.                                                                                                                                                                                                                                                                                                                                              |
| U5a, U7a                       | `c26bd5e`                                  | The compass (rows, not the rose; display only), the palette with sub-views, and the commands: steps, hop, swap, sub-view next and previous, cycle, and binding by typing (`std:view/bind`, Ctrl+Alt+B). A "plain" sub-view without ghosts. Typed bindings last only the session; no binding undo yet.                                                                                                                                                                                                                                                                                                                       |
| M2 closing round, A            | `813d17e`, `c721e0b`, `66c3202`            | The gate's behaviour findings: arrival never rebinds (a typed group and undo survive a move); an unbound point stays unbound; the visualizer says it cannot bind; points by name from `layout.bindingPoints`, case-insensitive, unique; a hop around a ring stops beside its start; a self-link is not a move; sub-view kept per slice and view kind; bindings and focus kept per slice; the bind dialog lists names and survives a refusal; the status line names the view and sub-view.                                                                                                                                   |
| M2 closing round, B            | `3084743`, `28c18ab`, `e5ca53a`, `73b68a9` | The capture race (four causes: a contributor finding work mid-frame, the camera and page still settling, undrawn chrome, and a 48-pixel blank page from `Doc::layoutFrom`); the page dimmed behind a slice and the focus row clear of it; strands clipped round labels and given a minimum width; ghosts and fades drawn at all (the canvas ignored a quad's alpha); key hints from the keymap for the active presentation; the presenter's per-frame allocation; layout guards on non-finite sizes; `view.draw.*`, `view.strand.*`, `view.page.*` settings; scenes for ghosts, wide neighbours, depth and quotation beams. |

What the A1 remainder changed:

- `.github/workflows/packaging.yml` checked for `dist/wasm/zigzag.html` and `zigzag.wasm`, which
  `packaging/wasm/build.sh` has not produced since the fold into xuzz: it writes `xuzz.html`. That
  check was already failing before the move (F12) and now looks for `xuzz.*`.
- `packaging/wasm/README.md` and the header of `build.sh` said `zigzag`.
- `tools/code-quality-audit.py` checked that `apps/xudu/` and `apps/zigzag/` did not include each
  other, which says nothing once both are gone. It now fails on any file under either directory (the
  source lists are `find` globs and would compile a stray file silently) and on any include of
  `common/ui/`, `ui/`, `xuzz/` or `apps/` from `apps/common/xanadu/`. Both failures were checked
  with a planted file and a planted include.
- A Makefile comment still named `apps/xudu/core/`.

A1's gate: `find apps/xudu apps/zigzag -type f` prints nothing (neither directory exists).

What A2 changed: `tests/zigzag/` is `tests/ui/` (`git mv`, 23 files) and the binary is `ui_test`, in
the Makefile, both CI workflows' build and test steps, the swarm Dockerfile, `AGENTS.md`, the
README, the spec's §16.1 (with a change-history line) and the design notes that link to a moved test
file. The Makefile's `ZIGZAG_PKGS` and `ZIGZAG_LIBS` were a copy of `XUDU_PKGS` and `XUDU_LIBS`,
`ZIGZAG_SHARED_CORE_OBJS` was `COMMON_XANADU_OBJS` under another name, and `ZIGZAG_CORE_OBJS` was
never defined; all four are gone and `ui_test` links `XUDU_CORE_OBJS` and `XUDU_LIBS`, the same
objects and libraries as before. The `zigzag` program symlink is unchanged. Mentions of
`zigzag_test` in dated records of past runs, and one stale link to a test file that no longer exists
(`test_system_projector.cpp`), are left as they were.

A2's gate: `make` builds `build/ui_test` and no `zigzag_test`; `ui_test` passes 191 and fails 1, the
same test `zigzag_test` failed; `gleditor_test`, `xudu_test` and `xuzz_test` are as at the baseline.
`make test` stops at its first red binary, which is `gleditor_test`'s font failure, so the other
three were run directly with the same environment; the swarm tests were therefore not reached.
Format, lint and the audit are green. `compare-backends.sh` was not rerun: nothing it drives
changed.

## Next

The project moves to a local machine; [`resume-view-project.md`](resume-view-project.md) is the
prompt that resumes it.

1. **M2's second closing round.** The second gate (2026-10-15) found no blocker and confirmed most
   of the first round fixed, but not all; the owner's rule is few things fully done.
   - Behaviour:
     - Focus is not restored on reopen. `SlicePresentationSwitch::show()`
       (`slice_presentation_switch.cpp:122`) pushes the visualizer's focus (home) over the kept one,
       and `persistFocus()` appends about three operations to `system://layout` per rested step that
       nothing reads, so the store grows without bound. Keep one value per slice, set in place;
       restore it; flush on exit; test move, close, reopen. Decide whether a reader's focus belongs
       in `system://layout` at all or in the activity store (R8).
     - Choosing a view's own palette row after a sub-view leaves the sub-view
       (`presentation_palette.cpp:175`); the announcement after it is stale.
     - A swap leaves the previous notice up; a notice hides the view and sub-view line; the hints
       bar is not in the accessibility tree; the bind dialog's example names a dimension the slice
       may not have; "posward" and "negward" in messages.
   - Drawing:
     - Ghosts too faint at opacity 0.09 and square-cornered (§5.1.1, §5.5 #3).
     - Labels overlapped: the focus box over the depth neighbour, the wide neighbour over its
       neighbour, the compass over text, the focus box over a strand label.
     - Depth invisible: the d.deep scene differs from the ghosts scene only in the status line.
     - Strands and ghosts run past the viewport edge (a hairline and a tick at the right).
     - The page is full white behind the slice in the default unified mode
       (`view_coordinator.cpp:86`), and PageDim is undone by the onion-skin toggle and
       `arrangeAlongside` (`views.cpp:2077`, `:2095`); `faded()` assumes a black ground.
     - The hints bar clips "Back to text" with no ellipsis; an empty "Walked:" line.
     - No capture shows a magenta xanalink ribbon beside dimension strands, and the gold beams sit
       behind the inspector panel; adding a slice to `unified_store` is refused ("context operation
       5 is in the future relative to parent 4"), which may be an engine defect.
   - Guards: a hand-edited dash pattern of tiny lengths hangs the presenter (no lower bound or cap);
     `stretch.maxCellSpan` up to 1e5 lets a huge measure hang layout.
   - Then a short re-check: the red team on these items and the frame inspector on the frames.
1. Then M3 (packs), one milestone at a time.

The spike branches were never meant to land and stay local to the first machine
(`worktree-agent-a1c8ca54…` V1–V3, `a2363aec…` V1b, `a418eeb9…` S1/S3/S5, `a6c0b4e5…` S2,
`a97312af…` R1/R2/R5, `a52dc9e4…` the R3 orbit harness); their results are in the plan's §3.1.

## Fixed along the way

| Commit    | What                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `61c5b95` | The store panel budgeted its button rows with the exact padding, but the layout rounds box edges to whole pixels; at a fractional padding a row came out a pixel too wide and its last button was laid out zero pixels tall. Not a font problem: it failed for every family. Fixed `StoreObjectManagerOverlayTest.FittedRows…` and `E2EBinaryOrchestrationTest.storePanelCreates…`.                                                                                 |
| `975a560` | Keymap actions only the slice presentation knows were registered unscoped, after the start-up pass that scopes built-in commands, so `std:zigzag/save_store` lost Ctrl+Shift+S to the global `std:xudu/publish` even in the ZigZag pane: the chord published. The "same key" warning was true. New test `E2EBinaryOrchestrationTest.aPaneChordIsNotTakenByTheGlobalOneOnItsKey`, failing before the fix.                                                            |
| `33488c8` | Font descriptions ("Monospace 16", "Sans Bold 12") went to Fontconfig whole, which reads them as one non-existent family and falls back to the default face: no role got its face, anywhere. Parsed as Pango does now; the glyph cache's bold and italic variants open at the right size and are keyed by style. Fixed `TextLayoutTest.InlineBoxAdvances…` and `E2EBinaryOrchestrationTest.textSurvives…`. Documents now render in a true monospace face.           |
| `f5d7218` | `rebuild()` walked the whole ancestry to find the active xanadoc in a store with none, so a checkpointed Chronofilade rebuild cost O(K) again (5 us at 500 operations against the documented 0.29). Now a per-operation lookup: 0.23 us. Fixed `ChronofiladeBenchmarkTest.ScalabilityAndSpeedup`.                                                                                                                                                                   |
| `0331519` | `aDraggedSelectionLandsWhereItIsDropped` dropped "into empty space" at a fixed x = 770, which a monospace page now covers. The drop point is found in the calibration frame, beside the page's right edge.                                                                                                                                                                                                                                                          |
| `f5bd52d` | The `config.h` race: only the two `main.o` files waited for the generated header; `xuzz_app.cpp` and `cli.cpp` include it too. The list of users is now read from the sources.                                                                                                                                                                                                                                                                                      |
| `edaf732` | The document view compared every open store's link spans with the document's by bare offset; system xanadocs' schema Comment links then coloured unrelated text in documents read against another permascroll. Caught by `compare-backends.sh`'s fresh data directory, hidden by `make test`'s. `spanIn()` names a span in the reader's coordinates first.                                                                                                          |
| `bb3b39b` | `log_at()` kept its logger in a function-local static, which is per instantiation of the template and so per argument types: the first category to log with a set of types took every later call with them.                                                                                                                                                                                                                                                         |
| `f43ea6a` | The "key-hint race" was not a race. System xanadocs were written against the permascroll of the session's first document, so a launch with `--permascroll` wrote the keymap's text into that scroll and every later launch without it read a keymap with no bindings: no chords, and no hint bar. System xanadocs now always use the user's own permascroll, and one that loads but cannot be resolved is moved aside like an unreadable one and a default written. |

## Baseline

Taken at `a863fbe`, each binary run alone and headless, outside `make`:

| Binary          | Passed | Failed | Skipped                                     |
| --------------- | ------ | ------ | ------------------------------------------- |
| `gleditor_test` | 738    | 1      | 3 (Vulkan resource tests; no Vulkan device) |
| `xudu_test`     | 1296   | 4      | publication-network and swarm tests         |
| `xuzz_test`     | 61     | 0      | 0                                           |
| `ui_test`       | 191    | 1      | 0 (was `zigzag_test`)                       |

`make format-check` and `make lint` are green on the base. A full `make -k test` after A2 ran every
binary and both rootless swarm runs: the swarm tests pass (12, then 4), so this container has
`veth`. Under it `xudu_test` failed 6, not 4: the two timing benchmarks marked below failed as well,
and both passed in every run of `xudu_test` on its own, so they are load-sensitive rather than
broken. `tools/compare-backends.sh`: OpenGL and GLES agree pixel for pixel, and the culling,
atlas-growth and minification checks pass; Vulkan is not available under SDL2 offscreen; the run
fails at its E2E stage on the three E2E reds below.

At `73b68a9` (the end of M2's closing round, 2026-10-15, 4 cores, llvmpipe, no Vulkan device):
`make -k test` exits 0 — `gleditor_test` 806, `xudu_test` 1319, `xuzz_test` 241, `ui_test` 257, both
swarm runs (12, 4) — and `compare-backends.sh` exits 0, every image check and E2E scenario agreeing
between OpenGL and GLES.

### Known reds, not this project's

None. After the fixes under "Fixed along the way", a full `make -k test` exits 0 (`gleditor_test`
743, `xudu_test` 1301, `xuzz_test` 61, `ui_test` 193, both swarm runs) and `compare-backends.sh`
exits 0, every image check and every E2E scenario agreeing between OpenGL and GLES.

`E2EBinaryOrchestrationTest.aSliceViewIsChosenFromThePaletteAndKept` failed once in a worker's run
and passed alone and in the next full run.
`PublicationUiTest.aSliceCanBeAddedToTheCurrentTextStoreAndReopened` failed once under four
concurrent builds (it runs `xuzz` under a 120-second timeout) and passed alone and in a whole run.
Two speedup benchmarks failed once each under a loaded `make -k test` and passed in every run alone:
`ArrayfiladeBenchmarkTest.VQLPredicatePushdownPruning` and
`VortexBenchmarkTest.MemoizedVsUnmemoizedExecution`. Watch them; neither is known to be a bug.

## Environment

What a fresh Ubuntu 24.04 container needs, beyond the submodules:

- The apt packages in `c-cpp.yml`'s build job, plus these; ThorVG is not packaged, and the build
  goes on without it:

  ```sh
  apt-get install libpoppler-cpp-dev libpoppler-dev libpoppler-private-dev libmagic-dev \
    libvlc-dev librnp-dev libsqlite3-dev libavcodec-dev libavformat-dev libavutil-dev \
    libflac++-dev libgif-dev libtiff-dev libjpeg-dev libwebp-dev libzstd-dev
  ```

- **Clang 19 with GCC 14's libstdc++** (`clang-19`, `g++-14`), built with `CXX=clang++-19`. Clang 18
  cannot use GCC 14's `<expected>`, and GCC 13's library is too old for `beman_optional`.

- **Poppler 24.02's headers** warn under `-Werror` because pkg-config hands them over with `-I`.
  Local copies of `poppler.pc` and `poppler-cpp.pc` with `-isystem`, on `PKG_CONFIG_PATH`, get round
  it without changing the tree.

- clang-format 19 from pip in `/tmp/cf19`; mdformat with its three plugins; `mdl`; yamlfmt; shfmt,
  shellcheck and yamllint.

## Findings for the owner

- **Minting cells is O(N²).** `Store::putOp` walks the parent chain back to each operation's context
  for every `makeCell`, so building 10⁶ cells does not finish in ten minutes (2·10⁴ take 2.4 s).
  E9's 10⁶ acceptance test runs at 10⁴ against 10³ until this is fixed.

- **`zigzag::OptionalCell` converts implicitly to `CellRef`**, so assigning one to a
  `std::optional<CellRef>` gives an engaged optional holding `noCell`.

- **The compiler cache lived under each checkout's `build/xdg/cache`**, because the Makefile
  redirects `XDG_CACHE_HOME`, so every worktree built cold and kept its own cache; the disk filled.
  Workers now share `CCACHE_DIR=/root/.cache/ccache-shared` (8 GB).

- **`make test` stopped at the first red binary**, so `gleditor_test`'s font failure kept the other
  three, and the swarm tests, from running. `make -k test` now runs them all (the recipe reads `-k`
  itself) and fails at the end if any did.

- **CI's build job installs neither Poppler, libmagic, libvlc nor librnp**, all of which the
  Makefile requires, so it most likely fails at pkg-config. Not checked against a CI run.

- **The `config.h` race.** `apps/xuzz/xuzz_app.cpp` includes the generated `config.h`, but only the
  two `main.o` files are ordered after it (`Makefile:781-782`), so a fresh parallel build can fail.

- **The engine includes more of the library than the governance rule allows.** The rule names
  `cpp26*`, `spatial.hpp` and `draw_budget.hpp`; the engine also includes, among others,
  `frame_contributor.hpp`, `pick_observer.hpp` and `a11y/tree.hpp`
  (`apps/common/xanadu/zigzag/presentation_surface.hpp`), `radial_menu.hpp` and `ui/theme.hpp`
  (`system_docs.hpp`) and `stepped_view.hpp` (`zigzag/cell_views.hpp`). All header-only uses; the
  link line, which is what the Makefile enforces, still holds. U0 moved `presentation_surface.hpp`'s
  drawing roles to the UI seam; the rest remain. The audit enforces only the UI and program boundary
  for now, so it does not go red on this.

## Decisions taken

- M2's second gate (2026-10-15): no-go until a second, short closing round; red team, frame
  inspector and UX validator found no blocker and most of the first round fixed.

- M2's gate (2026-10-14): no-go until the closing round. Red team, frame inspector and UX validator
  found no blocker; the closing round fixes their should-fix items rather than deferring them.

- M1 (the plan's §3.1): P2 steps to a velocity threshold with a cap, parity against the `docSlots`
  row; `PlaneSet` is one draw with a per-plane table; the glyph stage gets a "coverage is alpha"
  flag; one translucent list sorted by camera depth, beams included, before L6 and L7; U1's colour
  rule is the V1 re-run with three changes; E10 and P4 take the formula changes V2 and V3 named. The
  decorative palettes (per author, lineage, badges) are not reserved hues.

- System xanadocs live in the user's config directory, so their text lives in the user's own
  permascroll (`PermascrollRegistry::defaultUser()`), never the one a run's documents were opened
  against with `--permascroll`. An existing one that cannot be resolved is moved aside, not edited.

- A1's moves are taken as done by `66afa4b`; the package is closed by the remainder above rather
  than redone.
