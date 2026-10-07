# Rendering review of `world-space-rendering-plan.md` and the rendering parts of `view-system.md`

Reviewed against the tree at the commit the plans themselves cite (`af5f1d5`); every path:line below
was read directly, not inferred. Scope: library rendering work only (`src/`, `include/gleditor/`),
plus how the presenter in `apps/common/ui/view/` will have to drive it.

______________________________________________________________________

## 1. Claim check

Table rows are the plan's own "Conventions that exist and must not change" table plus the extra
claims the task called out by name.

| #   | Claim                                                                                                                                                                               | Verdict                                                          | Evidence                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | World-to-clip: `glm::perspective(fov, aspect, 0.1, 10000) * glm::lookAt(...)`, `src/renderer.cpp:373-379`                                                                           | **CONFIRMED**                                                    | Exact match, lines 373-379: `glm::perspective(glm::radians(view.fov), aspect, render::kDefaultNearClipZ, render::kDefaultFarClipZ)` then `viewProjection = projection * camera;`. `kDefaultNearClipZ`/`kDefaultFarClipZ` are `0.1F`/`10000.0F` (`include/gleditor/render/constants.hpp:21,27`).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| 2   | That matrix's convention is OpenGL's (y up, z=-1 near, after divide)                                                                                                                | **CONFIRMED**                                                    | Standard GLM right-handed `perspective`; nothing in the GL path negates or remaps it (`src/render/gl/device_gl.cpp` has no row rewrite). The Vulkan backend's own rewrite comment (see #4) explicitly treats this as the "OpenGL range `[-w,w]`" starting point.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| 3   | OpenGL/GLES use the matrix as given; depth test `GL_LEQUAL`, `device_gl.cpp:111`                                                                                                    | **CONFIRMED**                                                    | `device_gl.cpp:111`: `api.DepthFunc(GL_LEQUAL);`, with the comment explaining the glyph-vs-paper tie hazard this exists for.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| 4   | Vulkan rewrites each draw's matrix: y negated, `z' = (w-z)/2`; `device_vk_frame.cpp:318-333`                                                                                        | **CONFIRMED**                                                    | `recordBatch()`, `device_vk_frame.cpp:329-333`: `flipped.mvp[i+1] = -flipped.mvp[i+1]; flipped.mvp[i+2] = 0.5F * (flipped.mvp[i+3] - flipped.mvp[i+2]);`. Operates on a **copy** (`DrawUniforms flipped = batch.uniforms;`, line 329) — the caller's matrix is never mutated, so "nothing above the device learns Vulkan flips y" (plan §1) holds. Note this runs once **per `GlyphBatch`**, inside `recordBatch`, which is called from `recordSequentially`/`recordInParallel`/`drawGlyphs` — i.e. per draw, not once per frame.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| 5   | Depth compare `GREATER_OR_EQUAL`, `device_vk_resources.cpp:807`                                                                                                                     | **CONFIRMED**                                                    | Line 807: `depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| 6   | Depth cleared to 0, `device_vk_frame.cpp:108`                                                                                                                                       | **CONFIRMED**                                                    | Line 108: `clears[2].depthStencil = {.depth = 0.0F, .stencil = 0};`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| 7   | Vulkan viewport and scissor are dynamic state, set to the whole target at the start of every secondary command buffer, `device_vk_frame.cpp:162-171`, `device_vk_resources.cpp:836` | **CONFIRMED**                                                    | `beginSecondary()`, lines 162-171, builds a `VkViewport`/`VkRect2D` from `swapchainExtent` and calls `vkCmdSetViewport`/`vkCmdSetScissor` unconditionally at the top of **every** secondary buffer (both the sequential one and each parallel chunk's, since `recordInParallel` calls `beginSecondary` per chunk). `device_vk_resources.cpp:836-837` declares `VK_DYNAMIC_STATE_VIEWPORT`/`VK_DYNAMIC_STATE_SCISSOR` in the pipeline.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| 8   | Pixels from projection are bottom-up: `spatial::projectToScreen` and `ui::projectPlane` both put the origin at the bottom left                                                      | **CONFIRMED**                                                    | `spatial.hpp:86-88`: `screenY = (ndcY*0.5+0.5)*screenHeight` (positive NDC y → larger screen y). `src/ui/world_panel.cpp:59-60`: identical mapping for `projectPlane`. Both are Y-up/bottom-up.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| 9   | Pixels from the pointer are window coordinates, top-down; `requestPickingTag` takes those, `src/app.cpp:1451`                                                                       | **CONFIRMED** (citation off by one line)                         | The comment explaining this is at `src/app.cpp:1451-1453`; the `onMotion` handler it documents starts at line 1449 and keeps `state->mouseX/mouseY` in the same top-down convention. `RenderDevice::requestPickingTag(coordX, coordY, ...)` (`include/gleditor/render/device.hpp:198-211`) documents "pixels from the top left," and `DeviceGL::requestPickingTag` flips internally (`flipY`, `device_gl.cpp:802,856`) — the top-down contract is real, just one line off in the citation.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| 10  | Window units to pixels: `contentScale`, in `FrameContext::metrics`                                                                                                                  | **CONFIRMED**                                                    | `include/gleditor/ui/metrics.hpp:19`: `float contentScale{1.0F};` inside `UiMetrics`; `FrameContext::metrics` is a `ui::UiMetrics` (`include/gleditor/frame_contributor.hpp:70`).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| 11  | Per-draw state: `DrawUniforms{mvp, opacity, identity}` per `GlyphBatch`, `types.hpp:204`, `:242`                                                                                    | **CONFIRMED**                                                    | `DrawUniforms` at `include/gleditor/render/types.hpp:204-232`; `GlyphBatch` at `:242-247`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| 12  | `PipelineDesc::depthTest` couples test and write; Vulkan sets both from it, `device_vk_resources.cpp:802-803`                                                                       | **CONFIRMED**, with a GL nuance worth stating explicitly         | Vulkan: lines 802-803, `depthTestEnable`/`depthWriteEnable` both `= desc.depthTest ? VK_TRUE : VK_FALSE`. GL does **not** write this as explicitly: `bindPipeline()` only calls `glEnable`/`glDisable(GL_DEPTH_TEST)` (`device_gl.cpp:675-679`); there is no `glDepthMask` call anywhere in `device_gl.cpp` (confirmed by grep — `DepthMask` is not even resolved in `gl_api.cpp`/`gl_api.hpp`). The coupling still holds in effect, because per the GL/GLES spec, disabling `GL_DEPTH_TEST` also disables the depth-buffer update regardless of the depth mask — but the *code* doesn't model "write" as a concept at all today on GL, it falls out of spec behaviour. An implementer adding `depthWrite` must add the first-ever `glDepthMask` call to this backend, not just decouple an existing one.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| 13  | `drawGlyphBatches` may record a run of batches on worker threads                                                                                                                    | **CONFIRMED, but Vulkan-only and probabilistic**                 | The base class default (`include/gleditor/render/device.hpp:190-195`) calls `drawGlyphs` once per batch, in order, on the calling thread — this is what GL **and** GLES use (`DeviceGL` does not override `drawGlyphBatches`; confirmed by grep). Only `DeviceVK::drawGlyphBatches` (`device_vk_frame.cpp:399-482`) can split a run across `recorders.run()` worker threads, and even then only when `batches.size() >= parallelRecordingThreshold`, `recorders.parallelism() >= 2`, and (absent `GLEDITOR_RECORD_THREADS`) a running median-cost comparison between `recordSequentially` and `recordInParallel` decides it frame to frame. An implementer must not assume parallel recording is available, or that it is a fixed decision once made.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| 14  | "Each [secondary buffer] re-establishes the state its draws need"                                                                                                                   | **CONFIRMED**                                                    | `beginSecondary()` comment, `device_vk_frame.cpp:159-161`, plus the code immediately below it: viewport/scissor (162-171), `vkCmdBindPipeline` + `vkCmdBindDescriptorSets` from the **currently bound** pipeline/texture state (173-180) — all re-issued every time a secondary buffer is opened, because Vulkan secondary buffers inherit nothing else.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| 15  | Picking coordinates and the picking attachment work as described                                                                                                                    | **CONFIRMED**                                                    | GL: `requestPickingTag` reads `GL_COLOR_ATTACHMENT1` as `GL_RGBA_INTEGER`/`GL_UNSIGNED_INT` into a PBO behind a fence (`device_gl.cpp:830-868`), `takePickingTag` polls the fence and unmaps (`:870-906`). Vulkan: the pixel is noted at `requestPickingTag` time but the actual `vkCmdCopyImageToBuffer` is deferred to `endFrame()` because "the tag image only reaches `TRANSFER_SRC` layout when the pass ends" (`device_vk_frame.cpp:502-510, 579-599`), then polled via the frame's own fence in `takePickingTag` (`:512-543`). Both funnel through `unpackPickingTag` (`types.hpp:410-421`). Overlay-widget resolution (`render::resolveOverlayWidget`, `types.hpp:442-452`, and `render_state.hpp`'s scope allocators) is a separate, already-built layer on top that `PlaneSet` picking should reuse rather than re-derive — see §13.2's own claim and the picking-bits discussion below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| 16  | A `GlyphBatch` can address a contiguous sub-range of a page's glyph instances corresponding to a band of lines; a page's instances are ordered by line; where is the paper quad     | **CONFIRMED, with an important caveat**                          | `Page`'s constructor (`src/doc.cpp:261-362`) pushes **one page-sized paper quad first** (`pushBackground()` at line 275, before the glyph loop), then iterates `aShaping.glyphs` in order (line 287) pushing one `VBORow` per glyph; `text::TextLayout::layoutPage()` (`src/text/layout.cpp:810,828`) appends glyphs in an **outer loop over lines**, so the array is contiguous per line by construction — a `[first,count)` sub-range of the detail range genuinely does correspond to a band of lines. **But** the one paper quad at index 0 covers the *whole page*, not a band: `PagePose::band`'s "strip of paper behind them" (world-space-rendering-plan.md §... / view-system.md §10.3.2) is **new geometry that must be built and stored**, not a slice of the existing quad. The coarse (line-bar) range also starts with its own second full-page paper quad (`pushBackground()` again at line 340) before the per-line bars — same caveat applies there too.                                                                                                                                                                                                                                                                                                                                                                                                                |
| 17  | `Page` matrices are composed as `modelMatrix() * page->getModel()` everywhere including caret, selection and hit testing                                                            | **CONFIRMED for caret and hit-testing; "selection" is indirect** | `Doc::collect` → `Page::collect`: `docTransform = viewProjection * modelMatrix()` (`doc.cpp:621`), then `mvp = docTransform * model` inside `Page::collect` (`doc.cpp:579`), where `model` is the page's own `Drawable::model`. Hit-testing: `Doc::worldPoint`, `doc.cpp:713`: `modelMatrix() * pages[pageIndex]->getModel() * ...`. Caret: `doc.cpp:881`: `caret.draw(state, viewProjection * modelMatrix() * page.getModel())`. `PageFrame` (`doc.hpp:952`) is the same composition again. **Selection has no separate transform call at all** — it is decided per-fragment inside the already-drawn glyph quad via `HighlightRange` matching on identity+cluster (`glyph.frag.glsl:43-85`, `setHighlights`), so it automatically rides the same page `mvp` rather than being independently composed. Functionally consistent with the claim, but worth knowing: a page pose/band feature does not need to touch any "selection transform," because there isn't one.                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 18  | How many bits a picking identity has for document/page/overlay ids, and whether thousands of planes fit                                                                             | **CONFIRMED, fits, via a mechanism the plan doesn't name**       | `tagKindBits=4`, `tagDocBits=14`, `tagPageBits=14` (`types.hpp:368-370`): a 32-bit identity word is kind(4)+doc(14)+page(14). For *documents*, that is a hard ceiling of 16384 open documents/pages, already tight for "thousands" if taken literally as the plane count. But the actual mechanism that already exists for "thousands of non-document things with individual pick identities" is `Canvas::setTag(kind, index)` (`canvas.hpp:113-131`): `index` lands in the **cluster** field, which on the wire is a full 32-bit hardware word per picking read but is packed into `VBORow::paper`'s 16-bit cluster subfield when built through `Canvas`/`Doc` (`doc.hpp:646`, `maxClustersPerPage = 65535`). The existing convention (`render_state.hpp:61-83`, used by `doc_switcher.cpp`, `floating_toolbar_3d.cpp`, `radial_menu.cpp`, `media_widget.cpp`) is: **one scope** (a `docIndex`/`pageIndex` pair, allocated via `allocateOverlayPickScope`/`allocatePersistentOverlayPickScope`) **per owning widget/control**, and up to 65535 individually-addressable elements inside it via the cluster field. `PlaneSet` should follow the same pattern — one scope per `PlaneSet` instance, planes distinguished by cluster index — which comfortably covers "thousands," not the 14-bit doc/page fields directly. See Problem 8 below for the actual pressure point this creates. |
| 19  | `DeviceCapabilities` gains `regions`; `PipelineDesc` gains `depthWrite`                                                                                                             | **CONFIRMED not yet present**                                    | `DeviceCapabilities` today has only `parallelCommandRecording`/`recordingThreads` (`types.hpp:256-282`); `PipelineDesc` has only `depthTest` (`types.hpp:163-191`). Both are genuinely new fields, as the plan says.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| 20  | `ui::WorldPanel` is "one plane with a widget layout and a pool of canvases"                                                                                                         | **CONFIRMED, almost verbatim**                                   | `include/gleditor/ui/world_panel.hpp:30-78`: one `background_` `Canvas` plus one `Canvas` per font role (`std::array<std::unique_ptr<Canvas>, kFontRoleCount> text_`), all drawn through the **same single** `canvasToClip` matrix per `draw()` call (`world_panel.cpp:248-272`). This is the right mental model for why `PlaneSet` is new work and not an extension of `WorldPanel`: `WorldPanel` has one transform for a whole widget scene, `PlaneSet` needs an independent transform per plane.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| 21  | `outsideFrustum` tests the four side planes only; `projectToScreen` returns `(0,0)` for a point behind the camera                                                                   | **CONFIRMED**                                                    | `draw_budget.hpp:75-94`: only `x`/`y` vs `±w` are tested, with the comment explaining the OpenGL-`[-w,w]`-vs-Vulkan-`[0,w]` depth-range disagreement as the reason depth is excluded. `spatial.hpp:79-82`: `if (clip.w <= 0.0001F) return {0.0F, 0.0F};` — a near-zero threshold rather than exactly `<= 0`, but the sentinel behaviour the plan is warning about is exactly as described.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |

