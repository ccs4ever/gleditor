# Residual sentinel audit: library and tools

This is a targeted semantic review of first-party `include/gleditor/`, `src/` and `tools/`, after
the sentinel cleanup batch. It is not an exhaustive proof: searches located candidates, then each
candidate below was checked against its declaration and consumers. Ordinary zero initializers,
arithmetic bounds, native backend contracts, and values in a packed GPU representation were not
classified as absence without evidence.

## Must-fix API candidates

- `render::PickingResult::requestId` in `include/gleditor/render/types.hpp` uses zero to mean an
  uncorrelated/synthetic pick. Its consumers use that state to choose between scene-resolved async
  picks and direct dispatch (`src/renderer.cpp`, `src/doc_switcher.cpp`, `src/ui/overlay.cpp`,
  `src/media_widget.cpp`, `src/floating_toolbar_3d.cpp`, `src/radial_menu.cpp`). The renderer's
  request queue already uses `optional<uint64_t>`. Make the result field optional and migrate those
  routing checks; retain zero only if the device or attachment format itself requires it, at the
  decode boundary.
- `ui::LayoutItem::maximum` in `include/gleditor/ui/layout.hpp` defaults each axis to positive
  infinity to mean “no maximum.” `src/ui/layout.cpp` branches on infinity to implement that meaning.
  Use explicit optional per-axis maxima (or an equivalent bounded type) so an invalid nonfinite
  bound is not indistinguishable from an omitted maximum. The consumers are the layout algorithms
  and their `tests/lib` coverage.
- `DeviceVK::RecordingCost::median()` in `include/gleditor/render/vulkan/device_vk.hpp` returns
  infinity when `count == 0`; its caller compares the value to decide a recording strategy and emits
  it in diagnostics (`src/render/vulkan/device_vk_frame.cpp`). This is an internal API but still
  conflates “not sampled” with a numeric cost. Return `optional<double>` and keep the existing
  probing phase's explicit strategy choice.

## Wire or native boundary values to retain

- `render::PickingTag::kind == 0` and `tagKindNone` are the packed picking-attachment encoding for
  empty pixels. `PickingTag` is a decoded view of that payload and currently exposes `empty()`; a
  future API redesign could return an optional decoded tag, but the zero value belongs at the GPU
  encoding boundary and is not a candidate for mechanical replacement in shader data.
- `fromSentinel`/`toSentinel` in `include/gleditor/sentinel.hpp` are explicit storage-boundary
  adapters. Keep their use at those boundaries; the helper itself does not justify sentinel use in
  new application APIs.
- Native GL uniform locations and POSIX/codec callback return codes remain external contracts. Their
  `-1`/zero meanings should be converted once when crossing into first-party APIs.
- `screenScaleAt()` may mathematically return infinity on the camera plane, and Vulkan's median
  currently uses infinity only as an unmeasured value (listed above for conversion to optional).
  Algorithmic extrema such as initial bounding-box accumulators and nearest-hit distances are
  working values, not absent results.

## Ambiguous default initialization and policy values

- `DrawBudget::screenWidth{800.0F}` in `include/gleditor/draw_budget.hpp` is a public fallback that
  affects screen-space culling/detail calculations. The type does not say whether this is a test
  default, a neutral fallback, or an intended display assumption. Document the reason and units, or
  require callers to provide the drawable width if a guessed width can change behavior.
- `WidgetScene::virtualHitTargetMinPx{36.0F}` and `ButtonMetrics::virtualHitTargetMinPx{36.0F}` in
  `include/gleditor/ui/widgets.hpp` and `include/gleditor/ui/button_metrics.hpp` are duplicated
  accessibility sizing policy. They have units in the names but no stated rationale and may diverge.
  Consolidate the default and document why it is a library fallback, or route it through the
  existing theme/metrics policy if it is intended to vary.
- `Doc::formatDocTitle()` in `src/doc_switcher.cpp` treats both an empty name and the literal string
  `"0"` as unnamed. This may be intentional compatibility behavior, but the latter is a valid string
  unless another layer reserves it; confirm that contract before changing it.
- Zero-initialized IDs in focus, overlays, and scene construction are mostly required fields or
  internal counters. In particular, widget layout explicitly requires nonzero unique IDs, while
  focus nodes and overlay widget IDs are represented with optionals at public lookup boundaries.
  Keep those initialized fields distinct from missing values and avoid inferring absence from a
  default initializer alone.

The API candidates above are findings for a later implementation batch, not changes made by this
audit. This pass did not run a compiler or tests because the consolidated native Make is in
progress.
