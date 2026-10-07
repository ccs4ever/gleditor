# A Pluggable View System for Xanadocs and ZigZag Slices

Status: proposal. Date: 2026-10-07.

## Abstract

`ZigzagVisualizer` is a single 4,000-line class that owns one fixed-axis layout algorithm, and
`xanadu::Views` is a single coordinator that owns xanadoc presentation; neither exposes a seam
another layout strategy could plug into, and `apps/xuzz/view_coordinator.{hpp,cpp}` composes exactly
one of each, program-wide. This document specifies a `View` abstraction that sits between the store
and the renderer: a registry of swappable layout strategies, a `ViewManifold` that separates
configuration a user set (which dimensions are bound to which axes) from presentation geometry a
view minted to draw the current frame, and a per-frame pipeline (`layout` →
`AnimationState::advance` → draw adapter) that is pure, allocation-disciplined, and testable without
a GPU. It specifies, precisely enough to implement without further design work, three ZigZag views —
**stretch vanishing**, **all-dim walk**, and **dimensional pack view** — the `d.pack`/`d.packing`
dimension pair a pack view needs, and the one invariant every ZigZag view must hold: a cell has at
most one neighbour per dimension per direction, including every view-minted cell, at every moment,
even while dimensions are being rebound. It specifies a toss operation that is literally O(1) — a
generation counter increment — and separates it honestly from the O(visible) and O(discarded) work
that follows it but never blocks it. Xanadoc-side (`PageView`) layout algorithms are explicitly out
of scope; this document fixes only the seam a follow-up specification will fill.

## How to read this, for implementing agents

Section 3 (requirements) is the acceptance test; section 18 maps every clause of the user's original
request to the requirement, section, and ruling that satisfies it. Section 6 (the view space) and
section 8 (the API) are normative — every signature there is the one to implement, not an example.
Sections 9 (the three views) are the detailed algorithms; implement them against section 8's types,
not against the ASCII diagrams alone. Section 15 (migration) is the order of work; each step keeps
`make test` green on its own. Section 16 (rulings) records what was decided and why, including the
alternatives each one refused — read it before arguing with a decision made here, because the
alternative was very likely already considered and priced. Section 17 (open questions) is not this
document's job to close; do not block implementation on it.

______________________________________________________________________

## 1. Motivation

### 1.1 What exists today

`ZigzagVisualizer` (`apps/zigzag/zigzag_visualizer.hpp:110-114`) multiply inherits
`gleditor::FrameContributor`, `gleditor::PickObserver`, `gleditor::a11y::Source`,
`gleditor::ModalInput`, and `xanadu::ZigzagPresentationSurface`. There is no `View`/`Layout`
interface and no registry: one instance owns the manifold engine, the focus cell, the dimension
bindings, render-state caching, the palette, the command bar, cell editing, and drawing. Two view
modes exist — `enum class ViewMode { CellContent, Topology }`
(`apps/zigzag/zigzag_visualizer.hpp:191-200`) — both laid out by one function,
`rebuildActiveViewTopology()` (`apps/zigzag/zigzag_visualizer.cpp:1158-1390`), which walks exactly
three bound dimensions out to a fixed radius and stacks each newly discovered cell at
`offset = unitDir * spacing` from its parent: a recursive chain layout along three fixed axes, not a
general N-dimensional placement.

Binding is `ViewAxisBinding` (`apps/common/xanadu/zigzag/zzstructure.hpp:103-109`):

```cpp
struct ViewAxisBinding {
  DimID x_dimension = "d.1";
  DimID y_dimension = "d.2";
  DimID z_dimension = "d.3";
};
```

three named fields, no group, no list, no runtime count — the entirety of "dimension binding" is
three slots. `enum class DimensionBundle` (`zzstructure.hpp:111-165`) adds five hardcoded named
triples (`Execution`, `Scope`, `Contract`, `Logic`, `Stdlib`, `Custom`); it is the closest existing
analogue to a dimension group, and it is a closed C++ enum of exactly-three-dimension presets, not
data a user can build.

`apps/xuzz/view_coordinator.{hpp,cpp}` holds exactly one `xanadu::Views&` and one
`shared_ptr<zigzag::ZigzagVisualizer>` and toggles between `Unified`, `XanadocOnly`, `ZigzagOnly` —
one xanadoc view and one zigzag view, program-wide, with no pane/viewport manager. `xanadu::Views`
(`apps/xudu/views.{hpp,cpp}`, 2043+263 lines) is likewise one coordinator, not a pluggable
abstraction — it owns document switching, onion-skin comparison, camera framing, link UI, and
drawing directly.

### 1.2 Xuzz is the only application

