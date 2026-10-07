# World-Space Rendering: An Implementation Plan

Status: plan, unbuilt. Date: 2026-10-07.

## Purpose

[`view-system.md`](view-system.md) places cells, pages, edges and labels in world space under a real
camera, in several panes. The library can already draw planes through a camera. It cannot yet turn a
pointer position back into the world, confine a draw to a rectangle of the target, give one draw a
slice of the depth range, or place many small planes cheaply. This plan adds those.

Everything here is generic and goes in `libgleditor` (`src/`, `include/gleditor/`). Nothing in it
names a cell, a link, a xanadoc or a view; the plain editor can use all of it. Every test is in
`tests/lib/` and runs in `gleditor_test`. No xanalogical test repeats them.

Three workstreams were asked for by name: **unprojection that matches the projection code**,
**device scissor** and **depth range**. Four more are what the view system needs besides, and are
marked as additions: an "entirely inside" frustum test, depth test without depth write, placed
planes, and a matrix of its own for every page.

## 1. Conventions that exist and must not change

Code is cited at commit `af5f1d5`. The API here follows the conventions of `view-system.md` §8: no
sentinel values, `function_ref` for callbacks that are not kept, and setters that return their
object.

| Fact                          | Where                                                                                                                                                                                                                                                                                                  |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| world to clip                 | `glm::perspective(fov, aspect, 0.1, 10000) * glm::lookAt(...)`, `src/renderer.cpp:373-379`; handed to contributors as `FrameContext::viewProjection`                                                                                                                                                   |
| that matrix's convention      | OpenGL's: after the divide x, y and z are in [−1, 1], y up, z = −1 at the near plane                                                                                                                                                                                                                   |
| OpenGL and GLES               | use the matrix as given; depth test `GL_LEQUAL`, `src/render/gl/device_gl.cpp:111`                                                                                                                                                                                                                     |
| Vulkan                        | rewrites each draw's matrix: y negated, and `z′ = (w − z) / 2`, which is reversed Z — near is 1, far is 0 — over the same clip volume; `src/render/vulkan/device_vk_frame.cpp:318-333`. Depth compare `GREATER_OR_EQUAL`, `device_vk_resources.cpp:807`; depth cleared to 0, `device_vk_frame.cpp:108` |
| Vulkan viewport and scissor   | dynamic state, set to the whole target at the start of every secondary command buffer, `device_vk_frame.cpp:162-171`, `device_vk_resources.cpp:836`                                                                                                                                                    |
| pixels produced by projection | bottom-up: `spatial::projectToScreen` and `ui::projectPlane` both put the origin at the bottom left                                                                                                                                                                                                    |
| pixels taken from the pointer | window coordinates, top-down; `requestPickingTag` takes those, `src/app.cpp:1451`                                                                                                                                                                                                                      |
| window units to pixels        | `contentScale`, in `FrameContext::metrics`                                                                                                                                                                                                                                                             |
| per-draw state                | `DrawUniforms{mvp, opacity, identity}` per `GlyphBatch`, `include/gleditor/render/types.hpp:204`, `:242`                                                                                                                                                                                               |
| depth test and depth write    | one flag, `PipelineDesc::depthTest`; Vulkan sets both from it, `device_vk_resources.cpp:802-803`                                                                                                                                                                                                       |
| recorded draws                | `drawGlyphBatches` may record a run of batches on worker threads                                                                                                                                                                                                                                       |

Two existing helpers have limits this plan works round and does not change:

- `projectToScreen` returns (0, 0) for a point behind the camera, which a caller cannot tell from a
  real corner of the screen (`include/gleditor/spatial.hpp`).
- `outsideFrustum` tests the four side planes only (`include/gleditor/draw_budget.hpp`).

**The rule that follows.** New code works in the backend-neutral clip space — the matrix the
renderer builds — and in bottom-up pixels. Each backend goes on adapting exactly where it adapts
today, inside the device. Nothing above the device learns that Vulkan flips y or reverses depth.

## 2. Unprojection

### 2.1 What is added

Header-only, in `include/gleditor/spatial.hpp`, beside `projectToScreen`:

