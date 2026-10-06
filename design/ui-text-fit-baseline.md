# UI text fitting: batch 0 baseline

This implements delivery batch 0 of the UI lift and text-fit plan supplied on 2026-10-06. The base
is main at `235d8a3`. The batch establishes measurements and diagnostic bounds before changing text
fitting, Canvas clipping, widgets or modal focus.

## Capture overflow

`Canvas::setTextBounds(ui::TextBounds)` declares the allocated row or container for subsequent text
calls. Bounds use the same pixel coordinates as Canvas: left/bottom/width/height, Y upwards.
`clear()` and `setTextBounds(std::nullopt)` remove the declaration. This metadata does not change
layout or rendering. It measures logical layout extents, not glyph ink bearings or decoration
protrusions. Form, document-switcher and link-panel text declare their existing bounds.

Enable `SPDLOG_LEVEL=ui.layout=debug` to report runs outside their declared parent. Reports contain
only geometry and picking identity; they never contain source text. Debug means a runtime logger
level, rather than a build mode: this repository does not distinguish its optimized and debug builds
with `NDEBUG`. At the default logger level no containment reporting runs. Undeclared containers and
ToastOverlay's separate vertex stream are outside this detector's coverage.

## Capture shaping

`text::ShapingStatsScope` captures layout requests, actual HarfBuzz calls, the subset of fallback
calls, and input bytes submitted to `layoutPage`. Both single-line overloads enter that canonical
layout function once. Empty inputs and missing fonts count as layout requests but perform no
HarfBuzz call. Cold glyph-cache cluster shaping also counts as a HarfBuzz call, while hits do not.
Without an active capture, each hook costs a function call and thread-local null check; active
capture uses thread-local integers and allocates nothing.

Scopes cover only their calling thread. Nested enabled scopes collect independently and restore the
outer scope on destruction. Disabled scopes leave an outer capture active. Renderer `--benchmark N`
reports p50/p95 counts per settled frame and p95 CPU frame time, alongside the existing median and
draw statistics. Asynchronous document pagination is excluded. Use `SPDLOG_LEVEL=ui.layout=trace` to
inspect counts on every rendered frame, including unsettled ones.

## Reproduce the workload

```sh
make -j$(nproc) all ui-text-baseline
env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME="$PWD/build/xdg/data" XDG_CONFIG_HOME="$PWD/build/xdg/config" \
  ./build/ui-text-baseline --backend opengl --frames 100 --warmup 10 \
  --screenshot-prefix /tmp/ui-text-gl
env SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME="$PWD/build/xdg/data" XDG_CONFIG_HOME="$PWD/build/xdg/config" \
  xvfb-run -s "-screen 0 1280x800x24" ./build/ui-text-baseline \
  --backend vulkan --frames 100 --warmup 10 --screenshot-prefix /tmp/ui-text-vk
```

The tool uses a hidden window. GL/GLES disable presentation; Vulkan presents to return swapchain
images to the virtual display. Its TSV output compares 36 labels at 1280x800 in three scenarios:
unbounded rebuild, width-limited rebuild, and width-limited retained geometry. Frame timing includes
canvas rebuilding, submission, device completion and Vulkan presentation; it is not directly
comparable to Renderer CPU submission timing. Captures and a comparison against the same scene
without text happen outside measured frames, ensuring that a successful backend also draws text. The
retained scenario should report zero steady-frame shaping. An uncompiled backend exits 77; a runtime
failure is an error, not a successful measurement.

`tests/samples/ui/long-labels.tsv` supplies full ASCII descriptions, long paths and fingerprints,
combining marks, ZWJ emoji, Devanagari, Arabic, mixed bidi and CJK labels. Fonts and driver versions
affect both dimensions and timings; keep those fixed when comparing later batches. These are a
controlled Canvas workload, not measurements of all application overlays.

## Current inventory

There are 172 Canvas text call sites in 19 consumer files on this base. Five files use a width limit
somewhere; their 43 calls are not necessarily all constrained. The remaining 14 files have 129 calls
and no width limits:

| Consumer                                                   |  Calls |
| ---------------------------------------------------------- | -----: |
| Quotation builder                                          |     36 |
| Swarm telescope                                            |     29 |
| Hypertime graph                                            |     19 |
| Clasp forge                                                |     18 |
| Pouch drawer                                               |     10 |
| Satelloid, kinetic tether, link panel                      | 3 each |
| Radial menu, transcopyright                                | 2 each |
| Collaborator, wireframe hull, page break, floating toolbar | 1 each |

