# UI lift: batch 3 — responsive layout foundation

Batch 3 adds application-neutral UI measurement, layout, widgets and retained screen overlays. The
existing application overlays stay on their current implementations until the migration batches.

## Metrics and units

`FrameContext` now carries `ui::UiMetrics` and a stable `ui::Theme` snapshot. Metrics combine
platform content scale, user scale and independent font scale. Font roles are caption, label, body,
title and mono; spacing comes from the resolved font metrics. Safe areas combine chrome and a
configurable margin share. The library provides typed fallback theme values, without reading
application stores. Applications can replace the shared theme snapshot and scale fields in
`AppState`.

SDL3 supplies display scale; SDL2 supplies the framebuffer-to-window ratio. Platform pointer events
are converted to framebuffer pixels before input dispatch, and IME rectangles are converted back to
window units at the SDL boundary. Synthetic pointer input continues to use framebuffer pixels.

`ui::layout` accepts Canvas pixels throughout. Convert logical style dimensions with `UiMetrics::px`
before layout; shaped text is already measured in pixels. Flow, stack, grid and split produce one
`LayoutResult` containing identities, parent relationships, allocated boxes, content boxes and focus
order. Shared edges round deterministically. Clipping back to the original parent keeps fractional
bounds safe. When minimum sizes cannot fit, containment takes precedence over those minima.

## Retained overlays and widgets

`ScreenOverlay` owns Canvas geometry and projection. It rebuilds on model, bounds, visibility,
theme, scale or window changes, and keeps unchanged frames free of shaping. Each font role has a
retained text Canvas, and backgrounds form a separate batch. New glyphs flush to the atlas before
the first draw. Drawing, hit testing, GPU pick identities and accessibility bounds use the same
layout boxes. Truncation keeps the full accessible label.

The generic widget models include labels, buttons, button flows, tabs, virtualized lists, cards,
badges, steppers, text fields, scrubbers, tooltips, panels, modals and docks. They carry values and
application action identifiers. Host callbacks run outside overlay locks. Numeric controls validate
ranges; text fields preserve UTF-8 and scroll horizontally to keep the caret visible; list layout
shapes only rows intersecting its viewport. Text fitting follows role policies, including middle
ellipsis for identifiers and wrapping for descriptions. Counters retain their full shaped value and
reserve intrinsic width before flexible siblings shrink.

Focus traversal, initial focus and manager-owned modal accessibility are batch 3b. Existing Form
traversal remains in place. The new modal container supplies geometry; applications still register
input scopes when they use it.

## Verification

Layout tests cover flow/stack/grid/split, safe-area clamp, deterministic edge rounding at scales 1,
1.25 and 2, flex constraints, duplicate identities and fractional containment. Overlay tests decode
actual submitted Canvas quads, compare picking and accessibility rectangles, preserve full Unicode
labels, and check retained frame shaping and invalidation.

`tools/ui-text-baseline` adds an `overlay-retained` workload alongside the earlier Canvas workloads.
It measures synchronized frame p50/p95 and shaping counts and checks captured glyph pixels against
allocated content rectangles. Captures run headlessly on software GL, GLES and Vulkan.

The full build and an isolated SDL2 event-boundary compile passed. The library suite passed 647
tests, including 34 new layout, widget and overlay cases. Lint and clang-format 19 on changed files
passed; repository-wide format-check still reports existing violations in untouched files.

The synchronized retained-overlay workload used 36 labels, Sans 10, 100 measured frames and 10
warmup frames. Its p50/p95 was 6.372/8.327 ms on OpenGL, 6.552/7.513 ms on GLES and 12.162/12.948 ms
on Vulkan. Every measured frame had zero layout, HarfBuzz and fallback calls. The GL and Vulkan p95
values were below the stage-0 retained baseline (9.654 and 17.972 ms respectively). These are
software-renderer measurements on this host, including GPU completion.

Captured overlay images match exactly across all three backends; the capture detector found no glyph
pixels outside their allocated boxes. The backend comparison suite also passed. A separate
global-new allocation probe measured zero C++ allocations in 1,000 warm `prepare()` calls with an
unchanged layout revision; it excludes driver rendering allocations.

The full test run passed 647 library, 1,252 engine, 57 Xuzz, 119 ZigZag and 12 swarm cases. The
publication-outbox network test missed its download-readiness deadline on the initial run; an
isolated rerun passed.