```cpp
namespace gleditor::spatial {

/// A rectangle of the target in bottom-up pixels: a pane, or all of it.
struct Viewport {
  float left{}, bottom{}, width{}, height{};
};

struct Ray {
  glm::vec3 origin{};
  glm::vec3 direction{}; // unit length
};

/// Pointer coordinates (window units, top-down) to bottom-up pixels.
[[nodiscard]] constexpr glm::vec2 windowToPixels(float x, float y,
                                                 float windowHeight,
                                                 float contentScale) noexcept;

/// The pixel a world point lands on, and its depth in [-1, 1]. Nothing for a
/// point behind the camera -- no sentinel.
[[nodiscard]] std::optional<glm::vec3>
projectToViewport(const glm::mat4 &worldToClip, const glm::vec3 &world,
                  const Viewport &viewport) noexcept;

/// The ray through a pixel. Nothing if the matrix cannot be inverted.
[[nodiscard]] std::optional<Ray>
unprojectToRay(const glm::mat4 &worldToClip, glm::vec2 pixel,
               const Viewport &viewport) noexcept;

[[nodiscard]] std::optional<glm::vec3>
intersectPlane(const Ray &ray, const glm::vec3 &point,
               const glm::vec3 &normal) noexcept;
/// Where a ray meets a flat box centred on a plane's origin, in the plane's
/// own coordinates; nothing if it misses.
[[nodiscard]] std::optional<glm::vec2>
intersectQuad(const Ray &ray, const glm::mat4 &planeToWorld, float halfWidth,
              float halfHeight) noexcept;
[[nodiscard]] float distanceToSegment(const Ray &ray, const glm::vec3 &from,
                                      const glm::vec3 &to) noexcept;

} // namespace gleditor::spatial
```

### 2.2 How it matches the projection

`projectToViewport` is the forward map: multiply, refuse `w ≤ 0`, divide, then
`pixel = (left + (x·½ + ½)·width, bottom + (y·½ + ½)·height)`. With the viewport set to the whole
target it gives what `projectToScreen` gives for every point in front of the camera.

`unprojectToRay` is its inverse and uses the same three steps backwards: pixel to normalised device
coordinates; the points at depth −1 and +1 through the inverse of `worldToClip`, each divided by its
`w`; origin at the near point, direction towards the far one. The inverse is computed in double
precision: the near and far planes are five orders of magnitude apart, and a single-precision
inverse of that matrix loses the digits a far point needs.

It must not undo Vulkan's flip. That rewrite happens inside the device, to a copy of the matrix; the
matrix a contributor holds, and the pixels it is told about, are the neutral ones on every backend.

`projectToScreen` stays, since it has callers, and is documented as the form with a sentinel. New
code uses `projectToViewport`.

### 2.3 Entirely inside (addition)

In `include/gleditor/draw_budget.hpp`, beside `outsideFrustum`:

```cpp
/// True when a flat box centred on the model origin lies wholly within the
/// view: every corner in front of the camera and inside all six planes.
[[nodiscard]] inline bool insideFrustum(const glm::mat4 &mvp, float halfW,
                                        float halfH) noexcept;
```

It can test depth where `outsideFrustum` chose not to, because it tests in the neutral clip space,
where the volume is `−w ≤ z ≤ w` whatever the backend later does with it. A plane that crosses the
near plane is cut, so it is not inside.

### 2.4 Tests

`tests/lib/spatial_unproject_test.cpp`:

- round trip: for a grid of pixels, several camera poses, fields of view and aspect ratios,
  `projectToViewport(intersectPlane(unprojectToRay(pixel)))` returns the pixel within a hundredth;
- the ray through the centre of the viewport is the camera's forward direction;
- a point behind the camera projects to nothing; a singular matrix unprojects to nothing;
- a viewport that is a sub-rectangle gives the same world ray as the whole target does for the same
  physical pixel under a camera built for that sub-rectangle;
- `windowToPixels` at content scales 1, 1.5 and 2;
- agreement with `projectToScreen` for every point in front of the camera.