There are 13 visible byte-truncation sites: two in satelloid, one in pouch, four in telescope, five
quotation-builder helper consumers, and one in kinetic tether. Accessibility debug serialization is
excluded from this inventory.

Findings to preserve for the next batches:

- `ellipsize` remains unused; width-limited single-line text wraps and can overflow a row
  vertically.
- Canvas width limits survive `clear()`. Document-switcher title measurements and store-manager
  headings can inherit a previous label's constraint.
- Link-panel geometry is already retained by a stamp of selection, configuration, window and
  anchors. Oversized panel placement still permits an off-screen left edge.
- This main checkout has no walks overlay. Its fitter must be assessed on the branch where it exists
  rather than counted here.
- Some quotation controls put text tops only five pixels above their bottom; telescope tabs use
  seven pixels. Both can overflow even with short labels. Quotation selectors also exceed a
  narrow-window modal's right edge.

## Verification

The diagnostic tests cover real wrapper call counts, nested/disabled/thread scopes, all four
containment edges, invalid rectangles, wrapped height overflow, Canvas bounds reset and logging
privacy. Batch 1 can change ellipsis behavior without rewriting these baseline tests: the
wrapped-height test requests explicit wrapping rather than asserting the broken ellipsis.

The full build (`all ui-text-baseline`), the initial full test target (552/1249/57/119 tests plus
rootless swarm checks), and `make lint` pass. The final library run passes 553 tests, including all
14 new tests after the cold-glyph hook. Changed C++ files pass clang-format 19.1.7. Repository-wide
`make format-check` fails in untouched files, including `vortex_stdlib.cpp`; running the same check
against that file at the base commit reproduces the failure.

`compare-backends.sh` passes its document/overlay pixel and picking checks, atlas growth, coarse
text and zoom checks. Its GL/GLES E2E runs pass. The Vulkan E2E run passes 25 of 26 cases, but
`aDraggedSelectionLandsWhereItIsDropped` times out after 120 seconds on its on-page drag. The script
therefore exits nonzero before its final E2E capture comparison. This remains an unresolved
verification finding on this Vulkan/Xvfb setup; it has not been attributed to the base or fixed by
this batch. The three new Canvas workload capture pairs independently match exactly.

## Measurements

Each scene used Sans 10, 36 labels at 1280x800, 10 warmup frames and 100 measured frames. OpenGL
reported Mesa 26.2.4; Vulkan used Lavapipe from matching `vulkan-swrast` 26.2.4 with LLVM 23.1.1,
API 1.4.354, on Xvfb with MIT-SHM disabled. The software ICD was extracted into `/tmp` without
changing installed system packages. These timings include GPU completion and are specific to this
host; Vulkan also includes returning swapchain images to the virtual display.

| Backend | Scene                | Frame p50 (ms) | Frame p95 (ms) | Layout calls p50/p95 | Width/height overflow rows |
| ------- | -------------------- | -------------: | -------------: | -------------------- | -------------------------- |
| OpenGL  | Unbounded rebuild    |         40.598 |         42.018 | 36/36                | 36/0                       |
| OpenGL  | Width-limit rebuild  |         41.607 |         43.396 | 36/36                | 0/36                       |
| OpenGL  | Width-limit retained |          9.048 |          9.654 | 0/0                  | 0/36                       |
| Vulkan  | Unbounded rebuild    |         48.720 |         49.986 | 36/36                | 36/0                       |
| Vulkan  | Width-limit rebuild  |         50.973 |         53.015 | 36/36                | 0/36                       |
| Vulkan  | Width-limit retained |         15.545 |         17.972 | 0/0                  | 0/36                       |

Every rebuilt frame performed 1,480 HarfBuzz calls, including 1,444 fallback calls, for 8,144 input
bytes. The multilingual fixture exposes the existing per-missing-glyph fallback work; this is much
more costly than a short ASCII label. Inspection also shows malformed fallback glyph rendering on
this base. The retained scene performs zero layout/HarfBuzz/fallback calls and submits zero layout
input bytes, while preserving the existing vertical overflow.

All three OpenGL/Vulkan capture pairs match exactly. Each capture changes nonzero pixels relative to
the same scene with text omitted. Raw TSV, logs and PPM captures live under
`build/ui-text-baseline-results/` in the worktree.

A separate eight-frame plain-editor benchmark verifies the Renderer report: it records 62 layout
requests and 62 HarfBuzz calls per settled frame for the quick-brown-fox sample, with no fallback
calls. Its CPU frame p50/p95 was 2.264/2.513 ms. This reporting smoke was concurrent with parity
checks; use the serialized Canvas measurements above as the timing baseline.