`xudu` and `zigzag` are retired as programs. Commit `b48bf09` ("fold xudu and zigzag into unified
sovereign xuzz application") deleted `apps/xudu/main.cpp` and `apps/zigzag/main.cpp`; the Makefile
links `$(XUZZ_OBJS) $(XUDU_OBJS) $(ZIGZAG_OBJS)` into the one `build/xuzz`, and `build/xudu` and
`build/zigzag` are symlinks to it. `apps/xudu/` and `apps/zigzag/` survive only as directories of
components that `xuzz` links, and `apps/xuzz/view_coordinator.hpp` already includes both
(`xudu/views.hpp`, `zigzag/zigzag_visualizer.hpp`).

This document therefore specifies a view system *for xuzz*, and adds no code to either legacy
directory:

- the framework and the built-in views' pure layout code go in the engine,
  `apps/common/xanadu/view/`, where `xuzz_test` (which links the engine and no graphics device) can
  test them;
- everything that touches the renderer, input or the window goes in `apps/xuzz/`;
- `ZigzagVisualizer` and `xanadu::Views` are the things being replaced. Each migration step (§15)
  moves a responsibility out of them into a view or into `apps/xuzz/`, and the step that empties one
  deletes it. Nothing new is written against "the zigzag side" or "the xudu side" as a separately
  buildable unit, and no seam exists to keep the two apart: a slice view and a page view are two
  kinds of view in one program, distinguished by what they lay out (cells or pages), not by which
  application owns them.

`.claude/rules/architectural_governance.md` §1 still words its isolation rule in terms of
`apps/xudu` and `apps/zigzag` as applications. That wording predates the fold and should be revised
alongside this work; the rule that survives unchanged is that `src/` and `include/gleditor/` never
include `apps/`.

### 1.3 Why none of this hosts three new views

- **Stretch vanishing** needs per-cell content-fit placement, edge opacity, and a hard clip rule
  none of today's fixed-radius chain layout or binary frustum culling expresses
  (`include/gleditor/draw_budget.hpp:54-92`'s `outsideFrustum` is "fully outside," never "fully
  inside," and there is no partial-clip test at all).
- **All-dim walk** needs every dimension a cell participates in, bound or not, arranged in 3-D —
  `ViewAxisBinding`'s three slots have no room for an unbound dimension at all, and `rasterize()`'s
  radius walk never visits a dimension that is not one of the three bound ones.
- **Dimensional pack view** needs user-authored, uncapped dimension *groups* and a new kind of cell
  — a pack container — that `DimensionBundle`'s closed five-entry enum cannot represent and that no
  existing cell kind models.
- All three need view-minted cells (pack containers, ring-slot placeholders, axis-label anchors)
  that must never become real document structure and must disappear cheaply when a dimension is
  rebound. No such mechanism exists above `apps/common/xanadu/zigzag/arena_manifold.hpp`'s
  ephemeral-cell machinery, and nothing above it enforces that a view obeys the
  one-neighbour-per-direction invariant while minting those cells.
- No multi-pane compositor exists: `ViewCoordinator` toggles visibility of one zigzag presentation
  and one xanadoc view; "many xanadoc views and many zigzag views mixed freely in one screen" has no
  extension point to build on.

### 1.4 What already exists and is reused, not reinvented

- `ArenaManifold` (`apps/common/xanadu/zigzag/arena_manifold.hpp`) already gives copy-on-write
  overlay over a base `Manifold`, an `ephemeralBit` on every ref it mints, `mark()`/`release()`/
  `discard()` choice-point undo, and a federation (`Space`) mechanism — this is reused wholesale as
  the substrate for every view's derived state (§6.1).
- `cell_views.hpp`'s `CellGraph` concept, `RankView`, and `neighbours()` already work over both
  `Manifold` and `ArenaManifold` and are reused unchanged for every traversal a view needs.
- `dimensionsOf(CellRef)` (`manifold.hpp:468-469`) is the exact, uncapped "every dimension a cell
  participates in" primitive all-dim walk needs.
- `system_docs.hpp`'s `LayoutConfig::fromStore`/`SettingSpec` pattern is reused unchanged for every
  new tunable this document introduces.
- `d.clone`'s single-dimension, dual-direction idiom (`Manifold::cloneMaster`,
  `manifold.hpp:446-458`) is the structural precedent `d.pack` follows (§9.3.2).

______________________________________________________________________

## 2. Scope

This document covers the shared view framework (`apps/common/xanadu/view/`), the ZigZag-side
invariants and the `ViewManifold`, the three required ZigZag views, mixed-viewport compositing, and
the `PageView` seam. It does not design any xanadoc (page) view's layout algorithm — see ruling V12
and section 10.3.

______________________________________________________________________

## 3. Requirements

Each requirement is a single testable sentence. "MUST" requirements are acceptance-blocking;
"SHOULD" requirements are strongly preferred but not blocking. Requirement IDs are stable and are
the traceability table's (§18) left-hand keys.

### 3.1 Framework

- **V-R1.** A `View` MUST be installable through a single registration call
  (`ViewRegistry:: registerView`) with no enum, switch statement, or other closed list anywhere in
  the framework naming a specific view kind.
- **V-R2.** A pane MUST be able to hold any installed `View`, slice-backed or page-backed, chosen
  independently of every other pane's choice.
- **V-R3.** `View::layout()` MUST be a pure function of
  `(ViewManifold state, focus, viewport, tunables)` — no GPU call, no heap allocation outside a
  caller-owned arena, no mutation of animation or store state — and MUST be callable from a headless
  unit test with no rendering device.
- **V-R4.** Every per-frame data structure a `View` writes into (`LayoutSink`'s spans) MUST be
  caller-owned and reused across frames; growth MUST be logged (`GLEDITOR_LOG_DEBUG`), never silent,
  and MUST never be a hard-capped truncation that drops data a view would otherwise draw.

### 3.2 ZigZag view-space invariants

- **V-R5.** For every `(cell, dimension, direction)` where `cell` is real or view-minted and
  `dimension` is one this view currently treats as structural (bound axis, group member, or a
  view-owned bookkeeping dimension), the view's manifold MUST answer at most one neighbour — I1,
  §6.3.
- **V-R6.** A view-minted `CellRef` MUST be refused, by construction, as the target of any operation
  that would persist it into a real `Store` — I2, §6.3.
- **V-R7.** Rebinding a dimension MUST make every cell minted under the previous binding unreachable
  through a single, constant-time operation, independent of how many cells were minted — I3, §6.4.
  The constant-time operation and the proportional-cost work that follows it MUST be stated and
  tested separately (§6.4, §13).
- **V-R8.** Every view-minted cell MUST resolve, within a bounded number of hops, to a real
  `CellRef` that a renderer or editor can act on — I4, §6.3.
- **V-R9.** A view MUST NOT write a `DimLink` for a bound dimension onto a real cell's own slot for
  binding or display purposes — I5, §6.3; a view that needs "the effective posward neighbour on this
  axis is a pack, not the real neighbour" expresses that through view-owned dimensions, never by
  shadowing the real one.
- **V-R10.** The focus/cursor identity MUST be stored as a real `CellRef`, never a view-minted one,
  so that it survives a toss — I6, §6.3.

### 3.3 Mixing

- **V-R11.** A pane MUST be able to show a `SliceView` or a `PageView` with no special-casing at the
  pane-tree level beyond the `View` base interface.
- **V-R12.** A `SliceView` embedded inside, or beside, a `PageView` MUST expose only real `CellRef`s
  across that boundary (never a view-minted one) and MUST expose the focus cell and a
  transform/placement hook, matching what `BridgeCoordinator`/`ZigzagPresentationSurface` provide
  today.
- **V-R13.** Splitting, closing, and cycling focus between panes MUST NOT alter any other pane's
  camera, caret, or binding state.

### 3.4 Stretch vanishing

- **V-R14.** Every cell this view displays MUST be rendered at its full, untruncated content-fit
  size.
- **V-R15.** Only the accursed cell's immediate neighbours along currently bound dimensions MUST be
  exactly axis-aligned with it; cells at radius ≥ 2 MAY depart from strict grid alignment.
- **V-R16.** A cell whose content-fit box would be partially clipped by the viewport boundary MUST
  NOT be drawn at all (not faded, not cropped).
- **V-R17.** Cell opacity MUST decrease monotonically as a function of distance from the viewport
  centre, with a configurable floor.
- **V-R18.** Given the same manifold state, focus, and viewport, two `layout()` calls MUST produce
  identical cell positions (determinism).

### 3.5 All-dim walk

- **V-R19.** Every dimension the accursed cell participates in, bound or not, MUST appear in the
  layout, labelled by that dimension's own name.
- **V-R20.** Bound dimensions MUST occupy fixed screen/world axes; unbound dimensions MUST occupy
  ring positions arranged in three-dimensional space such that the accursed cell remains visually
  unoccluded as valence grows.
- **V-R21.** A dimension's assigned ring position MUST be stable across focus changes and across the
  addition or removal of other dimensions (no global reassignment).
- **V-R22.** Dragging a ring edge onto a bound axis, and an equivalent keyboard action, MUST produce
  the identical rebind outcome.
- **V-R23.** A ring position MUST display the far cell's own valence (its neighbour count across its
  own dimensions).

### 3.6 Dimensional pack view

- **V-R24.** A dimension group MUST be user-authorable (create, rename, add/remove member, delete),
  of any size, and MUST be bindable to a single axis exactly as a single dimension would be.
- **V-R25.** A real cell's posward pack along a bound group MUST be the set of cells reached by one
  more hop, from any already-packed cell, along any member dimension of the group, continuing until
  no such hop exists (the BFS-frontier rule, §9.3.3), with duplicates, cycles, and ragged ends
  handled without special-casing (§9.3.3).
- **V-R26.** Movement from a pack container to the next pack container MUST be single-valued in each
  direction, and moving posward then negward (or vice versa) MUST return to the starting cell or
  container (reversibility, §9.3.5 proves this for the chosen representation).
- **V-R27.** Every constituent of a pack MUST be individually retrievable (focusable as itself,
  outside the pack framing) without copying it.
- **V-R28.** A pack MUST be representable as a constituent of another pack (nesting), to an
  implementation-chosen but not model-limited depth.

### 3.7 Non-functional

- **V-R29.** No new system xanadoc setting introduced by this document MAY be a naked numeric
  literal in application code; each MUST be a `SettingSpec` field read from `system://layout` or
  `system://settings`, with a bidirectionally linked Schema & Purpose page and Notes page, no
  markdown syntax, native format links for headers.
- **V-R30.** Every new key binding introduced by this document MUST be defined in `system://keymap`
  with a default chord and dispatch through a registered Vortex-reachable action name, never a
  hardcoded key handler.
- **V-R31.** `View::layout()` and the draw adapter MUST perform zero dynamic heap allocation on the
  steady-state per-frame path (reused arenas only).
- **V-R32.** Every view-minted cell and every synthetic geometry record MUST have an accessibility
  role distinct from `cell` (`pack`, `label`, `group`, etc.) and MUST never be announced as though
  it were stored data.
- **V-R33.** Every behaviour this document specifies MUST be exercisable and testable headless (no
  window, no real display, no audio device), per `.claude/rules/headless_tests.md`.
- **V-R34.** Movement within a view (stepping, entering/leaving a pack, following a ring spoke) MUST
  append no operation to the visited store (R8); a completed transition is recorded as a `Visit` in
  the reader's `system://activity` store through the existing `xanadu::ActivityLog`
  (`apps/common/xanadu/link_navigation.hpp`, `store_activity_log.hpp`) — §8.7, §10.4.

______________________________________________________________________

## 4. Concepts and vocabulary

| Term            | Meaning                                                                                                                                     |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| View            | An installed, swappable layout+interaction strategy over either a slice or a xanadoc page.                                                  |
| Viewport        | A screen rectangle (`ViewportDesc`) plus an optional camera/view-projection override.                                                       |
| Pane            | One slot in a `ViewHost` pane tree; owns exactly one `Viewport` and one `View` instance.                                                    |
| Host            | `ViewHost`: the pane tree manager that composes panes into one program window.                                                              |
| Slice view      | A `View` refinement (`SliceView`) whose layout source is a ZigZag `Manifold`.                                                               |
| Page view       | A `View` refinement (`PageView`) whose layout source is xanadoc span/paragraph structure; this document only fixes the seam (§10.3).        |
| View manifold   | `ViewManifold`: a pane's binding arena and derived arena over one real base `Manifold`.                                                     |
| Binding layer   | The view manifold's arena that survives a toss: axis slots, `d.binds` links, group cells and their membership.                              |
| Derived layer   | The arena that does not survive a toss: packs, ring-slot placeholders, placement helpers, anything minted purely to draw the current frame. |
| View cell       | Any cell minted by a view (`ephemeralBit` set, carries a `ViewEpoch`).                                                                      |
| Axis            | One slot in a view's `ViewAxisSet`, bound to a real dimension or a group cell.                                                              |
| Bound dimension | A dimension currently linked, via `d.binds`, to an axis slot.                                                                               |
| Dimension group | A user-authored, named, ordered set of member dimensions, itself a cell, bindable to an axis like a single dimension.                       |
| Pack            | A view-minted container cell standing for one step of a group's BFS-frontier walk from a real cell or a previous pack.                      |
| Container       | A pack cell in its role as the thing constituents hang off (`d.pack` posward).                                                              |
| Constituent     | A cell reached by a pack's BFS step, held in the container's `d.packing` rank.                                                              |
| Valence         | The count of a cell's own `(dimension, direction)` neighbour pairs across every dimension it participates in.                               |
| Toss            | `ViewManifold::toss()`: the O(1) epoch bump that makes every derived-layer `ViewCellRef` stale at once.                                     |
| Epoch           | `ArenaManifold::ViewEpoch`: a monotonically increasing counter; a `ViewCellRef` carries the epoch it was minted under.                      |

______________________________________________________________________

## 5. Architecture

### 5.1 Layer diagram

```text
   apps/xuzz/                      (the one application; renderer, input, window)
     view_host_app.{hpp,cpp}       pane tree wiring, focus, gesture routing
     view_draw_adapter.{hpp,cpp}   layout records -> Canvas/Beams, picking, a11y
     view_commands.{hpp,cpp}       registers every view action for system://keymap
                  |
                  v
   apps/common/xanadu/view/        (engine: no graphics device, linked by xuzz_test)
     View, SliceView, PageView, ViewRegistry, ViewHost, ViewManifold,
     ViewAxisSet, mintViewLink, LayoutInput/LayoutSink, AnimationState
     builtin/  StretchVanishingView, AllDimWalkView, DimensionalPackView
                  |
                  v
   apps/common/xanadu/zigzag/{manifold,arena_manifold,cell_views}.hpp
   apps/common/xanadu/{store,store_activity_log,system_docs}.hpp

   include/gleditor/{canvas,beams,render/*}.hpp   <- used by apps/xuzz only
```

The built-in views sit in the engine because their work is derivation and geometry: cells in,
placement records out. The two things a layout needs from the graphics side — the size of a cell's
content and the viewport — arrive as data in `LayoutInput` (§8.4), the first through a
`function_ref` measurer, so a test supplies a fixed-size measurer and asserts on records without a
font, a window or a GPU.

### 5.2 Package map

```text
apps/common/xanadu/view/                 (NEW — engine)
  view.hpp               View identity/lifecycle, ViewDescriptor, ViewRegistry
  view_host.hpp          ViewHost pane tree, Viewport, mixed composition (model only)
  view_error.hpp         ViewError, std::expected aliases
  slice_view.hpp         SliceView refinement (lays out cells)
  page_view.hpp          PageView refinement (lays out pages; declarations only, §10.3)
  view_manifold.hpp      ViewManifold: binding arena + derived arena over one Manifold
  view_binding.hpp       ViewAxisSet, dimension-group API
  view_link.hpp          mintViewLink choke point, verifyViewManifoldInvariant
  view_layout.hpp        LayoutInput/LayoutSink, PlacedItem/PlacedEdge/AxisGizmo/PackFrame
  view_gesture.hpp       ViewGesture drag-to-rebind state machine (pure: events in, intents out)
  view_strategy.hpp      PackBuilder / RingBuilder / VanishingTraversal
  view_animation.hpp     AnimationState, epoch-guarded stable-id scheme
  pack_dims.hpp          d.pack / d.packing accessors, PackBuilder implementation
  builtin/
    stretch_vanishing_view.{hpp,cpp}
    all_dim_walk_view.{hpp,cpp}
    dimensional_pack_view.{hpp,cpp}
    builtin_views.cpp    registerBuiltinViews(ViewRegistry&)

apps/xuzz/                               (the application)
  view_host_app.{hpp,cpp}     (REPLACES view_coordinator.{hpp,cpp}) owns the ViewHost, routes
                              input and focus, calls reclaim()/layout()/advance()/draw per pane
  view_draw_adapter.{hpp,cpp} (NEW) layout records -> Canvas/Beams, scissor/depth range, picking,
                              AccessKit nodes, the TextLayout-backed content measurer
  view_commands.{hpp,cpp}     (NEW; absorbs apps/zigzag/zigzag_commands.cpp) view actions

apps/zigzag/zigzag_visualizer.*          (RETIRED by §15; no new code)
apps/xudu/views.*                        (wrapped as the one legacy page view until the follow-up)

include/gleditor/render/viewport.hpp     (NEW) ViewportDesc, setScissorRect/setDepthRange
include/gleditor/spatial.hpp             (EXTEND) unprojectScreenToRay()
```

### 5.3 Dependency rules

1. `apps/common/xanadu/view/` includes the engine (`apps/common/xanadu/`) and the header-only
   `<gleditor/cpp26*.hpp>` facilities, and nothing that needs a graphics device. `xuzz_test` links
   it as it links the rest of the engine.
1. `apps/xuzz/` includes the view framework and the library (`include/gleditor/`). It is the only
   place a layout record meets a renderer call.
1. No new file is added under `apps/xudu/` or `apps/zigzag/`, and no new code includes
   `zigzag_visualizer.hpp`. Existing includes of it shrink to zero as §15 proceeds.
1. Nothing under `src/` or `include/gleditor/` includes anything under `apps/`. The draw adapter
   consumes the view framework's record types, which is why it lives in `apps/xuzz/` and not in
   `src/render/`.
1. A third-party view is a `ViewDescriptor` handed to `ViewRegistry`; it needs rule 1's headers
   only.

______________________________________________________________________

## 6. The view space

### 6.1 Decision: extend `ArenaManifold`, do not invent a new type

Three representations were considered for "where a view's minted cells live":

1. A bespoke `ViewManifold`-as-ground-up type, independent of `ArenaManifold`.
1. One `ArenaManifold` per derived-layer generation, dropped and replaced wholesale on rebind.
1. One `ArenaManifold` per pane, reused for the pane's lifetime, extended with a generation counter.

(1) is refused: it would duplicate the CSR `DimLink` one-neighbour-per-direction invariant,
copy-on-write shadowing, and the ephemeral/real boundary bit `ArenaManifold` already has, tested, in
production for Vlog and VQL — building a second ephemeral-cells-over-a-base type is the exact
mistake R12 (`design/store-slice-convergence.md:502-563`) warns against: a privilege ("which type
gets to mint without ops") pushed one level down, encoded twice, divergeable twice.

(2) is refused: `ArenaManifold`'s construction is not a trivial no-op (it may seed provenance cells
and federation `Space`s key by the arena instance's own identity), so swapping the whole object on
every rebind would re-seed that machinery every frame and invalidate every `CellRef` a page view or
a speculative drag-preview held across the swap — breaking V-R8's recoverability for exactly the
case (a discarded speculative preview) that most needs it to hold.

(3), chosen: `ArenaManifold` already has the right shape —
`mark()`/`release()`/`discard()`(`arena_manifold.hpp:586-601`) are a bounded-cost choice-point
mechanism. What it lacks is a cheap way to answer "is this ref still good" without walking anything.
§6.4 adds exactly that.

A `ViewManifold` therefore wraps `ArenaManifold`s constructed once per pane, for the pane's
lifetime, over `base = <the real Manifold the pane is showing>` — two of them, for the reason §6.2
gives.

### 6.2 Two layers, two sibling arenas

Two kinds of ephemeral state exist, and they have different lifetimes, so they live in two
`ArenaManifold`s. Both are constructed once per pane over the same real base `Manifold`; they are
siblings, not nested (an arena's base is a `const Manifold *`, so nesting is not available and is
not needed).

1. **Binding arena**: the axis-slot rank, `d.binds` links, group cells and their membership (the
   `d.dim-group` runs). This is configuration the user set — the equivalent of a window layout — and
   it survives every toss. Losing it on a pack recomputation would reset which dimensions are bound
   to which axis on every focus move, breaking "movement works as expected".
1. **Derived arena**: pack containers, ring-slot placeholders, placement helpers, *and the
   view-owned dimension cells those hang on* (`d.pack`, `d.packing`, one `axisStep` dimension per
   bound axis, `d.ring-dim`) — anything minted purely to answer "what does the current frame look
   like". Everything in it is tossed on every rebind.

A single arena with two nested marks was the first design and is refused. An arena is a stack: a
binding edited after display cells exist would be minted *above* the display mark, so either the
display layer is released synchronously before every binding edit (O(cells minted), on the rebind
path — exactly what V-R7 forbids) or the next release destroys the binding just made. Two arenas
remove the ordering problem. The price is a second arena object per pane and one rule: derived cells
never link to binding cells. They do not need to — the derived layer reads the binding arena to
learn which *real* dimension cells are bound, and real refs mean the same thing in every arena over
the same base.

### 6.3 The invariants

Each invariant below corresponds to one of V-R5 through V-R10. "Enforcement" names the mechanism;
"Test" names the check that would fail if the mechanism regressed.

**I1 — at most one neighbour per (cell, dimension, direction).** Scoped precisely: for every cell
`c` the view currently holds, real or view-minted, and every dimension `d` that is currently bound,
a group member of a currently-bound group, or a view-owned bookkeeping dimension (`d.pack`,
`d.packing`, `d.dim-group`, `d.binds`, a ring-slot dimension), `linked(c, d, dir)` answers at most
one `CellRef` for each `dir`. **Enforcement**: for free, by representation — `ArenaManifold::link()`
calls `setOneSide()` on both ends and evicts whatever either end held, so no code path through it
can produce two posward neighbours on one dimension. The only way to violate I1 is to write a
`DimLink` directly instead of through the single choke point (§6.5), which this document forbids as
an implementation rule and backs with a `clang-tidy` pattern match (§6.5). **Test**: a property test
(`verifyViewManifoldInvariant`, §6.5) run after randomised sequences of bind/pack/ring/toss
operations, asserting `dimensionsOf(c)` carries at most one `DimLink` per scoped dimension for every
`c` the arena holds.

**I2 — a view-minted cell never reaches a store.** Already a byte-level invariant one layer down:
`Manifold::applyStructure` refuses any `SetLink` whose target `isEphemeral()` (`manifold.hpp:699`),
and `Store::setLink` throws on an ephemeral ref. A view routine cannot, even by a bug, cause a pack
container's `CellRef` to end up as the target of a persisted `SetLink`, because the encoding refuses
it. **Enforcement**: inherited from the `ephemeralBit` scheme. **Test**: an
`ArenaManifoldTest`-style unit that mints a view cell, hands its `CellRef` to a real
`Store::setLink`, and asserts the typed refusal rather than success.

**I3 — rebinding tosses every view-minted cell in a single constant-time operation.** See §6.4.

**I4 — every view cell resolves to a real cell within a bounded number of hops.** For a shadowed
base cell this is free: `CellSlot::birthOp` on a shadow is the base cell's own ref. For a
constructed view cell (a pack container, a ring-slot placeholder), resolution walks the view-owned
bookkeeping dimension that names its real referent (`d.pack`/`d.packing` for a pack, a `d.ring-dim`
link for a ring slot) until it bottoms out at a non-ephemeral ref, bounded by the same
`traversalBound()` cycle guard `cell_views.hpp`'s `RankView` already uses. **Enforcement**:
`ViewManifold::resolveReal(ViewCellRef) -> std::expected<CellRef, ViewError>` (§8.2) is the only
sanctioned path. **Test**: for every view-minted cell reachable from the accursed cell at radius ≤
N, assert `resolveReal` terminates within the bound and returns a non-ephemeral ref.

**I5 — a view never shadows a real cell's bound-dimension `DimLink`.** A view answers "what is `c`'s
effective posward neighbour on bound dimension/group G" by computation — walking the real dimensions
in G and returning a view cell as the answer — never by calling
`link(c, boundDim, …, packContainer)` on the real dimension itself, because doing so would
copy-on-write-shadow `c` and leave a `DimLink` on a bound dimension whose far end is ephemeral,
visible to any ordinary real-dimension walk starting from the shadow. **Enforcement**:
`mintViewLink` (§6.5) is never called with a real cell as `from` and a bound real dimension as
`dim`; every pack/ring/placement mutation targets a view-owned dimension instead. **Test**: a
property check that no `DimLink` on any currently-bound real dimension ever points at an ephemeral
ref.

**I6 — focus survives a toss.** The focus cell is stored as a real `CellRef`
(`SliceView::focusCell()` returns one; `focusCell(CellRef)` refuses an ephemeral argument). After a
toss, the new derived layer is re-derived from the unchanged focus, never from a now-dead view cell.
**Enforcement**: typed at the API boundary (§8.2). **Test**: toss, then assert `focusCell()` is
unchanged and resolves without error.

### 6.4 The O(1) toss, stated exactly

**The mechanism: dimensions are cells, so a toss forgets the dimensions.** Every link a view mints
is keyed by a view-owned dimension cell living in the derived arena (§6.2). The `ViewManifold` holds
the handles to those dimension cells. A toss abandons the handles; from then on no reader can ask
for a link on `d.pack`, `d.packing` or an `axisStep` dimension of the old generation, because
nothing holds the `DimRef` to ask with. The cells and links are still physically in the arena. They
are unreachable, which is what the requirement asks for.

Concretely `ArenaManifold` gains two fields and two methods:

```cpp
// apps/common/xanadu/zigzag/arena_manifold.hpp -- additions to the existing class
using ViewEpoch = std::uint32_t;

[[nodiscard]] ViewEpoch currentEpoch() const noexcept { return epoch_; }
/// Dense index below which every cell this arena minted is dead. A cell is
/// current iff its dense index >= epochFloor(); one compare, no stamp per
/// slot, so CellSlot does not grow.
[[nodiscard]] std::uint32_t epochFloor() const noexcept { return epochFloor_; }

/// The O(1) toss: two integer stores. Touches no slot, link or content run
/// and runs no destructor. Reclamation is reclaimDead(), a separate call.
void invalidateEpoch() noexcept {
  ++epoch_;
  epochFloor_ = static_cast<std::uint32_t>(slots_.size());
}

private:
  ViewEpoch epoch_{0};
  std::uint32_t epochFloor_{0};
```

and `ViewManifold::toss()` is `derived_.invalidateEpoch()` plus resetting its own fixed-size table
of dimension handles to "not minted" — a constant number of stores, independent of how many cells
the view minted (the per-axis `axisStep` handles are themselves cells in the derived arena, found
through one `axisStepRoot` handle, so the table does not grow with the axis count).

What each kind of stale state becomes after a toss:

| Stale state                                              | Why it cannot be observed                                                                                                  |
| -------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| a `ViewCellRef` held by a gesture, preview or animation  | carries its epoch; `ViewManifold` refuses it with `ViewError::StaleEpoch` on one integer compare                           |
| a pack container, ring slot or other derived cell        | dense index is below `epochFloor()`; `ViewManifold`'s graph view (§8.2) does not answer for it                             |
| a link on a real cell's shadow, keyed by an old `DimRef` | the key is a dead dimension cell; no current reader holds it, and `dimensionsOf()` through the graph view filters by floor |

The third row is why I5 matters to the toss and not only to correctness: a view links real cells
only on view-owned dimensions, so the shadow a real cell acquires in the derived arena carries
nothing but old-generation keys, and there is nothing on a real dimension to undo. Valence (§9.2) is
read from the base manifold, which the view never wrote.

What is and is not constant-time:

- **O(1): the toss.** `invalidateEpoch()` and the handle reset. This is the operation V-R7 and I3
  name, and the only work on the rebind path. A drag-to-rebind that previews five candidate axes in
  one frame performs five tosses at this cost.
- **O(1): every staleness check.** One integer compare per `ViewCellRef` or per dense index.
- **O(discarded), off the rebind path: reclamation.** `ViewManifold::reclaim()` calls
  `derived_.release(m0)` — `m0` being the mark taken on the empty derived arena at construction —
  and resets the floor to zero. It runs at the start of the next derivation, before any
  current-generation cell is minted, at most once per frame however many tosses preceded it, and
  only when the dead cell count exceeds `view.arena.reclaimThresholdCells` (§11.3). Each cell is
  freed once, so the cost is amortised against the minting that created it. Correctness never
  depends on it having run.
- **O(visible): re-derivation.** Re-minting the dimension handles (a constant number plus one per
  bound axis) and the packs, rings and placement the new frame shows, lazily (§9.3.6) — bounded by
  what is on screen, never by the slice.

**How the claim is tested** (§13, §15 step 1). `ArenaManifold` exposes a debug operation counter
(slot writes, link writes, trail pushes, shadow-map operations). The test mints 0, 100 and 10,000
derived cells, calls `ViewManifold::toss()`, and asserts the counter delta is zero and identical in
all three cases; then asserts every pre-toss `ViewCellRef` is refused, that a walk from the focus
through the graph view meets no cell below the floor, and that `reclaim()` afterwards returns the
arena's cell count to its empty value. An operation count, not a wall-clock time, so it is
deterministic in CI.

A view never calls `mark()`, `release()` or `invalidateEpoch()` itself; `ViewManifold` owns all
three, so a view cannot leak a mark (the "no cap on outstanding marks" hazard of
`arena_manifold.hpp` does not arise at this call site).

### 6.5 The choke point and the verifier

```cpp
// apps/common/xanadu/view/view_link.hpp
[[nodiscard]] std::expected<void, ViewError>
mintViewLink(ArenaManifold &arena, zigzag::CellRef from, zigzag::DimRef dim,
             zigzag::DimVector dir, zigzag::CellRef to) noexcept {
  if (arena.linked(from, dim, dir).has_value()) {
    // ArenaManifold::link() would evict the old neighbour -- correct for
    // ordinary unification (Vlog rebinding a variable), wrong for a view,
    // where "two things want this slot" is a bug in the view's own
    // derivation, not an intentional rebind. Refuse instead of evicting.
    return std::unexpected(ViewError::OccupiedDirection);
  }
  auto result = arena.link(from, dim, dir, to);
  if (!result) return std::unexpected(ViewError::UnknownDimensionOrRef);
  return {};
}

struct InvariantViolation {
  zigzag::CellRef cell;
  zigzag::DimRef dimension;
  int neighbourCountFound; // > 1 means violated
};
[[nodiscard]] std::vector<InvariantViolation>
verifyViewManifoldInvariant(const ArenaManifold &arena,
                            std::span<const zigzag::DimRef> scopedDimensions);
```

`mintViewLink` is the only function in the codebase permitted to call `ArenaManifold::link()`/
`linkedMinting()` on behalf of a view; every strategy object (`PackBuilder`, `RingBuilder`,
`VanishingTraversal`, or a plugin's own strategy) is handed this free function by reference and
never an `ArenaManifold&` directly. This is enforced by code review and a `clang-tidy` check
matching `ArenaManifold::link(` outside `view_link.cpp`, the same discipline `Store::setLink()`
already gets as the one place a real `SetLink` happens. `verifyViewManifoldInvariant` is the
debug/test analogue of `Manifold::verifyAgainstFullRebuild()` — it reports every violation found,
not just the first.

### 6.6 Naming collision avoided

`arena_manifold.hpp:124-128,365-378` already defines `BoundDimensionMember`/`BoundDimensionSet`/
`bindDimension()`/`boundDimensionSet()` for a different purpose entirely — binding one arena
dimension across federated spaces. Every view-binding type in this document is named `ViewAxis*`/
`ViewGroup*` to stay clear of it; a reader of `arena_manifold.hpp` must not be led to think the view
system is the federation feature.

______________________________________________________________________

## 7. Binding model

### 7.1 Axes are cells, not struct fields

**Ruling (V4): a binding is cells on a view-owned rank inside the view manifold, not a struct field
and not a fixed array.** The view mints one `axisDim` dimension cell once per pane; each axis slot
is a cell on that rank, carrying a `d.binds` link to the real dimension or group cell it currently
targets. Binding dimension `d.1` to axis slot 2 is `link(axisSlot2, d.binds, POS, d1Cell)`. Adding
an axis mints one more rank member; there is no fixed count. `ViewAxisBinding`'s X/Y/Z struct and
`DimensionBundle`'s closed five-entry enum are retired as live storage (§16, ruling V4) and reduced
to two roles only: (a) the serialization/preset shape persisted into `system://layout` (§7.4), since
dimension names, not `CellRef`s, are what survives a session boundary; and (b) an initial seed a
user can pick from when creating a new group, exactly as `DimensionBundle` already enumerates five
named presets today.

The cost of this representation, argued rather than assumed: one cell (32-byte `CellSlot`) plus one
`DimLink` (12 bytes) per axis slot, versus three `DimID`/string fields in `ViewAxisBinding` — noise
at the handful-of-axis-slots scale a view operates at. The *traversal* cost of "what's bound on X"
is O(bound-axis-count) hops instead of an O(1) struct read; that is the real price, paid for the
same reason R12 paid an analogous price for `d.dims` — the number of simultaneously bound dimensions
in all-dim walk and pack view is not known at compile time and must not be capped.

### 7.2 Dimension groups

A group is a cell `g`, minted on a view-owned `d.dim-group` run (deliberately not
`system://settings`' `d.groups`/`d.subgroups`, which group *settings fields*, not dimensions of a
slice being browsed — reusing that name risks a collision the first time a slice happens itself to
be a system xanadoc). `g`'s `d.dim-group` run links the real dimension cells (or nested group cells,
§9.3.7) that are its members, any number, uncapped. `g`'s own `CellRef` is what gets bound to an
axis slot via `d.binds`, exactly as a single dimension would be — axis-binding code never cases on
"is the target a dimension or a group." Rebinding an entire set at once is: change `g`'s membership;
every axis slot bound to `g` picks up the new membership on the next read, because the slot points
at `g`'s identity, not a snapshot of its members.

### 7.3 Rebind sequence

```text
1. ViewManifold::rebind(op):
     -> op(axes)                          -- binding arena only, e.g.
          mintViewLink(axisSlot, d.binds, POS, target)   -- choke point, I1
          undo stack entry pushed
     -> toss()                            -- derived arena: O(1), section 6.4
2. ViewHost observes the return and invokes view->onBindingChanged()
3. Next frame, before layout(): ViewManifold::reclaim() if over threshold
     -- O(discarded), once per frame however many rebinds preceded it
4. layout() lazily re-derives dimension handles, packs, rings, placement
     -- O(visible)
```

The binding edit and the toss touch different arenas, so step 1 costs the edit itself (a constant
number of link writes for a single bind; O(members changed) for a group edit) plus the constant-time
toss. No step on the rebind path is proportional to the number of derived cells.

### 7.4 Undo and persistence

Undo is view-local, never hypertime: each mutating `ViewAxisSet` call pushes one entry to an
in-process stack scoped to the pane's lifetime; `undo()` pops and reverses it. No operation is
appended to the real store by a bind, an undo, or a redo (R8).

Bindings persist across sessions in `system://layout` under `layout.zigzag.<sliceId>.axisBindings`
and `layout.zigzag.<sliceId>.dimensionGroups`, keyed by dimension *name*, not `CellRef` — the view's
own arena is fresh every session, so persisted `CellRef`s would already be dead; `attach()` reads
the last-known names back and replays them through `bind()`/`createGroup()`. This is per-slice, not
per-user-global (open question VU1, §17) and is deliberately not stored in `system://activity`
(which records *completed visits*, not standing configuration) and not in the slice's own store
(binding a dimension for browsing is not a Structure fact about the document — R8's refusal of
implicit promotion applies identically here).

______________________________________________________________________

## 8. The API

### 8.1 View identity, registry, descriptor

```cpp
// apps/common/xanadu/view/view.hpp
namespace xanadu::view {

/// Opaque, stable identity for an installed View kind (not an instance) --
/// a small interned string, the same shape DimID/dispatchAction names
/// already use. No enum: a plugin needs no case added anywhere.
using ViewKindId = std::string; // e.g. "zigzag.stretch-vanishing"

class View {
public:
  virtual ~View() = default;
  [[nodiscard]] virtual ViewKindId kind() const noexcept = 0;

  /// Once, when a pane starts showing this instance. @p store is the real
  /// backing store -- a View never owns persistence, it is handed it.
  virtual void attach(const xanadu::Store &store) = 0;
  virtual void detach() noexcept                  = 0;

  /// Binding changed (axis rebind, group edit). Never mutates the real
  /// store; always followed by a derived-layer toss before the next layout().
  virtual void onBindingChanged() = 0;

  /// The real accursed cell moved. No layout obligation itself -- layout()
  /// is pulled, not pushed -- but lets a view drop caches keyed by old focus.
  virtual void onCursorMoved(zigzag::CellRef previous,
                             zigzag::CellRef current) noexcept = 0;

  virtual void onStoreAdvanced(xanadu::MicroversionId newVersion) noexcept = 0;

  /// Pure, allocation-free, GPU-free layout (V-R3). noexcept: the render
  /// path never throws; a recoverable problem is reported through
  /// LayoutSink's own typed fields.
  virtual void layout(const struct LayoutInput &in,
                      struct LayoutSink &out) const noexcept = 0;
};

/// The ZigZag-side refinement: exposes the real Manifold a SliceView
/// renders and the one ViewManifold it mints into.
class SliceView : public View {
public:
  [[nodiscard]] virtual const class ViewManifold &viewManifold() const noexcept = 0;
  [[nodiscard]] virtual zigzag::CellRef focusCell() const noexcept              = 0;
  virtual void focusCell(zigzag::CellRef real) = 0; // refuses an ephemeral ref
};

/// The xanadoc seam (§10.3). Thin on purpose: only what the follow-up spec
/// needs to not have to touch this chapter again.
class PageView : public View {
public:
  [[nodiscard]] virtual xanadu::DocRef document() const noexcept = 0;
};

struct ViewCapabilities {
  bool needsThreeD{false};
  bool supportsDragRebind{false};
  bool mintsViewCells{true};
  bool pageCompatible{false};
};

struct ViewDescriptor {
  ViewKindId id;
  std::string displayName, description, iconGlyph;
  ViewCapabilities capabilities;
  gleditor::cpp26::function_ref<std::vector<xanadu::settings::SettingSpec>()> settingSpecs;
  struct DefaultChord { std::string actionId, vortexCall, chord, context; };
  std::vector<DefaultChord> defaultChords;
  gleditor::cpp26::function_ref<std::unique_ptr<View>(const zigzag::Manifold &base)> makeInstance;
};

class ViewRegistry {
public:
  static ViewRegistry &instance() noexcept;
  /// Refuses (ViewError::DuplicateViewId) a second registration of the same
  /// id -- a collision is a config error to surface, not a silent override.
  std::expected<void, ViewError> registerView(ViewDescriptor descriptor);
  [[nodiscard]] std::span<const ViewDescriptor> installed() const noexcept;
  [[nodiscard]] gleditor::cpp26::optional<const ViewDescriptor &> find(const ViewKindId &id) const noexcept;
private:
  std::vector<ViewDescriptor> views_; // stable order: install order
};

} // namespace xanadu::view
```

A built-in view self-registers at static-init time through the same `registerView()` call a plugin
would call explicitly (a dynamically-loaded or Vortex-authored view has no static-init moment, so
the registry supports the explicit path universally, and built-ins happen to invoke it from a
static-init shim) — one registration path, mirroring how `registerZigzagCommands()` is one shared
function both ZigZag and Xuzz call (`zigzag_commands.hpp:46-48`).

**Vortex-authored views.** A view whose layout math is simple enough to express as Vortex
structure/arithmetic calls registers through the identical `ViewRegistry::registerView()`, wrapping
a `VortexCore` program reference in a `makeInstance` closure. What must stay C++: anything touching
the draw adapter, anything needing the epoch/mark machinery's direct manipulation, anything on the
hot path where a VM dispatch per cell would blow the frame budget (all-dim walk's ring geometry at
high valence). What can be Vortex: a custom pack-membership predicate, a custom opacity curve, a
custom grid-conflict tie-break rule — pure functions over already-resolved cell data. This is
exactly the line AGENTS.md's "new C++ must justify why it is not Vortex" draws.

### 8.2 `ViewManifold`

```cpp
// apps/common/xanadu/view/view_manifold.hpp
namespace xanadu::view {

/// A real or view-minted cell, tagged with the epoch it was minted/read
/// under -- staleness becomes an O(1) local check, never an arena walk.
/// Converts to CellRef for free at every read-only call.
struct ViewCellRef {
  zigzag::CellRef ref{zigzag::noCell};
  ArenaManifold::ViewEpoch epoch{0};
  [[nodiscard]] constexpr operator zigzag::CellRef() const noexcept { return ref; }
  [[nodiscard]] constexpr bool isEphemeral() const noexcept {
    return (ref & zigzag::ephemeralBit) != 0;
  }
  bool operator==(const ViewCellRef &) const = default;
};

/// Read-only graph over one pane's view space: the base manifold, plus the
/// derived arena's current generation. Cells below the derived arena's
/// epochFloor() and links keyed by dead dimension cells are not answered, so
/// RankView/neighbours() from cell_views.hpp work over it unchanged and can
/// never walk into a tossed generation.
class ViewGraph; // satisfies zigzag::CellGraph; holds two pointers

/// Owns the two sibling arenas of §6.2. A caller never sees a Mark or an
/// epoch setter -- rebind()/toss()/reclaim() are the only lifecycle entry
/// points, so "take a mark, forget to release it" cannot happen outside.
class ViewManifold {
public:
  explicit ViewManifold(const zigzag::Manifold &base);

  [[nodiscard]] const zigzag::Manifold &base() const noexcept { return base_; }
  [[nodiscard]] ViewGraph graph() const noexcept;
  [[nodiscard]] ArenaManifold::ViewEpoch epoch() const noexcept {
    return derived_.currentEpoch();
  }

  [[nodiscard]] class ViewAxisSet &axes() noexcept { return axes_; }
  [[nodiscard]] const class ViewAxisSet &axes() const noexcept { return axes_; }

  /// The view-owned dimensions of the current generation, minted on first
  /// use after a toss. Two DimRefs for packs (ruling V2).
  [[nodiscard]] zigzag::DimRef packDim();    // d.pack: container -> first
  [[nodiscard]] zigzag::DimRef packingDim(); // d.packing: constituent rank
  [[nodiscard]] zigzag::DimRef ringDim();    // d.ring-dim
  [[nodiscard]] zigzag::DimRef axisStepDim(ViewAxisId axis);

  /// Mint a derived cell / link in the current generation. link() is
  /// mintViewLink (§6.5) over the derived arena; strategies get these, never
  /// an ArenaManifold&.
  [[nodiscard]] std::expected<ViewCellRef, ViewError> mintCell();
  [[nodiscard]] std::expected<void, ViewError>
  link(ViewCellRef from, zigzag::DimRef dim, zigzag::DimVector dir,
       ViewCellRef to) noexcept;

  /// Edit the binding arena, then toss. See §7.3.
  template <typename BindOp>
  auto rebind(BindOp &&op) -> decltype(op(axes_)) {
    auto result = std::forward<BindOp>(op)(axes_);
    toss();
    return result;
  }

  /// The O(1) toss (§6.4). No loop, no allocation, no destructor.
  void toss() noexcept {
    derived_.invalidateEpoch();
    dims_ = {};
  }

  /// O(discarded). Called by the host at the start of a derivation, never
  /// from rebind(); a no-op under view.arena.reclaimThresholdCells.
  void reclaim() noexcept;

  /// I4: resolve any view cell to a real CellRef, bounded by traversalBound().
  /// Refuses a stale ref with ViewError::StaleEpoch.
  [[nodiscard]] std::expected<zigzag::CellRef, ViewError>
  resolveReal(ViewCellRef cell) const noexcept;

private:
  struct DerivedDims { // fixed size: the O(1) reset in toss()
    zigzag::DimRef pack{zigzag::noCell};
    zigzag::DimRef packing{zigzag::noCell};
    zigzag::DimRef ring{zigzag::noCell};
    zigzag::CellRef axisStepRoot{zigzag::noCell}; // rank of per-axis dims
  };

  const zigzag::Manifold &base_;
  ArenaManifold bindings_; // survives every toss
  ArenaManifold derived_;  // tossed on every rebind
  ArenaManifold::Mark derivedEmpty_; // taken at construction; reclaim() target
  DerivedDims dims_;
  class ViewAxisSet axes_;
};

} // namespace xanadu::view
```

### 8.3 Binding model API

```cpp
// apps/common/xanadu/view/view_binding.hpp
namespace xanadu::view {

/// A single real dimension, or a group cell's identity -- zzstructure never
/// distinguishes the two at the type level (a dimension is a cell, R2), so
/// axis code never cases on which.
using BindTarget = zigzag::CellRef;

class ViewAxisSet {
public:
  [[nodiscard]] std::size_t axisCount() const noexcept;      // not a cap
  std::size_t addAxis();                                     // open-ended
  std::expected<void, ViewError> removeAxis(std::size_t index);

  /// Goes through mintViewLink; a double-bind of the same axis slot is
  /// refused the same way any other occupied direction is (I1 extends to
  /// the binding layer itself).
  std::expected<void, ViewError> bind(std::size_t axisIndex, BindTarget target);
  [[nodiscard]] gleditor::cpp26::optional<BindTarget> boundTarget(std::size_t axisIndex) const noexcept;

  zigzag::CellRef createGroup(std::string_view name, std::span<const zigzag::DimRef> members);
  std::expected<void, ViewError> renameGroup(zigzag::CellRef group, std::string_view name);
  std::expected<void, ViewError> addMember(zigzag::CellRef group, zigzag::DimRef dim);
  std::expected<void, ViewError> removeMember(zigzag::CellRef group, zigzag::DimRef dim);
  /// Refused (ViewError::GroupCycle) if outer is reachable from inner.
  std::expected<void, ViewError> nestGroup(zigzag::CellRef outer, zigzag::CellRef inner);
  [[nodiscard]] std::vector<zigzag::DimRef> membersOf(zigzag::CellRef group) const;

  /// "Rebind an entire set at once": re-link every axis bound to oldTarget
  /// to newTarget instead.
  std::expected<void, ViewError> rebindAllAxesOf(zigzag::CellRef oldTarget, BindTarget newTarget);

  void undo();
  [[nodiscard]] bool canUndo() const noexcept;

private:
  ArenaManifold *arena_; // non-owning; the ViewManifold owns this set
  zigzag::DimRef axisDim_{zigzag::noCell};
  zigzag::DimRef dimGroupDim_{zigzag::noCell};
  zigzag::DimRef bindsDim_{zigzag::noCell};
  std::vector<struct BindingEdit> undoStack_;
};

} // namespace xanadu::view
```

### 8.4 Per-frame pipeline and output records

```cpp
// apps/common/xanadu/view/view_layout.hpp
namespace xanadu::view {

struct ViewportDesc {
  int x, y, width, height;               // pixels
  float nearDepth{0.0F}, farDepth{1.0F}; // depth-range slice, §12
  glm::mat4 viewProjection{1.0F};         // host-owned camera
};

struct ContentExtent {
  float widthPx{0.0F};
  float heightPx{0.0F};
};

struct LayoutInput {
  const ViewManifold &view;
  zigzag::CellRef focus;
  ViewportDesc viewport;
  /// Content-fit size of a real cell at a width limit. The application
  /// passes a cached TextLayout-backed measurer; a test passes a fixed one.
  /// This is what keeps layout() free of fonts and of any graphics device.
  gleditor::cpp26::function_ref<ContentExtent(zigzag::CellRef, float)> measure;
  std::uint64_t frameId; // deterministic tie-break
};

struct PlacedItem {
  ViewCellRef cell;
  glm::vec3 position;
  glm::quat orientation{1, 0, 0, 0};
  float width{}, height{};
  float opacity{1.0F};
  enum class Visibility : std::uint8_t { Visible, ClippedHidden, Lod } visibility{};
  std::uint16_t depthLayer{};
  enum class ContentMode : std::uint8_t { Full, Abbreviated, Badge } contentMode{};
  std::uint32_t packId{}; // 0 = not inside a pack
};

struct PlacedEdge {
  ViewCellRef from, to;         // `to` may be a ring-slot placeholder
  zigzag::DimRef dimension;
  zigzag::DimVector direction;
  glm::vec3 labelAnchor;
  bool boundToAxis{};
};

struct AxisGizmo {
  glm::vec3 origin, direction;
  zigzag::DimRef boundDimension{zigzag::noCell}; // noCell = unbound slot
  float snapRadiusWorld{};
};

struct PackFrame {
  std::uint32_t id{}, parentPackId{};
  glm::vec3 position{}; float width{}, height{}, depth{};
  std::uint16_t constituentCount{};
  bool collapsedToBadge{};
};

/// Caller-owned, reused across frames -- zero dynamic allocation (V-R31).
class LayoutSink {
public:
  void push(const PlacedItem &) noexcept;
  void push(const PlacedEdge &) noexcept;
  void push(const AxisGizmo &) noexcept;
  void push(const PackFrame &) noexcept;
  [[nodiscard]] std::span<const PlacedItem> items() const noexcept;
  [[nodiscard]] std::span<const PlacedEdge> edges() const noexcept;
  [[nodiscard]] std::span<const AxisGizmo> gizmos() const noexcept;
  [[nodiscard]] std::span<const PackFrame> packs() const noexcept;
  [[nodiscard]] std::size_t itemCountNeeded() const noexcept; // growth diagnostics
};

} // namespace xanadu::view
```

`View::layout(const LayoutInput&, LayoutSink&) const noexcept` reads `in.view.graph()` through
`CellGraph`-conformant calls, may call strategy objects (§8.6) that mint into the derived layer via
`mintViewLink`, but never touches `AnimationState`, Choreograph, or GPU state. Calling it twice with
different candidate bindings and discarding one — the drag-rebind preview needs exactly this — costs
nothing and corrupts nothing, because it is `const` on the view and writes only into caller-owned
`LayoutSink` storage.

```cpp
// apps/common/xanadu/view/view_animation.hpp
struct AnimId {
  zigzag::CellRef cellOrSyntheticHash;
  ArenaManifold::ViewEpoch mintedEpoch;
};

class AnimationState {
public:
  /// Before dereferencing any ViewCellRef in `target`, compares its epoch
  /// against viewEpoch: a stale epoch drops the entry from the animation
  /// set rather than easing toward a dangling identity. This IS the
  /// mechanism satisfying "nothing dereferences a tossed cell": an epoch
  /// comparison, not a liveness scan.
  void advance(const LayoutSink &target, float dtSeconds,
              ArenaManifold::ViewEpoch viewEpoch) noexcept;
};
```

Per-pane, per-frame pipeline, replacing `rebuildActiveViewTopology`/`updateCellPositions`'s fused
loop:

```text
attach()                          -- once, on pane open; replays system://layout bindings
onBindingChanged()/onCursorMoved()/onStoreAdvanced()  -- as triggered, never per-frame
layout(input, sink)                -- pure, every frame, into reused sink storage
AnimationState::advance(sink,dt,epoch)  -- epoch-guarded tween, Choreograph-backed (§12)
apps/xuzz view_draw_adapter draw(ctx, animated)  -- Canvas/Beams, picking, a11y
detach()                           -- once, on pane close; writes bindings back to system://layout
```

### 8.5 Extension points

| #   | Extension                    | Interface                                         | Guaranteed                                                                                      | Must never do                                                                                                                                   |
| --- | ---------------------------- | ------------------------------------------------- | ----------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| a   | New slice view               | `SliceView` + `ViewDescriptor` via `registerView` | Fresh `ViewManifold` per pane; valid `LayoutInput`; `mintViewLink` as the only mutation surface | Call `ArenaManifold::link()` directly; shadow a bound dimension (I5); persist via the real `Store` from a read path; allocate inside `layout()` |
| b   | New page view                | `PageView` (follow-up)                            | Same `View` contract; a `Viewport`; `Manifold::contentOf`/`textOf` for embedded content         | Depend on a graphics device in `layout()`; embed an ephemeral `CellRef` handed to it by a `SliceView`                                           |
| c   | New pack/derivation strategy | `PackBuilder`/`RingBuilder`/`VanishingTraversal`  | A `ViewManifold&`, the `mintViewLink` function, the current `ViewAxisSet`                       | Mint through anything but `ViewManifold::mintCell`/`link`; retain a `ViewCellRef` across a frame without re-validating its epoch                |
| d   | New layout record kind       | add a field/`push()` overload to `LayoutSink`     | Backward compatible: an unknown-to-them record kind is ignored, not a failure                   | Embed a raw `CellRef` without going through `ViewCellRef`                                                                                       |
| e   | New gesture                  | `ViewGesture` state machine + `View::hitTest`     | Exclusive ownership of `Idle→…→Idle`; a speculative `layout()` call per preview frame           | Commit a binding mutation before `{Committed}`; bypass `ViewAxisSet::bind()` to write `d.binds` directly                                        |

### 8.6 Strategy objects

```cpp
// apps/common/xanadu/view/view_strategy.hpp
namespace xanadu::view {

/// Dimensional pack view's d.pack/d.packing builder (§9.3) -- separable so
/// other views or a plugin reuse the same BFS-frontier semantics.
class PackBuilder {
public:
  /// Computes (lazily -- membership only; §9.3.6) the next pack step from
  /// @p source along every dimension in @p group.
  [[nodiscard]] std::vector<zigzag::CellRef>
  computeNextStep(const ArenaManifold &arena, zigzag::CellRef source,
                  std::span<const zigzag::DimRef> group,
                  std::span<const zigzag::CellRef> alreadyPacked) const noexcept;

  /// Mints the container (via d.pack), its d.packing rank, and the
  /// predecessor->container link on the axis's axisStep dimension. Calls
  /// mintViewLink for every edge it writes.
  [[nodiscard]] std::expected<zigzag::CellRef, ViewError>
  materialize(ViewManifold &view, std::span<const zigzag::CellRef> members,
             zigzag::CellRef previousRepresentative, zigzag::DimRef axisStep) const;
};

/// All-dim walk's unbound-dimension ring placement -- per-dimension slot
/// assignment, append-only (§9.2.1).
class RingBuilder {
public:
  [[nodiscard]] float ringSlotAzimuth(zigzag::DimRef dim) noexcept;
  [[nodiscard]] AxisGizmo gizmoFor(std::size_t axisIndex, const ViewAxisSet &axes) const noexcept;
};

/// Stretch vanishing's shelf/skyline BFS packer (§9.1.1).
class VanishingTraversal {
public:
  void reset() noexcept; // new focus -- full rebuild, cheap (frontier is small)
  [[nodiscard]] bool stepOnce(const ArenaManifold &arena, LayoutSink &sink) noexcept;
  // false once the overfill-margin budget (§9.1.6) is hit
};

} // namespace xanadu::view
```

### 8.7 Movement and editing

```cpp
// slice_view.hpp (continued)
enum class MoveDirection : std::uint8_t { Positive, Negative };
struct MoveRequest { std::size_t axisIndex; MoveDirection direction; };

/// Result: a real CellRef (I6) plus the view path taken, for breadcrumb UI.
struct MoveOutcome {
  zigzag::CellRef realCellAfter;
  std::vector<zigzag::CellRef> viewPath; // empty outside pack/ring views
  bool rebindOccurred{false};            // all-dim walk's §9.2.3 case
};

class SliceView : public View {
public:
  // ... (§8.1) ...

  /// Movement never calls anything on the real Manifold/Store that appends
  /// an operation (R8) -- enforced by construction: this signature has no
  /// access to a mutable Store, only ViewManifold's read-only base().
  virtual MoveOutcome move(MoveRequest request) = 0;

  /// Write-through editing: resolves a possibly-view-minted anchor to its
  /// real cell via ViewManifold::resolveReal(), then hands the resolved
  /// real CellRef to the host's existing edit path unchanged. Refuses
  /// (ViewError::EphemeralEditTarget) if resolution bottoms out at nothing.
  [[nodiscard]] std::expected<zigzag::CellRef, ViewError>
  resolveEditTarget(ViewCellRef shown) const;

  /// Explicit, user-named promotion (never implicit, never called from
  /// move()/layout()). Delegates to zigzag::promote(store, parent, arena,
  /// root, budget), which refuses above its own PromotionBudget rather
  /// than silently truncating.
  [[nodiscard]] std::expected<void, ViewError>
  promotePack(zigzag::CellRef packContainer, xanadu::Store &store);
};
```

**Activity-store recording.** The activity store exists: `xanadu::StoreActivityLog`
(`apps/common/xanadu/store_activity_log.hpp`) appends branching `Visit`s — parent, target
`OccurrenceSite`, `Arrival`, optional link context — to the reader's `system://activity` store, and
`tests/xuzz/store_activity_log_test.cpp` pins it. `SliceView::move()` returns a `MoveOutcome`;
`ViewHost::dispatchMove()` decides, after the call returns, whether the outcome is a completed
transition (debounced for a continuous drag, immediate for a discrete step) and appends one `Visit`
through the same `ActivityLog` interface link navigation uses, with the *real* focus cell as its
target. A view cell is never a visit target (I6). Whether a `Visit` should also carry the view kind
and the bindings in force, so Activity Back can restore them, is VU6.

### 8.8 Error model

```cpp
// apps/common/xanadu/view/view_error.hpp
namespace xanadu::view {

enum class ViewError {
  OccupiedDirection,       // mintViewLink: (cell,dim,dir) already has a neighbour
  StaleGeneration,         // a ViewCellRef's epoch no longer matches currentEpoch()
  EphemeralRefToStore,     // a view-minted CellRef was handed to a real Store call
  UnknownDimensionOrRef,   // arena op refused the ref/dim outright
  GroupCycle,              // nestGroup() would make a group reachable from itself
  DuplicateViewId,         // registerView() with an existing id
  ChordCollision,          // a plugin's default chord collides with an existing one
  EphemeralEditTarget,     // resolveEditTarget() bottomed out at nothing resolvable
  PromotionBudgetExceeded, // promotePack() over PromotionBudget::maxOps
  EmptyGroupBind,          // bind() with a group that has zero members
  AxisAlreadyBound,        // bind() target already bound to a different axis
  UnknownAxisIndex,        // move()/bind() referenced a nonexistent axis index
};

[[nodiscard]] constexpr std::string_view messageKey(ViewError e) noexcept; // i18n lookup key

} // namespace xanadu::view
```

Every refusal message specified in §11's status-line table maps onto exactly one of these. "No cell
further along `<dimension>` `<direction>`" is not an error in this model — it is a `MoveOutcome`
whose `realCellAfter == previous focus`, surfaced through status-line feedback, since "nowhere to
go" is a normal outcome, not a malformed request.

______________________________________________________________________

## 9. The three views

### 9.1 Stretch vanishing

#### 9.1.1 Purpose

Reading a lot of actual cell content fast — scanning a rank's worth of text without scrolling cell
by cell — when exact structural alignment matters less than seeing everything. Reach for it when
auditing content, not relationships.

#### 9.1.2 Semantics

Let `c` be the accursed cell and `B` the set of currently bound dimensions. The view displays the
largest set of cells reachable from `c` by hops along `B` that fits the viewport under the
shelf-pack rule below, each rendered at its full content-fit size, with radius-1 neighbours along
`B` exactly axis-aligned to `c` and every other displayed cell packed as tightly as content allows.

#### 9.1.3 Layout algorithm

**Placement: shelf/skyline packing seeded from the accursed cell (BFS order).**

Three candidates were weighed:

- *Row/column track sizing (ragged table)*: cheap and deterministic, but implies a grid the
  requirement explicitly rejects past radius 1 — most cells are not axis-aligned, so forcing them
  into rows/columns fights "only immediate neighbours aligned" and wastes space around differently
  sized content-fit boxes.
- *Constraint relaxation (iterative, spring-like)*: organic, but not deterministic or stable under
  small edits — a one-character content edit can cascade a relaxation solve into a visibly different
  arrangement elsewhere, reading as random reshuffling rather than "that cell grew a little."
  Refused for V-R18 (determinism) and the stability property stretch vanishing exists to provide.
- *Shelf/skyline packing (NFDH-style), chosen.* Walk cells in strict breadth-first order from `c`:
  first its axis-aligned immediate neighbours along `B` (each placed on its axis's unit vector at
  `axisUnit * axisSpacing`, with
  `axisSpacing = (focusExtent + neighbourExtent)/2 + rankClearancePx`), then every cell reached by
  one more hop from an already-placed cell, in a fixed tie-break order (ascending `CellRef`, so two
  runs with the same input always visit frontier ties identically). For each new cell, maintain a
  skyline (a sorted list of `(xStart, xEnd, yTop)` segments) and place the cell's content-fit box at
  the lowest skyline position reachable from the cell it was discovered through, sliding along the
  skyline until it touches that anchor's edge with the configured gap and no overlap.

This wins on the three properties the requirement needs:

- *Determinism (V-R18)*: fixed visit order plus a no-backtracking insertion rule → identical output
  for identical input, every time; a headless test computes twice and diffs positions.
- *Stability under small edits*: a cell's content growing by Δpx perturbs only its own skyline
  segment and whatever was placed after it in visit order — no global relaxation pass, so a one-cell
  edit's visual blast radius is bounded, matching "cells slide to stay adjacent to the cell they
  were reached from."
- *Cost*: O(n log n) for n visible cells (skyline insertion is a binary search over segments); the
  breadth-first budget below (§9.1.6) makes the practical cost O(viewport fill count), independent
  of the slice's total size.

#### 9.1.4 Immediate-neighbour exactness

Radius-1 neighbours along `B` are placed first, exactly axis-aligned, using measured content-fit
extents for spacing (so "drawn very closely together" is satisfied by measurement, not a fixed grid
pitch). Everything at radius ≥ 2 is placed by the skyline packer, never forced onto an axis — this
is the structural meaning of "only the immediate neighbours need be completely aligned."

#### 9.1.5 Edge fade

Computed in normalised viewport space so the fade reads as "near the screen's edge" regardless of
camera distance or FOV: project each candidate cell's four content-fit corners through
`viewProjection * presentationTransform` to clip space, perspective-divide to NDC, map to `[0,1]`
viewport UV. Let `d = min(u, 1-u, v, 1-v)` (the closest corner's normalised distance to the nearest
edge). The fade curve is:

```math
\text{opacity} = \mathrm{smoothstep}(\text{edgeFadeStartUv}, 0, d)
```

— full opacity while `d > edgeFadeStartUv` (a configurable band, default 0.65 of the way to the
centre), fading to the configured floor at the edge. `smoothstep` rather than a linear ramp because
a linear fade reads as a visible band edge; the cubic Hermite curve has zero derivative at both
ends. This is a layout-time scalar multiplied into `PlacedItem.opacity`; the animation layer may
further multiply a transition fade on top, never replace it.

#### 9.1.6 Partially-clipped-⇒-invisible rule, with hysteresis

The exact test is the mirror of `outsideFrustum`'s "fully outside": a cell is drawn only if its
content-fit box's clip-space AABB has all four corners inside every clip plane (fully inside). A
cell that straddles an edge is `ClippedHidden` — not drawn — same as fully outside; it is never
faded or cropped (satisfying V-R16's "not faded, not cropped," since a half-drawn cell with real
text reads as missing data, which this document treats as exactly the kind of silent ceiling the
project's conventions refuse). The accursed cell itself is exempt: `layout()` force-sets its
visibility to `Visible` after the clip pass regardless of the geometric test, and skips the
edge-fade multiplier for it — it must not vanish, though it may be scissor-clipped like any
oversized object.

Hysteresis against flicker: because a cell's drawn position is still easing toward its target,
testing the *animated* position every frame would flip visibility near the boundary mid-tween. A
cell transitions `Visible → ClippedHidden` only after its animated position has tested fully-outside
for a configurable sustained duration (`stretch.hiddenConfirmMs`); it transitions back the instant
it tests fully-inside again — appearing late is safe, disappearing late is the flicker risk, so the
asymmetry is deliberate. This state lives in `AnimationState`, not in the pure `layout()` function,
keeping layout itself frame-rate-independent and testable as a pure function.

#### 9.1.7 Breadth-first budget

The packer maintains the bounding region of everything placed so far; after each complete BFS level
it checks whether that region's projected viewport-space extent exceeds the viewport by a
configurable overfill margin (default 1.3×). Once exceeded, BFS stops enqueuing new frontier cells
for this frame; already-enqueued-but-unplaced cells from the same level are deferred, not placed
(keeping the level atomic), and the frontier queue is cached across frames so a window resize or
zoom-out resumes packing from where it stopped rather than restarting. This budget — not a
cell-count ceiling — is what bounds cost: a user with a densely connected cell sees BFS stop filling
once the screen is full, never a hard "top 50 neighbours only" cutoff.

#### 9.1.8 Interaction

Movement is the ordinary step action on each bound axis — stretch vanishing changes layout, not the
movement vocabulary. Pointer click focuses a visible cell through the library's `PickObserver` path;
hover previews a faded cell's full content in a tooltip without moving focus. There is no
pointer-only affordance, so no further keyboard twin is needed.

Because alignment is given up past radius 1, three cues keep the reader oriented: the accursed cell
carries the focus emphasis and is never faded; each radius-1 neighbour carries a tick naming the
dimension and direction that reached it; and a breadcrumb strip lists the last
`stretch.breadcrumbDepth` cells walked, read from the reader's activity log (§8.7).

#### 9.1.9 Degenerate cases

- A cell with no neighbours on any bound dimension: drawn alone, centred; the axis-tick decoration
  is simply absent (no "0 neighbours" badge — absence of the tick is the signal).
- A single-cell slice: the home cell alone fills the pane.
- Content taller/wider than the viewport: the cell scrolls internally; a corner glyph marks "more
  below/right."
- A rank hundreds of cells long: the breadth-first budget (§9.1.7) stops filling when the viewport
  is full; the tick on the last cell placed along that rank shows how many were not
  (`d.2 → (+214 more)`), so the walk never stops at an unannounced radius.

#### 9.1.10 Accessibility

Each visible cell is a node, role `cell`, name = full untruncated text, with a relation to its
axis-aligned immediate neighbours (`nextAlong:<dimension>`/`prevAlong:<dimension>`). A culled
(would-be-clipped) cell is absent from the tree, not present-but-hidden, matching what is drawn. The
breadcrumb strip (§9.1.8) is an ordered list node (`role: list`, children `role: listitem`). The
fade floor (`stretch.fadeFloorAlpha`) must be chosen so a faded cell's text still meets WCAG-AA
contrast against the background colour at the floor value — a testable constraint, checked in the
same headless harness that validates other rendering contracts.

#### 9.1.11 Tunables

See §11's settings table (`stretch.*`).

#### 9.1.12 Acceptance tests

Two `layout()` calls with identical input produce byte-identical `PlacedItem` positions
(determinism, V-R18); a cell whose box straddles the viewport boundary never appears in
`LayoutSink::items()` (V-R16); every radius-1 bound-dimension neighbour's position shares its bound
axis's coordinate with the focus cell exactly (V-R15); opacity at the configured fade band boundary
matches the `smoothstep` formula within floating-point tolerance (V-R17).

#### 9.1.13 Diagram

```text
  viewport
 +--------------------------------------------------+
 |  . faint .   +-------+ +----------+   . faint .  |
 |  +------+    | up 1  | | reached  |  +-------+   |
 |  | r=2  |    +-------+ | from up1 |  | r = 3 |   |
 |  +------+ +-------+ +=========+ +--------+----+  |
 |           | left1 | || c     || | right 1    |   |
 |           +-------+ +=========+ +------------+   |
 |    +----------+     +--------+ +------+          |
 |    | r = 2    |     | down 1 | | r=2  |          |
 +--------------------------------------------------+
   radius 1: on c's axes, spaced by measured size
   radius 2 and beyond: skyline-packed against the cell that reached them
   near the edge: fainter; a box the edge would cut is not drawn at all
```

______________________________________________________________________

### 9.2 All-dim walk

#### 9.2.1 Purpose

Understanding everything one cell connects to, across every dimension at once, before deciding which
relationship actually matters — "audit a contact cell's every relationship." Reach for it when the
structure, not the content, is the question.

#### 9.2.2 Semantics

For the accursed cell `c`, enumerate every `(dimension, direction)` pair where
`linked(c, dim, dir) ≠ noCell` — exactly `dimensionsOf(c)` read in both directions; its cardinality
is `c`'s valence. Bound dimensions occupy fixed spokes on the screen/world axes. Unbound dimensions
occupy ring positions arranged around the spoke structure in three dimensions, so the view's total
displayed valence equals `c`'s true valence exactly — nothing hidden (V-R19).

#### 9.2.3 Layout algorithm

**Geometry: concentric tilted rings assigned by dimension-arrival order; bound dimensions keep their
fixed spoke placement.**

Candidates weighed:

- *Helix* (`angle = n·goldenAngle, z = n·pitch`): elegant, but a dimension's angle is a function of
  total count under the naive formula, so adding one more connected dimension reshuffles every other
  dimension's position — violates V-R21 (stability).
- *Spherical Fibonacci cap*: near-optimal point distribution for large n, but the same reshuffle
  problem (points are indexed by position-among-n), and it does not naturally give the
  "posward/negward diametrically opposite" pairing the requirement asks for.
- *Concentric tilted rings by dimension, chosen.* Each dimension (not each cell) owns one ring at a
  fixed angular slot, assigned the first time that dimension is seen at this focus cell and cached
  per-dimension for the session — re-encountering the same dimension reuses its slot; removing a
  dimension frees its slot without renumbering the others; adding one allocates the next free slot.
  Within a dimension's ring, its posward neighbour sits at one fixed azimuth and its negward
  neighbour at `azimuth + π` (diametrically opposite). Bound dimensions sit on their axis spokes (as
  in §9.1.4) and are excluded from ring allocation. Unbound dimensions' rings stack at increasing
  radius and increasing depth tilt as valence grows: ring `k` (0-indexed by first-seen order among
  unbound dimensions) sits at `radius(k) = r0 + k·rStep`, tilted
  `tilt(k) = min(tiltMax, tilt0 + k·tiltStep)`. Tilt never reaches 90°, so every ring's near half
  stays in front of the focus along the view axis — the focus is never occluded as valence grows
  into the hundreds (V-R20).

Position formula, for dimension `d` with ring index `k`, member direction `s ∈ {+1, -1}`:

```math
\begin{aligned}
\text{azimuth}(d) &= d.\text{ringBaseAzimuth} \\
\text{radius}(k) &= r_0 + k \cdot r_{\text{step}} \\
\text{tilt}(k) &= \min(\text{tiltMax},\ t_0 + k \cdot t_{\text{step}}) \\
\text{position}(d, s) &= \text{focusPos} \\
&\quad + \text{radius}(k)\cos(\text{azimuth}(d) + [s<0]\pi)\cos(\text{tilt}(k))\,\hat{\text{right}} \\
&\quad + \text{radius}(k)\sin(\text{tilt}(k))\,\hat{\text{up}} \\
&\quad + \text{radius}(k)\sin(\text{azimuth}(d) + [s<0]\pi)\cos(\text{tilt}(k))\,\hat{\text{forward}}
\end{aligned}
```

`right̂`/`up̂`/`forward̂` are the host camera's own basis (`view.front`/`view.upward`), so the wheel
orients itself to the camera rather than to a fixed world frame and stays legible as the user
orbits.

Justified against the three criteria: *legibility* — each dimension gets a fixed, recognisable ring,
and tilt increases gradually so near rings (the ones a reader is likely cycling through) stay
near-face-on; *stability* — slot assignment is append-only per dimension id, never recomputed from
total count; *non-occlusion* — bounded tilt plus drawing the focus last/on top keeps every ring's
front arc visible.

#### 9.2.4 Dimension-name labels; ring-slot placeholders

A dimension is itself a cell whose content is its name, so an edge label is simply `textOf(dim)`
applied to the dimension cell itself — no view-minted label cell is needed for the label text. A
view-minted ring-slot placeholder cell *is* needed for each unbound dimension `c` links on, carrying
a `d.ring-dim` link to the real dimension cell, giving the renderer a stable screen anchor distinct
from the dimension cell's own text. This ring-slot cell is view-minted (ephemeral, tossed on rebind
per I3, recoverable per I4).

#### 9.2.5 Movement into an unbound ring neighbour

**Ruling: a temporary bind, scoped to the current view epoch, not a persistent rebind and not a
refusal.** Entering via a ring spoke whose dimension is not currently bound rebinds the
least-recently-used axis to that dimension and walks focus there, re-deriving the whole axis/ring
layout fresh at the new focus (I3's toss-and-rebuild naturally re-evaluates whichever dimension the
user just walked along). No `d.binds` link outside this one axis is rewritten. The temporary bind is
stacked on that axis slot ahead of the binding the user chose, is not written to `system://layout`,
is not an undo entry, and is lifted — restoring the user's binding — when the reader next moves
along a different axis or switches view. The move is announced on the status line
(`"<dimension> is now bound to <axis>."`) so the user is never surprised by a side-effect of walking
somewhere. This is argued against two alternatives: a hard refusal ("movement only along bound
dimensions") would contradict Nelson's "move along any connection, bound or not," and a *silent
persistent* rebind would corrupt the user's deliberately chosen bindings on an ordinary lateral
move.

#### 9.2.6 Drag-to-rebind

State machine:
`Idle → Picking(edge) → Dragging(edge, candidateAxis?) → {Committed | Cancelled} → Idle`.
`Idle → Picking`: a CPU-side ray-vs-capsule hit test (via a new `unprojectScreenToRay`,
`spatial.hpp` extension) against each visible edge, confirmed against the next frame's GPU pick
result before committing to a drag (CPU prediction and GPU ground truth must agree; a mismatch means
no drag starts, never a wrong one). `Picking → Dragging`: past a small pixel threshold, re-test
every frame against the bound-axis `AxisGizmo`s; the nearest one within a screen-space snap radius
becomes `candidateAxis`. While dragging, a speculative `layout()` call previews where the dragged
dimension's neighbours would relocate, discarded if not committed (safe and cheap because `layout()`
is pure, §8.4). `Dragging → Committed`: pointer-up with a `candidateAxis` set calls
`ViewAxisSet::bind(candidateAxis, draggedDimension)`. `Dragging → Cancelled`: pointer-up with no
candidate, or Escape, discards the preview with no state change. The keyboard twin (select the
spoke, invoke "Bind selected spoke to axis…", confirm with Enter) produces the identical end state
(V-R22).

#### 9.2.7 LOD at high valence

**Second-hop stubs**: a ring member with further neighbours on other dimensions draws a short,
unlabelled, reduced-alpha stub beam — an affordance, not a recursive placement; activating it moves
focus there, producing a fresh `layout()` call. **Ring aggregation**: rings beyond a configurable
index collapse their members into one "aggregate ring" badge per dimension (`+37`), still positioned
and tilted per §9.2.3, expandable by activation into individually placed members — a display budget,
never a silent omission.

#### 9.2.8 Interaction

Arrow-key movement along a bound axis works as in every other view. `[`/`]` (ring-select) move a
selection cursor around unbound spokes without moving focus, previewing the far cell's content in a
side panel. `Return` on a selected spoke walks focus there (§9.2.5). `B` binds the selected spoke to
the next available axis (keyboard twin of drag).

#### 9.2.9 Degenerate cases

A cell with no neighbours on any dimension: empty ring, hub drawn alone, side-panel note
`"No linked cells on any dimension."` A single-cell slice: same. Valence in the hundreds, one per
dimension — that is, hundreds of dimensions: aggregate rings per §9.2.7, never a frame-rate cliff
from individually rendering hundreds of positions.

#### 9.2.10 Accessibility

The hub is `role: cell` with a `valence` property per dimension (`"d.2: 3 neighbors"`); each spoke
is a child `role: group` named by its dimension, containing the neighbour node(s), or, when
clustered, a single `role: group` named `"d.7: 214 more, collapsed"` with an expand action. Reading
order is a stable, declared order — bound axes first, in axis-rank order, then unbound dimensions in
the slice's own declaration order — never angular/visual position, which is meaningless to a screen
reader.

#### 9.2.11 Tunables

See §11's settings table (`ring.*`, `axis.*`, `label.*`).

#### 9.2.12 Acceptance tests

A dimension's ring slot is unchanged across a focus move that does not remove it (V-R21); drag and
keyboard rebind paths produce the identical final `d.binds` target (V-R22); every ring position's
`PlacedItem`/side-panel data carries the far cell's own neighbour count (V-R23); a property test
confirms no dimension's ring slot is ever recomputed from the total dimension count.

#### 9.2.13 Diagram

```text
                 d.employer (ring k=1, tilted)
                      *
                      |
   d.2 (bound, X)  ---c---  d.1 (bound, Y)
                      |
                      *
                 d.phone (ring k=0)
       (unbound rings fan back into depth as valence grows; bound
        dimensions stay on the flat screen axes)
```

______________________________________________________________________

### 9.3 Dimensional pack view

#### 9.3.1 Purpose

Browsing a cell's data by category rather than by one dimension at a time — "browse a
person-by-(email|phone|address) pack and pull one phone number out," or collapsing a many-dimension
table onto one axis for continuous reading. Reach for it when several dimensions should be treated
as one unit.

#### 9.3.2 The break a pack repairs

Let `G = {d1, …, dk}` be a group bound to one axis. A real cell `c` may have up to `k` distinct
posward neighbours (one per `di`) and up to `k` distinct negward neighbours — not single-valued,
which is exactly I1 broken by construction if rendered naively as "the posward neighbour along the
axis." A **pack** restores single-valuedness by replacing "the posward neighbour" with "the posward
pack," one view-minted cell standing for the set of posward neighbours across every member
dimension.

#### 9.3.3 Two dimensions, not one: `d.pack` and `d.packing`

**Ruling (V2): `d.pack` and `d.packing` are two distinct `DimRef`s, matching the names the
requirement used.**

- **`d.pack`** relates a pack container to its own contents. The container is the headcell; its
  `d.pack` posward neighbour is the pack's first constituent. Entering a pack is one step posward on
  `d.pack`; leaving is one step negward from that first constituent back to the container. A
  constituent that is itself a container for a nested sub-pack carries its own `d.pack` posward link
  to its own first sub-constituent — nesting falls out of this for free, because the same dimension,
  read from a different cell's slot, plays "container" or "constituent" depending on which direction
  is read, exactly the dual-role idiom `d.clone` already uses (`Manifold::cloneMaster`,
  `manifold.hpp:446-458`: a member holds the negward link to its master; the master is whichever
  cell a negward walk bottoms out at).
- **`d.packing`** is the rank of constituents within one pack: `constituent_k`'s `d.packing` posward
  neighbour is `constituent_{k+1}`. This is a separate dimension from `d.pack` specifically so the
  container is never itself a member of its own constituent rank — a container's own `d.packing`
  slot is simply unused. Movement along `d.packing` from constituent to constituent never needs to
  special- case "am I at the head, where the container lives instead."
- **Movement from pack to pack** (one position on the bound axis to the next) uses a *third*
  relationship: the bound axis's own `axisStep` dimension (`ViewManifold::axisStepDim(axis)`, a
  derived-arena dimension cell, §6.2), walked exactly as a single bound dimension would be — the
  predecessor (a real cell at step 0, or a container at step ≥ 1) has a posward neighbour on this
  axis equal to the next container, and vice versa negward.

**Why two dimensions here, against the alternative of one dimension with two conventional direction
names** (following `d.clone`'s single-`DimRef` precedent to its conclusion): the requirement names
`d.pack` as "the container" and `d.packing` as "the constituents" as two separate vocabulary items,
and the container→first-constituent step and the sibling-rank-among-constituents step are genuinely
different relations — collapsing them onto one `DimRef` would make the container a member of its own
constituent rank (its own `d.packing` slot would need to mean something, and the obvious candidate,
"the container is constituent zero," reintroduces exactly the "wrap the origin cell in a pack of
one" problem §9.3.7 explicitly avoids for real cells at step 0). Keeping `d.clone`'s idiom for the
*single* cross-container relationship (`d.pack`, used dual-role exactly as `d.clone` is) while
giving the *rank among siblings* its own dimension (`d.packing`) is the smaller, more precise
change: one extra `DimLink` per constituent cell (12 bytes), paid only by view-minted cells, never
by real ones, in exchange for a representation with no double duty anywhere. This is priced in
ruling V2 (§16); the single-dimension alternative, and the reasoning for it, is recorded in refused
alternatives there.

#### 9.3.4 The BFS-frontier pack-step definition

Step `n` of the pack rank is the set of cells reached by stepping from members of step `n-1` along
*any* dimension in `G`, from the real cell `c` at step 0. Formally, with
`Reach(S) = {m : ∃s ∈ S, ∃di ∈ G, linked(s, di, POS) = m}` (mirror with `NEG` for the negward pack):

```math
\begin{aligned}
\text{Pack}^+_0 &= \{c\} \\
\text{Pack}^+_n &= \text{Reach}(\text{Pack}^+_{n-1}) \setminus \left(\text{Pack}^+_0 \cup \dots \cup \text{Pack}^+_{n-1}\right), & n \ge 1
\end{aligned}
```

The walk stops at the first `n` with `Pack⁺ₙ = ∅`.

This is chosen over a per-dimension "n-th neighbour" zip because a zip has no natural alignment
between dimensions whose ranks differ in length — dimension A's 3rd posward cell has no reason to
correspond to dimension B's 3rd — and needs ad hoc ragged-end handling the frontier definition gets
for free.

- **Duplicates**: two member dimensions reaching the same cell from the same source — the set
  subtraction in `Reach(S)` de-duplicates within one step; the cell appears once in `Pack⁺ₙ`, with
  (optionally, for the UI) both `di` edges recorded as provenance.
- **Cycles**: a member dimension looping back onto an already-packed cell — the `\ (earlier packs)`
  subtraction means a cell already claimed by a closer step is never re-claimed, so a loop simply
  stops contributing once it closes, the same safety `cloneMaster`'s loop guard already has.
- **Ragged ends**: a member dimension running out of cells before others — handled automatically by
  `Reach`; a dimension contributing nothing from a given source simply does not appear in that
  frontier, and the walk naturally narrows to the dimensions still producing cells.

**Worked example 1 (simple group).** `G = {d.author, d.topic}`. `Pack⁺₀ = {c}`.
`Reach({c}) = {a1, t1}` → `Pack⁺₁ = {a1, t1}`. `Reach({a1, t1}) = {a2, t2}` → `Pack⁺₂ = {a2, t2}`.
If `d.author` ends at `a2` but `d.topic` continues: `Reach({a2,t2}) = {t3}` → `Pack⁺₃ = {t3}` (the
ragged end, handled automatically); `Reach({t3}) = ∅` → stop. Four containers along the axis from
`c`: `[c] [a1,t1] [a2,t2] [t3]`.

```text
   step0        step1         step2        step3
  +-----+      +------+      +------+      +----+
  |  c  | ---> | a1   | ---> |  a2  | ---> | t3 |
  +-----+      | t1   |      |  t2  |      +----+
               +------+      +------+
```

**Worked example 2 (nested group with a duplicate and a ragged end).**
`G' = {d.author, d.topic, d.region}` where `d.region` nests the group `{d.city, d.country}` as one
of its "dimensions" (§9.3.7): resolving `G'`'s contribution to `Reach` at a source cell first
resolves `d.region`'s own nested pack from that source and treats its container's membership as
`d.region`'s contribution to the outer step. Suppose at step 1, `d.author`'s posward from `c` is
`a1` and `d.topic`'s posward from `c` is *also* `a1` (a convergence — both dimensions reach the one
cell): `Reach({c})` evaluated over `{d.author, d.topic}` yields `{a1}` with two recorded provenance
edges, so `Pack⁺₁ = {a1}` — a pack of one with two "how reached" tags, not two packs. If
`d.region`'s nested pack from `c` is empty (no `d.city`/`d.country` links from `c`), `d.region`
contributes nothing at step 1 — the ragged end — and `Pack⁺₁`'s membership is unaffected by the
dimension that had nothing to offer.

```text
   step0              step1 (convergence + ragged nested dim)
  +-----+            +--------------------------+
  |  c  | ---------> | a1  (via d.author AND     |
  +-----+            |      d.topic; d.region    |
                      |      contributed nothing) |
                      +--------------------------+
```

#### 9.3.5 Movement is single-valued and reversible

**Single-valued**: "move posward on the group axis" from a representative `r` is "move to the
container of `Pack⁺₁(r)`," lazily materialized — exactly one target, by construction (`Pack⁺₁` is
one set, hence one container). I1 holds for the view-visible axis: `r` has exactly one packed
posward neighbour even though it may have `k` real per-dimension neighbours underneath.

**Reversible, proved**: the axis `DimRef` (§9.3.3's third relationship) is maintained by
`ArenaManifold::link()`'s own two-sided-link guarantee — every call to
`link(predecessor, axisStep, POS, container)` also sets `container`'s negward `axisStep` slot to
`predecessor` in the same call, exactly as every other `link()` call in the codebase does for every
other dimension. Therefore `linked(linked(r, axisStep, POS), axisStep, NEG) == r` holds by the
existing link-maintenance invariant applied to pack containers as cells like any other —
reversibility is not a new property requiring new machinery to prove; it is the pre-existing
two-sided-link guarantee, inherited for free (V-R26).

#### 9.3.6 Laziness: nothing minted until the cursor steps there

Building `Pack⁺ₙ` from `Pack⁺ₙ₋₁` costs `O(|Pack⁺ₙ₋₁| × |G|)` link reads — cheap, bounded by the
group's size (typically 2-4) times the previous pack's width. `PackBuilder::computeNextStep`
performs only this read-only computation, eagerly enough to answer "is there a next step" (for a
disabled- further-movement affordance at a ragged end), but the container cell and its
`d.pack`/`d.packing` links are minted (`PackBuilder::materialize`) only when the cursor actually
steps there. This is the same "lazy frontier" pattern `ArenaManifold::materializeFrontier()`
(`arena_manifold.hpp:380-385`) already implements for federation proxies, reused rather than
reinvented: compute reachability cheaply, mint lazily, cap the eager pre-mint at a small `maxSteps`.

#### 9.3.7 Nesting

A dimension group can itself include another group as one of its "members" (a `d.dim-group` run may
contain another group cell). Operationally, "stepping along a nested group's contribution" first
resolves that inner group's own pack at the current source cell and treats the resulting container's
membership as that member's contribution to the outer `Reach()`. This is recursive by construction
and terminates because each nesting level strictly reduces the group-membership list being expanded
— a group may not contain itself, detected and refused (`ViewError::GroupCycle`) the same way
`cloneMaster`'s loop guard treats a self-referencing rank, never infinitely recursed (V-R28).

#### 9.3.8 The origin cell is a pack of one, never minted

`Pack⁺₀ = {c}` is never wrapped in a container — `c` is already its own recoverable real cell with
its own identity, and wrapping it would both double the cell count of every pack-view axis for no
benefit and violate I4 trivially (there is nothing to recover; it is already real). A one-member
pack at step `n ≥ 1` (the convergence case) *is* minted as an ordinary container with one
constituent, and renders with the same dashed-border/bracket visual distinction as any other pack —
collapsing a one-constituent pack to look like an ordinary cell is refused (§16, ruling V9): it
would reintroduce exactly the "mistake a view cell for stored data" risk the visual language exists
to prevent.

#### 9.3.9 Pack frame rendering

**Fan, chosen over stacked-deck or grid.** Stacked-deck reads well for ordered small decks but hides
member count at a glance and implies an ordering the group's members do not necessarily have (they
arrive from different dimensions, not one sequence). A rigid grid wastes space for members with very
different content-fit sizes, the same problem stretch vanishing's rejected row/column option has. A
fan — members arranged along a shallow arc inside the frame, each slightly rotated and z-offset so
all are edge-visible and no two fully overlap — scales legibly from one to a few dozen visible
members (bounded by `pack.fanMaxVisible`, beyond which it degrades to a count badge, never a silent
omission), and its arc radius grows with member count without requiring a relayout of the parent
axis. Each fanned member's edge-facing side carries a thin colour tab matching the contributing
dimension's colour, the same one its axis and ring edges use (read from `system://ui`, one encoding
per dimension everywhere).