`tests/lib/draw_budget.cpp` gains `insideFrustum` cases: wholly inside, straddling each of the six
planes, wholly outside, behind the camera.

Backend agreement is checked where it can only be checked, on a device: a scene in
`tools/compare-backends.sh` draws planes at known world positions and asserts that a pick requested
at `projectToViewport(centre)` returns that plane on OpenGL, GLES and Vulkan.

## 3. Render regions: scissor and depth range

### 3.1 One concept for both

A pane needs its draws confined to a rectangle and, when it overlaps another, kept in front of it.
Both are properties of *where a draw goes*, so they are one thing:

```cpp
// include/gleditor/render/types.hpp
struct RenderRegion {
  /// Where normalised device coordinates map to: bottom-up pixels. May reach
  /// outside the target.
  int viewLeft{}, viewBottom{}, viewWidth{}, viewHeight{};
  /// What may be written: bottom-up pixels, clamped to the target.
  int clipLeft{}, clipBottom{}, clipWidth{}, clipHeight{};
  /// The slice of the depth range this region's draws occupy. 0 is nearest.
  float nearDepth{0.0F}, farDepth{1.0F};
};
using RegionId = std::uint16_t;

struct GlyphBatch {
    // ...
  std::optional<RegionId> region; // none: the whole target at full depth
};
```

```cpp
// include/gleditor/render/device.hpp
/// Valid until endFrame(). The table is small and rebuilt each frame.
virtual RegionId defineRegion(const RenderRegion &region) = 0;
/// For draws issued immediately rather than recorded as batches. None is
/// the whole target. Returns the device, as every setter here does.
virtual RenderDevice *setRegion(std::optional<RegionId> region) = 0;
```

`DeviceCapabilities` gains `regions`. The view and clip rectangles are separate because an embedded
view can lie partly off its parent: its projection must still map to its full rectangle while only
the visible part is written.

**Why a region is data on the draw and not a state of the device.** Batches are recorded now and
replayed later, possibly on other threads. A "current scissor" would be whatever it had become by
then. A batch that names its region is right whenever it is replayed.

### 3.2 Device scissor

- **OpenGL and GLES.** When the region changes: `glViewport(view…)`, `glEnable(GL_SCISSOR_TEST)`,
  `glScissor(clip…)`. Both take bottom-up pixels, which is what a region holds. No region disables
  the scissor test and restores the full viewport. `src/render/gl/gl_api.cpp` gains the entry
  points. The state is reset in `beginFrame()`.
- **Vulkan.** Viewport and scissor are already dynamic state, and each secondary command buffer
  already sets both at its start (`device_vk_frame.cpp:162-171`). That is the place: set them from
  the batch's region, and again when a later batch in the same buffer names another. Vulkan's
  framebuffer rectangle is top-down, so `offset.y = targetHeight − clipBottom − clipHeight`, and the
  same for the viewport's `y`. No pipeline is rebuilt.

The picking attachment is part of the same pass, so a region confines picks as it confines colour,
and `requestPickingTag` is unchanged. `captureColorTarget` is unchanged.

### 3.3 Depth range

A region's `[nearDepth, farDepth]` is stated once, with 0 nearest, and each backend maps it to its
own convention:

- **OpenGL and GLES**: `glDepthRangef(nearDepth, farDepth)`.
- **Vulkan**, whose depth is reversed: the viewport's `minDepth = 1 − farDepth` and
  `maxDepth = 1 − nearDepth`, so the nearest thing in the region still has the greatest depth value
  and `GREATER_OR_EQUAL` still means "nearer".

A caller says "this region is in front of that one" and never learns which way a backend counts.

**Use slices sparingly.** Tiled panes do not overlap; the scissor separates them, and they all use
the whole range. Only a region that overlaps another needs a slice: an embedded view, or chrome
drawn over a scene. That matters on OpenGL, where depth is fixed-point and a slice of a tenth of the
range has a tenth of the steps; on Vulkan's reversed floating-point depth there is room to spare.
The existing hazard — glyphs a tenth of a unit in front of their paper, which is why OpenGL uses
`GL_LEQUAL` — is the case to test inside a thin slice.

