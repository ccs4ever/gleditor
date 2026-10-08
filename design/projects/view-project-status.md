# View Project: Status

The running record of the view project that [`start-view-project.md`](start-view-project.md) starts.
Updated with every package. A session that picks the project up reads this first and continues from
**Next**.

Last updated: 2026-10-08.

## Where it stands

| Milestone                 | State                                                                         |
| ------------------------- | ----------------------------------------------------------------------------- |
| M0, the tree in its shape | A1 done (this commit, on top of `66afa4b`); A2, the `ui_test` rename, is next |
| M1, spikes                | not started                                                                   |
| M2 onward                 | not started                                                                   |

## Done

| Package or change              | Commit      | Notes                                                                                                                                                                                                                                                               |
| ------------------------------ | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A1 moves                       | `66afa4b`   | Landed upstream before this project started, inside "Add private reader layers for signed link packages": every file of `apps/xudu/` to `apps/common/ui/xanadoc/` and of `apps/zigzag/` to `apps/common/ui/slice/`, include spellings and Makefile lists with them. |
| Build without ThorVG           | `a863fbe`   | `readFile` in `tests/lib/svg_cache_test.cpp` was unused, and fatal under `-Werror`, wherever ThorVG is absent.                                                                                                                                                      |
| `compare-backends.sh` isolated | `5142e20`   | Writes under `build/compare-backends/`; the E2E tests take their workspace from `XUDU_WORKSPACE_DIR`. It can now run beside `make test`.                                                                                                                            |
| No xvfb for GL and GLES        | `0d52fa3`   | Offscreen SDL over llvmpipe needs no display; the Makefile exports `GALLIUM_DRIVER=llvmpipe`. Only a Vulkan capture under SDL2 still needs `xvfb-run`.                                                                                                              |
| A1 remainder                   | this commit | See below.                                                                                                                                                                                                                                                          |

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

## Next

1. **A2**: rename `zigzag_test` to `ui_test` and `tests/zigzag/` to `tests/ui/` in the Makefile, CI,
   `AGENTS.md` and the README. Full gate; the same 192 tests under the new name.
1. **The baseline reds** (below): settle the keymap clash first, since it may be real.
1. **The `config.h` race** (below), as its own commit.
1. **M1**: the spikes, through spike runners. S1, S4, S5, S2 and S3 are engine-only and can run now.
   R1 to R5 can measure OpenGL and GLES headless; Vulkan needs `xvfb-run` or an SDL3 build here. V1
   and V2 also need the frame inspector.

## Baseline

Taken at `a863fbe`, each binary run alone and headless, outside `make`:

| Binary          | Passed | Failed | Skipped                                     |
| --------------- | ------ | ------ | ------------------------------------------- |
| `gleditor_test` | 738    | 1      | 3 (Vulkan resource tests; no Vulkan device) |
| `xudu_test`     | 1296   | 4      | publication-network and swarm tests         |
| `xuzz_test`     | 61     | 0      | 0                                           |
| `zigzag_test`   | 191    | 1      | 0                                           |

`make format-check` and `make lint` are green on the base. The full `make test`, with the rootless
swarm tests, has not been run in this environment. `tools/compare-backends.sh`: OpenGL and GLES
agree pixel for pixel, and the culling, atlas-growth and minification checks pass; Vulkan is not
available under SDL2 offscreen; the run fails at its E2E stage on the three E2E reds below.

### Known reds, not this project's

All six reproduce alone, without any concurrent run.

| Test                                                                           | Failure                                           | Suspected cause                                                                                                             |
| ------------------------------------------------------------------------------ | ------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `TextLayoutTest.InlineBoxAdvancesThePenAndIsSkippedByLaterGlyphs`              | advance off by 0.77 px against a 0.5 px tolerance | installed fonts: here `Monospace` is DejaVu Sans Mono and `Sans` is Inter                                                   |
| `StoreObjectManagerOverlayTest.FittedRowsShareSafeGeometryAndFullLabels`       | a control's fitted label has no glyphs            | fonts, unconfirmed                                                                                                          |
| `ChronofiladeBenchmarkTest.ScalabilityAndSpeedup`                              | 5.4 µs against 3.96 µs at 500 operations          | a microsecond timing assertion                                                                                              |
| `E2EBinaryOrchestrationTest.textSurvivesAtWholePageDistance`                   | 72 inked pixels, at least 100 expected            | fonts, unconfirmed                                                                                                          |
| `E2EBinaryOrchestrationTest.aDraggedSelectionLandsWhereItIsDropped`            | the dropped text lands at the wrong offset        | glyph metrics, unconfirmed                                                                                                  |
| `E2EBinaryOrchestrationTest.storePanelCreatesObjectsThroughNamedDrawnControls` | `xuzz` exits 1                                    | unknown; the log warns that `std:xudu/publish` and `std:zigzag/save_store` share a key, which may be a real keymap conflict |

Forcing other fonts through a private `FONTCONFIG_FILE` did not take effect, so the font cause is
not yet shown.

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