#### 9.3.10 Axis placement; nesting LOD

The bound group's axis places packs at `packSpacing` intervals exactly as a single bound dimension
places cells, sized to each pack's fan bounding box with a `packMinWidthPx`/`packMinHeightPx` floor
so a one-member pack does not look degenerate beside a twenty-member one. A pack whose constituent
is itself a pack draws the inner frame inside the fan slot at up to `pack.nestingLodDepth` levels
before collapsing to a count badge — a display budget, not a model ceiling; the manifold may nest
arbitrarily deep (V-R28), the draw pass simply stops expanding frames past the configured depth.
Depth-`N` frames dim by `nestDimFactor^depth` as a legibility cue.

#### 9.3.11 Enter/retrieve/leave

**Entering** a pack descends `d.pack` posward from container to first constituent, then `d.packing`
posward to reach any further constituent — a focus change, not a separate mode: the pack's
representative becomes the new context, the camera dollies toward it, its fan expands, and sibling
packs fade via the same edge-fade mechanism as stretch vanishing (§9.1.5), reused rather than
duplicated. **Leaving** ascends `d.pack` negward from the first constituent back to the container,
then the group axis to a sibling if desired — the reverse animation. **Retrieving** a constituent
("Retrieve") focuses that cell as itself, outside the pack's container framing, via a focus move to
its real `CellRef` — never a copy (V-R27); the view afterward shows that cell as an ordinary focused
cell, either in the view that was active before pack view or in pack view with the retrieved cell as
the new representative.