An alternative was weighed: clear depth inside the front region's rectangle and reuse the whole
range. It costs no precision, but it requires every draw of the back region — glyph batches, beams,
canvases — to be submitted before the clear, which the recorded, multi-threaded batch path does not
promise. Depth slices need no ordering.

### 3.4 Tests

`tests/lib/render_region_test.cpp`, against the mocks in `tests/lib/mocks/`: no region is the whole
target; a region is valid for one frame; a batch's region survives recording; clip rectangles are
clamped. `tests/lib/device_capabilities.cpp` gains the capability.

On devices, one new scene in `tools/compare-backends.sh` — two tiled regions and a third embedded in
one of them:

- no pixel outside a region's clip rectangle is changed by that region's draws;
- a marker drawn at the bottom left of a region is at the bottom left on every backend, which pins
  Vulkan's flipped rectangle;
- the embedded region's content is in front of its host's at every world depth;
- a far page with glyphs, inside a slice of a tenth of the range, still shows its glyphs;
- picks inside each region return that region's identities;
- the capture pairs match across OpenGL and GLES exactly and Vulkan within the existing threshold;
- strict diagnostics (`setStrictDiagnostics`) report nothing.

## 4. Depth test without depth write (addition)

`PipelineDesc` gains `depthWrite{true}`, separate from `depthTest`. OpenGL: `glDepthMask`. Vulkan:
`depthWriteEnable` no longer copied from `depthTest`. Translucent planes — a deck's faded pages, a
faded cell — are drawn with the test and without the write, after the opaque ones, back to front.

*Tests:* a translucent plane behind an opaque one is hidden; in front, it blends; two translucent
planes drawn back to front both show.

## 5. Placed planes (addition)

### 5.1 The need

A `Canvas` is one plane with one transform. A `WorldPanel` is one plane with a widget layout and a
pool of canvases. A view places hundreds of small planes, each with its own position, orientation
and opacity, and moves them every frame of an animation. The library already draws many planes that
way — a document's pages are batches sharing a buffer, each with its own matrix and opacity — but
only for pages.

### 5.2 `ui::PlaneSet`

```cpp
// include/gleditor/ui/plane_set.hpp
namespace gleditor::ui {

using PlaneId = std::uint32_t;


struct PlaneState {
  glm::mat4 toWorld{1.0F};
  float opacity{1.0F};
    bool faceCamera{}; // turn the plane to the viewer; position and scale kept
  bool visible{true};
  /// When set, toWorld is relative to that plane, which carries this one.
    std::optional<PlaneId> parent;
};

/// Many retained planes in shared buffers, each drawn with its own state.
class PlaneSet {
public:
  PlaneId add(Size size);
    PlaneSet *remove(PlaneId plane);

  /// Rebuild one plane's content. Only that plane's range is uploaded.
    class Painter; // addRect, addLine, addImage, addText(box, FittedText),
                 // addBand: a rectangle whose alpha falls off to one edge
  Painter paint(PlaneId plane);

  /// Uniforms only: moving, fading and turning a plane uploads nothing.
    PlaneSet *setState(PlaneId plane, const PlaneState &state);
    PlaneSet *setPick(PlaneId plane, render::PickingTag tag,
               std::shared_ptr<const render::PickSemanticTarget> target);

    void draw(RenderState &state, const glm::mat4 &worldToClip,
            const spatial::Viewport &viewport,
            std::optional<render::RegionId> region,
            const LabelLodPolicy &lod);

  /// The plane's projected box, for a caller's accessibility bounds.
  [[nodiscard]] std::optional<ProjectedPlane>
  projected(PlaneId plane, const glm::mat4 &worldToClip,
            const spatial::Viewport &viewport) const;
};

} // namespace gleditor::ui
```

- One vertex stream for the set; a plane owns a range of it, with its rectangles and its text in
  separate sub-ranges.
- `draw()` emits one `GlyphBatch` per visible plane. Opaque planes go first, front to back, with
  depth write; translucent ones after, back to front, without.