### Additional findings surfaced while checking the above

- **`ui::split()` already exists** with exactly the shape `PaneTree` wants:

  ```cpp
  split(Rect bounds, const LayoutItem &first, const LayoutItem &second, SplitOptions{axis, firstShare, gap, parentId})
  ```

  (`include/gleditor/ui/layout.hpp:74-89`). `PaneTree` is low-risk precisely because it can be a
  thin bookkeeping layer over this, already tested.

- **`BufferPool` is already fully generic** (`include/gleditor/buffer_pool.hpp`) — row-granular
  reserve/resize/release/erase/reuse over one device buffer, used today by `Canvas` and `Beams` as
  well as `Doc`. `PlaneSet`'s "one plane owns a range of [the] stream... only that plane's range is
  uploaded... removal frees the range for reuse" is exactly what `BufferPool::Allocation` +
  `write`/`release`/`eraseRows`/`reuseRows` already provide. This substantially de-risks the storage
  half of `PlaneSet`; the genuinely new part is the per-plane `GlyphBatch`/state bookkeeping on top
  (see Problem 1).

- **`projectPlane`/`labelLOD` are already implemented and generic**
  (`src/ui/world_panel.cpp:36-102`), including the degenerate/near-plane/finite-ness guards the
  plan's `ProjectedPlane`/`LabelLodPolicy` ask for. `PlaneSet` should call these directly rather
  than re-deriving the math; the plan's §5 doesn't say this explicitly but nothing stops it, and the
  existing functions are unit-testable today (`tests/lib/world_panel_test.cpp`).

