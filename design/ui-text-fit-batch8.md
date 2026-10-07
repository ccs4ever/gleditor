# UI text fitting: batch 8

Batch 8 migrates satelloid cards, kinetic blueprint cards, wireframe loading telemetry and ZigZag
world labels to fitted, retained presentation. C++ implements projection, GPU drawing, font
measurement and raw picking adapters. Neighborhoods, cell topology, drag/drop operations and
publication workers remain in their existing domain components.

## Shared world presentation

The core WorldPanel paints an existing widget scene through a caller-supplied transform. Its local
layout provides text fitting, full accessible names and the same projected bounds for drawing and
accessibility. Camera movement changes projected label legibility without changing text or font
constraints. Background geometry remains available for picking below the label threshold.

projectPlane rejects non-finite, degenerate, clipped-behind-camera and near-plane-crossing planes. A
conservative projected scale sets the smallest readable line size; narrow labels also obey a
projected width threshold. Canvas fonts can change without allocating another rendering pipeline.

## Applications and configuration

Floating cards use live role typography and system UI settings. satelloidCard, tetherCard and
hullCard each provide preferred widthPx and heightPx, maxWidthShare and maxHeightShare, and
maxLines. Defaults are 200 by 180, 190 by 88 and 260 by 46 logical pixels; safe shares default to
0.9 and previews to three lines. Font metrics grow the cards within available safe space. Native
schema and setting notes describe positive finite lengths, shares from 0.1 to 1 and line counts from
1 to 10. These displayed dimensions remain separate from the existing satelloid physics settings.

Xuzz registers the kinetic renderer with its existing drag engine. The hull renderer receives real
publication status snapshots when the reader opens or refreshes the download form. The displayed
count measures completed dependencies. Closing or cancelling the form clears its telemetry; opening
the completed publication also clears it. This introduces no UI-thread network calls or worker
protocol changes.

ZigZag keeps its aggregate GPU canvases for cells and links. Fitted topology labels, rich content
shaping and projected legibility are separate from cell and dimension identities. Visual fitting
never changes canonical cell text or persistent operations.

## Verification

The new regression coverage exercises projection and label LOD at zoom 0.25, 1 and 2; rejects
nonfinite, degenerate, off-screen and behind-camera planes; keeps full accessible names and whole
card picking when labels hide; and checks stable activation after card removal. Responsive card
fixtures cross 640x480, 1280x800 and 2560x1440 with four font scales and Sans, Serif, Monospace and
Noto Sans CJK JP. Populated satelloid neighborhoods verify that badge padding leaves room for a
shaped line. Rich ZigZag content retains complete input and underline decorations. Warm frames
perform no shaping or buffer uploads; live font changes reuse rendering pipelines.

The first full run exposed concurrent keyboard mutation of the ZigZag manifold while accessibility
read it on the render thread. ZigZag command hooks now let the host marshal the entire action to its
owning thread; Xuzz uses the render queue. A deferred-dispatch test checks that mutation occurs only
when the owner executes the action. The existing cell-edit orchestration journey passes after this
fix. The drag journey's screenshot calibration also picked up blue HUD labels as selection pixels;
it now requires nearby white document paper. Both page and empty-space drops pass.

Strict compare-backends passes on Mesa software OpenGL, GLES and Vulkan/Lavapipe under an isolated
Xvfb. GLES is pixel-identical to GL; the plain-editor Vulkan capture differs on 0.165% of pixels,
within the 1% threshold. The actual nine world-card/label captures were inspected: GLES matches GL
exactly; Vulkan differs on at most 0.154% of pixels. At distant zoom, labels hide while cells
remain; normal and close zooms show fitted topology labels and wrapped content. Projected-bound
changes during animation or live typography updates invalidate accessibility independently of model
changes. These captures introduce no new view or axis-binding model.

CPU probes use 200 warm frames and 1,000 measured frames against the pre-batch adapters from
21ecda4. The domain fixtures are equivalent, but final cards retain more text and rich content, so
these measurements are not pixel-identical scene comparisons. They count C++ new allocations,
layout/HarfBuzz calls and buffer uploads; they exclude device execution and driver allocations.

| Presentation                          | Previous p95 CPU, microseconds | Retained p95 CPU, microseconds | Previous/new allocations per frame |
| ------------------------------------- | -----------------------------: | -----------------------------: | ---------------------------------: |
| ZigZag, both modes across three zooms |                  155.54–157.85 |                      0.73–0.84 |                            830 / 4 |
| Satelloid                             |                          22.61 |                           0.63 |                             42 / 0 |
| Blueprint tether                      |                          22.99 |                           0.68 |                             57 / 0 |
| Loading hull                          |                          15.92 |                           0.54 |                             23 / 0 |

All retained measurements have zero layout/HarfBuzz calls, texture uploads, geometry uploads and
pipeline creations. ZigZag's four small allocations come from its frame-local semantic pick map.
Separate device-synchronized probes use 200 warm frames and 100 measured frames per scenario. Final
p95 ranges are 4.16–5.92 ms on GL, 4.70–10.96 ms on GLES and 8.29–10.36 ms on Lavapipe; all have
zero warm layout/HarfBuzz calls. These software-device timings establish that every path renders,
but do not establish an application-wide hardware GPU speedup.

The final `make -j$(nproc) test` run passes 739 library tests, 1,257 xudu tests, 61 xuzz tests, 184
ZigZag tests, 12 rootless swarm tests and the publication-outbox network test. One video interaction
test is skipped because this build lacks SDL_image animation support; one existing test remains
disabled. `make format-check`, `make lint` and `git diff --check` pass. The previously recorded
intermittent publication seed-review failure did not recur.