- `faceCamera` replaces the rotation in `toWorld` with the inverse of the camera's at draw time. It
  is a matrix, so there is no shader change and no new vertex attribute.
- For each plane `projectPlane` and `labelLOD` decide whether its text sub-range is drawn. The
  rectangles are drawn regardless, so a plane too small to read is still there to see and to pick.
- Text arrives already fitted (`FittedText`), so the set never shapes.
- A plane with a parent is drawn at `parent × own`. Moving the parent moves its children with no
  further call, which is how a group of planes is kept glued; opacity multiplies down the same way.

`WorldPanel` and `Canvas` are unchanged. A `WorldPanel` could later sit on a `PlaneSet`; that is not
part of this plan.

### 5.3 Tests

`tests/lib/plane_set_test.cpp`: repainting one plane uploads one range; `setState` uploads nothing
and a `ShapingStatsScope` over a frame of pure motion reports zero; draw order is as stated; a plane
below the legibility threshold keeps its rectangle and its pick; `faceCamera` yields a plane normal
to the view direction at its own position; removal frees the range for reuse; moving a parent moves
its children and uploads nothing. A probe modelled on `tools/ui-text-baseline.cpp` reports frame
time and upload counts for 100, 1,000 and 10,000 planes.

## 6. A matrix for every page (addition)

### 6.1 The need

A `Doc` has a model matrix, and moving it moves the document as a whole (`Doc::animateMoveTo`). A
`Page` has a matrix too, relative to its document, but only the `Doc` writes it, and always as one
vertical column (`src/doc.cpp`). A deck of pages receding in depth, or one page lifted out of its
column while the rest stay, needs a page to be movable on its own, relative to the document it
belongs to.

### 6.2 The change

A page owns its placement, as a document owns its own:

```cpp
// include/gleditor/doc.hpp, on Page
struct PageBand {
  float topPx{}, bottomPx{};
};
struct PagePose {
  glm::mat4 toDocument{1.0F}; // the page's plane, relative to its document
    float opacity{1.0F};
  bool visible{true};
  /// Draw only this band of the page, measured from its top: the lines in
  /// it, on a strip of paper. None: the whole page.
  std::optional<PageBand> band;
};

/// Where the document's own flow would put this page. Always available.
[[nodiscard]] PagePose flowPose() const;
/// Where the page is. The flow pose until someone says otherwise.
[[nodiscard]] const PagePose &pose() const noexcept;
Page *setPose(const PagePose &pose);
/// Tween from the current pose; the same timeline documents move on.
Page *animatePoseTo(ch::Timeline &timeline, const PagePose &target,
                    double seconds, double delay = 0.0);
/// Back to the flow.
Page *clearPose();
```

```cpp
// on Doc
/// Pages to build first, nearest need first. The default is the pages near
/// the viewport in the flow; a caller that has moved pages knows better.
Doc *setBuildPriority(std::span<const std::size_t> pages);
```

- A page with no pose set behaves exactly as today, and so does a document none of whose pages has
  one.
- The page's world transform is `document × page`, everywhere: `Doc::collect`, `PageFrame`
  (`doc.hpp:948-956`), the caret, selection, hit testing. They compose
  `modelMatrix() * page->getModel()` today, so they keep doing so and the page's matrix is what
  changed. A caret is therefore where its page is.
- A band draws a sub-range of the page's glyph instances — a batch already names an offset and a
  count (`GlyphBatch`) — and a strip of paper behind them. The page's text, caret and picking are
  unchanged; it is the same page, seen through a slot.
- Opacity is the per-draw uniform a page batch already has (`DrawUniforms::opacity`), multiplied by
  the document's.
- The renderer sorts *documents* back to front today (`src/renderer.cpp`, before `doc->collect`).
  When any page is translucent it sorts *page batches* by depth instead, and draws the translucent
  ones without depth write (§4).

The first draft of this plan had a document ask an arrangement object where each page goes. Poses on
the page are simpler and say what is true: a page is a thing with a place, and its document is its
frame of reference.

### 6.3 Tests

