# View Project: Status

The running record of the view project that [`start-view-project.md`](start-view-project.md) starts.
Updated with every package. A session that picks the project up reads this first and continues from
**Next**.

Last updated: 2026-10-08.

## Where it stands

| Milestone                 | State                  |
| ------------------------- | ---------------------- |
| M0, the tree in its shape | A1 and A2 done; closed |
| M1, spikes                | not started            |
| M2 onward                 | not started            |

## Done

| Package or change              | Commit    | Notes                                                                                                                                                                                                                                                               |
| ------------------------------ | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A1 moves                       | `66afa4b` | Landed upstream before this project started, inside "Add private reader layers for signed link packages": every file of `apps/xudu/` to `apps/common/ui/xanadoc/` and of `apps/zigzag/` to `apps/common/ui/slice/`, include spellings and Makefile lists with them. |
| Build without ThorVG           | `a863fbe` | `readFile` in `tests/lib/svg_cache_test.cpp` was unused, and fatal under `-Werror`, wherever ThorVG is absent.                                                                                                                                                      |
| `compare-backends.sh` isolated | `5142e20` | Writes under `build/compare-backends/`; the E2E tests take their workspace from `XUDU_WORKSPACE_DIR`. It can now run beside `make test`.                                                                                                                            |
| No xvfb for GL and GLES        | `0d52fa3` | Offscreen SDL over llvmpipe needs no display; the Makefile exports `GALLIUM_DRIVER=llvmpipe`. Only a Vulkan capture under SDL2 still needs `xvfb-run`.                                                                                                              |
| A1 remainder                   | `3e48652` | See below.                                                                                                                                                                                                                                                          |

| A2, `zigzag_test` to `ui_test` | this commit | See below. |

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

1. **The key-hint start-up race** that `compare-backends.sh`'s GL/GLES parity stage exposed (under
   "Known reds").
1. **The `config.h` race** (below), as its own commit.
1. **M1**: the spikes, through spike runners. S1, S4, S5, S2 and S3 are engine-only and can run now.
   R1 to R5 can measure OpenGL and GLES headless; Vulkan needs `xvfb-run` or an SDL3 build here. V1
   and V2 also need the frame inspector.

## Fixed along the way

| Commit      | What                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `61c5b95`   | The store panel budgeted its button rows with the exact padding, but the layout rounds box edges to whole pixels; at a fractional padding a row came out a pixel too wide and its last button was laid out zero pixels tall. Not a font problem: it failed for every family. Fixed `StoreObjectManagerOverlayTest.FittedRows…` and `E2EBinaryOrchestrationTest.storePanelCreates…`.                                                                       |
| `975a560`   | Keymap actions only the slice presentation knows were registered unscoped, after the start-up pass that scopes built-in commands, so `std:zigzag/save_store` lost Ctrl+Shift+S to the global `std:xudu/publish` even in the ZigZag pane: the chord published. The "same key" warning was true. New test `E2EBinaryOrchestrationTest.aPaneChordIsNotTakenByTheGlobalOneOnItsKey`, failing before the fix.                                                  |
| `33488c8`   | Font descriptions ("Monospace 16", "Sans Bold 12") went to Fontconfig whole, which reads them as one non-existent family and falls back to the default face: no role got its face, anywhere. Parsed as Pango does now; the glyph cache's bold and italic variants open at the right size and are keyed by style. Fixed `TextLayoutTest.InlineBoxAdvances…` and `E2EBinaryOrchestrationTest.textSurvives…`. Documents now render in a true monospace face. |
| `f5d7218`   | `rebuild()` walked the whole ancestry to find the active xanadoc in a store with none, so a checkpointed Chronofilade rebuild cost O(K) again (5 us at 500 operations against the documented 0.29). Now a per-operation lookup: 0.23 us. Fixed `ChronofiladeBenchmarkTest.ScalabilityAndSpeedup`.                                                                                                                                                         |
| this commit | `aDraggedSelectionLandsWhereItIsDropped` dropped "into empty space" at a fixed x = 770, which a monospace page now covers. The drop point is found in the calibration frame, beside the page's right edge.                                                                                                                                                                                                                                                |

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

### Known reds, not this project's

Every test that failed at the baseline is fixed (see "Fixed along the way"). A full `make -k test`
after them exits 0: `gleditor_test` 743, `xudu_test` 1301, `xuzz_test` 61, `ui_test` 192, both swarm
runs.

`compare-backends.sh` passes every image check, and its E2E stage now passes on both backends (34 of
34), which lets it reach, for the first time here, the per-scenario OpenGL-against-GLES parity
stage. There one run failed five scenarios of the transclusion lifecycle by about 6 % against a 3 %
limit. The frames are drawn alike except that the GLES capture has no key-hint bar at the foot and
everything sits 18 px lower: it was captured before the hints, posted to the render thread with
`runWithState` during start-up, had arrived. Run alone, six times on each backend, the bar was
always there. So it is a start-up race between that post and the first capture, surfaced rather than
caused by these fixes; the fix belongs in how start-up hands the hints over, not in the tolerance.

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
  link line, which is what the Makefile enforces, still holds. `presentation_surface.hpp` is a
  drawing interface in the engine and is where U0's seam will start, so it is reviewed there. The
  audit enforces only the UI and program boundary for now, so it does not go red on this.

## Decisions taken

- A1's moves are taken as done by `66afa4b`; the package is closed by the remainder above rather
  than redone.