#### 9.3.12 Interaction

Step actions move pack-to-pack along the bound axis. "Enter pack" (`Return`) descends into the
container; "Leave pack" (`Escape`) ascends back out. Selecting a constituent chip and invoking
"Retrieve" (`Shift+Return`) focuses it directly.

#### 9.3.13 Degenerate cases

A group with one member dimension: the pack degenerates to an ordinary rank-walk along that
dimension; the container still renders (for visual consistency) holding exactly one constituent, so
entering and retrieving show the same cell. An empty group: cannot be bound to an axis — refused
(`ViewError::EmptyGroupBind`). A pack with zero constituents (its representative has no links on any
member dimension): still enterable, immediately empty, with a stated message, rather than refused
outright — the user sees the empty state rather than being blocked from checking.

#### 9.3.14 Accessibility

A pack container is `role: group`, never `role: cell` (this matters more, not less, for a non-visual
user, who has no border-dash cue available), named `"Pack: <N> constituents from <M> dimensions"`;
each constituent chip is a child node named `"<cell text> (via <dimension>)"`, explicitly naming
which member dimension contributed it (V-R32). Entering/leaving fires an ordinary a11y focus-change
event.

#### 9.3.15 Tunables

See §11's settings table (`pack.*`).

#### 9.3.16 Acceptance tests