`tests/lib/doc_page_pose_test.cpp`: with no pose set, every page's matrix is what `Doc` gives today,
so `doc_gap_test.cpp` and `onion_skin_test.cpp` hold unchanged; a page given a pose is drawn,
picked, and has its caret, where the pose puts it; moving the document moves a posed page with it
and leaves its pose unchanged; `clearPose()` returns it to the flow; an invisible page produces no
batch; a banded page draws only the lines inside its band and is picked only there;
`setBuildPriority` changes build order; translucent pages are drawn back to front.

## 7. Pane tree (addition)

`include/gleditor/ui/pane_tree.hpp`: a tree of splits over `ui::Rect`, built on `ui::split()`.

```cpp
namespace gleditor::ui {
using PaneId = std::uint32_t;
class PaneTree {
public:
  PaneId root() const noexcept;
  PaneId split(PaneId pane, Axis axis, float firstShare = 0.5F); // returns the new pane
    PaneTree *close(PaneId pane);
  PaneTree *resize(PaneId pane, float share);
  /// Leaf rectangles for the given bounds, edge-rounded so neighbours meet.
    void rects(Rect bounds,
             gleditor::cpp26::function_ref<void(PaneId, Rect)> visit) const;
  std::span<const PaneId> order() const noexcept; // focus order
};
} // namespace gleditor::ui
```

It draws nothing and knows nothing of focus; a host registers each leaf with
`FocusManager::addPane()` and turns each rectangle into a `RenderRegion`.

*Tests:* `tests/lib/ui_pane_tree_test.cpp`: splits partition the bounds exactly; closing gives the
space to the sibling; order is stable under resize.

## 8. Order of work

| Step | Work                                             | Depends on | Gate                                          |
| ---- | ------------------------------------------------ | ---------- | --------------------------------------------- |
| 1    | `projectToViewport`, `unprojectToRay`, ray tests | —          | §2.4 unit tests                               |
| 2    | `insideFrustum`                                  | —          | §2.4                                          |
| 3    | regions with scissor, on all three backends      | —          | §3.4, without the depth cases                 |
| 4    | depth slices in regions                          | 3          | §3.4 in full                                  |
| 5    | `depthWrite`                                     | —          | §4                                            |
| 6    | `ui::PlaneSet`                                   | 1, 3, 5    | §5.3; a planes scene in `compare-backends.sh` |
| 7    | page poses                                       | 5          | §6.3; existing document tests unchanged       |
| 8    | `ui::PaneTree`                                   | —          | §7                                            |

Steps 3 and 4 land on OpenGL, GLES and Vulkan in the same change, so the backends never disagree
about what a region means. Every step passes `make test`, `make lint`, and
`xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh` before the next begins.

## 9. Risks

| Risk                                                             | Handling                                                                               |
| ---------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| Vulkan region state inside secondary buffers recorded on workers | the region table is fixed before recording starts; a batch carries an id, not a state  |
| Vulkan's top-down rectangle against bottom-up regions            | the bottom-left marker test of §3.4                                                    |
| HiDPI: regions are pixels, pointers are window units             | `windowToPixels`; tests at content scale 1.5                                           |
| OpenGL depth precision in a thin slice                           | slices only for overlapping regions; the far-glyph test of §3.4                        |
| Desktop OpenGL without `glDepthRangef`                           | fall back to `glDepthRange`                                                            |
| The WebAssembly and Android builds, which take the GLES path     | the loader gains the same entry points; `packaging/wasm/build.sh` is built in the gate |
| A single-precision inverse losing a far point                    | the inverse is in double; the round-trip test covers the far plane                     |
| A posed page parting from its caret or its pick                  | the caret and pick tests of §6.3                                                       |
| Draw count with thousands of planes                              | the `PlaneSet` probe of §5.3 before the view system depends on it                      |

## Change history

- 2026-10-07 — Initial plan.
- 2026-10-07 — Pages get their own poses in place of an arrangement object (§6); `PlaneSet` gains
  parent planes and soft bands (§5).
- 2026-10-07 — No sentinels, chaining setters and `function_ref` visitors throughout; a page pose
  can carry a band, so a page can be shown as a window round a passage.