- **`WorldPanel::draw` ties a *content* decision (label visibility by legibility) to a *geometry
  rebuild*.** `world_panel.cpp:253-268`: every frame, for every visual, it recomputes `labelLOD`,
  and `if (visible != drawnLabels_[i]) drawingDirty_ = true;`, which then forces `rebuild()` →
  `canvas->commit()` (an upload) the moment any label's visibility flips. If `PlaneSet` is built the
  same way — deciding a plane's *text* sub-range visibility as part of the thing that triggers
  `paint()`/upload — then a camera dolly that crosses the legibility threshold for even one plane,
  every frame it hovers near that threshold, forces a GPU upload on an otherwise "settled" frame.
  This directly threatens V-R44 ("a settled frame performs no... buffer upload") and §5.3's own test
  ("`setState` uploads nothing"). The plan's wording ("rectangles are drawn regardless... text
  sub-range is drawn [or not]") suggests visibility should be a **per-draw flag/uniform choice**
  (which `GlyphBatch`'s `instanceCount`/offset can express for free, by choosing whether to include
  the text sub-range in the draw) rather than a rebuild-triggering condition — but this needs to be
  stated as a hard constraint, because the one existing precedent in the codebase (`WorldPanel`)
  does it the expensive way.

______________________________________________________________________

## 2. Work packages

Listed in the dependency order the plan itself proposes (§8), with what I found while reading the
real code layered in.

### WP1 — `projectToViewport` / `unprojectToRay` / ray helpers

- **Files**: `include/gleditor/spatial.hpp` (header-only additions, alongside the existing
  `projectToScreen`/`onScreen`/`framingDistance`/`framingFov`).
- **Depends on**: nothing.
- **Tests**: new `tests/lib/spatial_unproject_test.cpp` — round trip through
  `projectToViewport(intersectPlane(unprojectToRay(pixel)))`; ray through viewport centre equals
  camera forward; behind-camera and singular-matrix give nothing; a sub-rectangle viewport agrees
  with a whole-target viewport built for the same physical pixel; `windowToPixels` at content scale
  1/1.5/2; agreement with `projectToScreen` everywhere the latter is defined (not behind the
  camera).
- **Device scene**: none required for this step alone; the cross-backend pick-agreement scene
  belongs with WP3 once regions/picking are touched together.