Moving posward then negward along the group axis returns to the starting representative for every
step along both worked examples (V-R26, §9.3.5); `computeNextStep` on worked example 1 produces
exactly `{a1,t1}`, `{a2,t2}`, `{t3}`, `∅` in order; on worked example 2 it produces `{a1}` with two
provenance edges at step 1; "Retrieve" on a constituent focuses its real `CellRef` and leaves no new
operation in `xudu-dump --section=ops`; binding an empty group is refused with `EmptyGroupBind`.

______________________________________________________________________

## 10. Mixing xanadoc and zigzag views

### 10.1 Panes

A `ViewHost` owns a pane tree; each `Pane` owns one `Viewport{ViewportDesc}` and one
`unique_ptr<View>`. Splitting, closing, and cycling focus are generic actions (`pane.split.*`,
`pane.close`, `pane.focus.next`) — not "split zigzag"/"split xanadoc" — because either half may hold
either kind of content. A `ViewHost` is the direct generalisation of
`apps/xuzz/view_coordinator.{hpp,cpp}`'s single `Unified`/`XanadocOnly`/`ZigzagOnly` toggle to an
actual tree; §15 gives the migration path from one to the other.

### 10.2 Compositing

Each pane's `ViewportDesc{x, y, width, height}` plus an optional owned `viewProjection` describes a
screen rectangle. Scissor is required (`RenderDevice::setScissorRect`, new, thin per backend) so one
pane's draws never bleed into a sibling's rectangle. Depth is partitioned two ways: depth-clear
between panes for non-overlapping splits (the common case — simpler, each pane owns the full `[0,1]`
range), or depth-range partitioning (`setDepthRange(min,max)`, new) for true embedding (a slice view
drawn inside a xanadoc cell's rectangle), extending the transform-resolver pattern
`ZigzagVisualizer::setPresentationOrigin`/`setPresentationTransformResolver` already use. Render
order is back-to-front for overlapping translucent panes (reusing `src/renderer.cpp:388-391`'s
existing document sort, generalised from documents to panes); a nested sub-viewport draws after its
host's background and before the host's own foreground overlays.

### 10.3 The `PageView` seam

This document specifies only the seam a xanadoc view implements — not its layout algorithm, which is
explicitly a follow-up specification's job (ruling V12). What is guaranteed to the follow-up:

- `PageView` derives from `View` and implements the identical `attach`/`detach`/`onBindingChanged`/
  `onCursorMoved`/`onStoreAdvanced`/`layout` contract `SliceView` implements; `ViewHost` never
  special-cases "is this pane a slice pane or a page pane" beyond this one refinement boundary.
- `LayoutInput`/`LayoutSink`/`ViewportDesc` are reused unchanged; a `PageView`'s `layout()` pushes
  `PlacedItem`/`PlacedEdge` records whose `cell` field is left default — nothing about
  `LayoutSink`'s shape is slice-specific.
- A `PageView` needs nothing from `ViewManifold`/`ArenaManifold` — it has no zzstructure invariant
  to keep, and this document does not require it to acquire one.
- `Manifold::contentOf`/`textOf` already give a cell's content as an ordinary `PrimediaSpan`, so a
  page embedding "the content of cell X" is an ordinary transclusion, not a special integration.
- A `SliceView` embedded inside or beside a `PageView` exposes only real `CellRef`s across that
  boundary and exposes focus identity plus a transform/placement hook (V-R12), matching what
  `BridgeCoordinator`/`ZigzagPresentationSurface` already provide.

What the follow-up specification must still decide: the xanadoc-side layout algorithm itself (onion-
skin, flow, pagination inside a pane); which parts of today's `xanadu::Views` coordinator get carved
into a concrete `PageView` versus staying host-level; and the UI for creating/arranging page panes
analogous to §11's chord table. Nothing in this document's `View`/`ViewHost`/`LayoutSink` contract
is expected to change to accommodate those decisions — that is the guarantee this section exists to
make.

### 10.4 Link-navigation contract across panes

`design/ui_workflow_xuzz_navigation.md`'s "one link identity and both endsets in view" and
"companion document" rules generalise without new rules: selecting a link pins its context
regardless of which pane holds the caret; entering an endpoint in a different pane moves that pane's
focus without touching another pane's camera or caret; if no pane currently shows the target's kind
of content, a new pane opens with an origin marker connecting back, never a silent replace of the
pane the user was reading. Activity back/forward restores which pane layout was active at a visit's
save point, since a visit's saved view already covers camera and companion state.

______________________________________________________________________

## 11. Interaction summary

### 11.1 Default chords

Context key: **Z** = active while a ZigZag pane has the keyboard; **X** = reachable from anywhere in
Xuzz (the `Alt`-prefixed twin); **AW** = all-dim walk only; **PV** = pack view only. Scheme: a
single unclaimed leader, `Ctrl+Alt+V` ("View"), matching the project's existing `Ctrl+Alt+*` leader
family (`Ctrl+Alt+N`, `Ctrl+Alt+[`/`]`, `Ctrl+Alt+L`).

| Action id                      | Vortex call                            | Default chord         | Context              |
| ------------------------------ | -------------------------------------- | --------------------- | -------------------- |
| `view.palette.toggle`          | `std:view/toggle_palette`              | `Ctrl+Alt+V`          | Z/X                  |
| `view.cycle.forward`           | `std:view/cycle`                       | `Ctrl+Alt+Shift+V`    | Z/X                  |
| `view.select.stretchVanishing` | `std:view/select("stretch-vanishing")` | `Ctrl+Alt+V` then `1` | Z                    |
| `view.select.allDimWalk`       | `std:view/select("all-dim-walk")`      | `Ctrl+Alt+V` then `2` | Z                    |
| `view.select.packView`         | `std:view/select("pack-view")`         | `Ctrl+Alt+V` then `3` | Z                    |
| `pane.split.horizontal`        | `xuzz.pane.split("horizontal")`        | `Ctrl+Alt+Shift+H`    | X                    |
| `pane.split.vertical`          | `xuzz.pane.split("vertical")`          | `Ctrl+Alt+Shift+J`    | X                    |
| `pane.close`                   | `xuzz.pane.close`                      | `Ctrl+Alt+Shift+W`    | X                    |
| `pane.focus.next`              | `xuzz.pane.focusNext`                  | `Ctrl+Alt+Shift+Tab`  | X                    |
| `pane.openAsPage`              | `xuzz.pane.openSelectionAsPage`        | `Ctrl+Alt+Shift+O`    | X (selection active) |
| `axis.rebind.cycle`            | `std:view/cycle_axis_dimension`        | `Ctrl+Tab`            | Z (HUD row focused)  |
| `axis.rebind.pick`             | `std:view/pick_axis_dimension`         | `Ctrl+Alt+D`          | Z                    |
| `group.new`                    | `std:view/new_dimension_group`         | `Ctrl+Alt+G`          | Z                    |
| `group.edit`                   | `std:view/edit_dimension_group`        | `Ctrl+Alt+Shift+G`    | Z                    |
| `rebind.undo`                  | `std:view/undo_rebind`                 | `Ctrl+Alt+Z`          | Z                    |
| `ring.selectSpoke.next`        | `std:view/ring_select_next`            | `]`                   | AW                   |
| `ring.selectSpoke.prev`        | `std:view/ring_select_prev`            | `[`                   | AW                   |
| `ring.bindSelectedToAxis`      | `std:view/ring_bind_selected`          | `B`                   | AW                   |
| `pack.enter`                   | `std:view/pack_enter`                  | `Return`              | PV                   |
| `pack.leave`                   | `std:view/pack_leave`                  | `Escape`              | PV                   |
| `pack.retrieveConstituent`     | `std:view/pack_retrieve`               | `Shift+Return`        | PV                   |

Every row is additive to `defaultSettingSpecs(SystemDocKind::Keymap)` and follows the existing
`both()` registration idiom (`zigzag_commands.cpp:28-35`), giving each a bare-Z and `Alt`-X twin
where applicable; a chord already starting with `Ctrl+Alt` needs no twin, since it is already
globally reachable.

### 11.2 Status-line and announcement text

| Situation                                    | Text                                                                  | Error                       |
| -------------------------------------------- | --------------------------------------------------------------------- | --------------------------- |
| No neighbour further along a dimension       | `"No cell further along <dimension> <direction>."`                    | none (normal `MoveOutcome`) |
| Axis rebound by walking an unbound dimension | `"<dimension> is now bound to <axis>."`                               | none                        |
| Binding discarded old helper cells           | `"Discarded N helper cells for <old binding>."`                       | none                        |
| Axis already bound elsewhere                 | `"<dimension> is already on the <other-axis> axis."`                  | `AxisAlreadyBound`          |
| Binding an empty group                       | `"Group '<name>' has no dimensions yet — add one before binding it."` | `EmptyGroupBind`            |
| Entering a pack with zero constituents       | `"This pack has no constituents."`                                    | none (allowed, not refused) |

### 11.3 Settings table

Rendering/physics tunables live in `system://settings` (beside existing `Zigzag*`/`Bridge*` keys);
chord-adjacent UI behaviour in `system://ui`; per-slice layout/binding state in `system://layout` —
the split `.claude/rules/architectural_governance.md` §2 already requires.

| Setting                                   | Lives in | Type/unit         | Default                     | Rationale                                                   |
| ----------------------------------------- | -------- | ----------------- | --------------------------- | ----------------------------------------------------------- |
| `view.arena.reclaimThresholdCells`        | settings | count             | 4096                        | dead derived cells tolerated before `reclaim()` runs (§6.4) |
| `stretch.edgeFadeStartUv`                 | settings | UV fraction [0,1] | 0.65                        | fade starts this far from centre                            |
| `stretch.overfillMargin`                  | settings | ratio             | 1.3                         | BFS fill-stop multiplier over viewport extent               |
| `stretch.hiddenConfirmMs`                 | settings | ms                | 120                         | sustained-clip time before hysteresis hides a cell          |
| `stretch.fadeFloorAlpha`                  | settings | 0-1               | 0.15                        | never fully invisible before culled; WCAG-checked           |
| `stretch.breadcrumbDepth`                 | settings | count             | 6                           | breadcrumb strip length                                     |
| `stretch.maxVisibleCellsReserve`          | settings | count             | 256                         | initial `LayoutSink` arena reservation                      |
| `ring.radius0`                            | settings | px (world)        | 160                         | innermost unbound-dimension ring radius                     |
| `ring.radiusStep`                         | settings | px (world)        | 70                          | radius increment per ring                                   |
| `ring.tilt0`                              | settings | degrees           | 15                          | innermost ring's tilt from vertical                         |
| `ring.tiltStep`                           | settings | degrees           | 8                           | tilt increment per ring                                     |
| `ring.tiltMax`                            | settings | degrees           | 75                          | cap on ring tilt (keeps focus unoccluded)                   |
| `ring.lodCollapseIndex`                   | settings | ring index        | 6                           | ring index beyond which members collapse to a badge         |
| `ring.depthCueExponent`                   | settings | exponent          | 1.5                         | alpha falloff exponent vs. `cos(tilt)`                      |
| `ring.clusterThreshold`                   | settings | count             | 12                          | valence above which a spoke clusters                        |
| `axis.snapRadiusScreenPx`                 | settings | px (screen)       | 36                          | drag-to-rebind snap radius                                  |
| `axis.hitRadiusWorld`                     | settings | world units       | 6                           | capsule radius for edge/gizmo ray hit tests                 |
| `axis.gizmoLength`                        | settings | world units       | 50                          | drawn length of an axis drop-target gizmo                   |
| `label.maxNudgeWorld`                     | settings | world units       | 30                          | collision-avoidance max nudge before abbreviation           |
| `pack.fanMaxVisible`                      | settings | count             | 24                          | members shown in a fan before badge LOD                     |
| `pack.fanArcDegrees`                      | settings | degrees           | 110                         | angular spread of a pack's fan                              |
| `pack.nestingLodDepth`                    | settings | levels            | 3                           | max recursive pack-frame draw depth before badge            |
| `pack.nestDimFactor`                      | settings | ratio/level       | 0.85                        | opacity multiplier per nesting level                        |
| `pack.minWidthPx`/`minHeightPx`           | settings | px (world)        | 80 / 60                     | comparable-size floor for axis-placed packs                 |
| `pack.spacingPx`                          | settings | px (world)        | 24                          | clearance between packs along the bound axis                |
| `pack.packingTightnessPx`                 | settings | px                | 6.0                         | gap between constituent chips inside a pack                 |
| `pack.maxConstituentsShown`               | settings | count             | 40                          | simultaneous chip render cap, never a data cap              |
| `view.labelSizePx`                        | settings | px                | 12.0                        | shared axis/dimension-name label text size                  |
| `view.transitionSpeed`                    | settings | units/s           | 10.0                        | view-switch crossfade rate                                  |
| `transition.tossFadeSeconds`              | settings | s                 | 0.25                        | fade-out duration for discarded view-minted geometry        |
| `ui.zigzag.reducedMotion`                 | ui       | bool              | false                       | collapses crossfades/springs to instant cuts                |
| `ui.zigzag.viewOnlyCellOpacity`           | ui       | 0-1               | 0.55                        | fixed opacity for pack/axis-label helper cells              |
| `viewport.scissorEnabled`                 | settings | bool              | true                        | diagnostics override for mixed-viewport scissor             |
| `layout.zigzag.<sliceId>.lastView`        | layout   | string            | unset → `stretch-vanishing` | which View a slice reopens into                             |
| `layout.zigzag.<sliceId>.axisBindings`    | layout   | structured        | `{}`                        | persisted axis→dimension-name bindings                      |
| `layout.zigzag.<sliceId>.dimensionGroups` | layout   | structured        | `{}`                        | user-authored groups, per slice                             |

Existing tunables reused unchanged: `scene_.layout_speed`/`alpha_speed`, `presentation_config_`'s
padding/clearance fields, `scene_.neighborhood_radius`. Every new setting group needs the Schema &
Purpose page and Notes page bidirectionally xanalinked per
`.claude/rules/architectural_governance.md` §2 (V-R29) — this table is that page's content.

______________________________________________________________________

## 12. Rendering additions

| Addition                                            | New/reuse                                                                      | Size                                                                                                         | Backend coverage                                                              |
| --------------------------------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------- |
| Fully-inside clip test                              | New, beside `outsideFrustum` in `draw_budget.hpp`                              | ~20 lines, shares corner-building code                                                                       | Pure math; covered by headless layout unit tests                              |
| Per-instance billboard rotation for labels          | New: extend `glyph.vert.glsl`'s instance transform with a face-camera flag bit | Small-medium: one attribute bit + a `mat3` camera-basis uniform already derivable from `view.front`/`upward` | New comparison scene with rotated/tilted labels, diffed across GL/GLES/Vulkan |
| Scissor rect on `RenderDevice`                      | New `setScissorRect`/`clearScissor`, per backend                               | Small per backend (`glScissor`; Vulkan dynamic scissor state)                                                | Two side-by-side mock viewports, assert no bleed, across all three backends   |
| Depth-range partition                               | New `setDepthRange(min,max)`, per backend                                      | Small, same shape as scissor                                                                                 | Same comparison scene extended with an embedded sub-viewport                  |
| CPU ray construction/unprojection                   | New `unprojectScreenToRay` in `spatial.hpp`, inverse of `projectToScreen`      | Tiny, pure math                                                                                              | Headless unit test only                                                       |
| Pack-frame & ring/gizmo draw primitives             | Reuse `Canvas::addRect`/`addLine`, `Beams`                                     | None beyond composing existing primitives                                                                    | Added to existing scene-generator tooling                                     |
| `FrameContext` per-contributor viewport/VP override | New optional field                                                             | Tiny, header-only                                                                                            | Exercised by the mixed-viewport scene                                         |

Nothing here requires a new vertex format class beyond the billboard-flag bit; the glyph, beam, and
image pipelines already cover rects, lines, text, edges, and images — everything every proposed view
draws.

______________________________________________________________________

## 13. Performance budget and probes

| Pass                           | Complexity                                                                         | Cached as                                              | Invalidated by                                                                                                                  |
| ------------------------------ | ---------------------------------------------------------------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------- |
| Stretch BFS packing            | O(k log k), k = cells placed before fill-stop                                      | Skyline state + BFS frontier queue, kept across frames | Focus change (full rebuild); resize (resume, no restart); content edit (re-measure + reflow only cells after it in visit order) |
| Clip/fade classification       | O(k) per frame, 8-corner test per visible cell                                     | Hysteresis timers per `CellRef`                        | Fully-inside state transition; cleared on focus change                                                                          |
| All-dim ring placement         | O(valence) per focus change, O(1) amortised per dimension (append-only slot table) | Per-dimension ring-slot table, session-scoped          | Explicit dimension deletion only; never by navigation                                                                           |
| Edge-label collision avoidance | O(v²) worst case, bounded by the LOD cutoff                                        | Per-frame, not cached                                  | N/A                                                                                                                             |
| Pack fan layout                | O(m log m) per pack, m bounded by `pack.fanMaxVisible`                             | Cached per pack id like cell measurement               | Pack membership change, member content edit, enter/exit                                                                         |
| Content-fit measurement        | One `measureText` call per uncached `(CellRef, ViewKind, widthLimit)`              | `cell_layouts_`-equivalent map                         | Text/format edit; `widthLimit` change                                                                                           |
| Mixed-viewport compositing     | O(views) scissor/depth-range state changes per frame                               | N/A                                                    | N/A                                                                                                                             |
| Toss (§6.4)                    | O(1) epoch bump; O(discarded) deferred reclamation; O(visible) re-derivation       | N/A                                                    | Rebind, focus change invalidating the display layer                                                                             |

**Measurement**: `tools/layout-latency-probe.cpp` is the existing headless-probe shape; extend it
(or add a sibling) to call each view's `layout()` against synthetic manifolds at increasing valence
(10/100/1000 connections per dimension, 1/10/100 dimensions) and report p50/p99 microseconds plus
`itemCountNeeded`/arena-growth events, following `tools/benchmark-kjv-load.py`'s existing
JSON-report convention. This is what makes "degrades gracefully, no ceiling" falsifiable rather than
asserted: the probe shows the curve stays sub-frame-budget well past any valence a user would reach,
rather than capping and calling it done. The O(1) toss claim specifically is tested as described in
§6.4: an operation-count assertion on `ViewManifold::toss()`, independent of pre-existing cell count
— not a wall-clock timing, so it is deterministic in CI.

______________________________________________________________________

## 14. Testing strategy

All tests run headless per `.claude/rules/headless_tests.md` (V-R33): `SDL_VIDEODRIVER=offscreen`,
`SDL_AUDIODRIVER=dummy`, `LIBGL_ALWAYS_SOFTWARE=1`, the `make test`-exported XDG pair.

- **Unit (engine-only, no GPU)**: `tests/xuzz/view_manifold_test.cpp` — I1 property test after
  random bind/pack/toss sequences; I2 (hand a `ViewCellRef` to a real `Store::setLink`, assert typed
  refusal); I3/epoch staleness (mint, toss, assert the old `ViewCellRef` fails a liveness check); I4
  (`resolveReal` terminates within the bound for nested packs). `tests/xuzz/pack_builder_test.cpp` —
  both worked examples from §9.3.4 verbatim, plus cycle and ragged-end fixtures.
- **Invariant**: `verifyViewManifoldInvariant` run as a standing check inside every other new test
  file, not only its own — any test that mutates a `ViewManifold` calls it before asserting on
  anything else.
- **No-ops-appended check**: a shared test helper `expectNoOpsAppended(store, fn)` wraps every
  movement/rebind/toss test with a before/after `xudu-dump --section=ops` byte-identity check (R8),
  added once and reused by every subsequent view test.
- **Golden-layout**: determinism fixtures for stretch vanishing (skyline packing) and all-dim walk
  (ring-slot assignment), stored under `tests/samples/` alongside existing fixtures — these are
  `LayoutResult` snapshots, not `CompactOpNode`-shaped, so they are not invalidated by the
  `CompactOpNode`-layout-change rule, but are regenerated whenever a layout algorithm's tie-break
  order changes intentionally.
- **UX journeys**: `design/ux_workflow_real_work.md` gains J17-J23 (audit-and-reaxis, read-a-table,
  browse-a-pack, mix-doc-and-slice, switch-views-without-losing-place,
  plugin-view-no-special-casing, rebind-and-undo), each with evidence (frames, a11y dumps,
  `xudu-dump` before/after) and a watch-for list, following the existing journey format exactly —
  see `.claude/skills/xuzz-ux-validation/ SKILL.md`'s "a step reachable only by a flag, script, or
  file is a finding, not a pass" standard.
- **Performance**: the `layout-latency-probe` extension in §13, run as part of the standing
  benchmark suite, not merely on demand.

______________________________________________________________________

## 15. Migration plan

Each step builds and keeps `make test` green; each is committable independently.

1. **Add `invalidateEpoch()`/`currentEpoch()`/`epochFloor()` to `ArenaManifold`** (§6.4). Pure
   addition, no behaviour change for existing callers (Vlog, VQL, federation). *Tests*: new
   `ArenaManifoldTest` cases asserting `currentEpoch()` starts at 0 and increments exactly once per
   call via an instrumented operation counter, checked with 0, 100, and 10,000 pre-minted cells
   present. Existing `arena_manifold_test.cpp`/`arena_federation_test.cpp` unchanged.
1. **Introduce
   `apps/common/xanadu/view/{view,view_error,view_manifold,view_binding,view_link}.hpp`** with
   `ViewManifold`, `ViewAxisSet`, `mintViewLink`, `verifyViewManifoldInvariant`; no app wiring yet.
   *Tests*: new `tests/xuzz/view_manifold_test.cpp` covering I1-I4, all headless, linking only the
   engine (`xuzz_test`).
1. **Implement `d.pack`/`d.packing` via `PackBuilder`** (§9.3) as a standalone library
   (`pack_dims.hpp`) with its own test exercising both worked examples directly against a fixture
   `Manifold`. *Tests*: `tests/xuzz/pack_builder_test.cpp`, new. No application change yet.
1. **Add `view_layout.hpp`'s record types and `view_animation.hpp`'s epoch-guarded
   `AnimationState`**, still with no concrete view. *Tests*: a synthetic "null view" exercising
   layout → animate → nothing-dereferences-a-stale-ref-after-`toss()`, using step 1's operation
   counter to make "toss is O(1)" an assertion, not a claim.
1. **Implement `StretchVanishingView`** in `apps/common/xanadu/view/builtin/`, as pure layout over
   `LayoutInput`, registered through `registerBuiltinViews()`. No application wiring yet. *Tests*:
   new `tests/xuzz/stretch_vanishing_view_test.cpp` with a fixed-size measurer covers determinism,
   axis alignment at radius 1, the fade curve and the partially-clipped-invisible rule.
1. **Implement `AllDimWalkView` and `DimensionalPackView`** the same way, each with its own
   `xuzz_test` file: ring-slot stability across focus changes for the former; pack reversibility
   (move posward then negward returns to the same real cell, §9.3.5) for the latter.
1. **Add `apps/xuzz/view_draw_adapter` and `view_host_app`; retire `ViewCoordinator`.** The host
   owns the pane tree and draws each pane's records. `ZigzagVisualizer` and `xanadu::Views` are each
   registered as one legacy view behind the same `View` interface, so the three new views and the
   two old presentations are selectable side by side and nothing regresses. *Tests*: new
   `tests/xuzz/view_host_test.cpp` for split/close/focus-cycle; `tests/zigzag/test_visualizer.cpp`
   unchanged; `tools/compare-backends.sh` gains a scene per new view.
1. **Move view actions into `apps/xuzz/view_commands`** with their `system://keymap` defaults
   (§11.1), absorbing `apps/zigzag/zigzag_commands.cpp`. **Retire `ViewAxisBinding` as live
   storage** — it survives only as the `system://layout` serialisation and preset format (§7.1,
   §7.4) — and reduce `DimensionBundle` to seed presets for dimension groups. *Tests*:
   `test_visualizer.cpp`'s
   `NavigationAlongDimensions`/`SwapDimensions`/`CycleDimensions`/`DimensionBundleSwitchingAndCycling`
   are **adapted**, not retired — same observable behaviour, asserted against `ViewAxisSet`.
1. **Port the legacy zigzag presentations to views and delete `ZigzagVisualizer`.** Its Cell Content
   and Topology modes become two more built-in slice views; palette, command bar and cell editing
   move to `apps/xuzz/`. `apps/zigzag/` then holds only `unified_transclusion_engine.*`, which moves
   to the engine (below), and the directory is removed along with `ZIGZAG_SRCS` in the Makefile.
   *Tests*: `tests/zigzag/test_visualizer.cpp` cases are re-homed into `tests/xuzz/` against the
   views that replaced each behaviour; a case with no replacement is retired with a line naming why.
1. **Standing no-ops-appended CI check**, added once as the `expectNoOpsAppended` helper (§14),
   reused by every subsequent view test rather than reimplemented per test.
1. **Golden-layout determinism fixtures** for stretch vanishing and all-dim walk, stored under
   `tests/samples/`, regenerated whenever a layout algorithm's tie-break order changes intentionally
   (not subject to the `CompactOpNode`-layout regeneration rule, since these are `LayoutResult`
   snapshots).

`UnifiedTransclusionEngine::ephemeralSlots_`'s bespoke ephemeral-cell cache is migrated at step 9:
the engine moves to `apps/common/xanadu/` and mints through `ViewManifold::mintCell`/`link` rather
than its own `ephemeralByParentAndIndex_` map, so the tree ends with one ephemeral-cell mechanism
(`ArenaManifold`) and not three. It is its own sub-step so it does not block the three required
views.

______________________________________________________________________

## 16. Rulings

**V1. A pane's view space is two sibling `ArenaManifold`s, and the toss is an epoch bump on the
derived one.** Why: `ArenaManifold` already has the one-neighbour-per-direction link maintenance and
the ephemeral/real boundary bit, tested and in production; because dimensions are cells, abandoning
the derived generation's dimension handles makes every link minted under them unreachable in a
constant number of stores (§6.4). Price: two integer fields and three small methods on a class
already shipping; a second arena object per pane; dead cells occupy memory until `reclaim()` runs,
bounded by `view.arena.reclaimThresholdCells`; every read goes through `ViewGraph`'s floor compare.
Refused: (a) a ground-up `ViewManifold` storage type — duplicates a tested invariant, the mistake
R12 warns against; (b) dropping and reconstructing an arena per generation — re-seeds provenance and
federation machinery on every rebind; (c) one arena with nested binding and display marks — an arena
is a stack, so a binding edited after display cells exist forces a synchronous O(minted) release on
the rebind path (§6.2); (d) `release(mark)` as the toss — O(work since the mark), which is not what
the requirement asks for.

**V2. `d.pack` and `d.packing` are two distinct dimensions.** Why: the requirement names them as two
separate vocabulary items with genuinely different relations — container→first-constituent versus
rank-among-constituents — and collapsing them onto one `DimRef` makes the container a member of its
own constituent rank. Price: one extra `DimLink` (12 bytes) per view-minted constituent cell, never
paid by real cells. Refused: a single `DimRef` carrying both roles via `d.clone`'s dual-direction
idiom, by direct analogy to `d.clone` — refused because `d.clone` has only one relation
(member↔master) to encode in two directions, while a pack has two distinct relations (containment,
and sibling rank) that happen to both be needed at once; forcing them onto one `DimRef` would
require the container to occupy a slot in its own members' rank (as "member zero"), which conflicts
with §9.3.8's rule that the origin/container is never itself minted as a constituent.

**V3. A pack step is a BFS frontier across the group's dimensions.** Why: it handles duplicates,
cycles and ragged ends without special cases (§9.3.4); the per-dimension "n-th neighbour" zip
alternative has strictly worse-defined behaviour on exactly those cases. Price: none of substance.
Refused: the zip definition.

**V4. Bindings are cells on a view-owned rank (`ViewAxisSet`), not a struct field or a fixed enum.**
Why: all-dim walk and pack view both need more than three simultaneously bound axes/groups, and
`ViewAxisBinding`'s three named fields and `DimensionBundle`'s closed five-entry enum are both
ceilings the project's own conventions forbid. Price: O(bound-axis-count) traversal to answer
"what's on X," versus O(1) struct read — accepted because the axis count must not be capped, the
same trade R12 already accepted for `d.dims`. `ViewAxisBinding` and `DimensionBundle` are retired as
live storage at migration step 8 and reduced to two remaining roles: the `system://layout`
persistence/ serialisation shape (dimension names survive a session; `CellRef`s do not) and an
initial-preset seed a user may pick from when creating a group. Refused: deleting them outright,
which would leave no seed format and no backward-compatible session-restore shape.

**V5. The one-neighbour-per-direction invariant is enforced at a single link choke point
(`mintViewLink`) returning `std::expected`, plus a verifier (`verifyViewManifoldInvariant`).** Why:
trusting every strategy object to call `arena.link()` correctly is not an invariant, it is a hope;
one auditable function plus a `clang-tidy` pattern match against direct calls makes I1 a property of
the code, not a convention. Price: one extra indirection per link mint. Refused: per-view discipline
with no shared choke point.

**V6. A view never shadows a real cell's bound-dimension `DimLink`; redirection lives entirely on
view-owned dimensions.** Why: shadowing a real cell for display purposes would leave a `DimLink` on
a bound real dimension whose far end is ephemeral, visible to any ordinary real-dimension walk
starting from the shadow, and there would be nothing to "undo" on toss because the base would
already be mutated (copy-on-write). Price: a pack/ring cell is always one hop further from the real
cell than a shadow would be. Refused: allowing shadowing plus an "undo shadow" pass on toss — the
stronger, by-construction option (no shadow is ever written) was chosen over the weaker one (shadow,
then undo).

**V7. Bindings persist in `system://layout`, keyed per-slice, by dimension name.** Why: a binding is
configuration the user set, not document structure and not a completed visit — R8's distinction
applies to configuration exactly as it applies to a cursor. Price: a group built for one slice does
not travel to another slice automatically (open question VU1). Refused: per-user-global groups;
storing bindings in the slice's own store; storing in `system://activity` (which records visits, not
standing configuration).

