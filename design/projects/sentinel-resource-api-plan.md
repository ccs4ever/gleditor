# Resource and parent absence API migration

This note records the resource, layout-parent, and focus-snapshot migrations completed in the
sentinel cleanup, along with their integration surfaces and invariants.

## Device resource handles

`render::Handle<Tag>` in `include/gleditor/render/types.hpp` is a CPU-side resource-map key whose
`id` is optional. `DeviceGL` and `DeviceVK` index their maps by `uint32_t`; the ID is not a GL name
or Vulkan handle. Presence determines validity, so an engaged zero is a valid key.

The API and implementation surface is:

- `include/gleditor/render/device.hpp` returns handles from resource creation and accepts them for
  update, binding, copy, and destruction.
- `include/gleditor/render/gl/device_gl.hpp` owns the three resource maps and the ID counter;
  `src/render/gl/device_gl.cpp` creates IDs and looks them up.
- `include/gleditor/render/vulkan/device_vk.hpp` owns the Vulkan maps and counter;
  `src/render/vulkan/device_vk_resources.cpp` creates and resolves resources;
  `src/render/vulkan/device_vk_frame.cpp` resolves bound draw and readback resources.
- Library holders include `Canvas` (`include/gleditor/canvas.hpp`), the glyph cache, image cache,
  `BufferPool`, caret, beams, curve ribbons, media widget, and render state. `GlyphBatch` carries a
  buffer handle in a vector of draw records.
- Fake devices and tests that key storage by `handle.id` include `tests/lib/mocks/world_device.hpp`,
  `canvas_image.cpp`, `form_layout_test.cpp`, `media_widget_test.cpp`, `svg_cache_test.cpp`,
  `ui_overlay_test.cpp`, and `buffer_pool.cpp`.

Backend lookup checks presence before map lookup. Device ID allocation has explicit exhausted state:
the last representable ID is usable once, then allocation fails instead of wrapping and reusing an
identity. Native GL/Vulkan null values remain confined to backend boundaries. The optional ID
enlarges the wrapper and containing draw records; no handle size or offset assertion was found.

## Buffer pool allocations

`BufferPool::Allocation` in `include/gleditor/buffer_pool.hpp` is another CPU-side map identity. Its
`id` is optional; `reserve(0)` returns no allocation and releasing an empty allocation remains a
no-op. Engaged zero is a valid map key.

The ID is only dereferenced by `BufferPool` itself. Allocation values are stored in canvas commands,
document pages, caret, beams, and curve ribbons. `src/doc.cpp` and `src/canvas.cpp` check emptiness.
`tests/lib/buffer_pool.cpp` asserts the zero-row behavior and exercises the allocation map.

Allocation IDs have explicit exhausted state and do not wrap or reuse a previously issued ID. The
allocation value grows by optional storage. Zero-row requests retain their established behavior.

No BufferPool location query returns zero for a missing allocation: `byteOffset`, `rowCount`,
`roomFor`, and `erasedRows` use `placementOf`, which throws `invalid_argument` for a stale or
unknown allocation. Byte offset zero is a valid first allocation location; zero row/erased counts
are valid sizes. `reuseRows` already returns `optional<uint32_t>`. `rowsInUse()` is a count and may
be zero for an empty pool.

## Layout parent identity

`LayoutBox::parentId` and the `parentId` fields on `FlowOptions`, `StackOptions`, `GridOptions`, and
`SplitOptions` in `include/gleditor/ui/layout.hpp` are optional. Root construction passes `nullopt`;
an engaged zero remains a valid parent ID.

The parent relation is read in `src/ui/overlay.cpp`, `src/ui/world_panel.cpp`, and
`apps/common/ui/xanadoc/swarm_telescope_overlay.cpp`. `src/form.cpp` and `src/ui/segmented_pill.cpp`
build root entries; `tests/lib/doc_switcher.cpp`, `tests/lib/ui_layout_test.cpp`, and
`tests/lib/ui_overlay_test.cpp` cover the relationship. The migration preserves the existing layout
tree and accessibility roots; the library regression distinguishes absent parent from parent ID 0.

## Focus pick snapshot

`FocusManager::pointerPickTarget()` in `include/gleditor/ui/focus_manager.hpp` and
`src/ui/focus_manager.cpp` returns an optional registration snapshot containing the registration ID
and sequence together. The target retains its revision and modal state when no scope is focused.

`acceptsPointerPick` continues comparing that revision; callers must not treat absence as an
unversioned event. Focus state and registration lookup use explicit optional state, and registration
ID exhaustion is reported instead of wrapping.

Call sites are `src/app.cpp` (three pointer event origins), `src/renderer.cpp` (pointer-pick
acceptance, caret placement, and automation), and `tests/lib/focus_manager_test.cpp`.
`PointerDispatch::gpuPick` remains optional. A no-focus target preserves the revision, so
asynchronous events remain invalidated by later focus or modal changes.

## Integration follow-up

The root case is now an explicit `FocusTarget::rootXanadoc()` tag, separate from a minted Structure
focus. Phase 11 also introduces `XanadocSelection` for automatic, explicit root, and positive minted
birth selection through Store replay, Session authoring, Views and pouch source locations. Resolving
Auto before calling Chronofilade keeps its optional birth parameter a resolved scope: absent means
legacy root text, and present means that minted structure.

Review caught two additional replay hazards: Chronofilade still resolving an absent scope as Auto,
and incremental replay applying a new active document's operations onto the previous document's
text. Both are corrected in source, with regressions for root replay and Auto scope transitions.
Malformed present pouch fields are refused rather than treated as legacy absence. View navigation
and map callbacks preserve the selected scope.

The initial phase-11 full native build passed. The subsequent review fixes await a final rebuild and
runtime gate; see [the cleanup status](sentinel-cleanup.md) for current verification and remaining
API work.