- **Size**: M — the double-precision-inverse requirement (near/far five orders of magnitude apart,
  per the plan's own §2.2) means the round-trip test has to be written tightly or it will pass by
  accident at small distances and fail only at realistic ones.
- **Main risk**: a silently-wrong unprojection (e.g. a single-precision inverse, or an off-by-one in
  the NDC↔pixel mapping) that still "looks plausible" in a quick visual check; the round-trip test
  at the far plane is the thing that actually catches it.

### WP2 — `insideFrustum`

- **Files**: `include/gleditor/draw_budget.hpp`.
- **Depends on**: nothing (parallel with WP1).
- **Tests**: additions to the existing `tests/lib/draw_budget.cpp` (which already has the
  camera-construction helper and the six-case pattern `outsideFrustum` uses — see
  `draw_budget.cpp:21-93`): wholly inside, straddling each of the six planes, wholly outside, behind
  the camera.
- **Size**: S.
- **Main risk**: unlike `outsideFrustum` (deliberately four-plane, side-only, per
  `draw_budget.hpp:70-73`'s own comment), `insideFrustum` *does* test depth, which only means
  something if it is always evaluated in the pre-Vulkan-rewrite, neutral clip space the plan
  promises (§1's "rule that follows"). Getting this right is mechanical; getting a test to prove it
  stays that way across a future Vulkan change is the part worth being deliberate about.

### WP3 — Render regions: scissor, on OpenGL, OpenGL ES and Vulkan together

- **Files**: `include/gleditor/render/types.hpp` (`RenderRegion`, `RegionId`, `GlyphBatch::region`);
  `include/gleditor/render/device.hpp` (`defineRegion`/`setRegion`);
  `src/render/gl/gl_api.hpp`/`gl_api.cpp` (new entry points — **`Scissor` and `Viewport`'s sibling
  calls plus `DepthMask`/`DepthRangef` are not currently resolved at all**; grepping `gl_api.cpp`'s
  `GLEDITOR_RESOLVE` list confirms `Viewport` is resolved but `Scissor`, `DepthMask`,
  `DepthRangef`/`DepthRange` are not — this is a clean addition, not a partial one);
  `src/render/gl/device_gl.cpp`/`.hpp` (`DeviceGL::defineRegion`/`setRegion`, state reset in
  `beginFrame()`); `src/render/vulkan/device_vk.hpp`/`device_vk_frame.cpp`/`device_vk_resources.cpp`
  (`DeviceVK::defineRegion`/`setRegion`; `recordBatch`/`beginSecondary` setting viewport+scissor
  from the batch's region instead of always the whole target).
- **Depends on**: nothing structurally, but logically wants WP1 done first since the cross-backend
  pick-agreement scene uses `projectToViewport`.
- **Tests**: new `tests/lib/render_region_test.cpp` against `tests/lib/mocks/`; an addition to
  `tests/lib/device_capabilities.cpp` for the new capability flag.
- **Device scene**: a new `tools/compare-backends.sh` scene — two tiled regions plus one embedded —
  per the plan's own §3.4 acceptance list.
- **Size**: L — three backends, a genuinely empty GL entry-point table to fill in, and a Vulkan
  state machine that has to stay correct whether or not a frame is split across recording threads.
- **Must land on all three backends together** (the plan's own §8 rule, and the right one): a region
  that means something different on Vulkan than on GL is worse than no regions at all.
- **Main risk**: Vulkan's framebuffer-rectangle is top-down while regions are specified bottom-up
  (plan §3.2); this is exactly the kind of off-by-one that a bottom-left-marker test (as the plan
  proposes) catches and a purely numeric unit test does not, because the mock devices in
  `tests/lib/mocks/` don't model a real framebuffer's row order at all.

### WP4 — Depth slices in regions

- **Files**: extends WP3's region plumbing: `device_gl.cpp` (`glDepthRangef`, with a fallback to
  `glDepthRange` for a desktop GL context that doesn't expose the `f`-suffixed entry point — both
  need resolving); `device_vk_frame.cpp`/`device_vk_resources.cpp` (remap `[nearDepth,farDepth]` to
  `minDepth=1-farDepth`, `maxDepth=1-nearDepth` given the existing reversed-Z convention, confirmed
  at #4/#5 above).
- **Depends on**: WP3.
- **Tests**: depth cases added to `render_region_test.cpp`; the compare-backends scene gains the
  "far page with glyphs, inside a slice of a tenth of the range, still shows its glyphs" case the
  plan names — this is a direct, deliberate re-run of the exact LEQUAL-tie hazard documented in
  `device_gl.cpp:100-111`'s comment, now inside an artificially narrowed depth range, which is
  precisely where it would get worse, not better.
- **Size**: M.
- **Main risk**: this is the one place in the whole plan where "the real check" and "a
  plausible-looking frame" diverge most sharply — OpenGL's fixed-point depth buffer loses precision
  multiplicatively inside a narrow slice, and a page whose glyphs survive at full range can start
  failing the same LEQUAL comparison once the slice shrinks. The plan already names this; it is
  worth treating as the gating test for WP4, not an afterthought.

### WP5 — `depthWrite`

- **Files**: `include/gleditor/render/types.hpp` (`PipelineDesc::depthWrite{true}`);
  `include/gleditor/render/gl/gl_api.hpp`/`.cpp` (resolve `DepthMask` — **does not exist yet**, see
  WP3); `device_gl.cpp` (store it on the `PipelineRecord`, call `glDepthMask` in `bindPipeline()`);
  `device_vk_resources.cpp` (decouple `depthWriteEnable` from `desc.depthTest` at lines 802-803).
- **Depends on**: nothing structurally; the plan's own table lists it as a root dependency for both
  `PlaneSet` (WP6) and page poses (WP7).
- **Tests**: a new small test (translucent-behind-opaque is hidden; translucent-in-front blends; two
  translucents drawn back to front both show) — natural home is a new
  `tests/lib/depth_write_test.cpp` or an addition near `tests/lib/picking.cpp`'s device-mock style;
  a compare-backends case.
- **Size**: S–M.
- **Main risk**: low. The one thing worth checking in review: "test on, write off" is the standard,
  safe combination (the fragment still has to pass the existing opaque content's depth to blend at
  all) — there is no GL "test disabled implies no write" trap here because the test stays enabled
  for translucent draws; that trap only bites a caller who tries to turn *test* off while leaving
  *write* on, which this plan never asks for.

### WP6 — `ui::PlaneSet`

- **Files**: new `include/gleditor/ui/plane_set.hpp`, `src/ui/plane_set.cpp`. Reuses `BufferPool`
  (storage, confirmed generic above) and `src/ui/world_panel.cpp`'s `projectPlane`/`labelLOD`
  (confirmed generic and already tested above) rather than re-deriving either.
- **Depends on**: WP1, WP3, WP5 (matches the plan's own table).
- **Tests**: new `tests/lib/plane_set_test.cpp` — repainting one plane uploads one range (verify
  with a device mock that counts `updateBuffer` bytes, the same pattern
  `tests/lib/mocks/world_device.hpp`'s `WorldRecordingDevice::uploads` already implements for
  `Doc`); `setState` uploads nothing; a `ShapingStatsScope` over a frame of pure motion reports
  zero; draw order (opaque front-to-back with write, translucent back-to-front without); a
  below-threshold plane keeps its rectangle and its pick; `faceCamera` normal check; removal frees
  the range (`BufferPool::release`); moving a parent moves children with no further call and uploads
  nothing.
- **Probe**: `tools/layout-latency-probe.cpp` gains a mode for 100/1,000/10,000 planes (plan's own
  §5.3), reporting frame time and upload counts — model its percentile reporting on
  `tools/ui-text-baseline.cpp:240` (`percentile()`) and its per-frame sample columns.
- **Size**: L.
- **Main risks**: see Problems 1–4 below; this is the work package almost every open risk in this
  review lands on.

### WP7 — Page poses and bands

- **Files**: `include/gleditor/doc.hpp` (`PageBand`, `PagePose`,
  `Page::flowPose`/`pose`/`setPose`/`animatePoseTo`/`clearPose`, `Doc::setBuildPriority`);
  `src/doc.cpp` (implementation, **plus mandatory changes to existing code the plan does not mention
  — see Problems 6–7**: the unconditional `page.setModel(trans)` in the reflow tail path
  (`doc.cpp:1273`), the `prevPage.getModel()[3][1]`-based next-page placement in both `reflowFrom`'s
  rebuilt range (`doc.cpp:1241`) and `Doc::newPage` (`doc.cpp:1698`), and a new per-band paper quad
  since the existing one (confirmed caveat under claim #16) covers the whole page).
- **Depends on**: WP5 (plan's own table).
- **Tests**: new `tests/lib/doc_page_pose_test.cpp` per the plan's own list; **must** re-run
  `tests/lib/doc_gap_test.cpp` and `tests/lib/onion_skin_test.cpp` unchanged, which the plan calls
  out explicitly as the regression gate — but see Problem 6: neither of those files currently sets a
  pose across a reflow, so passing them is necessary but not sufficient evidence that reflow
  respects poses.
- **Size**: L — touches the most heavily-used internal path in the library (`collect`, `worldPoint`,
  `newPage`, `reflowFrom`, caret drawing all read or write a page's matrix today).
- **Main risk**: the reflow/pose interaction (Problems 6–7) is the single most likely source of a
  silent, hard-to-reproduce bug in this whole plan, because it only manifests when a *posed* page's
  *document* is edited elsewhere, which is exactly the scenario the base view's "a page that flies"
  (§10.3.2) and the stacked vanishing view both create routinely (editing while a link is active, or
  while a deck is mid-transition).

### WP8 — `ui::PaneTree`

- **Files**: new `include/gleditor/ui/pane_tree.hpp`, `src/ui/pane_tree.cpp`, built directly on the
  already-existing, already-tested `ui::split()` (`include/gleditor/ui/layout.hpp:74-89`).
- **Depends on**: nothing; can run in parallel with literally everything else in this plan.
- **Tests**: new `tests/lib/ui_pane_tree_test.cpp` — splits partition the bounds exactly; closing
  gives space to the sibling; order is stable under resize.
- **Size**: S.
- **Main risk**: low; this is the best-de-risked item in the whole plan because its only real
  dependency (`split()`) already exists and is already unit-tested.

### Parallelization guidance

- **Can start immediately, independently**: WP1, WP2, WP5, WP8 (all header-only or single-file
  additions with no cross-backend coordination requirement).
- **Must land as one unit, not staggered per backend**: WP3+WP4 (regions + depth slices) — write the
  GL and Vulkan halves in parallel if staffing allows, but gate the merge on all three backends
  passing the *same* region test together, per the plan's own rule and the
  Vulkan-top-down-vs-bottom-up-regions risk.
- **Blocked until WP1+WP3+WP5 land**: WP6 (`PlaneSet`).
- **Blocked until WP5 lands, otherwise independent of WP6**: WP7 (page poses) — it touches
  `doc.hpp`/`doc.cpp` exclusively and nothing in `PlaneSet`'s files, so WP6 and WP7 can proceed in
  parallel once WP5 is in.

______________________________________________________________________

## 3. The presenter (`apps/common/ui/view/`, not in the library)

Per frame, for each stale placement (`view-system.md` §8.9): `prepare()` (may mint/cache,
O(visible)), then the pure `layout()` call refilling the placement's `LayoutSink`, then (page views
only) `transition()` emitting `MotionHint`s. Then per pane: `animation.advance(dt)` tweens every
`SubjectId` toward its new record, and `presenter.draw(pane)` turns the (possibly tweened) records
into device work. That last step is the one with teeth:

- **`PlacedItem` for a cell/label/badge/marker → one `PlaneSet` plane.** The presenter must keep its
  own `SubjectId → PlaneId` map across frames (a `PlaneId` is allocated once via `add()`;
  `LayoutSink` hands back a fresh list every `layout()` call with no "same as last frame" bit
  attached). The presenter — not `View`, not `LayoutSink` — is therefore where the diffing lives:
  which `SubjectId`s are new (→ `add()` + `paint()`), which persist with changed content (→
  `paint()` again), which persist with only a moved/faded transform (→ `setState()`, no upload), and
  which vanished (→ the animation layer fades the *old* record, per §8.9, and only once that fade
  finishes does the presenter call `remove()`). This bookkeeping is real, non-trivial code that the
  plan does not spell out as its own deliverable, even though §8.9 names the mechanism ("the
  animation layer keeps... a copy of the last record") that makes it possible.
- **`PlacedItem` for a page → `Page::setPose()`** on the document's own `Page`, plus the page's
  parent `PlacedFrame` (kind `Document`) → the `Doc`'s own model matrix / `animateMoveTo`.
- **`PlacedFrame` for a pack → a parent plane in `PlaneSet`**, with constituents as child planes
  (`PlaneState::parent`). Same `SubjectId`-to-`PlaneId` stability problem as above, compounded by
  `ViewCellRef`'s epoch: a derived cell's `SubjectId` carries its epoch (§6.5/§8.9), so after a toss
  its old id matches nothing new and the presenter must let it fade and free its `PlaneId`, not
  reuse it.
- **`PlacedEdge` of kind `Strand` → thin `gleditor::Beams` segments along the bundle's two gather
  points.** `Beams` genuinely supports this: `Row::colour` is per-instance (`beams.hpp:58-77`), so
  "per-strand colour" is free, and `Beams::addPath` already fades-by-arc-length across a
  multi-segment route (`beams.cpp:110-132`), which is exactly what a *curved* strand needs — `Beams`
  has no literal curve primitive, so a curved strand must be a short polyline (sample
  `spatial::evaluateQuadraticBezier` at a handful of `t` and feed the points to `addPath`). Instance
  cost is cheap: one `Row` is 44 bytes (`beams.cpp:34`'s `static_assert`), and **the whole committed
  `Beams` object draws in one pipeline bind + one atlas bind + one instanced draw**
  (`beams.cpp:148-163`) regardless of how many rows it holds — dramatically cheaper per-item than
  `PlaneSet`'s one-`GlyphBatch`-per-plane model (§ Problems below), which argues for pushing as much
  of "many small coloured things" as possible through `Beams` rather than `PlaneSet` wherever the
  item doesn't need independent rotation/opacity/picking beyond what a beam already carries. The one
  real gap: `beam.vert.glsl`'s perpendicular-width vector is `cross(run, vec3(0,0,1))` (world Z),
  which degenerates for any strand whose world displacement is exactly parallel to world Z — see
  Problem 5.
- **Edge-heat bands and ghost outlines.** A ghost outline (`itemGhost`, "an empty outline of its
  box") maps cleanly onto `PlaneSet::Painter::addLine` — four calls, no gap. An edge-heat band
  (`itemGlow`, "a soft band whose opacity rises with its count") is the one the plan's own `addBand`
  primitive ("a rectangle whose alpha falls off to one edge") was written for — and that primitive
  cannot be built from the existing solid-fill fragment path; see Problem 2.
- **Face-camera planes.** `faceCamera` is specified (§5.2) as "replaces the rotation in `toWorld`
  with the inverse of the camera's [rotation] at draw time," but `PlaneSet::draw()`'s stated
  signature takes only `worldToClip` (the combined `projection * view` matrix — the same type as
  `FrameContext::viewProjection`). A camera's pure rotation is not recoverable from that combined
  matrix without already knowing the projection parameters and un-mixing them, which is strictly
  more roundabout than just passing the camera's basis (or the `view` matrix alone) into
  `draw()`/`setState()`. This is a signature gap the presenter will hit on day one of implementing
  `faceCamera` — see Problem 3.
- **Translucency ordering across pipelines.** Within `PlaneSet`, planes sort back-to-front (§5.2).
  Within a document's pages, the renderer sorts documents back-to-front today and the plan extends
  this to sort *page batches* once any page is translucent (§6.2, confirmed accurate against
  `src/renderer.cpp:403-410`). Within `Beams`, nothing sorts at all — `addPath`/`add` append in call
  order and the whole buffer draws as one instanced call. **Nothing sorts *across* these three**
  into one global back-to-front order for a region. Depth-test-without-depth-write makes blending
  between any two mutually-transparent surfaces order-dependent by construction (that is what "test
  without write" *means*), so if a translucent `PlaneSet` plane and a translucent page genuinely
  overlap in a region, which one blends over the other depends on which of
  `PlaneSet::draw`/`Doc::collect`+`drawGlyphBatches`/`Beams::draw` the presenter happens to call
  later that frame — not on which is actually nearer the camera. The presenter needs one explicit,
  global translucent-submission order per region, not three independently-sorted lists submitted in
  whatever order is convenient to write.

______________________________________________________________________

## 4. Problems the plan has not seen

Ordered roughly by how much damage each would do if it surfaced only after `PlaneSet`/pose code
exists.

1. **One `GlyphBatch` per plane means one draw call per plane, and the plan already says so without
   drawing out the consequence.** §5.2: "`draw()` emits one `GlyphBatch` per visible plane."
   Confirmed from the type system: `DrawUniforms` (mvp, opacity, identity) is a **per-draw**
   uniform, not per-instance (`types.hpp:194-232`), so two planes with different transforms
   genuinely cannot share one draw today. `DeviceGL::drawGlyphs` rebinds the vertex buffer and
   re-issues `VertexAttribPointer`/`VertexAttribDivisor` for every attribute **on every single
   call** (`device_gl.cpp:770-797`) — there is no VAO caching across draws. For 10,000 planes (the
   plan's own probe target, §5.3) that is 10,000 buffer binds and ~50,000 attribute-pointer calls
   per frame on GL/GLES, which cannot be parallelized (GL/GLES have no `parallelCommandRecording`,
   confirmed by claim #13) and will very plausibly dominate a 16 ms frame budget on `llvmpipe`
   before 10,000 is even reached. Vulkan can spread *recording* cost across threads but still
   submits one `vkCmdBindVertexBuffers`+`vkCmdDraw` per plane, with a `vkCmdPushConstants` for the
   matrix each time. **Recommendation**: measure this (Spike 1) before writing any `PlaneSet`
   geometry code, and if the numbers are bad, the fix is architectural — pack several planes'
   transforms into per-*instance* vertex data (e.g. a small table of recent transforms indexed
   per-instance, or accept that planes sharing one static layout group draw as one batch) — not a
   tuning knob.

1. **`PlaneSet::Painter::addBand`'s "alpha falls off to one edge" cannot be produced by the existing
   solid-fill fragment path, contradicting §13.3's "no new shader or vertex format" claim as
   literally read.** `glyph.frag.glsl:92-93`: a solid (`vSolid`) quad writes
   `outColor = vec4(vFgColor, vOpacity)` — one flat colour and one *uniform, per-draw* alpha across
   the entire quad; there is no per-fragment gradient path for solid fills today. A workaround
   exists and costs nothing in shaders/vertex format: bake a 1-D gradient as a synthetic "glyph"
   into the atlas once (reusing `updateTextureLayer`, exactly as `GlyphCache` already does for real
   glyphs) and draw `addBand` as a **non-solid** quad sampling it through the atlas-texturing branch
   that already exists for real glyphs (`glyph.frag.glsl:94-104`). This is the right fix, but it is
   non-obvious, needs its own small design note, and is easy to discover only after someone tries to
   implement `addBand` as a flat quad and finds it can't fade.

1. **`faceCamera` needs the camera's own basis, which `PlaneSet::draw()`'s stated signature cannot
   supply.** See the presenter section above. The fix is mechanical (add a camera-rotation or
   `view`-matrix parameter to `draw()`/the state used to resolve `faceCamera`), but it is a real gap
   in the §5.2 API as specified, not a detail left for "bodies not shown."

1. **Nothing sorts translucent draws across `PlaneSet`, `Doc` page batches and `Beams` together.**
   See the presenter section. This is a correctness gap (wrong blend order under genuine overlap),
   not a performance one, and it will be invisible in every scene where the three kinds of
   translucent content don't actually overlap on screen — which is most test scenes unless one is
   built specifically to force it.

1. **`beam.vert.glsl`'s perpendicular-width vector degenerates for beams parallel to world Z, and
   the view system's own depth axis produces exactly that case.** `beam.vert.glsl:58-59`:

   ```cpp
   sideways = cross(run, vec3(0.0,0.0,1.0)); if (dot(sideways,sideways) <= 0.0) { /* collapse off-screen */ }
   ```

   The comment at line 56 blames this on "a beam of no length," but the real degenerate condition is
   *any* beam whose `to - from` is parallel to world Z, regardless of length. Stretch vanishing's
   own depth axis (§9.1.4: "Binding point `z` runs away from the viewer... starting at `p`'s own x
   and y") places a cell's z-neighbour at the *same x, y*, different z — an edge between them is
   exactly the degenerate case. A dimensional-pack rank bound to `z` has the same shape. This will
   manifest as edges that are present in the layout record but invisible on screen whenever the
   camera is near head-on to that axis, which is an easy thing to miss in a quick visual check and a
   nuisance to debug once report because "the edge is there, it's just zero-width."

1. **Document reflow unconditionally overwrites every page's matrix, and the plan's `PagePose` does
   not account for this.** `src/doc.cpp:1273`: the reflow tail loop calls `page.setModel(trans)` on
   every surviving page, computed purely from the vertical-column formula, with no check for any
   pose the page might be carrying. `Doc::newPage` (`doc.cpp:1698-1710`) and the rebuilt-range half
   of `reflowFrom` (`doc.cpp:1241`) both **read back** `prevPage.getModel()[3][1]` — the previous
   page's *live* matrix's Y translation — to decide where the next page in the column goes. If a
   page has been posed (flown out of the column per §10.3.2, say), the very next edit anywhere in
   that document will either (a) silently snap the posed page back into its column position, undoing
   the pose with no signal to the view layer, or (b) if the posed page is the one being read back
   from, use its *displaced* position as the baseline for every page stacked after it in that reflow
   pass, corrupting the whole column below it. The plan's stated regression gate —
   `doc_gap_test.cpp` and `onion_skin_test.cpp` staying green — does not catch this, because neither
   file currently sets a pose and then reflows.

1. **`Page::setModel()` is already public today, so "only the `Doc` writes it" is a convention, not
   an enforced invariant — and nothing in the new API changes that.** `Page : public Drawable`
   (`doc.hpp:155`), and `Drawable::setModel`/`getModel` are public (`drawable.hpp:13-14`). Any
   caller can already bypass the whole flow/pose system by calling `page->setModel()` directly;
   `PagePose` adds a second, parallel way to move a page (`setPose`/`animatePoseTo`) without closing
   the first, so the two can race or silently desync (`pose()` saying one thing, the actual drawn
   matrix saying another) unless the plan explicitly says `setModel` becomes private/removed, or
   `setPose` becomes the single source of truth that writes through to `model` and the Doc's flow
   code stops calling `setModel` directly too.

1. **The picking-scope budget is shared, global and smaller than "thousands of planes" suggests at
   first glance.** The 14-bit doc/page fields that an overlay "scope" is built from are shared
   across the *whole* application — `allocateOverlayPickScope` cycles through `(1<<13)-1 ≈ 8191`
   frame-local scopes, and `allocatePersistentOverlayPickScope` hands out at most `1<<13 = 8192`
   persistent ones before throwing `std::length_error` (`render_state.hpp:61-74`). A single
   `PlaneSet` using one scope for all its planes (cluster field distinguishes the planes,
   comfortably up to 65535 of them) is fine. The risk is if the view system's own design — several
   placements, several panes, embedded placements each "its own scene and pane" (§11.5) — ends up
   minting one persistent scope *per placement* or *per pack* rather than one per `PlaneSet`
   instance; that consumes the same global 8192-scope budget every other `ui::Widget`-based control
   in the whole program (toolbars, doc switcher, radial menu, media widgets) already draws from.

1. **The GL entry-point table has none of the calls this plan needs, and it is worth saying so
   plainly rather than discovering it mid-WP3.** Grepping `src/render/gl/gl_api.cpp`'s
   `GLEDITOR_RESOLVE` list confirms `Scissor`, `DepthMask`, and both `DepthRangef`/`DepthRange` are
   **absent**; only `Viewport`, `Enable`/`Disable`, `DepthFunc`, `BlendFunc`, `Clear`/`ClearColor`
   exist today. This is a complete, not incremental, addition to the loader, and `resolveOptional`
   (used for the `GL_KHR_debug` entry points, `gl_api.cpp:24-28,152-165`) vs. the hard-failing
   `resolve` (used for everything else, `gl_api.cpp:30-38`) is the choice to make for
   `glDepthRangef` specifically, given WP4's note about it possibly being absent on some desktop GL
   contexts.

1. **The multi-region/multi-pane test scenes the plan asks `tools/compare-backends.sh` to grow do
   not yet have anything to drive them.** The `gleditor` binary's current CLI (`--pick`, `--click`,
   `--select`, `--toast`, `--screenshot`, confirmed by reading `tools/compare-backends.sh`)
   addresses one whole-window scene at a time; there is no existing way to describe "two tiled
   regions and a third embedded in one of them" from the command line. The plan names the *scene* as
   a deliverable of WP3/WP4 but the *harness* that would let `compare-backends.sh` build and drive
   such a scene is new surface on `apps/gleditor` (or a dedicated test tool) that isn't itemized
   anywhere in the plan's own "tests added" lists.

______________________________________________________________________

## 5. Spikes

All headless (`SDL_VIDEODRIVER=offscreen`), all runnable with tools/tests that already exist or are
a thin, throwaway extension of one.

1. **Draw-call scaling.** Extend `tools/layout-latency-probe.cpp` (or a disposable harness) to push
   synthetic `GlyphBatch`es — reusing one existing buffer/pipeline, no `PlaneSet` code required —
   through `device->drawGlyphBatches()` for N = 100 / 1,000 / 10,000 / 50,000, on GL and Vulkan
   (`llvmpipe`/ `lavapipe`), and report wall-clock per frame. *Pass criterion*: frame time stays
   under budget (pick a number from Performance Gates below) at the plane counts a *realistic*
   on-screen scene produces — get that count from `view-system.md`'s own windowing language (stretch
   vanishing tiles "until the viewport is tiled," §9.1.3 — dozens to low hundreds of boxes on a
   normal window, not 10,000; 10,000 is a stress ceiling, not the target workload). Retires Problem
   1 before a line of `PlaneSet` geometry code exists.
1. **`addBand` via a baked gradient glyph.** Hand-build one gradient texel row into the glyph atlas
   with `updateTextureLayer`, draw it as a non-solid quad with a stretched UV range, capture it the
   way `compare-backends.sh` already captures PPMs, and check the alpha ramp numerically against a
   reference smoothstep curve. *Pass criterion*: a visible, smooth falloff with zero shader or
   vertex-format changes. Retires Problem 2.
1. **Beam-along-Z degeneracy.** Add one beam whose `from`/`to` differ only in Z to a small headless
   scene (or a unit test driving `Beams` directly, the way `tests/lib/beams.cpp` already does),
   confirm it renders as nothing, then try a candidate fix (fall back to a different reference axis,
   e.g. `cross(run, worldUp)` or the camera's own right vector, when `cross(run, worldZ)` is near
   zero) and confirm the same beam becomes visible while every existing in-plane beam capture stays
   byte-identical. Retires Problem 5.
1. **Can a camera's rotation be recovered from `worldToClip` alone?** A ten-line unit test: build a
   known off-centre-orbit camera, compute `worldToClip`, try to recover the camera's right/up/front
   basis from it alone, and assert the attempt is ambiguous/impossible for a general perspective
   projection. No device needed; runs in `gleditor_test`. *Pass criterion*: the test either proves
   the gap (expected, confirming Problem 3 and that `draw()`'s signature needs a camera-basis
   parameter) or finds a closed-form recovery (which would retire the Problem instead).
1. **Does reflow clobber a posed page, today, with zero new code?** `Page::setModel()` is already
   public (claim/Problem 7) — take an existing multi-page `Doc` in a test, call
   `page->setModel(someMatrix)` directly on a middle page, trigger any edit that causes
   `reflowFrom`/`buildPendingPages` to touch that page or later ones, and assert whether the moved
   matrix survives. *Pass/fail is itself the finding*: a failure (expected) is concrete,
   reproducible proof of Problem 6/7 before any `PagePose` code is written, and scopes the exact fix
   (gate the `setModel` calls at `doc.cpp:1241,1273,1698` behind a pose check) from the test's own
   assertion rather than from guesswork.

______________________________________________________________________

## 6. Performance gates

**Tools to use**: `tools/ui-text-baseline.cpp` is the existing model to copy — its `percentile()`
helper (`ui-text-baseline.cpp:240`) and its per-frame sample columns (frame ms,
layout/shaping/fallback call counts via `gleditor::text::ShapingStatsScope`, input bytes) are
exactly the shape a `PlaneSet` probe needs, and the plan's own §5.3/§14 ask for a probe "modelled
on" it. `tools/layout-latency-probe.cpp` is named by the plan for the per-view layout-latency work
(toss time, derivation cost) and should gain the 100/1,000/10,000-plane mode directly. For
upload-byte counting, reuse the pattern already implemented in `tests/lib/mocks/world_device.hpp`'s
`WorldRecordingDevice::uploads` (sums every `updateBuffer` call's byte count) rather than inventing
a new counting mechanism.

**What to measure, every gate**:

- frame time p50/p95 on OpenGL, OpenGL ES and Vulkan (`llvmpipe`/`lavapipe` headless);
- draw-call count per frame (the size of the list handed to `drawGlyphBatches`, plus the handful of
  `Doc`/`Beams` calls) — a number the plan never asks for explicitly but that Problem 1 makes the
  single most important one to chart against plane count;
- upload bytes on a settled frame (pure camera/position motion, nothing edited) — target **exactly
  zero**, per V-R44 and the plan's own `plane_set_test.cpp` assertion;
- shaping/layout call counts via `ShapingStatsScope` on the same settled frame — target **zero**,
  the other half of V-R44;
- toss time (`ViewManifold::toss`) at 0 / 10³ / 10⁶ derived cells — target: flat, per
  `view-system.md` §6.5's own "how the claim is tested" paragraph.

**Numbers that should stop the work and trigger a redesign, not a tuning pass**:

- **Any non-zero upload or shaping count on a settled, motion-only frame.** This is a correctness
  bug against V-R44, not a performance number to optimize later.
- **Any non-flat toss time curve from 10³ to 10⁶ derived cells.** Per `view-system.md` §6.5, this
  would mean the `d.store-refs` fallback scan it names (`Manifold::dimensionNamed`,
  `manifold.cpp:785`) crept back into the hot path unguarded — a regression to fix, not a budget to
  absorb.
- **Draw-call count per frame exceeding roughly 2,000–3,000 for anything meant to be a normal, not
  deliberately worst-case, scene on GL/GLES.** At that point Problem 1's per-draw CPU overhead
  (`device_gl.cpp:770-797`'s per-attribute pointer setup, repeated every draw with no VAO caching)
  is likely to exceed a 16 ms budget on `llvmpipe` well before the plane count gets anywhere near
  the plan's own 10,000-plane probe ceiling — and the fix at that point is architectural
  (per-instance transform packing), not a smaller gap or a faster inner loop.
- **p50 frame time on `llvmpipe`/`lavapipe` exceeding ~33 ms (the 30 fps floor) for the view
  system's own documented "normal" scene** (e.g. stretch vanishing fully tiling a default-sized
  pane). Software rasterizers are not an edge case for this codebase — `AGENTS.md`'s
  headless-everything rule and `LIBGL_ALWAYS_SOFTWARE=1` mean this *is* the CI and the over-SSH
  reality every test in this repo already runs under, so a design that only performs on a real GPU
  cannot be validated by this repo's own gates and needs to be redesigned, not footnoted as a known
  slow path.