**V8. Promotion of a pack to real structure is explicit, per-pack, and never automatic.** Why: an
ephemeral viewing choice becoming permanent document structure without the user asking is the exact
failure mode R8 exists to prevent. Price: a user who wants a pack kept must ask by name, every time,
via `promotePack()`, which delegates to the existing `promote()`/`PromotionBudget` refusal rather
than silently truncating. Refused: auto-promoting a pack that has been open "a while";
auto-promoting on edit.

**V9. A pack of one constituent still renders with full pack chrome (dashed border, bracket glyph,
`role: group`), never collapsed to look like an ordinary cell.** Why: collapsing it would
reintroduce the "mistake a view cell for stored data" risk the visual/accessibility language exists
to prevent, for the minor convenience of one fewer visual distinction in one case. Price: a
one-member pack always looks slightly more elaborate than the single cell it could be confused with.
Refused: collapsing a one-constituent pack.

**V10. Movement into an unbound ring neighbour is a temporary, visibly announced bind, not a
persistent rebind and not a refusal.** Why: a hard refusal contradicts "move along any connection,
bound or not"; a silent persistent rebind corrupts the user's deliberate bindings on an ordinary
lateral move. Price: the user must read a one-line status message on every such move to know their
axes changed. Refused: requiring an explicit confirm dialog before every such move (too heavy for an
action with fully reversible, visibly announced undo); forcing an explicit separate "rebind" step
before any lateral move through an unbound dimension.

