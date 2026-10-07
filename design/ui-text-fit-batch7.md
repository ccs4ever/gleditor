# UI text fitting: batch 7

Batch 7 migrates the quotation builder, swarm telescope and hypertime graph to retained shared
widgets. Application C++ remains the adapter for GPU rendering, raw events and queued domain
actions; quotation, publication and hypertime operations remain in the existing engine.

## Presentation and interaction

The quotation builder exposes Source, Selector, Preview and Commit pages. Store/root selection,
rank/closure/query selectors, dimension choices, preview cells and local placement remain reachable
through drawn controls at large configured font sizes. Shared text fields edit Unicode graphemes and
retain their caret across model rebuilds. Preview metadata and quotation budgets remain available
without byte truncation. Commits validate the captured source and local store state.

The telescope retains a search field, category controls, channels, publication results and an
inspector. Wide layouts show lists together; smaller layouts use Channels, Publications and
Inspector pages. Topic filtering uses the catalog, and Refresh exposes catalog changes. Inspector
descriptions wrap; identifiers use middle ellipsis. Full labels and values remain accessible.
Publication opening uses a captured entry rather than a mutable result index.

The hypertime graph preserves branching topology, navigation, comparison, pan and zoom. A retained
positioned widget panel supplies graph node and alias bounds, focus, picking and accessibility.
Decoration geometry retains the graph edges. Comparison details use a virtual list; the scrubber and
comparison actions remain drawn controls. Nodes carry full version identities, while concise drawn
operation letters stay readable.

All three use live system UI presentation settings: quotationModal, telescopeModal and
hypertimeModal have widthPx, heightPx, maxWidthShare and maxHeightShare. Default preferred sizes are
840 by 560, 860 by 560 and 640 by 460 logical pixels. Safe-area shares default to 0.95. Setting
specs, schema and notes describe positive finite lengths and shares from 0.1 to 1. Empty application
font overrides follow the configured shared typography roles.

## Shared widgets

PositionedPanel is a reusable diagram container: child logical rectangles are relative to its
content origin, scaled and clipped to the parent. Fully off-panel nodes contribute no focus,
accessibility or pick targets. One retained overlay supplies the graph's widgets, so rendering
resource count does not grow with the number of nodes. Buttons can use concise captions with
complete accessible labels. Virtual list rows choose the existing identifier/description fitting
policies. Text-field action callbacks include the updated byte caret so application model rebuilds
preserve editing position.

## Verification

The final full build, format-check and lint passed. The full headless test target passed 737
library, 1,257 engine, 60 Xuzz and 179 ZigZag tests, followed by 12 swarm tests and the publication
outbox network test. The previously recorded intermittent publication seed check did not recur in
this run; its investigation note remains in
[the publication failure record](publication-intermittent-seed-review-2026-10-07.md).

All 13 focused modal tests passed. Their layout matrices cross 640 by 480, 1280 by 800 and 2560 by
1440 viewports with font scales 0.8, 1, 1.5 and 2, and Sans, Serif, Monospace and Noto Sans CJK JP.
Quotation checks six page/mode states at each combination (288 cases); telescope checks results and
inspector views (96 cases); hypertime checks Graph and Comparison (96 cases). Coverage includes
fitted glyph containment, full accessibility labels, shared bounds, virtual scrolling, retained
shaping/uploads, stale actions, Unicode caret preservation and registered pointer/keyboard focus.
Repeated Left/Right scrubber input moves one history entry at a time while retaining focus. Thirteen
shared widget tests and two panel configuration tests also passed.

Real-application journeys use named drawn controls to create and save a quotation while preserving
its source cell, navigate telescope pages and edit its Unicode search, and switch hypertime views.
The latter two leave background document text and operation counts unchanged. The existing modal
isolation journey also passed. Strict backend comparison passed rendering, picking, culling, atlas
growth and minified-text checks. Optional application E2E inside that script was disabled;
application E2E ran in the engine suite. The comparison sample had too few draws to split threaded
recording.

A temporary no-op-device probe measured 1,000 warm frames after 20 warm-up frames, with old adapters
copied from ae4c762 and retained adapters drawing the same domain fixtures. All three retained
adapters performed zero C++ allocations, text layouts, HarfBuzz calls, texture uploads or buffer
uploads in measured frames. These timings measure CPU submission, excluding GPU work.

| Modal     | Old allocations/frame | Old upload bytes/frame | Old CPU p50/p95 (µs) | Retained CPU p50/p95 (µs) |
| --------- | --------------------: | ---------------------: | -------------------: | ------------------------: |
| Hypertime |                   112 |                 10,896 |        41.81 / 47.36 |               0.21 / 0.21 |
| Quotation |                   407 |                  7,920 |      100.38 / 113.55 |               0.19 / 0.19 |
| Telescope |                   644 |                 19,992 |      272.69 / 315.89 |               0.19 / 0.19 |

Hypertime uses the same five-node history and preferred panel size. Quotation compares the previous
whole-modal presentation with the new Source page; telescope uses the same five catalog entries with
different fitted list/inspector geometry. These are equivalent domain fixtures, not pixel-identical
layouts or equal visible workloads. The counts establish retained warm-frame behavior; the quotation
and telescope times are not an isolated layout speedup measurement.

Software rendering p50/p95 milliseconds, including device synchronization (100 measured frames after
10 warm-up frames):

| Modal     | OpenGL      | OpenGL ES   | Vulkan       |
| --------- | ----------- | ----------- | ------------ |
| Quotation | 4.14 / 4.58 | 4.12 / 4.52 | 8.84 / 9.54  |
| Telescope | 4.39 / 4.78 | 4.42 / 4.85 | 9.96 / 10.83 |
| Hypertime | 3.82 / 4.18 | 3.82 / 4.18 | 8.47 / 9.09  |

These actual-modal GPU scenarios use the configured default theme, a 12-cell quotation source, 20
long publication entries and a six-node annotated history. Every measured warm frame had zero layout
and HarfBuzz calls. The modal captures were pixel-identical across OpenGL, OpenGL ES and Vulkan.
Captures of all three modals and the comparison document were inspected: fitted text stays within
controls, multilingual glyphs remain sharp, and graph operation letters retain full accessible
identities. Logs and temporary captures are under `/tmp/ui-text-fit-batch7-*.log` and
`/tmp/ui-text-fit-batch7-captures`.