**V11. A View switch preserves the pane's current binding state; it never snapshots and silently
restores a per-View "own" prior binding.** Why: axis state belongs to the pane/slice, not to the
View, and a stash-and-restore the user never asked for would hide the rebind-discard feedback
(V-R7's honesty requirement) behind an invisible cache. Price: switching views and back does not
"remember" what was bound before the detour, by design. Refused: per-View binding memory.

**V12. Xanadoc (page) view layout algorithms are out of scope for this document; only the `PageView`
seam is specified.** Why: the user's own request scopes xanadoc views to a follow-up; specifying a
layout algorithm for them here would exceed that scope and risk constraining a design nobody has yet
argued through this document's own standard (argued, priced, refused alternatives recorded). Price:
the follow-up spec must still answer what a page's own layout computes; this document only
guarantees it will not need to change the `View`/`LayoutSink` contract to do so. Refused: sketching
a concrete `PageView` layout algorithm here "for completeness" — the one sketch in §9
(`OutlinePageView`) is explicitly illustrative, not normative, and is labelled as such.

______________________________________________________________________

## 17. Open questions

**VU1.** Whether a dimension group's membership should be portable across slices (a user-authored
"contact channels" group reused everywhere) versus the per-slice `system://layout` key shape this
document specifies. Settled by: a real UX validation pass once the feature exists, checking whether
users actually rebuild the same group per slice; if so, an additional slice-independent
`layout.zigzag.groups.*` key can be added without a format break.

**VU2.** Whether nested embedded viewports (a slice view inside a page, inside a pack) exhaust
depth- buffer precision under depth-range partitioning past some small nesting depth. Settled by:
measuring against the actual depth-buffer bit depth in use on each backend once mixed-viewport
compositing is implemented; `ViewportDesc` already carries `nearDepth`/`farDepth` so the mechanism
exists regardless of where the limit lands.

**VU3.** Exact enforcement of a plugin's declared `ViewCapabilities` — can a capability-less plugin
still mint view-only cells, or write to the store at all. Settled by: a sandboxing/trust-model
decision above this document's scope (the API shape has room for a future `capabilityToken` field
but does not specify enforcement).

**VU4.** Whether `VanishingTraversal`'s skyline state and `RingBuilder`'s per-dimension slot table
should live inside `ViewManifold` (survive toss, like bindings) or inside the concrete view
(recomputed on `attach`). Settled by: whichever the implementer of migration steps 5-6 finds
simpler; neither choice affects any invariant in §6, so this document does not force one.

**VU5.** Whether a dimension group's member order matters for anything beyond pack-step computation
— e.g. whether it should tie-break a grid conflict if stretch vanishing and pack view are ever
combined on the same axis. Settled by: this document assumes stretch vanishing operates on
individually bound dimensions, not groups, since the requirement does not describe packs appearing
inside stretch vanishing's raster; if that combination is wanted later, a tie-break rule keyed to
`d.dim-group`'s rank order is the natural extension, not a redesign.

**VU6.** Whether a `Visit` (`apps/common/xanadu/link_navigation.hpp`) should record the view kind
and axis bindings in force, so Activity Back restores how the reader was looking and not only where.
Today a `Visit` carries a target, an arrival and an optional link context. Settled by: a UX
validation pass over journeys that walk back across a view switch; if it is wanted, the answer is
more cells on the activity store's own dimensions, not a wider `Visit` struct.

**VU7.** Whether one dimension may be bound to two axes at once, as classic ZigZag allows. §7.1
links an axis slot to the dimension cell on `d.binds`, and a cell has one negward neighbour per
dimension, so a dimension can sit under one axis slot only. Settled by: deciding whether the doubled
binding is wanted; if so, the slot links instead to a view-minted binding cell whose *value* is a
handle to the dimension (`ArenaManifold::handleTarget`), which costs one cell per binding and
removes the limit.

______________________________________________________________________

## 18. Traceability

| Requirement clause (from the user's request)                                                     | Satisfied by                                                      |
| ------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------- |
| "highly pluggable, highly extensible View system"                                                | V-R1, V-R2; §5 package map; §8.1 `ViewRegistry`; ruling V4        |
| "decides how xanadocs (pages) and slices (cells) should be laid out in a given viewport"         | §8.1 `View::layout`; §8.4 `LayoutInput`/`ViewportDesc`            |
| "view should sit on top of the store"                                                            | §6.1; §8.1 `attach(const Store&)`; §6.3 I2                        |
| "divided into different views for xanadocs and zigzag slices so the two can be mixed freely"     | V-R11, V-R12, V-R13; §10.1-10.3; §15 step 7                       |
| "each cell can only be connected on the two directions of all the bound dimensions"              | V-R5; §6.3 I1; §6.5 `mintViewLink`/verifier                       |
| "any minted cells used by the view live in the view only"                                        | V-R6, V-R9; §6.3 I2, I5; ruling V1, V6                            |
| "quickly tossed in O(1) time as dimensions are rebound"                                          | V-R7; §6.4; ruling V1; §13's probe; §15 step 1                    |
| **Stretch vanishing** — "full content of all cells … always displayed"                           | V-R14; §9.1.2-9.1.3                                               |
| "drawn very closely together"                                                                    | §9.1.3 skyline packing; §9.1.4                                    |
| "only the immediate neighbors … completely aligned to the cell's dimensional axes"               | V-R15; §9.1.4                                                     |
| "sacrificing structural clarity for raw data visibility"                                         | §9.1.3's refused-alternatives argument                            |
| "cells to the edges … become less opaque"                                                        | V-R17; §9.1.5                                                     |
| "partially clipped cells are completely invisible"                                               | V-R16; §9.1.6                                                     |
| **All-dim walk** — "surrounded by every cell they are connected to on every dimension"           | V-R19; §9.2.2                                                     |
| "ring that utilizes 3 dimensional space to keep the accursed cell visible as … connections grow" | V-R20; §9.2.3                                                     |
| "dimension names are used as edge labels"                                                        | §9.2.4                                                            |
| "bound dimensions' cells are aligned to the cell's dimensional axes like spokes on a wheel"      | V-R20; §9.2.3                                                     |
| "movement between them works as expected"                                                        | §9.2.5, §9.2.8                                                    |
| "maintaining the zigzag invariant that a cell can only have one connection in each direction"    | V-R5; §6.3 I1 (restated precisely for bound-vs-unbound in §9.2.2) |
| "aids in visualizing the valence of each cell"                                                   | V-R23; §9.2.10                                                    |
| "quickly rebinding dimensions by dragging an edge to the bound axis"                             | V-R22; §9.2.6                                                     |
| **Dimensional pack view** — "dimensions can be grouped to quickly rebind an entire set at once"  | V-R24; §7.2, §7.3                                                 |
| "dimension group can also be bound to a single dimension"                                        | §7.1 `BindTarget`; §7.2                                           |
| "each real cell is represented as a pack of every cell connected along a dimension in the group" | V-R25; §9.3.2-9.3.4                                               |
| "extending as far as there is at least one cell to pack"                                         | §9.3.4 BFS-frontier stop rule                                     |
| "pack of cells maintain the movement invariant"                                                  | V-R26; §9.3.5                                                     |
| "can be retrieved individually"                                                                  | V-R27; §9.3.11                                                    |
| "nested with d.pack (the container) and d.packing (the constituents)"                            | ruling V2; §9.3.3                                                 |
| "xanadoc views will be explored in a follow-up specification"                                    | ruling V12; §10.3                                                 |

______________________________________________________________________

## 19. Change history

- 2026-10-07 — Initial proposal.
- 2026-10-07 — Rehomed onto xuzz as the only application (§1.2, §5, §15): built-in views in the
  engine, renderer and input wiring in `apps/xuzz/`, no new code under `apps/xudu/` or
  `apps/zigzag/`, `ZigzagVisualizer` deleted by the migration. Corrected the activity store's
  status: it is implemented (`StoreActivityLog`).
