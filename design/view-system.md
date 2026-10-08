# A Pluggable View System for Xanadocs and ZigZag Slices

Status: proposal, unbuilt. Date: 2026-10-07.

## Abstract

Xuzz draws ZigZag slices through one 5,000-line class, `ZigzagVisualizer`, and xanadoc pages through
one coordinator, `xanadu::Views`. Neither exposes a seam another layout could plug into, and
`ViewCoordinator` composes exactly one of each. This document specifies a view system that sits
between the store and the renderer:

- a registry of **slice views**, which lay out cells, and **page views**, which lay out pages, with
  no closed list of either;
- a **view space** for slice views in which view-minted cells live apart from the store, keep
  ZigZag's one-neighbour-per-direction rule, and are tossed in constant time when dimensions are
  rebound;
- a **binding model** in which axes, dimension groups and their order are cells, with no fixed
  number of any;
- three slice views — **stretch vanishing**, **all-dim walk** and **dimensional pack view** — and
  two page views — the **base view** and the **stacked vanishing view** — each specified to the
  level of an algorithm and its acceptance tests;
- **scenes and panes**, so pages and cells share one world or sit side by side;
- where every piece of code goes: generic rendering backbone in `libgleditor`, everything
  xanalogical in `apps/common/`, and only program start-up in `apps/xuzz/`.

Everything a view places — cells, pages, edges, labels — is placed in world space under a real
camera. The library work that needs (unprojection, device scissor, depth range and placed planes) is
planned separately in [`world-space-rendering-plan.md`](world-space-rendering-plan.md).

## How to read this, for implementing agents

Section 3 is the acceptance test, and section 20 maps every clause of the requests and notes behind
this document to it. Sections 5 to 8 are normative: where code lives, the view space, the binding
model and the API. Sections 9 and 10 are the views. Section 17 is the order of work; each step keeps
`make test` green. Section 18 records each ruling with its price and what it refused — read it
before arguing with a decision, because the alternative was probably considered. Section 19 lists
what is still open; do not block on it.

Code is cited by path, and by line where the line was checked against the tree at `af5f1d5`. Paths
under `apps/xudu/` and `apps/zigzag/` are where the code sits today; §17 step 1 moves it.

[`view-system-implementation-plan.md`](view-system-implementation-plan.md) orders the work. Its §2
lists corrections and gaps found when this document was checked against the code; where it and this
document disagree, it is the later word until the package it names amends this one.

______________________________________________________________________

## 1. Motivation

### 1.1 What exists today

**Slices.** `ZigzagVisualizer` (`apps/zigzag/zigzag_visualizer.hpp`) inherits
`gleditor::FrameContributor`, `PickObserver`, `a11y::Source`, `ui::FocusScope` and
`xanadu::ZigzagPresentationSurface`. One instance owns the engine, the focus cell, the bindings, the
layout, the palette, the command bar, cell editing and drawing. It has two modes,
`enum class ViewMode { CellContent, Topology }` (`zigzag_visualizer.hpp:209`), both laid out by
`rebuildActiveViewTopology()`, which walks exactly three bound dimensions out to a fixed radius
(`mapAxis` over unit vectors x, y and z, `zigzag_visualizer.cpp:1410-1414`). Every cell is then
drawn on one planar `Canvas` at its x and y (`zigzag_visualizer.cpp:1971-2070`); the z a depth-bound
cell was given reaches the beams between cells but not the canvas the cells are drawn on.

Binding is three named fields:

```cpp
// apps/common/xanadu/zigzag/zzstructure.hpp:103
struct ViewAxisBinding {
  DimID x_dimension = "d.1";
  DimID y_dimension = "d.2";
  DimID z_dimension = "d.3";
};
```

and `enum class DimensionBundle` (`zzstructure.hpp:111`) adds a closed set of named triples. That is
the whole of "dimension binding": no list, no group, no runtime count.

**Pages.** The library places each open `Doc` in a horizontal row (`AbstractRenderer::documentSlot`,
`include/gleditor/renderer.hpp:312`; `render::kDefaultDocumentGap`) and flows a document's pages
downwards (`Doc::pageGapPx`, `include/gleditor/doc.hpp:775`; each `Page` carries its own model
matrix). When a link or a transclusion is activated, `LinkBeams` re-seats the row and moves the far
*document* so the linked passages are level: it builds one `TensionBody` per document and one
`TensionConstraint` for the link, steps `TensionLayoutEngine` 25 times, and animates the documents
to the result (`apps/xudu/beams.cpp`, the block ending in `far->animateMoveTo(...)`;
`apps/common/xanadu/tension_layout.hpp`). `xanadu::Views` (`apps/xudu/views.hpp:54`) owns document
switching, alongside and onion-skin comparison, camera framing and the link UI. None of it is
replaceable: the arrangement is the code.

**Composition.** `apps/xuzz/view_coordinator.hpp` holds one `xanadu::Views&` and one
`shared_ptr<zigzag::ZigzagVisualizer>` and switches between `Unified`, `XanadocOnly` and
`ZigzagOnly` (`apps/xuzz/cli.hpp:24`). There is no pane, no second camera and no registry.

### 1.2 One application, and where code goes

`xudu` and `zigzag` are retired as programs: commit `b48bf09` deleted their `main.cpp` files, and
`build/xudu` and `build/zigzag` are symlinks to `build/xuzz`. Their directories still hold the
components `xuzz` links. That is now also wrong. The rule this document follows, and
`.claude/rules/architectural_governance.md` §1 states, is:

1. **`libgleditor`** (`src/`, `include/gleditor/`) holds generic components only — the backbone that
   xuzz and others specialise. `apps/gleditor` uses it and has no Xanadu reference by design, so
   nothing with a cell, a link, a xanadoc or a view kind in its name goes there. Its tests are in
   `tests/lib/` and are not repeated anywhere else.
1. **`apps/common/`** holds everything xanalogical: `apps/common/xanadu/` for code that needs no
   graphics device and can be shared with `vquery`, `vpl`, `vprolog` and the compilers, and
   `apps/common/ui/` for code that draws or takes input through the library.
1. **`apps/xuzz/`** holds only what is unique to the xuzz program: `main.cpp`, the command line and
   the application wiring. That should stay rare.
1. **`apps/xudu/` and `apps/zigzag/` hold nothing.** §17 step 1 moves what is there today.

### 1.3 Why none of this hosts the views

- **Stretch vanishing** needs content-fitted placement, an edge fade and an all-or-nothing clip
  rule. The fixed-radius chain layout has none, and the library's culling test is "entirely outside"
  (`outsideFrustum`, `include/gleditor/draw_budget.hpp`), never "entirely inside".
- **All-dim walk** needs every dimension a cell is linked on, arranged in three dimensions. Three
  named fields have no room for a fourth dimension, and one planar canvas has no third dimension.
- **Dimensional pack view** needs user-built dimension groups and a new kind of cell, a pack, that
  must never become document structure.
- **The stacked vanishing view** needs each *page* placed on its own: at its own depth, with its own
  opacity. A `Doc` places its pages itself, in one fixed column.
- **The base view** exists, but only as `LinkBeams` moving whole documents; nothing can replace or
  refine it.
- **Mixing** needs more than one of each. `ViewCoordinator` has one of each.

### 1.4 What is reused

- `ArenaManifold` (`apps/common/xanadu/zigzag/arena_manifold.hpp`): cells with no operations behind
  them, refs that carry `ephemeralBit` and are refused by every write to a store, and
  `mark()`/`release()` as truncation. §6 uses it unchanged.
- `cell_views.hpp`: the `CellGraph` concept, `RankView` and `neighbours()` work over `Manifold` and
  `ArenaManifold` alike.
- `Manifold::dimensionsOf()` (`manifold.hpp:469`): every dimension a cell is linked on, uncapped —
  what all-dim walk enumerates.
- `ValueKind::OpHandle` and `handleTarget()` (`manifold.hpp:440`): a cell whose value names another
  cell. §6.3 uses it for every view cell that stands for a real one.
- `TensionLayoutEngine` (`apps/common/xanadu/tension_layout.hpp`): today's link alignment, kept as
  the base view's first coalescing strategy (§10.3).
- `LinkOccurrences`, `DocumentSite`, `LinkKey` (`apps/common/xanadu/link_occurrences.hpp`) and the
  activity log (`link_navigation.hpp`, `store_activity_log.hpp`): where link ends are and where the
  reader has been.
- `system_docs.hpp`'s `SettingSpec` pattern for every tunable.

### 1.5 The fitted UI layer

The UI text-fit work (`design/ui-text-fit-baseline.md` to `ui-text-fit-batch9.md`) gave the library
shared answers that views use and do not repeat:

| Need                                      | Library facility                                                                    |
| ----------------------------------------- | ----------------------------------------------------------------------------------- |
| Text fitted to a box, never cut by byte   | `text::fit()`, `TextFit` (`<gleditor/text/fit.hpp>`)                                |
| Shape once                                | `text::ShapingCache`                                                                |
| Boxed text and CPU clipping               | `Canvas::addText(box, ...)`, `pushClip`/`popClip`                                   |
| Typography and scale                      | `ui::Theme` font roles, `ui::UiMetrics`, the `ui.*` settings                        |
| Legibility of a projected plane           | `ui::projectPlane()`, `ui::labelLOD()` (`<gleditor/ui/world_panel.hpp>`)            |
| Focus, modality, pane focus, GPU picks    | `ui::FocusScope`, `ui::FocusManager` (`addPane`, `cyclePane`, `push`, pick targets) |
| Screen chrome                             | `<gleditor/ui/layout.hpp>`, `<gleditor/ui/widgets.hpp>`, `ui::ScreenOverlay`        |
| Proof that a settled frame shapes nothing | `text::ShapingStatsScope`, `ShapingCache::Stats`                                    |
| Text policy lint                          | `tools/check-ui-text-policy.py`                                                     |

All of it is in `libgleditor`, which the engine does not link. So a view's layout sees text only as
sizes returned by a measurer, and everything in the table is called from `apps/common/ui/`.

______________________________________________________________________

## 2. Scope

In scope: the view framework; the slice view space and binding model; three slice views; the page
model and two page views; scenes, panes and mixing; interaction, settings, accessibility, tests and
migration; and the placement of all of it under §1.2.

Out of scope: how the library implements unprojection, scissor, depth range and placed planes (see
[`world-space-rendering-plan.md`](world-space-rendering-plan.md)); the page views listed as
candidates in §10.5; the schema of `system://activity` beyond what a view writes to it.

______________________________________________________________________

## 3. Requirements

Each is one testable sentence. IDs are stable; §20 uses them.

### 3.1 Framework

- **V-R1.** A view MUST be installed by one registration call, with no enum, switch or other closed
  list naming a view kind anywhere in the framework.
- **V-R2.** `layout()` MUST be a pure function of its input: no graphics call, no allocation outside
  caller-owned storage, no mutation of a view space, a store or animation state; and it MUST run in
  a test with no graphics device.
- **V-R3.** Anything a view must mint or cache MUST be produced in a separate `prepare()` phase that
  runs only when an input changed, never per frame.
- **V-R4.** Every placed thing MUST be placed in world space — a position, an orientation and a size
  in the scene — so that one camera transform moves, rotates and scales all of it together.
- **V-R5.** A layout record that cannot fit the caller's storage MUST cause the storage to grow and
  the growth to be logged; it MUST never be dropped.

### 3.2 Slice view space

- **V-R6.** For every cell a view holds, real or view-minted, and every dimension, the view space
  MUST answer at most one neighbour in each direction.
- **V-R7.** A view-minted cell MUST be refused as the subject or target of any write to a store.
- **V-R8.** No view MUST ever cause a real cell to be shadowed in a view arena.
- **V-R9.** Rebinding MUST discard every derived view cell, and reclaim its storage, in a number of
  steps that does not depend on how many were minted.
- **V-R10.** Every view-minted cell MUST resolve to a real cell in a bounded number of steps.
- **V-R11.** The cursor MUST be recoverable after a toss from real cells and plain numbers alone.
- **V-R12.** Movement, rebinding and view switching MUST append no operation to the visited store.

### 3.3 Binding

- **V-R13.** The number of binding points, the number of points one dimension or group is bound to,
  the number of groups a dimension belongs to, and the size and nesting depth of a group MUST each
  be uncapped in the model; binding points are limited only by the movement keys there are to give
  and by how many can be told apart on screen.
- **V-R14.** A group MUST be bindable to an axis exactly as a dimension is, and changing its
  membership MUST change every axis that shows it.
- **V-R15.** Bindings, groups and ring order MUST survive a toss and a session, and MUST be undoable
  without touching hypertime.

### 3.4 Stretch vanishing

- **V-R16.** Every cell drawn MUST be drawn at the size that fits its whole content.
- **V-R17.** The accursed cell's immediate neighbours on bound dimensions MUST be exactly aligned to
  its axes; no other cell need be.
- **V-R18.** A cell that the viewport would cut MUST NOT be drawn with its content; it MAY be drawn
  as an empty ghost, and the cells not shown in a direction MAY be indicated at the pane's edge.
- **V-R19.** Opacity MUST fall monotonically toward the viewport's edges, to a configurable floor.
- **V-R20.** The same input MUST produce the same placement.

### 3.5 All-dim walk

- **V-R21.** Every neighbour of the accursed cell, on every dimension and in both directions, MUST
  be shown, its edge labelled with the dimension's name.
- **V-R22.** Neighbours on bound dimensions MUST lie on the cell's axes; the rest MUST be arranged
  on rings that use the third dimension, and the accursed cell MUST stay unoccluded at the rest
  camera whatever the valence.
- **V-R23.** The two neighbours on one dimension MUST lie diametrically opposite through the
  accursed cell.
- **V-R24.** The order of dimensions around the rings MUST be the same at every cell and MUST be the
  user's to change.
- **V-R25.** Dragging an edge onto an axis, and a keyboard action, MUST produce the same rebind.
- **V-R26.** Each neighbour MUST show its own valence.

### 3.6 Dimensional pack view

- **V-R27.** With a group bound to an axis, step *n* from a real cell MUST be the pack of the cells
  *n* steps from it along each member dimension, one lane per member, and the rank of packs MUST
  continue while any lane still has a cell.
- **V-R28.** Movement from pack to pack MUST be single-valued and reversible.
- **V-R29.** Each constituent MUST be retrievable as the real cell it stands for, never as a copy.
- **V-R30.** A group that contains a group MUST produce a pack that contains a pack; the container
  relation MUST be `d.pack` and the rank of constituents MUST be `d.packing`.

### 3.7 Page views

- **V-R31.** A page view MUST place each page individually: position, orientation, size, opacity.
- **V-R32.** The base view MUST flow a document's pages vertically and documents horizontally when
  no link is active, and MUST bring the pages that hold an active link's ends together, aligned at
  the linked passages. Pages MAY overlap, and a page MAY be shown only round its passage, when the
  ends cannot otherwise be seen in one pane; a linked passage MUST never be covered.
- **V-R33.** A page that flies MUST leave a marker in its home place and a tether to it.
- **V-R34.** The stacked vanishing view MUST stagger a document's pages along a line receding in
  depth, at the direction that maximises the legible text of the current page and its neighbours.
- **V-R35.** In that view opacity MUST fall slowly with distance along the line, and a page holding
  a non-formatting link or a transclusion MUST be fully opaque wherever it stands.
- **V-R36.** Navigating to a page MUST cycle the intervening pages when the target is near and MUST
  split the deck and fly the target to the top of its stack when it is far; which applies MUST be a
  pure function of the distance and two timings.
- **V-R37.** A page view MUST never change the text, the pagination or the store.

### 3.8 Mixing

- **V-R38.** A scene MUST be able to hold slice views and page views together in one world, and a
  window MUST be able to show several scenes side by side.
- **V-R39.** Splitting, closing and focusing a pane MUST NOT change another pane's camera, cursor or
  bindings.
- **V-R40.** Only real cells and document sites MUST cross between a slice view and a page view.

### 3.9 Non-functional

- **V-R41.** Every tunable MUST be a `SettingSpec` in a system xanadoc with its schema and notes
  pages; none MAY be a literal in code, and none MAY name a font or a text size.
- **V-R42.** Every action MUST have a default chord in `system://keymap`, dispatch through a
  registered Vortex-reachable name, and have a keyboard form if it has a pointer form.
- **V-R43.** View text MUST be fitted by `text::fit()`, drawn in a theme font role, and keep its
  full text as its accessible name when shortened or hidden.
- **V-R44.** A settled frame MUST perform no layout call, no shaping call and no buffer upload.
- **V-R45.** A view-only thing MUST have an accessibility role other than `cell` or `page`.
- **V-R46.** Views MUST take input only through `ui::FocusManager`, and a view command MUST run on
  the thread that owns the pane's view space.
- **V-R47.** Everything here MUST be testable headless.
- **V-R48.** Code MUST be placed as §1.2 says, and a test MUST sit with the code it tests: generic
  behaviour in `tests/lib/` only, view behaviour never re-testing the library.

### 3.10 Added by the notes of 2026-10-07

- **V-R49.** A view MAY offer sub-views, cycled by one action and listed in the palette; the
  sub-view MUST be an input to `layout()` and never hidden state.
- **V-R50.** A binding point MUST have a name, a pair of movement actions and a role; the built-in
  roles MUST include spatial, subspace and hypertime, and a role MUST be addable without changing
  the framework.
- **V-R51.** The reader MUST be able to bind a dimension or a group by dragging an edge to an axis,
  by dropping on the compass, and through the dimension selector, and each MUST end in the same
  binding call.
- **V-R52.** The selector MUST open round the accursed cell in three tiers reached with the z
  movement keys — groups with their members, dimensions at hand (most used, most likely, pouch), and
  every dimension — and an item activated there MUST be bound by pressing a binding point's name.
- **V-R53.** "Most used" and "most likely" MUST be pure functions of recorded activity, and movement
  MUST be recorded as condensed runs, never as a record per step.
- **V-R54.** The compass MUST show every binding point and what it shows, and MUST be a drop target.
- **V-R55.** A pack MUST be drawn as one glued thing with its parts demarcated, and MUST move as
  one.
- **V-R56.** The edge between two packs MUST be a bundle of strands, one per lane present at both
  ends, each in its dimension's colour, gathered between the packs and separate where they join.
- **V-R57.** A pack MUST open into a spread — parts apart, strands apart, lane names shown — on
  hover, by sub-view and by key, and any view that shows a pack MUST get this from shared code.
- **V-R58.** In all-dim walk, neighbours out to a depth the reader sets MUST be able to show their
  own wheels, and no wheel MUST cover a real cell at the rest camera.
- **V-R59.** A wheel under pressure MUST simplify in a fixed order in which dimension names condense
  first and the valence count goes last.
- **V-R60.** A page MUST have its own transform relative to its document, so that a page can move
  without its document and a document can move carrying its pages.

______________________________________________________________________

## 4. Concepts and vocabulary

| Term            | Meaning                                                                                                |
| --------------- | ------------------------------------------------------------------------------------------------------ |
| View            | An installed layout and interaction strategy. A slice view lays out cells; a page view lays out pages. |
| Placement       | One view instance with its state and its origin in a scene.                                            |
| Scene           | One world: a set of placements that share coordinates, so beams can join them.                         |
| Pane            | A rectangle of the window showing one scene through its own camera.                                    |
| View space      | `ViewManifold`: the real manifold plus a placement's binding arena and derived arena.                  |
| Binding arena   | View cells that survive a toss: axes, occurrences, groups, ring order.                                 |
| Derived arena   | View cells that do not: packs and whatever else a view mints to show the current place.                |
| View cell       | A cell minted in a view arena. Its ref has `ephemeralBit`; a store refuses it.                         |
| Occurrence      | A view cell that stands for one use of a real cell or a group; its value is a handle to it.            |
| Axis            | A slot in the binding arena that shows a dimension or a group.                                         |
| Binding point   | An axis with a name (`x`, `y`, `z`, `u`, `t`, …), movement keys and a role.                            |
| Role            | What binding at a point means: spatial, subspace, hypertime, or one a plug-in adds.                    |
| Dimension group | A named, ordered set of dimensions and groups; itself a view cell.                                     |
| Lane            | The position of one group member inside every pack of that group.                                      |
| Pack            | A container view cell: step *n* along a bound group. Its constituents are one per lane.                |
| Strand          | One lane's thread in the bundle that joins two packs; coloured by its dimension.                       |
| Spread          | A pack opened up: parts and strands apart, lane names shown.                                           |
| Origin          | The real cell a rank of packs is counted from.                                                         |
| Valence         | The number of (dimension, direction) pairs on which a cell has a neighbour.                            |
| Toss            | Discarding the derived arena's contents in constant time.                                              |
| Epoch           | A counter a toss increments; a `ViewCellRef` carries the epoch it was minted in.                       |
| Deck            | A document's pages in order, as the stacked vanishing view arranges them.                              |
| Mark            | A page's fact that it holds a non-formatting link end or transcluded content.                          |
| Riffle, split   | The two ways a deck moves to a new page: page by page, or parted in one motion.                        |
| Sub-view        | A variant of a view the reader cycles through; an input to its layout.                                 |
| Selector        | The three-tier picker of dimensions and groups that opens round the accursed cell.                     |
| Compass         | The rose in a slice pane's corner showing every binding point and what it shows.                       |
| Dimension pouch | Dimensions the reader keeps at hand in the selector.                                                   |
| Ghost           | The empty outline of a cell or page that is not being drawn in full where it stands.                   |

______________________________________________________________________

## 5. Architecture

### 5.1 Layers

```text
 apps/xuzz/                      the program: main, command line, application wiring
      |
      v
 apps/common/ui/view/            draws and takes input; links libgleditor
   ViewHost, scene presenter, draw adapter, chrome, commands
      |                    \
      v                     v
 apps/common/xanadu/view/     libgleditor  (generic; knows no cell, link or view)
   no graphics device           ui::PaneTree, ui::PlaneSet, Beams, Doc with page matrices,
   View, SliceView, PageView,   spatial: project and unproject, render regions,
   ViewRegistry, ViewManifold,  text::fit, ShapingCache, FocusManager, widgets
   ViewAxisSet, layout records,
   text raster, built-in views
      |
      v
 apps/common/xanadu/            zigzag/, store, link_occurrences, tension_layout, system_docs
```

The split between the two `apps/common` layers is the graphics device. Deriving packs, choosing ring
slots, packing content boxes and staggering a deck are geometry over cells, pages and numbers, so
they sit in the engine, where `vquery`, `vpl` and `vprolog` can run them too: a result slice can be
printed through any slice view by the text raster (§8.7) with no window. Turning the records into
quads, glyphs, beams and accessibility nodes needs the library, so it sits in `apps/common/ui/`.

### 5.2 Package map

```text
apps/common/xanadu/view/                 engine; linked by xuzz_test and the language tools
  view.hpp               View, ViewDescriptor, ViewRegistry, ViewSubject
  view_error.hpp         ViewError
  view_ids.hpp           ViewEpoch, ViewAxisId: the numbers the records share with the space
  view_records.{hpp,cpp} SubjectId, PlacedItem, PlacedEdge, PlacedFrame, DropTarget,
                         MotionHint, LayoutSink, PaneFrame, ContentExtent, placedPose
  view_manifold.{hpp,cpp}  ViewManifold, ViewCellRef, verifyViewSpace
  view_binding.{hpp,cpp}   ViewAxisSet: axes, occurrences, groups, ring order, undo
  slice_view.hpp         SliceView, SliceCursor, SlicePrepareInput, SliceLayoutInput
  page_view.hpp          PageView, PageRef, PageCatalog, PageCursor, PageLayoutInput
    view_gesture.{hpp,cpp} drag-to-rebind as a pure state machine: events in, intents out
  selector.{hpp,cpp}     the dimension selector: tiers, cursor, pure layout (§7.7)
  dimension_ranking.{hpp,cpp}  most used and most likely, from activity (§7.8)
  raster.{hpp,cpp}       layout records -> text grid, for REPLs, --raster and golden tests
  slice/
    stretch_vanishing_view.{hpp,cpp}
    all_dim_walk_view.{hpp,cpp}
    dimensional_pack_view.{hpp,cpp}
        pack_rank.{hpp,cpp}  lanes and steps (§9.3), usable by any view
    pack_presentation.{hpp,cpp}  glue, strands and the spread (§9.3.7), for any view
  page/
    base_view.{hpp,cpp}
    coalesce.{hpp,cpp}   CoalesceStrategy; the TensionLayoutEngine strategy
    stacked_vanishing_view.{hpp,cpp}
    deck.{hpp,cpp}       stagger search, opacity, riffle-or-split (§10.4)
  builtin_views.{hpp,cpp}  registerBuiltinViews(ViewRegistry &)

apps/common/ui/view/                     draws and takes input
  view_host.{hpp,cpp}      scenes, placements, panes; owns each pane's FocusScope
  view_presenter.{hpp,cpp} layout records -> ui::PlaneSet, Beams, Doc and Page matrices;
                           the text::fit measurer; picking; AccessKit nodes
  view_animation.{hpp,cpp} tweens between successive layouts by SubjectId
    view_chrome.{hpp,cpp}    compass, view palette, group editor as ui::Widget scenes
  view_commands.{hpp,cpp}  registers every view action

apps/xuzz/                               the program
  main.cpp, cli.{hpp,cpp}, xuzz_app.{hpp,cpp}   builds the host, loads settings, runs

libgleditor additions                    generic; planned in world-space-rendering-plan.md
  include/gleditor/spatial.hpp           projectToViewport, unprojectToRay, ray tests
  include/gleditor/draw_budget.hpp       insideFrustum
  include/gleditor/render/device.hpp     render regions: scissor rectangle and depth slice
  include/gleditor/ui/plane_set.hpp      many retained planes, one transform and opacity each
  include/gleditor/ui/pane_tree.hpp      split, close, resize and focus order over rectangles
    include/gleditor/doc.hpp               a settable matrix and opacity on each Page
```

### 5.3 Dependency rules

1. `apps/common/xanadu/view/` includes the engine and those library headers that are header-only and
   need no device: `<gleditor/cpp26*.hpp>`, `<gleditor/spatial.hpp>` and
   `<gleditor/draw_budget.hpp>`. The projection and frustum arithmetic a layout needs is therefore
   the library's own, tested once in `tests/lib/`, and not a second copy.
1. `apps/common/ui/view/` includes the view framework and the library. It is the only place a layout
   record meets a renderer call.
1. `apps/xuzz/` includes `apps/common/` and the library, and holds no view, layout or drawing code.
1. Nothing under `src/` or `include/gleditor/` includes anything under `apps/`, and no library type
   is named for a xanalogical thing. A `ui::PlaneSet` holds planes, not cells; a page's matrix moves
   a page of any document, the plain editor's included.
1. Nothing is added under `apps/xudu/` or `apps/zigzag/`.
1. A third-party view needs rule 1's headers only.

______________________________________________________________________

## 6. The slice view space

### 6.1 Two arenas over one manifold

A slice placement owns a `ViewManifold`: the real `Manifold` it shows, read-only, and two
`ArenaManifold`s over it, each constructed with no store so that no provenance cells are projected
into them (`ArenaManifold(base, nullptr)`).

- The **binding arena** holds what the user set: axes, the occurrence under each, groups and their
  members, and the ring order. It lives as long as the placement.
- The **derived arena** holds what a view mints to show where the cursor is: pack containers and
  their constituents. It is emptied by every toss.

They are siblings, not layers of one arena. An arena is a stack: a binding edited after derived
cells exist would be minted above them, so emptying the derived cells would either destroy it or
have to be done first, on the rebind path. Two arenas remove the ordering. Derived cells never link
to binding cells; a view reads the binding arena to learn which real dimensions are bound and then
works with those real refs, which mean the same thing in every arena over the same base.

### 6.2 A view arena never shadows a real cell

`ArenaManifold` is copy-on-write. Linking a real cell, in either direction, calls `shadow()`
(`arena_manifold.cpp:1422`), which copies the cell's whole slot, link run and content run into the
arena and enters it in an `unordered_map`; from then on every read of that cell is answered from the
copy. For a view that is three defects in one:

- the copy is stale the moment the store advances, and the view goes on reading it;
- each shadow is a hash insert at mint time and a hash erase inside `release()`, which makes
  emptying the arena proportional to the number of shadows;
- a real cell now carries view links that an ordinary walk of the arena would find.

`ensureDimension()` has the same effect by another road: it registers a name in a map that
`release()` does not roll back, and links the new dimension into the base's `d.dims` rank, shadowing
a real cell to do it (`arena_manifold.cpp:78`).

So the rule is absolute, and it is what makes everything else in this section simple:

**No view code passes a real ref as either end of `link()`, calls `setContent()` or `setValueBits()`
on a real cell, or calls `ensureDimension()`.** A view-owned dimension is a bare cell from
`makeCell(name)`, used as a link key; `link()` only requires that the arena contains it. Reads of
real structure fall through to the base (`ArenaManifold::linked` does this for any cell the arena
does not hold), so they are always current.

### 6.3 Standing for a real cell

A view cell that represents a real one — a pack constituent, the origin of a pack rank, the
occurrence under an axis — is an **occurrence**: a bare cell whose typed value is a handle to its
target.

```cpp
const auto cell = arena.makeCell();
arena.setValueBits(cell, xanadu::ValueKind::OpHandle, target); // target is a CellRef
// later
const auto real = arena.handleTarget(cell); // one read
```

This is the handle cell the engine already has for version annotations, and `promote()` already
knows how to turn one into a real `OpHandle` cell (`arena_manifold.cpp:1838`). The relation between
an occurrence and its target is a value and not a link, deliberately: a link would shadow the target
(§6.2). Everything *between* view cells — which constituents a pack has and in what order, which
pack follows which, which occurrence an axis shows — is links on view-owned dimensions.

### 6.4 The invariants

**I1 — one neighbour per direction.** For every cell and dimension the view space answers at most
one neighbour posward and one negward. *Held by:* the link representation, which has one `pos` and
one `neg` per (cell, dimension), and by `ViewManifold::link` (§6.6), which refuses to displace an
occupant at either end. *Tested by:* the verifier after random sequences of bind, derive and toss.

**I2 — a view cell never reaches a store.** *Held by:* `ephemeralBit`; `Manifold::applyStructure`
and `Store::setLink` refuse such a ref. *Tested by:* handing a view cell to `Store::setLink` and
asserting the typed refusal.

**I3 — no real cell is shadowed.** *Held by:* `ViewManifold::link` accepting only view cells of its
own arena as ends and only that arena's dimensions as keys. *Tested by:* `shadowCount() == 0` on
both arenas after every test, and in debug builds after every `prepare()`.

**I4 — a toss is constant-time.** §6.5.

**I5 — every view cell resolves to a real cell.** An occurrence resolves in one read. A pack
container resolves to its first present constituent's real cell, at most one step per nesting level.
*Held by:* `ViewManifold::resolveReal`. *Tested by:* resolving every derived cell after each
`prepare()`.

**I6 — the cursor is real.** A `SliceCursor` (§8.5) is a real origin cell plus an axis, a signed
step count and lane indices. It names no view cell, so a toss cannot invalidate it. *Tested by:*
tossing at every cursor position of the worked examples and re-deriving the same real cell.

### 6.5 The toss

```cpp
ViewManifold *ViewManifold::toss() noexcept {
  derived_.release(empty_); // truncate to the mark taken on the empty arena
  empty_ = derived_.mark();
  ++epoch_;
  dims_ = {}; // forget the view-owned dimension cells of the old generation
  return this;
}
```

This is constant-time, and it reclaims the storage in the same step. The argument is
`ArenaManifold::release()` itself (`arena_manifold.cpp:1566`). Its loops run over, in order: quote
spaces, proxy shadowed edges, quote occurrences and proxies added since the mark — federation state,
which a view arena has none of; trail entries since the mark — none, because `trail()` skips any
cell minted under the innermost mark and every derived cell is; and shadows since the mark — none,
by I3. What is left is four `resize()` calls on vectors of trivially destructible records and a
fix-up of the `d.store-refs` tails of attached spaces. No step visits a minted cell.

That last fix-up is the one thing to change. It begins by looking up the name `d.store-refs`, and on
a slice with no such dimension `Manifold::dimensionNamed` falls back to scanning every dimension of
the base and reading its name (`manifold.cpp:785`). That does not depend on how much was minted, but
it is neither constant nor allocation-free, and it serves only arenas with attached spaces. Guarding
it with `if (!spaces_.empty())` makes `release()` on a view arena a fixed number of steps and
changes nothing for any other caller.

Because `release()` truncates, a later mint reuses the same dense indices. A `CellRef` kept across a
toss would silently name a different cell, so nothing keeps a bare one: a `ViewCellRef` pairs the
ref with the epoch it was minted in, and `ViewManifold` refuses a pair whose epoch is not current
with one integer compare. Animation identity includes the epoch for the same reason (§8.9).

What a toss does not do is rebuild. Re-deriving what the new place needs is `prepare()`'s work
(§8.5), bounded by what is on screen (§6.7).

*How the claim is tested.* The test asserts the preconditions that make `release()` loop-free —
`shadowCount()`, `trailSize()` and the space count are zero on the derived arena — then mints 0, 10³
and 10⁶ cells, tosses, and asserts `cellCount()` is back to its empty value and every earlier
`ViewCellRef` is refused. The preconditions are the proof; `tools/layout-latency-probe` additionally
reports toss time at each size, which must be flat.

`ArenaManifold` therefore gains two things and no new concept: the guard above, and `shadowCount()`,
a `const` accessor for a count it already keeps for `Mark`.

### 6.6 The choke point and the verifier

```cpp
// apps/common/xanadu/view/view_manifold.hpp
[[nodiscard]] ViewResult // std::expected<ViewManifold *, ViewError>
ViewManifold::link(Layer layer, ViewCellRef from, ViewDim dim, zigzag::DimVector dir,
                   ViewCellRef to) noexcept;
```

It refuses, changing nothing, when: either ref or the dimension is stale (`StaleEpoch`); either end
is not a view cell of that layer's arena (`RealCellInViewLink`); `from` already has a neighbour on
`dim` in `dir`, or `to` already has one in the opposite direction (`OccupiedDirection`).
`ArenaManifold::link` would evict in that last case, which is right for unification and wrong for a
view, where two claimants for one slot is a bug in the derivation. Unlinking is the same call with
no `to`. Strategies and views are handed a `ViewManifold &` and have no other way to write.

`verifyViewSpace(const ViewManifold &, report)` reports every violation it finds of: I3; links that
are not two-sided; a link whose key or far end is not a view cell of the same arena; an occurrence
whose handle is not a real cell of the base or a live group; a group reachable from itself; a
`d.binds`, `d.dim-group`, `d.pack` or `d.packing` rank of the wrong shape. Every test that mutates a
view space calls it before asserting anything else.

### 6.7 Derivation is windowed

A view mints derived cells only for what it is about to show: the packs within the viewport of the
cursor, plus a margin. Walking a long rank of packs mints at the leading edge. When the derived
arena passes `view.arena.windowCells`, the placement tosses and `prepare()` re-derives the window
around the cursor. That is always possible, because a pack is a function of (origin, axis, step) and
the pane keeps, as real refs, where each lane has reached. The derived arena is therefore bounded by
what is on screen, not by how far the reader has walked.

______________________________________________________________________

## 7. The binding model

### 7.1 Structure

Bindings are cells in the binding arena. Five view-owned dimensions carry them:

| Dimension      | Rank                                                   | Reads as                           |
| -------------- | ------------------------------------------------------ | ---------------------------------- |
| `d.axes`       | the placement's axis head, then one slot per axis      | "these are the axes, in order"     |
| `d.binds`      | an axis slot, then the occurrence it shows             | "this axis shows that"             |
| `d.dim-group`  | a group cell, then one occurrence per member, in order | "this group contains these"        |
| `d.ring-order` | the ring head, then one occurrence per dimension       | "this is the order round the ring" |
| `d.dim-pouch`  | the pouch head, then one occurrence per dimension kept | "these are at hand" (§7.7)         |

```text
 d.axes:        [head] --- [axis 0] --- [axis 1] --- [axis 2]
                              |            |            |
 d.binds:                  (occ: d.1)   (occ: d.1)   (occ: G)

 d.dim-group:   [G "contact"] --- (occ: d.email) --- (occ: d.phone) --- (occ: d.address)

 d.ring-order:  [head] --- (occ: d.2) --- (occ: d.clone) --- (occ: d.email) --- ...

 (occ: X) is an occurrence whose handle names X. d.1 is on two axes; d.email is in a
 group and in the ring order. Each use is its own cell, so nothing is spent twice.
```

An axis slot, a group and a head are bare view cells; a group's content is its name. An occurrence
(§6.3) names a real dimension cell or a group cell. Because each *use* is its own cell, the
arithmetic that would otherwise cap things does not arise: a dimension can be on any number of axes
and in any number of groups, a group can be nested in several parents and shown on several axes, and
none of it touches the real dimension cell.

Resolving what an axis shows is two reads: the slot's posward neighbour on `d.binds`, then its
handle. The reverse questions — which axes show this dimension, which groups contain it — are scans
of the axis rank and of the groups. Those are a handful of cells each, and the compass and the
selector are the only callers.

`ViewAxisBinding` and `DimensionBundle` stop being live storage. `ViewAxisBinding` remains as the
shape of a three-axis preset, and `DimensionBundle`'s triples become seed presets from which a group
can be created.

### 7.2 Groups

A group is created, renamed, reordered, extended, shrunk and deleted through `ViewAxisSet` (§8.3). A
member is a dimension or another group; a group that would become reachable from itself is refused
(`GroupCycle`). An empty group can exist but cannot be bound (`EmptyGroupBind`).

A group is bound as a dimension is, by an occurrence under the axis slot, so binding code never asks
which it has. "Rebinding a whole set at once" is editing the group: every axis that shows it reads
the new membership at the next `prepare()`, because its occurrence names the group and not a copy of
its members. Deleting a group removes every occurrence of it and tells the user which axes and
parent groups lost it.

Group membership is ordered, and the order is meaningful: it is the order of the lanes (§9.3).

### 7.3 Binding points

An axis slot is a **binding point**: a named place where a dimension or a group can be bound, with
its own movement keys and its own way of being shown. Three is not the number. The limit is
practical — how many pairs of movement keys there are to give, and how many points a reader can tell
apart on screen — and it is well above three.

A binding point has:

- a **name**, one letter by default, which is its label on the compass (§7.9) and the key that picks
  it in the selector (§7.7);
- two **movement actions**, posward and negward — `std:nav/step_x_pos` and its five siblings are
  these for `x`, `y` and `z` today;
- a **role**, which says what binding there means: where a view puts the neighbours, and what a step
  does.

| Point         | Role      | A dimension bound here                                                                                            |
| ------------- | --------- | ----------------------------------------------------------------------------------------------------------------- |
| `x`, `y`, `z` | spatial   | has its neighbours along a direction of the placement's space: right, up, away                                    |
| `u`           | subspace  | has each neighbour as the root of its own cluster, in a separate 3-D space attached to the cell; a step enters it |
| `t`           | hypertime | is walked through time, not space                                                                                 |

These five are the defaults. Binding points are configuration, not code: `layout.bindingPoints` in
`system://layout` lists them in order, each with a name and a role, and their movement keys are rows
of `system://keymap`. Adding a point is adding a row. The binding arena has one slot on `d.axes` per
configured point, whose content is the point's name. Roles are an extension point (§8.10): a role is
registered as a view is, and a binding point names one.

**Spatial.** A view gives each spatial point a direction (`axisDirection`, §8.5). Stretch vanishing
and the pack view put `x` and `y` in the plane and `z` in depth; all-dim walk makes them spokes.

**Subspace.** A cell with a neighbour on the dimension bound to `u` shows that neighbour's cluster
as an inset 3-D space inside itself: an embedded placement (§11.5) of the same view, rooted at the
neighbour. A step posward zooms into the inset until it is the scene; a step negward zooms back out
of the containing cell. The zoom is the transition: nothing is cut to, so the reader sees which cell
the new space is inside. While inside, a band round the periphery of the pane shows the next space
along `u` — what one more step posward would enter — and the rim of the containing cell stays at the
pane's edge as the way back (`subspace.rimBand`). A cell can so be the door to a cluster that would
not fit, or would not make sense, in the space the cell itself is in.

**Hypertime.** A step on `t` moves through time. If a history rank such as `d.version` is bound
there, it is walked like any dimension. With nothing bound the meaning is still open (VU12). The
leading candidate is the slice as a whole at the previous hypertime operation, and at the next for a
step posward: the placement shows the state of every cell as it was then. A ref is an operation
index and means the same cell in every version, so the cursor and the bindings would carry across;
the host would build the view space over that state's manifold and replay the bindings by ref.

A point whose role a view does not present is still bound, still on the compass and still moves the
cursor; all-dim walk shows its neighbours on the rings, badged with the point's name.

When one dimension is bound to two points, stepping on either moves along it; its immediate
neighbours are placed once per point, so a placement's identity includes the point (§8.4).

### 7.4 The rebind sequence

```text
1. ViewAxisSet edit                binding arena: mint or unlink occurrences; push undo entry
2. ViewManifold::toss()            derived arena: constant time (§6.5)
3. placement marked stale          nothing else happens on the input path
4. next frame: prepare()           re-derive what the cursor's place needs: O(visible)
5. layout()                        pure; fills the pane's records
```

Step 1 costs the edit: a constant number of cells for one bind, the number of members changed for a
group edit. Nothing on the path depends on how much was derived.

### 7.5 Undo and persistence

Undo is local to the placement: each `ViewAxisSet` edit pushes its inverse, and undo replays it and
tosses. It is not hypertime, and it appends nothing to any store.

Bindings, groups and ring order are written to `system://layout`, and the dimension pouch to
`system://pouches`, per slice, by dimension *name* (refs do not survive a session), under
`layout.slice.<sliceId>.axes`, `.groups` and `.ringOrder`. On attach they are replayed through
`ViewAxisSet`; a name that no longer resolves is skipped and reported. They are not written to the
slice's own store — how a reader looks is not a fact about the document (R8) — and not to
`system://activity`, which records visits.

### 7.6 Ways to bind

Binding is the act the whole system turns on, so there are several ways to do it, for different
moments, and all end in `ViewAxisSet::bind` (V-R51):

- **From what is in front of you.** In all-dim walk, drag an edge to an axis (§9.2.7).
- **From the corner.** Drop anything that is a dimension or a group on an arm of the compass (§7.9),
  or click an arm.
- **From everything there is.** Open the selector (§7.7).
- **Without looking.** The existing cycle and swap actions step a point through the dimensions and
  exchange two points.

### 7.7 The dimension selector

The selector opens round the accursed cell, which stays in view at its centre, and holds the
keyboard as a modal scope until it closes. It has three tiers, one behind another, and the z
movement keys go from tier to tier.

```text
   tier 1: groups                tier 2: at hand               tier 3: every dimension

      (g2)--m m m                  . most used .                +----+ +------+ +---+
     /                           .  . likely .  .               | d1 | | d.em | |d.3|
   (g1)   c    (g3)             .  .  pouch  .  .               +----+ +------+ +---+
     \                            .  .   c   .  .               +------+ +--+ +-----+
      (g4)                        .  .       .  .               | d.ph | |d7| | d.x |
   the selected group              .  .     .  .                +------+ +--+ +-----+
   fans out its members             three rings                 stretch placement, most
   in a second layer                                            relevant nearest the centre
```

- **Tier 1, groups.** The placement's groups stand in a ring round the cell — a torus when there are
  more than one ring can hold. The selected group fans its members out in a second layer beyond it,
  so the reader sees what a group contains before binding it and can change it without leaving:
  remove a member, reorder members, drop a dimension in from the pouch or from another tier, or
  start a new group. Every edit is a `ViewAxisSet` call and can be undone.
- **Tier 2, at hand.** Three concentric rings of dimensions: outermost the **most used**; inside it
  the **most likely** next, from the reader's recent and past activity (§7.8); innermost the
  **pouch**, dimensions the reader has put there to keep. Any dimension shown anywhere in the
  selector can be sent to the pouch or taken out of it.
- **Tier 3, everything.** Every dimension cell of the slice, laid out by stretch vanishing's
  placement (§9.1.3) over a ranked list, the most relevant to this context nearest the centre.

A dimension is a real cell, so the items are the dimension cells themselves, drawn like any cell,
with their names as content. A group is a binding-arena cell. The selector mints nothing else.

**Choosing.** An item is activated by a click; by one of the quick keys `0` to `9`, shown on the ten
nearest items; or by travelling to it — the x movement keys go round a ring and the y keys go
between rings or rows — and pressing `Return`. An activated item is *armed*. Pressing a binding
point's name — `x`, `y`, `z`, `u`, `t` — binds it there and closes the selector. An armed or unarmed
item can instead be dragged to the compass. `Escape` closes without binding.

The selector is laid out in the engine as a pure function of the binding arena, the ranking and its
own cursor, emits the same records as a view, and is drawn by the same presenter in the world round
the cell. It needs no chrome of its own.

```cpp
// selector.hpp
enum class SelectorTier : std::uint8_t { Groups, AtHand, Everything };
struct SelectorCursor {
  SelectorTier tier{SelectorTier::AtHand};
  std::uint32_t ring{}, index{};
  std::optional<SubjectId> armed; // activated, waiting for a point's name
};
struct SelectorInput {
  const ViewManifold &space;
  zigzag::CellRef accursed;
  SelectorCursor cursor;
  PaneFrame frame;
  Measure measure;
  std::span<const RankedDimension> ranking; // §7.8
};
struct SelectorLayout {
  /// What the keys 0 to 9 activate. Ten is the number of digit keys, so the
  /// bound is a fact and inplace_vector is the right container.
  gleditor::cpp26::inplace_vector<SubjectId, 10> quick;
};
[[nodiscard]] SelectorLayout layoutSelector(const SelectorInput &in,
                                            LayoutSink &out) noexcept;
[[nodiscard]] SelectorCursor moveSelector(const SelectorInput &in,
                                          MoveRequest request) noexcept;
```

### 7.8 Most used and most likely

Both orders are computed from what the reader has done. What is recorded, and how much, matters more
than the arithmetic.

**Movement is condensed before it is recorded.** A reader crosses cells many times a second. One
activity record per step would bury the activity store in noise and make every later reading of it
slower. So steps are not recorded. A placement accumulates the run in progress in memory, and when
the run *settles* it appends one record — a **walk summary**:

- where the run began and where it ended, as real cells;
- for each dimension moved along, how many steps;
- for each ordered pair of dimensions, how many times the second followed the first.

A run settles when the reader pauses for `activity.settleMs`, or does anything that is not a step:
edits, binds, follows a link, opens the selector, changes view or pane. A step that is undone within
`activity.bounceMs` — out and straight back — is dropped from the run as a slip, not counted twice.
The number of records is then the number of times the reader *stopped somewhere*, not the number of
cells passed, and the pairs are exactly what the model below needs, already counted. A walk summary
is also the unit a visit already is: one completed transition, to the place the reader settled.

```cpp
// dimension_ranking.hpp
struct DimensionSteps {
  zigzag::DimRef dimension;
  std::uint32_t steps{};
};
struct DimensionChange {
  zigzag::DimRef from, to;
  std::uint32_t times{};
};
/// One settled run of movement, condensed.
struct WalkSummary {
  std::uint64_t ordinal{}; // position among the reader's runs, oldest first
  zigzag::CellRef began, ended;
  std::span<const DimensionSteps> steps;
  std::span<const DimensionChange> changes;
};

/// Accumulates the run in progress. Pure state: time comes in as an argument.
class WalkRecorder {
public:
  WalkRecorder *step(zigzag::CellRef from, zigzag::CellRef to,
                     zigzag::DimRef dimension, std::uint64_t atMs);
  /// The run so far, if it has any steps left after slips are dropped.
  [[nodiscard]] std::optional<WalkSummary> settle();
};

struct RankedDimension {
  zigzag::DimRef dimension;
  float used{};   // how much, lately
  float likely{}; // how probably next, here
};
/// Pure. @p last is the dimension most recently moved along, if any;
/// @p present is the dimensions the accursed cell is linked on.
void rankDimensions(
    std::span<const WalkSummary> history, std::optional<zigzag::DimRef> last,
    std::span<const zigzag::DimRef> present,
    gleditor::cpp26::function_ref<void(const RankedDimension &)> out);
```

- **Used.** A dimension's steps count, and a count halves every `rank.halfLife` runs of age, so
  "most used" means lately without forgetting the past.
- **Likely.** A first-order Markov model: for the dimension the reader used last, how often each
  dimension followed it, with the same decay, smoothed towards "used" by `rank.smoothing` so an
  unseen pair is unlikely and not impossible. A dimension the accursed cell is actually linked on is
  weighted up by `rank.presentBoost`, because the likeliest next move is one that is possible.

The model is a product of the walk summaries and is rebuilt from them; nothing else is stored. Two
things are still open. The store grows by a record per settled run, which is slow but unbounded, so
old summaries may need folding into one aggregate per slice (VU13). And the summaries need a home in
the activity store's own structure, which is that store's design to make (VU5).

### 7.9 The compass

A rose in the top left corner of every slice pane shows every binding point and what is bound to it.

```text
          y  d.2
          |
  d.1 x --+-- x  d.1          x, y: the cardinal arms
         /|
   z d.3  |                   z: the arm drawn into the page
          y
    (u) contacts   (t) --     other points: petals round the rim, each with its role's glyph
```

Each arm is labelled with the name of what it shows and coloured as that dimension is coloured
everywhere else — its edges, its strands, its lane. A group shows its name and how many members it
has, and lists them on hover. An unbound point is an empty arm.

The compass is the binding display and a drop target: an edge dragged from the wheel, an item from
the selector, a lane label from a pack, or another arm (which swaps the two). Clicking an arm opens
the selector with that point already chosen, so the next activation binds without a further key. It
is chrome: a widget scene, in screen space (§11.7).

______________________________________________________________________

## 8. The API

Declarations are normative in shape and naming; bodies are not shown. Everything in §8.1 to §8.7 is
in `apps/common/xanadu/view/`, namespace `xanadu::view`.

Three conventions hold throughout, here and in the rendering plan:

- **No sentinels.** "No cell", "no axis", "no frame" are `std::optional`, never a reserved value.
  `zigzag::noCell` belongs to the wire and the disk and does not appear in this API. A field that
  must always have a value has no default and is given one where the object is made.
- **The C++26 facilities, from `<gleditor/cpp26*.hpp>`.** A callback that is called and not kept is
  a `function_ref`, so reporting, visiting and measuring allocate nothing. A container whose bound
  is a fact of the design is an `inplace_vector` — ten quick keys, eight corners of a frustum. A
  bound that is the reader's — lanes, binding points, group depth, pages — is never one.
- **Setters chain.** A function that changes an object and has nothing else to return gives back the
  object, as `ArenaManifold` does; one that can refuse gives back `std::expected` of the object.

### 8.1 Views and the registry

```cpp
// view.hpp
enum class ViewSubject : std::uint8_t { Slice, Page };

/// A layout and interaction strategy. One instance per placement, so an
/// instance may keep caches; it keeps no cursor and no binding.
class View {
public:
  virtual ~View() = default;
  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;
};

struct ChordSpec {
  std::string action, call, chord, context;
};

struct SubviewSpec {
  std::string id, name;
};

struct ViewDescriptor {
  std::string kind; // "slice.stretch-vanishing", "page.base"
  std::string name, description, glyph;
  ViewSubject subject{};
  /// Variants the reader cycles through (V-R49). The first is the default.
  std::vector<SubviewSpec> subviews;
  std::vector<SettingSpec> settings; // xanadu::SettingSpec, system_docs.hpp
  std::vector<ChordSpec> chords;
  std::function<std::unique_ptr<View>()> make;
};

class ViewRegistry {
public:
  /// Holds the default keymap's chords from the start.
  ViewRegistry();
  /// Refuses a second descriptor of the same kind (DuplicateViewKind) and a
  /// default chord that is already taken in its scope, by an action or by
  /// the same descriptor (ChordCollision). A refusal leaves nothing behind.
  std::expected<ViewRegistry *, ViewError> add(ViewDescriptor descriptor);
  [[nodiscard]] std::span<const ViewDescriptor> views() const noexcept;
  [[nodiscard]] gleditor::cpp26::optional<const ViewDescriptor &>
  find(std::string_view kind) const noexcept;
  /// The call holding a chord in a scope: the words of a ChordCollision.
  [[nodiscard]] std::optional<std::string_view>
  chordHolder(std::string_view chord, std::string_view context) const;
};

// builtin_views.hpp, with the first built-in view (E15)
void registerBuiltinViews(ViewRegistry &registry);
```

A chord collides with another when their scopes are equal and their chords are equal in
`canonicalChord()`'s spelling (lower case, modifiers sorted), the rule
`SystemDocsTest.DefaultKeymapGivesEachChordOneActionPerScope` already applied to the default keymap;
the normaliser moved from that test into `system_docs` so the registry and the test cannot disagree.
A `ChordSpec`'s `context` is a keymap scope as `keymapScope()` answers it — empty for anywhere,
`zigzag`, `document` — or a narrower one a view names. The registry is seeded with the default
keymap rather than the live one: a reader who rebinds a key has chosen what wins, and the conflict
gate is about defaults. Price: a view's chord can collide with a reader's own binding, which the
keymap resolves as it does today. Refused: checking only between views — the † rows of §12.1 exist
because the collision that matters is with the keymap already shipped.

The registry is an object the host owns, not a singleton, and built-in views are added by one
explicit call, as `registerZigzagCommands()` is one call today. A plugin calls `add()` the same way.
The host seeds each descriptor's settings and chords into `system://settings` and `system://keymap`
when it is added, so a third-party view's tunables and bindings appear where the built-in ones do.
`make` is a `std::function` because a descriptor outlives the call that registered it; a
`function_ref` would dangle.

**Views in Vortex.** A descriptor's `make` may wrap a Vortex program. What can be Vortex is whatever
is a pure function over resolved cells: a pack's lane filter, an opacity curve, a tie-break, a whole
small layout. What stays C++ is the per-item arithmetic of the built-in views, where a VM dispatch
per cell at high valence would cost the frame, and everything under `apps/common/ui/`, which calls
the renderer.

### 8.2 `ViewManifold`

```cpp
// view_manifold.hpp
using ViewEpoch = std::uint64_t;
enum class Layer : std::uint8_t { Binding, Derived };

/// A view cell and the generation it belongs to. Binding cells are epoch 0
/// for the placement's life; a derived cell is valid only in its own epoch.
struct ViewCellRef {
  zigzag::CellRef ref; // always a cell; "no cell" is an empty optional
  ViewEpoch epoch{};
  Layer layer{Layer::Derived};
  bool operator==(const ViewCellRef &) const = default;
};
using ViewDim = ViewCellRef; // a view-owned dimension is a view cell
using ViewResult = std::expected<class ViewManifold *, ViewError>;

class ViewManifold {
public:
  explicit ViewManifold(const zigzag::Manifold &base);

  [[nodiscard]] const zigzag::Manifold &base() const noexcept;
  [[nodiscard]] ViewEpoch epoch() const noexcept;
  [[nodiscard]] class ViewAxisSet &axes() noexcept;
  [[nodiscard]] const class ViewAxisSet &axes() const noexcept;

  // -- mint: every write of a view cell goes through these four ------------
  [[nodiscard]] std::expected<ViewCellRef, ViewError> mint(Layer layer);
  [[nodiscard]] std::expected<ViewCellRef, ViewError>
  mint(Layer layer, std::string_view text);
  /// A cell that stands for @p target: a real cell, or a group of this space.
  [[nodiscard]] std::expected<ViewCellRef, ViewError>
  mintOccurrence(Layer layer, zigzag::CellRef target);
  /// §6.6. Refuses rather than displaces.
  [[nodiscard]] ViewResult link(Layer layer, ViewCellRef from, ViewDim dim,
                                zigzag::DimVector dir, ViewCellRef to) noexcept;
  [[nodiscard]] ViewResult
  unlink(Layer layer, ViewCellRef from, ViewDim dim,
         zigzag::DimVector dir) noexcept;

  // -- read ----------------------------------------------------------------
  [[nodiscard]] std::optional<ViewCellRef>
  linked(ViewCellRef from, ViewDim dim, zigzag::DimVector dir) const noexcept;
  [[nodiscard]] std::optional<zigzag::CellRef>
  target(ViewCellRef occurrence) const noexcept;
  /// I5. An occurrence: its target. A pack: its first present constituent's.
  [[nodiscard]] std::expected<zigzag::CellRef, ViewError>
  resolveReal(ViewCellRef cell) const noexcept;

  // -- the derived generation ----------------------------------------------
  /// View-owned dimensions of the current epoch, minted on first use.
  [[nodiscard]] ViewDim packDim();    // d.pack: container, then first constituent
  [[nodiscard]] ViewDim packingDim(); // d.packing: the constituents, in lane order
  [[nodiscard]] ViewDim axisStepDim(ViewAxisId axis); // packs along one axis
  [[nodiscard]] std::size_t derivedCellCount() const noexcept;
  ViewManifold *toss() noexcept; // §6.5

private:
  const zigzag::Manifold &base_;
  zigzag::ArenaManifold bindings_;
  zigzag::ArenaManifold derived_;
  zigzag::Mark empty_; // taken on the empty derived arena
  ViewEpoch epoch_{1};
  // handles to the current generation's dimension cells; cleared by toss()
};

struct ViewSpaceViolation {
  ViewCellRef cell;
  std::string_view rule; // "I3", "two-sided", "rank-shape", ...
};
/// Calls @p report for each violation and answers how many there were.
std::size_t verifyViewSpace(
    const ViewManifold &space,
    gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report);
```

A view reads real structure from `base()` with the existing `CellGraph` tools and view structure
through `linked()` and `target()`. There is no merged graph type, because the two never mix: no link
joins a real cell to a view cell (§6.2).

### 8.3 `ViewAxisSet`

```cpp
// view_binding.hpp
using ViewAxisId = std::uint32_t; // position on the d.axes rank

/// A real dimension cell, or a group cell of this placement's binding arena.
using BindTarget = zigzag::CellRef;
using AxisResult = std::expected<class ViewAxisSet *, ViewError>;

class ViewAxisSet {
public:
  // -- axes ----------------------------------------------------------------
  [[nodiscard]] std::size_t axisCount() const noexcept; // not a cap
  ViewAxisId addAxis();
  AxisResult removeAxis(ViewAxisId axis);
  /// Replace what the axis shows. The target's other axes are left alone.
  AxisResult bind(ViewAxisId axis, BindTarget target);
  AxisResult unbind(ViewAxisId axis);
  AxisResult swap(ViewAxisId first, ViewAxisId second);
  [[nodiscard]] std::optional<BindTarget> shown(ViewAxisId axis) const noexcept;
  [[nodiscard]] bool isGroup(BindTarget target) const noexcept;

  // -- groups --------------------------------------------------------------
  std::expected<BindTarget, ViewError>
  createGroup(std::string_view name, std::span<const BindTarget> members);
  AxisResult renameGroup(BindTarget group,
                                             std::string_view name);
  AxisResult insertMember(BindTarget group,
                                              std::size_t position,
                                              BindTarget member);
  AxisResult removeMember(BindTarget group,
                                              std::size_t position);
  AxisResult moveMember(BindTarget group, std::size_t from,
                                            std::size_t to);
  AxisResult deleteGroup(BindTarget group);
  [[nodiscard]] std::size_t memberCount(BindTarget group) const noexcept;
  [[nodiscard]] std::optional<BindTarget>
  member(BindTarget group, std::size_t position) const noexcept;

  // -- ring order (§9.2) ---------------------------------------------------
  /// The dimension's place in the order, appending it if it is new.
  std::size_t ringPlace(zigzag::DimRef dimension);
  [[nodiscard]] std::optional<std::size_t>
  ringPlaceIfKnown(zigzag::DimRef dimension) const noexcept;
  AxisResult moveInRing(zigzag::DimRef dimension,
                                            std::size_t place);

  // -- who uses what: scans of a handful of cells --------------------------
  void forEachAxisShowing(
      BindTarget target,
      gleditor::cpp26::function_ref<void(ViewAxisId)> visit) const;
  void forEachGroupContaining(
      BindTarget target,
      gleditor::cpp26::function_ref<void(BindTarget)> visit) const;

  // -- undo: local to the placement, never hypertime -----------------------
  bool undo();
  bool redo();
};
```

Every mutator edits the binding arena through `ViewManifold::mint` and `link`, and the caller tosses
afterwards (§7.4). `ringPlace()` is the one mutator a view calls for itself, and only from
`prepare()`.

### 8.4 Layout records

```cpp
// view_records.hpp

/// What a placed thing is: its identity for picking, accessibility and
/// animation. Two placements of the same cell differ in `slot`.
enum class SubjectKind : std::uint8_t {
  Cell,     // value: a real CellRef
  ViewCell, // value: a view CellRef; epoch says which generation
    Page,     // value: document << 32 | page
  Document, // value: the document's place in the list; a frame for its pages
  Label,    // value: the dimension or link the label names
  Badge,    // value: what is counted
    Marker,   // value: view-defined (ghost, tail, tick, edge heat)
};
struct SubjectId {
  SubjectKind kind{};
  std::uint32_t slot{}; // axis, lane, deck or ring place of this placement
  std::uint64_t value{};
  ViewEpoch epoch{};
  bool operator==(const SubjectId &) const = default;
};

/// The pane as a layout sees it. Coordinates are placement-local: x right,
/// y up, z towards the viewer, one unit per Canvas pixel on the plane z = 0
/// at the rest camera.
struct PaneFrame {
  float widthPx{}, heightPx{};
  glm::mat4 localToClip{1.0F}; // the clip space FrameContext::viewProjection uses
  float minReadableLinePx{};   // from zigzag.minReadableTextPx and ui.minFontPx
};

struct ContentExtent {
  float width{}, height{}, lineHeight{};
  std::uint32_t lines{};
};
/// Size of a subject's content at a width limit, in local units. The
/// presenter measures with text::fit() in the subject's font role; a test
/// supplies fixed sizes. This is all a layout knows about text.
using Measure =
    gleditor::cpp26::function_ref<ContentExtent(SubjectId, float maxWidth)>;

enum class Facing : std::uint8_t { Plane, Camera };
enum class ContentMode : std::uint8_t { Full, Abbreviated, Coarse, Badge, None };
enum ItemFlags : std::uint32_t {
  itemFocus    = 1U << 0,
  itemViewOnly = 1U << 1, // draws with view-only chrome; never role cell/page
  itemMarked   = 1U << 2,
    itemGhost    = 1U << 3, // an empty outline standing for something not drawn here
  itemGlow     = 1U << 4, // a soft band whose opacity is an intensity
  itemTinted   = 1U << 5, // face tinted by the strand that joins it
};


/// A band of an item's own height, measured from its top.
struct Band {
  float top{}, bottom{};
};

/// An item or frame that names a frame is placed relative to it, and moves
/// with it: a pack carries its parts, a document carries its pages.
struct PlacedItem {
  SubjectId id;
  glm::vec3 centre{};
  glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F}; // used when facing == Plane
  float width{}, height{};
  float opacity{1.0F};
  Facing facing{Facing::Plane};
  ContentMode content{ContentMode::Full};
  std::uint32_t flags{};
  std::optional<std::uint32_t> frame; // the PlacedFrame this item sits in
  std::optional<Band> window; // draw only this band of the item (§10.3.2)
};

enum class EdgeKind : std::uint8_t {
  Dimension,
  Strand, // one lane's thread in a bundle between packs
  Link,
  Transclusion,
  Tether,
};
struct PlacedEdge {
  SubjectId from, to;
  glm::vec3 a{}, b{};
  EdgeKind kind{};
  std::uint64_t relation{}; // DimRef, or the link's cell
  float opacity{1.0F};
  std::uint32_t dashClass{}; // the second cue beside colour (plan G13)
  std::optional<std::uint32_t> label; // the PlacedItem that names this edge
  /// Strands of one bundle share an id and pass through the same two points
  /// between their ends, where the bundle is gathered.
  std::optional<std::uint32_t> bundle;
  std::array<glm::vec3, 2> gather{};
};

/// A container drawn round other items: a pack, a document, a deck. Its own
/// centre is relative to its parent frame, if it has one.
struct PlacedFrame {
  SubjectId id;
  glm::vec3 centre{};
  glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
  float width{}, height{};
  std::optional<std::uint32_t> parent;
  std::uint32_t count{}; // what it stands for, when collapsed to a badge
  bool collapsed{};
};

/// Where a dragged edge may be dropped: an axis, as a segment with a radius.
struct DropTarget {
  ViewAxisId axis{};
  glm::vec3 a{}, b{};
  float radius{};
};

enum class MotionPath : std::uint8_t { Straight, Arc };
/// How one subject should travel to its new place. Absent: the default ease.
struct MotionHint {
  SubjectId id;
  float delayMs{}, durationMs{};
  MotionPath path{MotionPath::Straight};
};

/// Caller-owned and reused. push() never drops: the sink grows, and logs
/// that it did on the view.layout category.
class LayoutSink {
public:
  std::uint32_t push(const PlacedItem &item);
  std::uint32_t push(const PlacedFrame &frame);
  LayoutSink *push(const PlacedEdge &edge);
  LayoutSink *push(const DropTarget &target);
  LayoutSink *push(const MotionHint &hint);
  LayoutSink *clear() noexcept;
  [[nodiscard]] std::span<const PlacedItem> items() const noexcept;
  [[nodiscard]] std::span<const PlacedFrame> frames() const noexcept;
  [[nodiscard]] std::span<const PlacedEdge> edges() const noexcept;
  [[nodiscard]] std::span<const DropTarget> dropTargets() const noexcept;
  [[nodiscard]] std::span<const MotionHint> hints() const noexcept;
};
```

`SubjectId` is made by its factories — `cell(ref, slot)`, `viewCell(ref, epoch, slot)`,
`page(document, page, slot)`, `document`, `label`, `badge`, `marker` — the one way a layout makes
one, so a real cell is never given an epoch and a view cell never loses its own; `std::hash` is
specialised for the animation layer's map. `ViewEpoch` and `ViewAxisId` live in `view_ids.hpp`,
which `view_manifold.hpp` and `view_binding.hpp` include, because the records carry both and must
not depend on either class: a raster or a presenter reads records with no view space in sight.
`placedPose(sink, record)` composes a record's frames (V30) and answers nothing when a frame index
is out of range or the frames form a cycle, rather than placing the record at the origin. A
`PlacedEdge`'s `dashClass` is the second cue beside colour (plan G13, §5.2 of the plan): one class
per dimension, like its colour, unbounded, so the presenter cycles its stroke patterns rather than
the layout capping how many dimensions can be told apart. The sink logs growth at debug level, so a
layout that keeps outgrowing it, and so allocates every frame, can be found with
`SPDLOG_LEVEL=view.layout=debug`.

Every record is a position in the world and nothing else: a cell, a page, a label and a pack frame
are all planes with a centre, an orientation and a size. `Facing::Camera` asks the presenter to turn
the plane to the camera each frame, which is a matrix and not new geometry. Labels are items like
any other, so they are placed, faded and culled by the same rules.

### 8.5 Slice views

```cpp
// slice_view.hpp

/// I6: real cells and numbers only. step == 0 is the origin itself.
struct SliceCursor {
  zigzag::CellRef origin;          // always a real cell
  std::optional<ViewAxisId> axis;  // the group axis the cursor has stepped along
  std::int32_t step{};             // signed packs from the origin on that axis
  std::vector<std::uint32_t> lanes; // into nested packs; empty: the pack as a whole
  /// All-dim walk: the selected spoke, a dimension and a direction (§9.2.6).
  std::optional<zigzag::DirectedDim> spoke;
  bool operator==(const SliceCursor &) const = default;
};

/// The real cell under the cursor: the origin, or the selected (else first
/// present) constituent's target.
[[nodiscard]] std::optional<zigzag::CellRef>
cellAt(const ViewManifold &space, const SliceCursor &cursor) noexcept;

struct BindingPreview { // "as if this axis showed that": for a drag in flight
  ViewAxisId axis{};
  BindTarget target;
};

struct SliceLayoutInput {
  const ViewManifold &space;
  const SliceCursor &cursor;
  PaneFrame frame;
  Measure measure;
  std::optional<BindingPreview> preview;
  std::uint32_t subview{};        // index into the descriptor's subviews
  std::optional<SubjectId> hover; // what the pointer is over, if anything
};

enum class MoveKind : std::uint8_t {
  AlongAxis,  // one step on `axis` in `direction`
  AlongSpoke, // one step to the selected spoke's neighbour
  NextSpoke,
  PreviousSpoke,
  EnterPack,
  LeavePack,
  NextLane,
  PreviousLane,
  Retrieve, // the cell under the cursor becomes the origin
};
struct MoveRequest {
  MoveKind kind{MoveKind::AlongAxis};
  std::optional<ViewAxisId> axis; // for AlongAxis
  zigzag::DimVector direction{zigzag::DimVector::POS};
};
struct MoveOutcome {
  SliceCursor cursor;
  bool moved{};
  bool originChanged{}; // the host tosses and re-prepares
};

class SliceView : public View {
public:
  /// The local direction posward on an axis points, or nothing if this view
  /// does not place that axis (§7.3).
  [[nodiscard]] virtual std::optional<glm::vec3>
  axisDirection(ViewAxisId axis) const noexcept = 0;

  /// The only phase that may mint, extend the ring order or fill caches.
  /// Runs when the cursor, the epoch, the store or the frame changed.
  virtual std::expected<SliceView *, ViewError>
  prepare(ViewManifold &space, const SliceCursor &cursor,
          const PaneFrame &frame, Measure measure) {
    return this;
  }

  /// Pure (V-R2).
  virtual void layout(const SliceLayoutInput &in,
                      LayoutSink &out) const noexcept = 0;

  /// Pure: where the cursor goes. Every cursor motion is one of these, so a
  /// view never mutates to move. The default handles AlongAxis by stepping
  /// along the dimension the axis shows, from cellAt(cursor).
  [[nodiscard]] virtual MoveOutcome move(const ViewManifold &space,
                                         const SliceCursor &cursor,
                                         MoveRequest request) const noexcept;
};
```

Editing is write-through and belongs to the host: it resolves `cellAt(cursor)`, which is always a
real cell, and hands it to the existing edit path. Keeping a pack is explicit and separate:

```cpp
/// Mints the pack as real structure by zigzag::promote(), which refuses
/// above its budget. Never called by a view, a move or a layout.
[[nodiscard]] std::expected<xanadu::Store *, ViewError>
promotePack(const ViewManifold &space, ViewCellRef container,
            xanadu::Store &store, xanadu::MicroversionId parent);
```

### 8.6 Page views

```cpp
// page_view.hpp
struct PageRef {
  std::uint32_t document{}; // position in the placement's document list
  std::uint32_t page{};
  auto operator<=>(const PageRef &) const = default;
};

struct DocumentFacts {
  DocumentId id;
  std::uint32_t pages{};  // known so far
  bool paginating{};      // more are coming
  bool background{};      // shown for context, behind the row
  bool rightToLeft{};     // base direction of the text
};
struct PageFacts {
  float width{}, height{}, lineHeight{}; // local units
  bool link{};         // holds an end of a non-formatting link
  bool transclusion{}; // holds content also present elsewhere
};

/// What pagination and the link index currently know. The presenter
/// implements it over the library's documents; a test implements it by hand.
class PageCatalog {
public:
  virtual ~PageCatalog() = default;
  [[nodiscard]] virtual std::uint32_t documents() const noexcept = 0;
  [[nodiscard]] virtual DocumentFacts
  document(std::uint32_t index) const noexcept = 0;
  [[nodiscard]] virtual PageFacts page(PageRef page) const noexcept = 0;
  [[nodiscard]] virtual std::optional<PageRef>
  pageOf(const DocumentSite &site) const noexcept = 0;
};

/// One end of a link, or of a transclusion, on a page.
struct LinkEnd {
  PageRef page;
  float top{}, bottom{}; // of the passage, from the page's top, local units
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
};
struct ActiveLink {
  LinkKey key;
  std::span<const LinkEnd> ends; // every member of both endsets
  std::uint32_t anchor{};        // the end the reader is at
};

struct PageCursor {
  PageRef page; // the page that holds the caret
  bool operator==(const PageCursor &) const = default;
};

struct PageLayoutInput {
  const PageCatalog &catalog;
  PageCursor cursor;
  std::optional<ActiveLink> active;
  PaneFrame frame;
  std::uint32_t subview{};
  std::optional<SubjectId> hover;
};

class PageView : public View {
public:
  /// Fill caches (the stagger search of §10.4). Mints nothing: a page view
  /// has no view space.
  virtual PageView *prepare(const PageLayoutInput &in) { return this; }
  virtual void layout(const PageLayoutInput &in,
                      LayoutSink &out) const noexcept = 0;
  /// How to travel from one cursor to another: MotionHints into @p out.
  virtual void transition(const PageLayoutInput &from,
                          const PageLayoutInput &to,
                          LayoutSink &out) const noexcept {}
};
```

A page view is a pure function of a catalog, a cursor and an optional active link. It cannot reach
the text, the pagination or a store, which is how V-R37 holds.

### 8.7 The text raster

```cpp
// raster.hpp
struct RasterOptions {
  std::uint32_t columns{80}, rows{24};
  bool edges{true};
};
/// Draws the records orthographically onto a character grid, nearest first.
[[nodiscard]] std::string
rasterise(const LayoutSink &layout, const RasterOptions &options,
          gleditor::cpp26::function_ref<std::string(SubjectId)> text);
```

This is what `xuzz --raster` prints today, generalised to any view. `vquery`, `vpl` and `vprolog`
can show a result slice through a slice view with it, and golden layout fixtures (§16) are stored as
rasters beside their numeric dumps, so a reviewer can read a layout change as a picture.

As built (E6): the projection is orthographic down local z and fitted to the grid, each axis
separately, so the records' extent fills the columns and rows and no character aspect ratio has to
be assumed. Items are drawn nearest first and keep their cells; edges, then frames, fill only the
cells left empty, and a box's inside is blanked, so a line never crosses a box drawn in front of it.
A real cell is ruled `+-|`, a focused one `+=|`, a view-only one has `.` corners (nothing view-only
is square), a ghost is dots, a frame `#=!`, a collapsed frame its count in brackets; labels and
badges are bare text, read whole; dimension edges and strands take `- | / \` by slope, links and
transclusions `*`, tethers `:`. Text is cut at the box, by code point, and never measured: the
raster is for reading a layout, not for checking fit, which is the presenter's.

### 8.8 The host

In `apps/common/ui/view/`, namespace `xanadu::view`.

| Type             | Holds                                                                                     | Does                                                             |
| ---------------- | ----------------------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| `SlicePlacement` | a `ViewManifold`, a `SliceCursor`, one `SliceView`, an origin transform, a `LayoutSink`   | move, bind, toss, prepare, lay out                               |
| `PagePlacement`  | a document list, a `PageCursor`, one `PageView`, an origin transform, a `LayoutSink`      | follow the caret, lay out, plan transitions                      |
| `Scene`          | placements that share one world                                                           | resolves beams between its placements (§11.2)                    |
| `Pane`           | a scene, a camera, a `ui::FocusScope`, the placement that has the keyboard                | routes input; draws its scene through its camera into its region |
| `ViewHost`       | the `ViewRegistry`, the scenes, a library `ui::PaneTree`, the presenter and the animation | split, close, focus; per-frame pipeline; persistence             |

Switching the view of a placement replaces its `View` and keeps its cursor and, for a slice, its
bindings. Switching a placement between a slice and pages replaces the placement.

### 8.9 The pipeline

```text
on a command, on the owner thread:
    placement.move / bind / switch view
    bindings changed  -> ViewManifold::toss()          constant time
    placement marked stale

each frame, for each stale placement:
    view.prepare(...)       may mint and cache; O(visible)
    view.layout(...)        pure; refills the placement's LayoutSink
    view.transition(...)    page views: MotionHints for the change just made

each frame, for each pane:
    animation.advance(dt)   tween every SubjectId towards its new record
    presenter.draw(pane)    render region = the pane; planes, beams, pages; picks; a11y
```

A placement is stale when its cursor, its epoch, its store's version, its frame, the UI metrics or
the theme changed. A frame in which nothing is stale and no tween is running re-submits retained
geometry and does nothing else (V-R44).

The animation layer keeps, for each `SubjectId` it is showing, a copy of the last record. A subject
missing from a new layout fades out from that copy; nothing is looked up. A derived view cell's id
includes its epoch, so after a toss its old id matches nothing and it fades, while the real cells
and pages around it tween.

### 8.10 Extension points

| To add                  | Implement                              | You are given                                       | You must never                                                            |
| ----------------------- | -------------------------------------- | --------------------------------------------------- | ------------------------------------------------------------------------- |
| a slice view            | `SliceView`, a `ViewDescriptor`        | a `ViewManifold`, a cursor, a frame, a measurer     | write in `layout()`; link a real cell; keep a `ViewCellRef` across a toss |
| a page view             | `PageView`, a `ViewDescriptor`         | a `PageCatalog`, a cursor, the active link, a frame | reach a document's text or a store                                        |
| a way to coalesce pages | `CoalesceStrategy` (§10.3)             | page boxes, link ends, the anchor                   | depend on wall-clock time                                                 |
| a deck order            | `DeckSource` (§10.5)                   | the catalog                                         | reorder a document's own pages in the store                               |
| a pack rule             | a function over `pack_rank.hpp`        | the base manifold, a group's leaf dimensions        | mint outside `prepare()`                                                  |
| a record kind           | a new `SubjectKind` or `EdgeKind`      | presenters ignore kinds they do not know            | carry a view `CellRef` without its epoch                                  |
| a gesture               | a state machine in `view_gesture`      | pointer events and the current records              | change a binding before the gesture commits                               |
| a binding point's role  | a role, registered as a view is (§7.3) | the bound target, the cursor, the placement         | assume a view presents it: a view may only list the point on the compass  |
| a sub-view              | a `SubviewSpec` on the descriptor      | its index in every layout input                     | keep which sub-view is showing anywhere but the placement                 |

### 8.11 Errors

```cpp
// view_error.hpp
enum class ViewError : std::uint8_t {
  OccupiedDirection,  // link(): an end already has a neighbour there
  RealCellInViewLink, // link(): an end or the key is not a view cell of that arena
  StaleEpoch,         // a ViewCellRef from a tossed generation
  UnknownTarget,      // an occurrence's target is neither real nor a live group
  UnknownAxis,
  GroupCycle,
  EmptyGroupBind,
  DuplicateViewKind,
  ChordCollision,
  PromotionRefused,   // promote()'s own budget or refusal
  ArenaRefused,       // the arena refused; carries nothing more
};
/// Everything a view tells the reader (§12.2), errors or not.
enum class ViewMessage : std::uint8_t { NothingThatWay, /* ... */ };
[[nodiscard]] constexpr ViewMessage messageKey(ViewError error) noexcept;
[[nodiscard]] constexpr std::string_view messageId(ViewMessage) noexcept;
[[nodiscard]] constexpr std::string_view messageText(ViewMessage) noexcept;
```

"Nothing further that way" is not an error: it is a `MoveOutcome` with `moved == false`.

Messages have their own enumeration (plan G8) because the two sets differ: six of the nine
situations of §12.2 are not refusals, and every refusal still needs words when it reaches the reader
through a third-party view. `messageKey()` is the one map from a refusal to its message, a switch
with no default, so an error added without a message does not compile. `messageId()` is a stable
name (`view.emptyGroup`) for tests, logs and a later translation table; `messageText()` is the
template, with its fields in braces for whoever raises it to fill. Price: two switches to extend per
message. Refused: `messageKey()` answering words directly, as first written — it left the non-error
messages nowhere, and rewording a message would have changed its key.

______________________________________________________________________

## 9. The slice views

Coordinates are placement-local (§8.4): x right, y up, z towards the viewer; "away" is −z. An axis
*shows* a dimension or a group (§7); where a view other than the pack view meets a group on an axis
it uses the group's first member dimension, and the compass says so.

### 9.1 Stretch vanishing

#### 9.1.1 Purpose

To read a great deal of cell content at once. Every cell is as large as its content needs and cells
are packed almost touching. Only the cells next to the accursed cell are guaranteed to line up with
it; further out, alignment is given up for density. Reach for it to read data, not to study
structure.

#### 9.1.2 What is shown

From the accursed cell `c`, every cell reachable by steps along the dimensions shown on the placed
axes, breadth first, until the viewport is tiled. A cell reached twice is placed once, where it was
first reached; the second route is drawn as an edge only.

#### 9.1.3 Placement

Let `e(x, u)` be half the extent of cell `x`'s content box along direction `u`, and `g` the gap
(`stretch.gap`). Binding point `x` runs along +x and `y` along +y.

1. **The focus.** `c` is placed at the origin at its measured size.

1. **Radius 1, exactly aligned.** For each placed in-plane axis, in axis order, with direction `u`,
   and each sign `s`: the neighbour `n` of `c` is centred at

   ```math
   s \cdot u \cdot \bigl(e(c,u) + e(n,u) + g\bigr)
   ```

   so its centre is on `c`'s axis line and its edge is one gap from `c`'s. This is the only place
   alignment is promised (V-R17).

1. **Further out, anchored slide.** Take placed cells in the order they were placed. For each `p`,
   each placed in-plane axis in order with direction `u` and perpendicular `v`, and each sign: let
   `n` be `p`'s neighbour. If `n` is already placed, emit the edge and continue. Otherwise the
   wanted centre is `p`'s centre plus `s·u·(e(p,u) + e(n,u) + g)`. If the box there overlaps a
   placed box grown by `g`, slide it along `v` by the smallest distance that clears, trying first
   the side that points away from the focus's axis line, and accept the result only while `n` still
   overlaps `p` along `v` by at least `stretch.minContact`, so it visibly belongs to the cell it
   came from. If no slide within that range clears, move the wanted centre outward along `u` past
   the nearest blocker and try again.

1. **Stop at the viewport.** A placed cell is *expanded* — its own neighbours visited — only if its
   box meets the viewport rectangle grown by `stretch.overfill`. Cells beyond that are not expanded,
   so the walk ends when the pane is tiled, whatever the size of the slice.

Placed boxes are kept in a uniform grid, so an overlap query touches a constant number of boxes on
average and the whole placement is linear in the number of cells placed. The order of every loop is
fixed, so the same input gives the same placement (V-R20). A change to one cell's content moves that
cell and those placed after it by the slide rule; nothing is relaxed globally, so a small edit has a
small effect.

Two alternatives were weighed. A ragged table of row and column tracks is a grid, which is what the
view gives up beyond radius 1, and it wastes the space around boxes of different sizes. Spring
relaxation fills space well but is not stable: one character typed can rearrange the far side of the
pane.

#### 9.1.4 The depth axis

Binding point `z` runs away from the viewer. A cell reached along it from `p` is placed in the next
plane behind or in front, `stretch.layerDepth` apart, starting at `p`'s own x and y and sliding by
the same rule within that plane, which has its own grid. Planes in front of the focus are drawn only
where they do not cover it.

#### 9.1.5 Fade and clip

Project the four corners of a cell's box with `frame.localToClip`, and let `m` be the least distance
from any corner to any edge of the pane, as a fraction of the pane's size. With `e = min(1, 2m)`, so
that `e` is 0 at the edge and 1 at the centre:

```math
\text{opacity} = f + (1 - f)\cdot\operatorname{smoothstep}(0,\ b,\ e)
```

where `f` is `stretch.fadeFloor` and `b` is `stretch.fadeBand`. Opacity is constant over the middle
of the pane and falls smoothly through the outer band to the floor (V-R19).

A cell whose box is not entirely inside the view is never drawn with its content (V-R18): the test
is the library's `insideFrustum`, the mirror of `outsideFrustum`. A cut cell is emitted as a
**ghost** instead: a `Marker` with `itemGhost` and no content, an empty outline of its box at
`stretch.ghostOpacity`, drawn only where it lies inside the pane. A ghost says that a cell is there
without showing half of one. The presenter applies the same test to each cell's *animated* box, so a
cell in flight appears only once it is wholly inside, and it uses a margin of `stretch.clipMargin`
pixels before showing a cell again, so a cell resting on the edge does not flicker as the camera
moves.

**Edge heat.** Ghosts show the cells that just missed. Beyond them the walk has stopped, but it
knows roughly how much it left: for each direction, the cells it placed outside the pane plus the
neighbours it did not go on to visit. Those counts are summed into `stretch.heatSectors` sectors
round the pane, and each sector is a `Marker` with `itemGlow` along the pane's edge whose opacity
rises with its count, reaching full at `stretch.heatFull`. The glow is where there is more to see
and how much. It is a lower bound, since unvisited cells have unvisited neighbours, and the chrome
says "at least". Ghosts are on by default; the heat is the view's second sub-view and a setting.

The accursed cell is never faded and never hidden. A cell larger than the pane cannot be shown
whole, so it is shown only when it is the accursed cell, scrolling within the pane; as a neighbour
it is a ghost with a tick (§9.1.6) that says so.

#### 9.1.6 Staying oriented

Alignment is gone past radius 1, so three things replace it: the accursed cell carries the focus
emphasis; each of its immediate neighbours carries a tick naming the dimension and direction that
reached it; and a breadcrumb strip in the chrome lists the last `stretch.breadcrumbs` cells walked,
read from the activity log. Where the walk stopped at the viewport with more cells along a rank, the
last cell placed on that rank carries the count not shown.

```text
 pane
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
   radius 1: on c's axes, one gap away
   radius 2 and beyond: slid against the cell that reached them
   near the edge: fainter; a box the edge would cut is not drawn
```

#### 9.1.7 Interaction, edge cases, accessibility

Movement is the ordinary step on each bound axis. A click focuses the cell clicked; hover shows a
faded cell's content in a `ui::Tooltip`. Nothing here is pointer-only.

A cell with no neighbours is drawn alone. A slice of one cell fills the pane with it.

Each cell drawn is an accessibility node with role `cell`, its full text as its name, and relations
to its neighbours along each bound dimension. A cell not drawn is not in the tree. The breadcrumb is
a list.

#### 9.1.8 Acceptance

Two layouts of the same input are identical. Every radius-1 neighbour shares the focus's coordinate
on the axis perpendicular to its own. No two emitted boxes overlap. No item with content fails
`insideFrustum`; every cut cell has exactly one ghost. A sector's glow is zero when nothing lies
that way and rises with the count. Opacity at the centre is 1, at the band's inner edge is 1, and at
the pane's edge is the floor. A slice of 10⁶ cells and one of 10³ place the same number of cells for
the same pane.

### 9.2 All-dim walk

#### 9.2.1 Purpose

To see everything one cell is connected to, on every dimension, and to choose which of those
connections the axes should follow. The number of connections is the cell's valence, and the view
makes it visible as how full the wheel is.

#### 9.2.2 What is shown

The accursed cell `c` at the hub. For every dimension `c` is linked on and each direction, the
neighbour, joined to the hub by an edge labelled with the dimension's name. Dimensions bound to a
spatial binding point put their neighbours on that axis; all others go on rings. Nothing `c` is
linked to is left out (V-R21).

This view mints no derived cells. The neighbours are real cells, the labels are the dimension cells'
own content, and the order round the rings is the `d.ring-order` rank in the binding arena (§7.1).

#### 9.2.3 The wheel

Let R, U and F be the local unit vectors right, up and away, the hub box be the accursed cell's
content capped at `ring.hubMaxShare` of the pane, and every ring cell use one slot box of
`ring.slotWidth` by `ring.slotHeight`, with gap `g`.

Ring `j` (0, 1, 2, …) is a circle centred on the hub, of radius

```math
r_j = r_0 + j\,\Delta r, \qquad r_0 = \max\bigl(\texttt{ring.radius},\ \tfrac{w_h + w_s}{2} + g,\ \tfrac{h_h + h_s}{2} + g\bigr)
```

lying in the plane spanned by R and

```math
U_j = \cos\tau_j\,U + \sin\tau_j\,F, \qquad \tau_0 = 0, \quad \tau_j = (-1)^{j+1}\left\lceil \tfrac{j}{2} \right\rceil \texttt{ring.tiltStep}
```

Ring 0 is the wheel, flat in the plane of the pane. Later rings share its centre and its horizontal
diameter and lean alternately away and towards the viewer, which is the use of the third dimension:
each new ring is a new circle of the same sphere-like figure, not a wider and wider disc.

A dimension occupies a *pair slot*: two places diametrically opposite, the posward neighbour at
angle θ and the negward at θ + π (V-R23). Ring `j` has

```math
m_j = \left\lfloor \frac{\pi\, r_j}{w_s + g} \right\rfloor
```

pair slots (rounded down to an even number on ring 0), at angles `θ = θ⁰_j + i·π/m_j`, with
`θ⁰_0 = 0` and `θ⁰_j` half a slot for `j ≥ 1`. A neighbour's centre is

```math
\text{hub} + r_j\bigl(\cos\theta\,R + \sin\theta\,U_j\bigr)
```

Because the pair is opposite through the hub, each dimension reads as a straight line through the
accursed cell, which is what an axis is.

**The axes are spokes of the same wheel.** On ring 0 the slot at θ = 0 belongs to binding point `x`
and the slot at θ = π/2 to `y`, whether or not they are bound, so binding and unbinding never moves
anything else. `z` is the line through the hub along F. Points with other roles have no direction
here: their neighbours take ring slots, badged with the point's name. Neighbours on a bound
dimension therefore sit exactly on the cell's axes (V-R22), and the step actions move along them as
in any view.

**Keeping the hub visible.** Seen from the rest camera, a ring leaning by τ projects to an ellipse
whose vertical half-axis is `r_j·cos τ`. The lean is clamped so that it never closes over the hub:

```math
\cos\tau_j \;\ge\; \frac{(h_h + h_s)/2 + g}{r_j}
```

Outer rings are larger and may lean further. The rest camera is turned by `view.camera.restYaw` and
`restPitch`, so the pair on axis 2 and the near halves of leaning rings do not stand directly in
front of the hub.

The wheel is fixed in the placement, not in the camera: orbiting turns the whole figure, and every
cell and label turns to face the viewer (`Facing::Camera`), so text stays readable while the
structure rotates. Two alternatives were refused. A helix and a Fibonacci sphere give even spacing
but index each point by the total count, so one more dimension moves all of them, and neither puts a
dimension's two neighbours opposite each other. A basis taken from the camera would make the figure
follow the viewer round, which defeats looking at it from another side.

#### 9.2.4 Which slot a dimension gets

The unbound dimensions that have a neighbour at `c` are taken in ring order and given the free pair
slots in order: ring 0's unreserved slots by increasing angle, then ring 1's, and so on. So the
wheel is exactly as full as the cell's valence, a low-valence cell is a small figure, and the
*order* of dimensions round it is the same at every cell (V-R24): `d.email` is always before
`d.phone`, though the angle each sits at depends on which other dimensions are present. A new
dimension is appended to the ring order the first time it is met, in `prepare()`. The user changes
the order by dragging a spoke round the wheel.

The alternative, a fixed slot per dimension for the whole slice, keeps angles constant but spreads a
cell's few neighbours over rings sized for every dimension in the slice, which hides valence and
pushes the figure out of the pane.

#### 9.2.5 Labels and valence

Each edge has a label item (`SubjectKind::Label`) at `ring.labelAt` of the way from the hub, facing
the camera, in the `Label` font role, fitted with a middle ellipsis so both ends of a name such as
`d.contact.phone.mobile` survive. It is a plane in the world like the cells. When it projects too
small to read the presenter does not draw it, and its edge and its accessible name remain.

Each neighbour carries a badge with its own valence (V-R26), and a short stub for each further
connection it has when `ring.stubs` is on. Activating a neighbour moves there.

#### 9.2.6 Walking an unbound dimension

`[` and `]` select a spoke — a dimension and a direction — without moving. `Return` walks to the
selected neighbour, and the selection stays on that dimension and direction, so `Return` again
continues along the rank. Walking never changes a binding. The selected spoke's cell is shown in
full in the side list while selected.

Binding the dimension is a separate, explicit act: `B` binds the selected spoke's dimension to the
next axis, and dragging does the same to a chosen axis.

#### 9.2.7 Dragging an edge to an axis

```text
 Idle --press on an edge or its label--> Armed
 Armed --moved past ring.dragThreshold--> Dragging     (release before that: a click)
 Dragging --pointer within a DropTarget's radius--> Dragging(over that axis)
 Dragging(over axis) --release--> Committed: ViewAxisSet::bind(axis, dimension); toss
 Dragging --release elsewhere, or Escape--> Idle; nothing changed
```

The view emits one `DropTarget` per placed axis. While a drag is over an axis the host lays out with
a `BindingPreview`, which is an input to a pure function and changes no binding, so cancelling has
nothing to undo. What is under the pointer is decided by the GPU pick delivered to the pane's focus
scope; the unprojected ray is used only to measure distance to the drop targets. A pick that arrives
after a toss is discarded by its epoch. Dropping on an axis binds that axis and leaves the others
alone; dragging a spoke along the wheel instead reorders the ring. The keyboard form — select the
spoke, `B`, choose the axis — ends in the same call (V-R25).

#### 9.2.8 The neighbours' wheels

The sub-views of this view are depths: the wheel alone, then the wheel with the wheels of its
neighbours, then of theirs, up to `ring.maxDepth`. The sub-view key steps through them, so the depth
is the reader's to set while reading.

A neighbour within the depth has its own wheel, built by the rules above with that neighbour as its
hub, at `ring.childScale` of its parent's size, and without the pair slot that leads back to its
parent — that connection is the edge already drawn.

A child wheel **bends**. It is not a flat circle: it opens away from its parent. Its slots lie on
the arc that leaves out `ring.childGap` either side of the direction back to the parent, and each
slot is set back from the viewer in proportion to its angle from the outward direction, by
`ring.childBend`, so the wheel curls round behind its hub like a cupped hand and its near edge does
not reach across its parent.

It then **flexes**. Slots are placed in a fixed order — depth first, then ring order. A slot whose
box would cover a real cell already placed, seen from the rest camera, is moved back by
`ring.flexStep` and outward along its own spoke until it is clear, at most `ring.flexTries` times. A
wheel that cannot be cleared drops a level of detail (§9.2.9) and is tried again. No wheel ever
covers a real cell (V-R58). The order is fixed and there is no relaxation, so the same input gives
the same figure.

#### 9.2.9 Detail under pressure

There is no cap on valence. A wheel that has more to show than it has room or legibility for
simplifies, in a fixed order in which the names go first and the number goes last (V-R59):

| Level           | Drawn                                                                            |
| --------------- | -------------------------------------------------------------------------------- |
| full            | every neighbour as a cell, every edge with its dimension's name                  |
| names condensed | names fitted with a middle ellipsis down to `ring.nameMinChars`                  |
| names hidden    | no names drawn; edges keep their colours, and the names stay as accessible names |
| badges          | neighbours as badges showing their valence                                       |
| summary         | one ring with a coloured tick per dimension, and the valence in the middle       |
| count           | the valence alone                                                                |

A wheel is drawn at the first level at which everything on it projects at readable size and clears
(§9.2.8). The accursed cell's wheel starts at full and is the last to give way; a neighbour's wheel
starts one level lower for each step of depth. The camera frames the outermost occupied ring of the
accursed cell's wheel, but not from further than a slot's text can be read; past that the reader
orbits, zooms, or steps the selection, which turns the selected spoke to the front. The side list in
the chrome always holds every dimension by name, in ring order.

#### 9.2.10 Edge cases and accessibility

A cell with no neighbours is a hub alone, and the side list says "No linked cells on any dimension."

The hub is role `cell`. Each dimension is a child group named for it, holding its neighbours, each
named with its text and its valence. Reading order is the axes in axis order and then the ring
order; it never depends on angle.

#### 9.2.11 Acceptance

The items placed equal the neighbours of `c` over `dimensionsOf(c)` exactly. For every dimension
with two neighbours, their centres sum to twice the hub's. Bound-axis neighbours lie on R, U and F.
The clamp inequality holds for every ring. The order of dimensions by (ring, angle) equals their
ring order, at any focus. Binding or unbinding an axis moves no unbound dimension. Drag and keyboard
leave `shown(axis)` equal. A cancelled drag leaves the binding arena unchanged. At every depth, no
item of a neighbour's wheel overlaps a real cell's box at the rest camera. Raising valence at a
fixed pane size lowers a wheel's level one step at a time, in the table's order, and never skips the
names.

```text
                 d.employer
                     o
          d.phone  \ | /  d.email          ring 0: flat, the wheel
                    \|/
   d.2 (axis 0) o----c----o d.2            axis 0 on the horizontal spoke
                    /|\
          d.email  / | \  d.phone          each dimension's two neighbours opposite
                     o
                 d.employer
        ( ring 1 leans back at the top and forward at the bottom; ring 2 the other way )
```

### 9.3 Dimensional pack view

#### 9.3.1 Purpose

To treat several dimensions as one. A group is bound to an axis, and each step along that axis is a
*pack*: the cells that many steps away along each of the group's dimensions. A person cell with
`{d.email, d.phone, d.address}` on the horizontal axis reads as a table: one row per dimension, one
column per step.

#### 9.3.2 Lanes and steps

Let group `G` have members `m₁ … m_k` in order; each is a dimension or a group. From a real cell `c`
and a signed step count `n ≠ 0`, lane `i` holds

- for a dimension `d`: the cell `|n|` links from `c` along `d` in the direction of `n`'s sign, if
  the rank reaches that far without returning to `c`;
- for a group `g`: the pack of `g` at step `n` from `c`, if it is not empty.

The pack of `G` at step `n` from `c` is that row of lanes. It exists while at least one lane holds
something, and the rank of packs ends at the first step where none does — "as far as there is at
least one cell to pack" (V-R27). If lane `i` holds something at step `n` it does at every step
nearer, so the rank has no holes. A ring rank is walked once round and stops.

Two definitions were refused. A breadth-first frontier — every cell one more hop from *any* cell
already packed, along *any* member — mixes dimensions: the second pack would hold the phone of an
e-mail address. It also grows as `kⁿ`, and a cell in it has no single dimension to say how it got
there. Taking only the immediate neighbours gives one pack and no rank.

When two members reach the same cell at the same step it appears in both lanes, because each lane
says which dimension reached it.

#### 9.3.3 The structure

Everything is in the derived arena, and no real cell is linked (§6.2). For origin `c` on axis `a`:

```text
 axisStep[a]:  ... [K-1] --- (o: c) --- [K+1] --- [K+2] ...   the rank of packs
                                          |
 d.pack:                               [K+1] --- (L1)          a container, then its first constituent
                                                  |
 d.packing:                    (L1) --- (L2) --- ( ) --- (L4)  the constituents, one per lane

 (o: c) an occurrence of the origin      (Li) an occurrence of lane i's real cell
 ( ) an empty place: lane 3 has run out   [K] a pack container
```

- `d.pack` relates a container to its contents: the container's posward neighbour is its first
  constituent. Entering a pack is that step; leaving is the step back.
- `d.packing` is the rank of constituents inside one pack, in lane order. A lane with nothing in it
  keeps an empty place, so a constituent's position in the rank is its lane and lanes line up from
  pack to pack.
- The packs along an axis are a rank on that axis's own `axisStep` dimension, with an occurrence of
  the origin as step 0.

`d.pack` and `d.packing` are two dimensions because they are two relations. On one dimension the
container would be the head of its own constituents' rank — "constituent zero" — and a nested pack,
which is both a constituent of its parent and a container of its children, would need two posward
neighbours on it.

Each of the three is a rank, so every cell has at most one neighbour each way on each (I1).

#### 9.3.4 Worked examples

*A simple group.* `G = (d.email, d.phone, d.address)`. From person `c`: e-mails `e1, e2`; one phone
`p1`; addresses `a1, a2, a3`.

```text
          step 0     +1        +2        +3
 d.email            [ e1 ]    [ e2 ]    [    ]
 d.phone    c       [ p1 ]    [    ]    [    ]
 d.address          [ a1 ]    [ a2 ]    [ a3 ]
                                                  step +4 does not exist: every lane is empty
```

*A nested group, a duplicate and ragged ends.* `H = (d.name, G, d.contact)`, where `d.contact`
happens to lead to `e1` as well.

```text
          step 0     +1                      +2
 d.name             [ n1 ]                  [    ]
 G          c       [[ e1 ][ p1 ][ a1 ]]    [[ e2 ][    ][ a2 ]]      lane 2 is a pack of G
 d.contact          [ e1 ]                  [    ]
```

`e1` is in two places at step +1: once inside the nested pack, reached by `d.email`, and once in
lane 3, reached by `d.contact`. They are two occurrences of one real cell; focusing either
highlights both. Step +3 exists, because `G`'s lane still holds `a3`.

#### 9.3.5 Movement

The cursor is the origin, the axis, the step and the lanes selected (§8.5). Stepping along the group
axis changes `step` by one if the pack there exists. That is single-valued — there is one pack at
each step — and reversible, since stepping back subtracts what was added; in the structure, the
`axisStep` rank is two-sided like any rank (V-R28). The origin does not change while the cursor
steps along its packs, so the packs do not change under it.

- **Enter** selects the first lane that holds something: `lanes` gains an index (`d.pack`, posward).
  **Next lane** and **previous lane** move along `d.packing`, skipping empty places. **Leave** drops
  the last index.
- **Retrieve** makes the cell under the cursor the origin: the cursor becomes that real cell with
  step 0, and the placement tosses (V-R29). Nothing is copied.
- **A step on another axis** from inside a pack acts on the cell under the cursor, as a retrieve
  followed by the step.

#### 9.3.6 Layout: the lane table

Packs stand side by side along the group axis's direction, and lanes stack across it, so a rank of
packs is a table. Lane `i` has the same height in every visible pack — the tallest content in that
lane, up to `pack.laneMaxLines` — so rows line up; each pack is as wide as its widest constituent,
up to `pack.chipMaxWidth`. The origin column shows the origin cell in full, and the lane names — the
members' names — are label items beside it, once.

Each pack is a `PlacedFrame` with view-only chrome: one outline round all its parts and a header
with the group's name and the step. Each constituent is a `PlacedItem` in that frame, placed
relative to it: `itemViewOnly` for an empty place and an ordinary cell item otherwise. A nested pack
is a frame inside a lane, laid out the same way, drawn to `pack.nestDepth` levels and collapsed to a
badge with its count below that. The depth drawn is a display budget; the structure nests as deep as
the groups do (V-R30).

When a second in-plane axis shows a dimension, each cell of the origin's rank on it is a row with
its own rank of packs, derived only for the rows in view. When a second axis shows a group, it acts
as its first member.

#### 9.3.7 Glue, strands and the spread

This is `pack_presentation`, shared code: any view that shows a pack gets the same look and the same
way of opening it (V-R57).

**Glue.** A pack is one thing made of parts, and it is drawn that way. Its constituents sit edge to
edge inside one outline, with a seam of `pack.seam` between lanes, and they move as one, because
they are placed relative to the pack's frame and the frame is what moves (V-R55). Nothing else on
screen is drawn like that, so a pack is not taken for a row of separate cells, and its parts are not
taken for one cell.

**Strands.** The edge from one pack to the next is a bundle of thin strands: one for each lane that
holds a cell at both ends, each in its dimension's colour — the colour that dimension has on the
compass, on its edges in other views and on its lane's seam (V-R56). The strands leave the
constituents of one pack, gather into a bundle between the packs, and separate again as they reach
the next, each joining its own constituent. From the origin cell they start gathered.

```text
  +------+                       +------+
  | e1   |====\           /======| e2   |      three strands leave, gathered between
  +------+     \         /       +------+
  | p1   |======#=======#        |      |      d.phone has run out: its strand is gone
  +------+     /         \       +------+
  | a1   |====/           \======| a2   |      the bundle is one strand thinner
  +------+                       +------+
```

A lane that has run out has no strand, so the bundle thins as the rank tapers and the taper can be
read from a distance. Where a strand joins a constituent, that constituent's face takes a tint of
the strand's colour (`itemTinted`); when the join lies in front of the face from where the camera
is, the tint goes to the border instead, so the text is not washed. Each strand is a `PlacedEdge` of
kind `Strand` whose `relation` is its dimension; the strands of one bundle share a `bundle` id and
the two `gather` points where they run together.

**The spread.** A glued pack hides which part is which. The spread shows it: the lanes move apart by
`pack.spreadGap`, the strands run separately instead of gathering, and in the room that opens each
lane's dimension name fades in as a label. It happens three ways: for one pack, while the pointer is
over the place its strands join; for every pack, in the view's `spread` sub-view; and from the
palette. Since the sub-view and the hover are inputs to `layout()`, the spread is the same pure
function as the glued form.

#### 9.3.8 Cost

Extending a rank of packs by one step is one link read per leaf dimension of the group. The
placement keeps, for each row and direction, the real cell each lane has reached, so it never
re-walks from the origin, and so it can re-derive the visible window after a toss (§6.7). Only packs
in view, plus a margin of `pack.aheadSteps`, exist.

#### 9.3.9 Keeping a pack

A pack is a way of looking and vanishes when the binding changes. A reader who wants one kept asks
for it by name: `promotePack()` (§8.5) hands the container and its constituents to
`zigzag::promote()`, which mints real cells for them — a constituent becomes a real handle cell
naming its original, as `promote()` already does for handle cells (`arena_manifold.cpp:1838`) — and
refuses above its budget. It is never automatic. Whether the promoted `d.pack` and `d.packing`
should be the slice's own named dimensions is VU4.

#### 9.3.10 Edge cases and accessibility

A group of one dimension gives packs of one lane; they still have pack chrome, because a pack must
never be mistaken for the stored cell it stands for. An empty group cannot be bound. A cell with no
neighbour on any member has no packs, and the step action reports that there is nothing that way.

A pack is role `group`, named "*group* step *n*: *k* of *m* lanes"; each constituent is named with
its text and "via *dimension*"; an empty place is named "*dimension*: nothing here". Entering and
leaving move accessibility focus.

#### 9.3.11 Acceptance

Both worked examples derive exactly as drawn, including the two occurrences of `e1` and the
existence of step +3. For every cursor on a rank of packs, a step posward and then negward returns
the same cursor and the same real cell. Retrieve yields the real cell and appends no operation.
After a toss the same cursor resolves to the same real cell. `verifyViewSpace` passes after every
step. Binding an empty group is refused. Derived cell count is bounded by the window for a walk of
any length. Between two packs there is exactly one strand for each lane present in both, and each
has its dimension's colour. Every constituent's position is relative to its pack's frame. Layout
with the `spread` sub-view, and with `hover` on a pack's join, emits a label for every lane and no
gathered strand for that pack.

______________________________________________________________________

## 10. The page views

### 10.1 What a page view works with

A page view arranges pages; it does not make them. Text, pagination, glyphs and the caret stay with
the library's `Doc`, and links and transclusions stay with the engine. A page view sees only the
`PageCatalog` of §8.6: how many documents and pages there are, how big each page is, whether a page
is *marked* — holds an end of a non-formatting link (any `LinkType` but `Format`,
`apps/common/xanadu/ops.hpp:229`) or content that also appears elsewhere — and where the ends of the
active link fall. From that it emits one `PlacedItem` per page it wants drawn.

Each document is a `PlacedFrame` of kind `Document`, and its pages are items in that frame, placed
relative to it (V-R60). The presenter gives the frame's transform to the library `Doc` and each
item's to its `Page`. A page thus has its own matrix, relative to its document: a view moves one
page without moving the document, and moves the document without restating its pages. A `Page`
already has such a matrix, but only its `Doc` writes it, as a column (`src/doc.cpp`); making it
settable, with an opacity, is the generic library change (see the rendering plan). The plain editor
can use it too.

Three things hold for every page view:

- **The current page is live.** The page under the caret is drawn in full and is the editing
  surface. No page view intercepts typing, and none changes what is on a page (V-R37).
- **Pagination may be unfinished.** A document can gain pages while it is shown. A view arranges
  what the catalog knows, says when more are coming, and is laid out again when they arrive.
- **Legibility decides detail.** A page whose lines project smaller than
  `PaneFrame::minReadableLinePx` is emitted `Coarse`, which the library already draws as bars
  (`DrawBudget::coarseBelow`, `include/gleditor/draw_budget.hpp`).

A page view has no view space. Pages are not cells and it mints nothing.

### 10.2 Shared geometry

Local units are Canvas pixels (§8.4); the presenter's placement transform carries
`Doc::pixelsToWorld`. A document's pages have one width `W` and height `H` (from `PageFacts`).
"Foreground" documents take part in the arrangement; a "background" document, opened for context
(`RenderItemOpenDoc::depthZ`), is set back by `page.backgroundDepth` and dimmed to
`page.backgroundOpacity`, as it is today.

Link and transclusion beams are `PlacedEdge`s between passage anchors: for an end on page `p` whose
passage spans `top` to `bottom`, the anchor is the page's placed plane at that height, on the edge
facing the other end. They are world-space edges, so they follow pages wherever a view puts them.

### 10.3 The base view

`page.base`: what xuzz does today, as a view.

#### 10.3.1 At rest

Foreground documents stand in a row in list order, each one `page.base.documentGap` from the last:
those are the document frames. Within its frame a document's pages flow downwards,
`page.base.pageGap` apart. Every page is `Plane`-facing, upright and opaque. This is the library's
present arrangement (`documentSlot`, `kDefaultDocumentGap`, `Doc::pageGapPx`), now produced by a
view.

```text
   doc A        doc B        doc C
  +------+     +------+     +------+
  | A 1  |     | B 1  |     | C 1  |        documents left to right
  +------+     +------+     +------+
  +------+     +------+
  | A 2  |     | B 2  |                     pages top to bottom
  +------+     +------+
```

#### 10.3.2 With an active link

When a link or a transclusion is active, the pages that hold its ends come together.

- **Who moves.** The *participants* are the distinct pages among `ActiveLink::ends`, for every
  member of both endsets. `ActiveLink::anchor` names the end the reader is at; its page is the
  *anchor page* and does not move.
- **Where to.** Each other participant is tied to the anchor page by its end nearest the anchor end,
  and a `CoalesceStrategy` finds positions that best satisfy the ties.
- **What "best fit" means.** Over the participants' positions, minimise the sum over ties of the
  squared height difference between the two passages' centres and the squared difference between the
  pages' horizontal gap and `page.base.coalesceGap`, with no participant covering another's passage.
  Level passages a small gap apart is what lets both ends be read in one glance.
- **In front of the row.** Participants are lifted towards the viewer by `page.base.liftDepth`, so
  they pass in front of the columns they leave and cannot collide with pages that stayed. A page
  that flies moves by its own matrix; its document's frame does not move, and the rest of the
  document stays where it was.
- **When they do not all fit.** If the participants fit side by side at a readable size, they are
  placed whole and do not overlap. A link with many ends, or ends on pages too large to show
  together, cannot be shown that way in one pane. Then overlap is allowed, and used: the
  participants nearest the anchor in tie order stay whole for as long as they fit, and each of the
  rest is shown as a **window** — only the band of the page round its passage,
  `page.base.bandContext` lines either side — stacked in tie order beside the anchor's passage. A
  windowed page is still its own page, with its own text and caret, drawn through
  `PlacedItem::window`; it is the page-sized form of what a satelloid does today, gliding a proxy
  into reading alignment beside a line (`apps/xudu/satelloid.hpp`). Participants, whole or windowed,
  stand in front of the row and may cover pages that are not taking part, and the paper of one may
  cover the paper of another. A linked passage is never covered. If even the windows outrun the
  pane, the stack carries a count and scrolls.
- **What stays behind.** Each page that flew leaves a ghost marker in its home place (`itemGhost`)
  and a `Tether` edge from the ghost to the page (V-R33). Pages that do not take part stay at home
  at `page.base.contextOpacity`.
- **The camera.** If the group of participants is larger than the pane, the pane's camera frames it;
  the layout never shrinks a page.

When the link is released everything returns home.

```cpp
// page/coalesce.hpp
struct CoalesceBody {
  PageRef page;
  glm::vec3 home{}, position{}; // position: in, the start; out, the result
  float width{}, height{};
  bool pinned{}; // the anchor page
};
struct CoalesceTie {
  std::uint32_t from{}, to{};  // bodies
  float fromHeight{}, toHeight{}; // passage centres, from each page's top
  float gap{};
};
class CoalesceStrategy {
public:
  virtual ~CoalesceStrategy() = default;
  /// Pure: the same bodies and ties give the same positions.
  virtual void solve(std::span<CoalesceBody> bodies,
                     std::span<const CoalesceTie> ties) const noexcept = 0;
};
```

The built-in strategy is today's: it loads the bodies and ties into a `TensionLayoutEngine` as
`TensionBody` and `TensionConstraint` and steps it `page.base.coalesceSteps` times at a fixed time
step, exactly as `LinkBeams` does now with 25 steps. It is a fixed number of steps and not "until
settled", and it never reads a clock, so it is a pure function and can run inside `layout()`. A
closed-form solver can replace it through the same interface.

Today the unit that moves is the whole document. The base view's unit is the page: when every page
of a document takes part, or it has one page, the document moves as before; otherwise only the pages
that hold ends fly, and the rest of the document stays readable where it was.

#### 10.3.3 Motion

The page being brought to the reader is the subject of the move: it starts first and takes longest
(`page.base.subjectMs`); pages that make room start `page.base.rowDelayMs` later and take
`page.base.rowMs`. These are today's `anim::sworphSubject`, `sworphRow` and `sworphRowDelay`,
expressed as `MotionHint`s.

#### 10.3.4 Acceptance

With no active link, page positions equal the row-and-column formula for any catalog. With one, the
anchor page's position is unchanged; every tie's passages are level within
`page.base.levelTolerance`; no participant covers another's passage, and when all fit whole no two
overlap; with more ends than fit, the rest are windows containing their passages; every moved page
has exactly one ghost and one tether; non-participants are at home. Two layouts of the same input
are identical. A many-to-many link with three ends on a side brings all six pages.

### 10.4 The stacked vanishing view

`page.stacked-vanishing`: a document as a deck of pages receding to a vanishing point.

#### 10.4.1 Purpose

To read one page with the next and previous in view, and to see at a glance how long the document is
and where in it the connections are. The current page is large and whole; the rest stand behind it
in a line that fades with distance, and the pages that hold links or transclusions stand out of the
fade.

#### 10.4.2 The deck

A document's pages in order are its deck. The deck is parted at the current page `t` into two
stacks:

- the **upcoming stack**: page `t` on top, then `t+1`, `t+2`, … behind it;
- the **passed stack**: page `t−1` on top, then `t−2`, … behind it.

Both stacks recede along the same direction

```math
\hat{u} = (\sin\psi\cos\varphi,\ \ \sin\psi\sin\varphi,\ \ -\cos\psi)
```

where ψ is the angle between the line and the view axis and φ is the direction, in the plane of the
pane, in which each page is offset from the one in front. Page `t+i` is centred at

```math
b + i\,s\,\hat{u}, \qquad i = 0, 1, 2, \dots
```

and page `t−i` at `b′ + (i−1)·s·û`, all relative to the document's frame, which is the deck's place
in the scene; `s` is `stack.spacing`, `b` is where the current page stands and
`b′ = b − (W + stack.gutter)·x̂` puts the passed stack beside it, on the side a previous page lies
in a book: the left for left-to-right text, the right when `DocumentFacts::rightToLeft`. Every page
stays parallel to the pane. Turning pages to make a fan was refused: text foreshortened by rotation
is harder to read than the same text smaller.

Because the two stacks are parallel lines in space, they converge on one vanishing point, and the
spacing between pages shrinks with distance. That is the long line of pages. Both tops — the current
page and the one before it — are whole and unobstructed, which is most of what "legible text in the
current page and the pages close by" can mean; what the stagger direction controls is how much of
each page *behind* a top is readable.

When the pane is too narrow to show both tops at a readable size, the passed stack is tucked behind
the current page with only a strip of `stack.tuckStrip` showing.

#### 10.4.3 Choosing the direction

A page behind another shows a strip along one horizontal edge and a strip along one vertical edge. A
strip along the top shows whole lines — usually a heading and the opening lines. A strip down the
side shows only the start or the end of every line. So the direction matters, and it depends on the
shape of the pane, the page and the type size. The view searches for it.

For a candidate `(φ, ψ)` and each of the `stack.nearPages` pages behind a top, project the page and
subtract the pages in front of it and whatever falls outside the pane. Of what is left, count

- each line wholly visible, if lines at that depth project at least `minReadableLinePx` high: 1;
- each line partly visible: the fraction visible, times `stack.lineStartWeight` if its start is what
  shows and `stack.lineEndWeight` if its end is.

The candidate's score is the sum over those pages, each weighted by `stack.nearFalloff` raised to
its distance from the top. Candidates are φ from 0° to 180° in steps of `stack.search.azimuthStep`
and ψ from `stack.search.minRecession` to `stack.search.maxRecession` in steps of
`stack.search.recessionStep`. The best score wins; the previous choice is kept while it is within
`stack.search.hysteresis` of the best, so the deck does not swing as a pane is resized.

The pages are congruent rectangles under one perspective, so each term is rectangle arithmetic, and
the whole search is a few hundred candidates times a handful of pages. It runs in `prepare()`, when
the pane, the page size, the line height or a setting changes, and never per frame (V-R34).

#### 10.4.4 Opacity and the punctuation

For a page `i` places behind its stack's top:

```math
\alpha_i = \max\bigl(\texttt{stack.fadeFloor},\ e^{-i/\texttt{stack.fadePages}}\bigr)
```

a slow fall — with the default of 12, the twelfth page behind is still a third opaque. A marked page
is the exception: it is fully opaque wherever it stands, with an edge tab in its link type's colour
(V-R35). Looking down the line, the solid pages are where the document is connected to something.
The two tops are always opaque.

#### 10.4.5 How far the line goes

Unmarked pages are emitted until one would project shorter than `stack.minPagePx`; a tail marker
then carries the number not drawn, or says that pagination is still running. Marked pages go on
being emitted beyond that, as `Coarse` slivers, down to one pixel, so the punctuation continues to
the vanishing point. Nothing is capped: a longer document has a longer tail count.

#### 10.4.6 Several documents

Each foreground document is a deck. Deck bases stand in a row as documents do in the base view, one
deck's width plus `stack.deckGap` apart, and all share `û`, so every line converges on the same
vanishing point. Background documents' decks are set back and dimmed. Beams join the passages of
linked pages wherever those pages stand in their decks; the ends of the active link are marked pages
by definition, so they are always solid.

```text
            . vanishing point
          .   .
        ..     ..            pages thin out and fade with distance;
      .|.|     .|#|          # a marked page: solid wherever it stands
    . | | |   . | | |
   +------+  +---------+
   | t-1  |  |    t    |     two tops, whole and opaque:
   |passed|  | current |     the page before, and the page being read
   +------+  +---------+
```

#### 10.4.7 Going to another page

Moving the cursor from page `a` to page `b` of one deck, `Δ = |b − a|` pages away, is done one of
two ways (V-R36):

```math
\text{riffle} \iff \Delta \cdot \texttt{stack.minFlipMs} \le \texttt{stack.maxTransitionMs}
```

provided every page between is already paginated; otherwise **split**. With the defaults of 60 ms
and 420 ms a page up to seven away is riffled to. The rule is stated in time and not as a page count
because that is the real constraint: a riffle is only a riffle if each page can be seen to turn and
the whole thing is over quickly.

- **Riffle.** The pages between cross from one stack to the other one at a time, in order. Page `j`
  of the run starts at `j·τ` and takes `τ`, on an arc that lifts it towards the viewer, with
  `τ = max(minFlipMs, min(flipMs, maxTransitionMs / Δ))`. It reads as cycling through the pages.
- **Split.** The deck parts at `b`. The pages between `a` and `b` cross to the other stack together,
  as one block, in `stack.splitMs`; page `b` then flies forward out of the line to the top of its
  stack, starting `stack.splitLeadMs` later, and the pages behind it close up. It reads as cutting a
  deck of cards to a place.

Both are emitted by `transition()` as `MotionHint`s over the same target layout, so the rule and the
choreography are testable without drawing anything. A page that becomes current in another
document's deck — following a link — moves *that* deck by the same rule, measured from that deck's
own top. With reduced motion every transition is a cut.

#### 10.4.8 Interaction

Next page and previous page move the cursor by one, which is a riffle of one. Next link and previous
link are the existing link navigation; the target page comes to the top of its stack. Clicking any
page in a line goes to it. Dragging along a line, or the wheel, scrubs through it. All of these have
the existing keyboard forms; the view adds none that is pointer-only.

#### 10.4.9 Accessibility

Each deck is a list named for its document and its page count. Each page drawn is a list item named
"page *n* of *N*", with "has links" or "has shared content" when marked. The current page exposes
its text as it does today. The tail marker is an item named with the number of pages it stands for.
Order is page order, never depth.

#### 10.4.10 Acceptance

Page centres satisfy the two formulas for any cursor. For a fixed input the direction chosen is the
maximum of the score over the candidates, and a second `prepare()` with the pane 1% wider keeps it.
Opacity is non-increasing along each stack over unmarked pages and is 1 for every marked page. Every
marked page of the catalog is emitted. `transition()` chooses riffle exactly when the inequality
holds; a riffle's hints have strictly increasing delays in page order; a split gives every page of
the block the same delay and duration. A right-to-left document's passed stack is on the right. No
layout call changes the catalog.

### 10.5 Further page views and deck sources

Two more page views fall out of code that exists and are worth registering once the two above are
in. They are candidates, not requirements, and each needs its own section before it is built.

| View                 | What it shows                                                                                | Carved from                                   |
| -------------------- | -------------------------------------------------------------------------------------------- | --------------------------------------------- |
| `page.contact-sheet` | every page of every document as a grid of small pages, marked pages highlighted; click to go | `apps/xudu/overview_overlay.*`                |
| `page.alongside`     | two versions of a document in two columns, pages paired by the content they share            | `Views::showAlongside`, `Views::setOnionSkin` |

The stacked vanishing view takes its decks from a `DeckSource`, and the built-in source is "each
document's pages in order". The same view over a different source answers a different question, with
no new layout: the versions of the current page through hypertime, most recent on top; or the pages
the reader has visited, from the activity log, as a walk they can riffle back through.

```cpp
// page/deck.hpp
class DeckSource {
public:
  virtual ~DeckSource() = default;
  [[nodiscard]] virtual std::uint32_t decks() const noexcept = 0;
  /// The pages of one deck, in the order they stand.
  [[nodiscard]] virtual std::span<const PageRef>
  deck(std::uint32_t index) const noexcept = 0;
};
```

______________________________________________________________________

## 11. Scenes, panes and mixing

### 11.1 Scenes and placements

A **scene** is one world. A **placement** is a view instance with its state, standing at an origin
in a scene. A scene with one page placement and one slice placement is today's unified mode: pages
and cells in the same space, under one camera, with beams between them. A scene with one placement
is today's xanadoc-only or zigzag-only mode.

"Mixed freely" therefore has two forms, and both are needed (V-R38):

- **Together:** several placements in one scene. Their records are in one coordinate system, so a
  beam from a passage on a page to a cell is an ordinary world-space edge.
- **Side by side:** several panes, each showing a scene through its own camera. Two panes may show
  the same scene from different places.

A placement's origin is a transform the host owns. Each view lays out in its own local coordinates
(§8.4) and never knows where in the scene it stands.

### 11.2 Edges between placements

Within a scene the host adds the edges that join placements: for each link whose ends lie in
different placements of the scene, one `PlacedEdge` between the placed anchors, resolved from each
placement's records by `SubjectId`. Only real cells and document sites are exchanged (V-R40): a page
placement is told "this real cell is an end", never a view cell, and a slice placement is told "this
`DocumentSite` is an end". `BridgeCoordinator` does this today for the one pair it knows.

A link whose other end is in a different scene cannot be a beam, because there is no shared space to
draw it in. Its end is drawn as a short stub towards the pane's edge, labelled with where it goes;
activating it focuses the pane that shows the other end, or opens one.

### 11.3 Panes and focus

The window is divided by a library `ui::PaneTree`: rectangles from `ui::split()` over the window's
safe area, so a pane never lies under screen chrome and neighbours share an edge exactly. The tree
is generic and knows nothing about views.

Each pane is a `ui::FocusScope` registered with `FocusManager::addPane()`. Cycling panes is
`FocusManager::cyclePane()`. A pane's scope opts into GPU picking and receives picks through
`pointerPick`. Chrome that must hold the keyboard is a modal scope opened with
`FocusManager::push()`, which restores the pane's focus when it closes. None of this is new
mechanism.

Splitting, closing and focusing change the tree and nothing else: each pane has its own camera, each
placement its own cursor and bindings (V-R39).

Accessibility reads view state on the render thread, so a command must not change a view space from
the input thread. The host marshals each view action to the thread that owns the placement before it
runs, as `ZigzagCommandHooks::dispatch` does today (`apps/zigzag/zigzag_commands.hpp:33`).

### 11.4 Compositing

Each pane draws its scene into a *render region*: a scissor rectangle and a depth slice (see the
rendering plan). Tiled panes do not overlap, so they share the whole depth range and the scissor
alone keeps them apart. A depth slice is needed only where regions overlap — an embedded view, or
chrome over a scene — and the front region takes a slice nearer than what it covers. This keeps
depth precision where it is needed and answers the worry that many panes would exhaust it.

Within a region, opaque planes are drawn front to back with depth writes, and translucent ones back
to front without. A deck's faded pages and a stretch view's faded cells are the translucent ones.

### 11.5 Embedding

A placement can be shown inside another's plane: a slice view in a region of a page, or a page view
in a cell. The embedded placement is its own scene and pane whose rectangle is the projected box of
the host item, redrawn as that item moves; it takes a depth slice in front of its host. "Open this
cell's content as a page" and "open this passage's cells as a slice" create such placements, or
ordinary panes, at the reader's choice.

### 11.6 Navigation and activity

`design/ui_workflow_xuzz_navigation.md` requires one link identity and both endsets in view. Across
placements of one scene that is §11.2's edges. Across scenes it is the stubs, and the rule that
following a link to a pane that is not showing its target brings that pane's cursor there without
moving any other pane.

A completed move is recorded as a `Visit` through the existing `xanadu::ActivityLog`
(`apps/common/xanadu/link_navigation.hpp:73`, `store_activity_log.hpp`), with the real cell or the
document site as its target; a view cell is never a visit target. Stretch vanishing's breadcrumb and
a walk `DeckSource` read the same log.

### 11.7 Chrome

The compass (§7.9), the view palette, the group editor and the all-dim walk's side list are
`ui::Widget` scenes in `ui::ScreenOverlay`s: `List` rows for dimensions, `Tabs` for views,
`TextField` for a group's name, `Badge` for counts. They inherit fitted labels with full accessible
names, focus order, minimum touch size, safe-area clamping and live response to the `ui.*` settings.
A pane's chrome is clipped to the pane with `Canvas::pushClip`.

The palette lists the views whose subject matches the focused placement, from the registry, built-in
and third-party alike, and under the current view its sub-views. The dimension selector (§7.7) is
not chrome: it is drawn in the world round the accursed cell.

______________________________________________________________________

## 12. Interaction summary

### 12.1 Default chords

Contexts: **X** anywhere in xuzz; **S** a slice placement has the keyboard; **AW** all-dim walk;
**PV** pack view; **P** a page placement. Every row is a `ChordSpec` on a `ViewDescriptor` or on the
host, seeded into `system://keymap`; none is handled in code. Rows marked † have not yet been
checked against the existing keymap; registration refuses a collision (`ChordCollision`) and the
conflict is then resolved in the table, not in code.

| Action               | Call                            | Default chord                                | Context              |
| -------------------- | ------------------------------- | -------------------------------------------- | -------------------- |
| `view.palette`       | `std:view/palette`              | `Ctrl+Alt+V`                                 | X                    |
| `view.cycle`         | `std:view/cycle`                | `Ctrl+Alt+Shift+V`                           | X                    |
| `view.select`        | `std:view/select(n)`            | `Ctrl+Alt+V` then `1`…`9`                    | X                    |
| `pane.split.right`   | `std:view/pane_split("right")`  | `Ctrl+Alt+Shift+H`                           | X                    |
| `pane.split.down`    | `std:view/pane_split("down")`   | `Ctrl+Alt+Shift+J`                           | X                    |
| `pane.close`         | `std:view/pane_close`           | `Ctrl+Alt+Shift+W`                           | X                    |
| `pane.next`          | `std:view/pane_next`            | `Ctrl+Alt+Shift+Tab`                         | X                    |
| `pane.openAsPage`    | `std:view/open_as_page`         | `Ctrl+Alt+Shift+O`                           | S                    |
| `camera.rest`        | `std:view/camera_rest`          | `Ctrl+Alt+0` †                               | X                    |
| `camera.orbit`       | `std:view/camera_orbit(dx, dy)` | `Ctrl+Alt+Shift+`arrows †                    | X                    |
| `view.subview`       | `std:view/subview(±1)`          | `Ctrl+Alt+.`, `Ctrl+Alt+,` †                 | X                    |
| `step.u`, `step.t`   | `std:nav/step(point, ±1)`       | `Alt+`arrows † (`u` left/right, `t` up/down) | S                    |
| `dimension.select`   | `std:view/selector`             | `Ctrl+Alt+D`                                 | S                    |
| `selector.tier`      | the `z` step actions            | as bound                                     | selector             |
| `selector.travel`    | the `x` and `y` step actions    | as bound                                     | selector             |
| `selector.quick`     | `std:view/selector_quick(n)`    | `0`…`9`                                      | selector             |
| `selector.bind`      | `std:view/selector_bind(point)` | the point's name: `x` `y` `z` `u` `t`        | selector, item armed |
| `selector.pouch`     | `std:view/selector_pouch`       | `P`                                          | selector             |
| `axis.cycle`         | `std:view/axis_cycle`           | `Ctrl+Tab`                                   | S                    |
| `group.new`          | `std:view/group_new`            | `Ctrl+Alt+G`                                 | S                    |
| `group.edit`         | `std:view/group_edit`           | `Ctrl+Alt+Shift+G`                           | S                    |
| `bind.undo`          | `std:view/bind_undo`            | `Ctrl+Alt+Z`                                 | S                    |
| `ring.next`, `.prev` | `std:view/ring_select(±1)`      | `]`, `[`                                     | AW                   |
| `ring.walk`          | `std:view/ring_walk`            | `Return`                                     | AW                   |
| `ring.bind`          | `std:view/ring_bind`            | `B`                                          | AW                   |
| `pack.enter`         | `std:view/pack_enter`           | `Return`                                     | PV                   |
| `pack.leave`         | `std:view/pack_leave`           | `Escape`                                     | PV, inside           |
| `pack.lane`          | `std:view/pack_lane(±1)`        | the cross-axis step keys                     | PV, inside           |
| `pack.retrieve`      | `std:view/pack_retrieve`        | `Shift+Return`                               | PV                   |
| `pack.keep`          | `std:view/pack_keep`            | `Ctrl+Alt+K` †                               | PV                   |

`view.select(n)` picks the n-th installed view for the focused placement's subject, so the same
chord serves slice and page placements and a third-party view gets a number without a new row.
Movement along axes, next and previous page, and next and previous link keep their existing actions
and chords. Pointer forms and their keyboard forms: orbit (secondary drag); rebind (drag an edge to
an axis or anything to the compass; or `ring.bind`, or the selector and a point's name); open a pack
(hover its join; or the `spread` sub-view); reorder the ring (drag a spoke round, or the HUD); go to
a page (click, or page and link actions); scrub a deck (drag or wheel, or hold next page).

### 12.2 Messages

| Situation                        | Text                                                           | Error              |
| -------------------------------- | -------------------------------------------------------------- | ------------------ |
| Nothing that way                 | "No cell further along *dimension* *direction*."               | none               |
| No pack that way                 | "*group* has nothing further *direction* from here."           | none               |
| Rebind tossed view cells         | "Showing *target* on axis *n*. Discarded *N* view-only cells." | none               |
| Binding an empty group           | "Group '*name*' has no dimensions yet."                        | `EmptyGroupBind`   |
| A group inside itself            | "'*inner*' already contains '*outer*'."                        | `GroupCycle`       |
| A saved binding no longer exists | "Dimension '*name*' is gone; axis *n* is unbound."             | none               |
| Deleting a group in use          | "Removed '*name*' from axes *…* and groups *…*."               | none               |
| Keeping a pack refused           | "This pack is too large to keep (*N* cells)."                  | `PromotionRefused` |
| Pages still arriving             | "*document*: *N* pages so far, still paginating."              | none               |

Each is also an accessibility announcement. The refusals below are faults of a view or of the host
rather than of the reader, but a third-party view can still surface them, so each has words:

| Situation                              | Text                                                                        | Error                |
| -------------------------------------- | --------------------------------------------------------------------------- | -------------------- |
| A view link would displace a neighbour | "That view cell already has a neighbour *direction* along *dimension*; ..." | `OccupiedDirection`  |
| A view link names a real cell          | "A view links only its own view-only cells; a cell of the slice was ..."    | `RealCellInViewLink` |
| A view cell from a tossed generation   | "That view-only cell was discarded when the view changed."                  | `StaleEpoch`         |
| An occurrence of nothing               | "That is neither a cell of the slice nor a group of this view."             | `UnknownTarget`      |
| No such axis                           | "There is no axis *n*."                                                     | `UnknownAxis`        |
| A view kind installed twice            | "A view of kind '*kind*' is already installed."                             | `DuplicateViewKind`  |
| A view's default chord is taken        | "*chord* already runs *action*; '*kind*' was not installed."                | `ChordCollision`     |
| The arena refused                      | "The view space refused the change."                                        | `ArenaRefused`       |

The full words are in `messageText()`; the table elides the two longest.

### 12.3 Settings

Lengths are logical pixels, converted with `UiMetrics::px()`, so they follow `ui.scale`; angles are
degrees; times are milliseconds. No setting names a font or a text size: cell content uses the
`Body` role, labels and lane names `Label`, badges and ticks `Caption`, pack headers `Title`.
Existing settings are used, not copied: the `ui.*` scale, font-role and touch-size settings; and
`zigzag.cellHorizontalPaddingPx`, `cellVerticalPaddingPx`, `contentMaxWidthPx` (the width limit
given to the measurer), `rankClearancePx`, `minReadableTextPx` and `connectionBeamWidthPx`.

| Setting                                                | Default      | Meaning                                                                   |
| ------------------------------------------------------ | ------------ | ------------------------------------------------------------------------- |
| `view.arena.windowCells`                               | 4096         | derived cells before a placement tosses and re-derives (§6.7)             |
| `view.camera.restYaw`, `restPitch`                     | 12, 8        | the rest camera's turn, so depth is visible                               |
| `activity.settleMs`, `activity.bounceMs`               | 1200, 300    | pause that ends a run of movement; a step undone this soon is a slip      |
| `subspace.rimBand`                                     | 48           | band at the pane's edge showing the next space along `u`                  |
| `view.motion.reduced`                                  | false        | every tween and transition becomes a cut                                  |
| `view.viewOnlyOpacity`                                 | 0.7          | chrome of view-only items                                                 |
| `stretch.gap`                                          | 4            | space between neighbouring boxes                                          |
| `stretch.minContact`                                   | 12           | least overlap with the cell a box was reached from                        |
| `stretch.overfill`                                     | 1.3          | viewport multiple the walk fills                                          |
| `stretch.layerDepth`                                   | 120          | distance between depth planes                                             |
| `stretch.fadeBand`, `fadeFloor`                        | 0.35, 0.15   | outer fraction that fades; the opacity it fades to                        |
| `stretch.clipMargin`                                   | 6            | margin before a hidden cell is shown again                                |
| `stretch.ghostOpacity`                                 | 0.25         | outline of a cell the pane would cut                                      |
| `stretch.heatSectors`, `stretch.heatFull`              | 16, 24       | directions the edge heat is summed in; count at full glow                 |
| `stretch.breadcrumbs`                                  | 6            | cells in the breadcrumb strip                                             |
| `ring.radius`, `ring.radiusStep`                       | 220, 90      | ring 0's least radius; growth per ring                                    |
| `ring.tiltStep`                                        | 28           | lean added per pair of rings                                              |
| `ring.slotWidth`, `ring.slotHeight`                    | 140, 44      | a ring cell's box                                                         |
| `ring.hubMaxShare`                                     | 0.4          | most of the pane the hub's content may take                               |
| `ring.labelAt`                                         | 0.55         | where on an edge its label sits                                           |
| `ring.stubs`                                           | true         | draw second-hop stubs                                                     |
| `ring.maxDepth`, `ring.childScale`                     | 2, 0.5       | how far out neighbours show wheels; a child wheel's size                  |
| `ring.childGap`, `ring.childBend`                      | 50, 0.6      | arc left open towards the parent; how far a child wheel curls back        |
| `ring.flexStep`, `ring.flexTries`                      | 24, 6        | how a slot is moved clear of a real cell, and how often                   |
| `ring.nameMinChars`                                    | 6            | shortest a condensed dimension name gets before it is hidden              |
| `ring.dragThreshold`, `ring.dropRadius`                | 6, 36        | drag start distance; drop target radius (not below `ui.minTouchPx`)       |
| `pack.laneMaxLines`, `pack.chipMaxWidth`               | 3, 220       | limits on a constituent's box                                             |
| `pack.nestDepth`                                       | 3            | nested packs drawn before collapsing to a badge                           |
| `pack.seam`, `pack.spreadGap`                          | 1, 28        | line between glued lanes; room between them when spread                   |
| `pack.strandWidth`                                     | 2            | a strand's thickness                                                      |
| `rank.halfLife`, `rank.smoothing`, `rank.presentBoost` | 200, 4, 2    | ranking: decay in runs; pull towards "used"; weight of present dimensions |
| `pack.aheadSteps`                                      | 2            | packs derived beyond the pane                                             |
| `page.backgroundDepth`, `page.backgroundOpacity`       | 720, 0.42    | where and how dim a context document is                                   |
| `page.base.documentGap`, `page.base.pageGap`           | 432, 32      | between documents; between pages                                          |
| `page.base.coalesceGap`, `liftDepth`                   | 432, 90      | gap between coalesced pages; how far they come forward                    |
| `page.base.coalesceSteps`                              | 25           | fixed solver steps                                                        |
| `page.base.bandContext`                                | 2            | lines shown either side of a passage in a windowed page                   |
| `page.base.contextOpacity`                             | 0.42         | pages not taking part while a link is active                              |
| `page.base.levelTolerance`                             | 2            | how level tied passages must end up                                       |
| `page.base.subjectMs`, `rowMs`, `rowDelayMs`           | 620, 450, 90 | motion of the page brought over and of those making room                  |
| `stack.spacing`, `stack.gutter`, `stack.deckGap`       | 60, 48, 240  | along the line; between the two tops; between decks                       |
| `stack.tuckStrip`                                      | 40           | strip of the passed stack shown in a narrow pane                          |
| `stack.nearPages`, `stack.nearFalloff`                 | 6, 0.7       | pages scored behind each top; weight per place                            |
| `stack.lineStartWeight`, `lineEndWeight`               | 0.5, 0.2     | value of a partly visible line                                            |
| `stack.search.azimuthStep`, `recessionStep`            | 15, 5        | candidate grid                                                            |
| `stack.search.minRecession`, `maxRecession`            | 10, 60       | range of the line's angle from the view axis                              |
| `stack.search.hysteresis`                              | 0.05         | how much better a new direction must score                                |
| `stack.fadePages`, `stack.fadeFloor`                   | 12, 0.08     | fade rate along the line; least opacity                                   |
| `stack.minPagePx`                                      | 8            | height below which unmarked pages stop                                    |
| `stack.minFlipMs`, `flipMs`, `maxTransitionMs`         | 60, 120, 420 | riffle timings, and the bound that decides riffle or split                |
| `stack.splitMs`, `stack.splitLeadMs`                   | 320, 80      | block motion; delay before the target flies in                            |

The defaults that restate today's constants are converted from them at `Doc::pixelsToWorld` (1/18):
24 world units of document gap, the same again between coalesced pages, and 40 of background depth.
Each group of settings has its Schema and Notes pages (V-R41). The binding points themselves are
`layout.bindingPoints`. A dimension's colour is `ui.dimension.<name>.colour`, assigned from a
palette when first seen and the reader's to change; it is the one colour that dimension has
everywhere. The dimension pouch is kept in `system://pouches`. Per-slice state —
`layout.slice.<id>.view`, `.subview`, `.axes`, `.groups`, `.ringOrder` — and per-document-set state
— `layout.pages.<id>.view` — live in `system://layout`.

______________________________________________________________________

## 13. Rendering

### 13.1 Everything is in the world

A view's records are planes and segments in one scene (V-R4). The presenter draws each through the
pane's camera, so dollying, orbiting and zooming act on cells, pages, edges and labels alike, and
relative depth is real: a ring that leans back is further away, a deck's tenth page is smaller
because it is further. Screen space is used for chrome only (§11.7).

Text stays readable under that camera by three means that do not take it out of the world:
`Facing::Camera` turns a plane to face the viewer; the fitted UI's legibility test decides whether a
label's text is drawn at its projected size; and a page or cell too small for text is drawn coarse.
A label that is not drawn is still an edge with a name.

### 13.2 What draws what

| Record                                          | Drawn with                                                                                                    |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `PlacedItem` for a cell, label, badge or marker | a plane in a library `ui::PlaneSet`: retained fitted text and rectangles, one transform and opacity per plane |
| `PlacedItem` for a page                         | the document's own `Page`, through that page's matrix and opacity                                             |
| `PlacedFrame` for a document                    | the `Doc`'s model matrix                                                                                      |
| `PlacedFrame` for a pack                        | a parent plane in the `PlaneSet`; its items are child planes and move with it                                 |
| `PlacedEdge` of kind `Strand`                   | thin `gleditor::Beams` segments along a curve through the bundle's two gather points                          |
| `PlacedEdge`, other kinds                       | `gleditor::Beams`                                                                                             |
| `DropTarget`                                    | a beam and a plane while a drag is in flight; hit-tested with the unprojected ray                             |
| chrome                                          | `ui::ScreenOverlay` widget scenes                                                                             |

A plane's text is fitted once, when its record or the typography changes, and kept as `FittedText`.
Moving, fading and turning a plane change its transform and opacity, which are per-draw uniforms
(`DrawUniforms::mvp`, `opacity`, `include/gleditor/render/types.hpp:204`), not vertex data. That is
what makes a settled frame, and most of an animated one, free of shaping and uploads (V-R44).

Picking uses the renderer's picking target as today: each plane and page carries an identity, a
click requests the pick, and the result comes to the pane's scope with the `SubjectId` attached as
its semantic target. The CPU ray from unprojection is for measuring — drop targets, scrubbing along
a deck — not for deciding what is under the pointer.

### 13.3 What the library must gain

All generic, all tested in `tests/lib/`, none naming a xanalogical thing; planned in
[`world-space-rendering-plan.md`](world-space-rendering-plan.md):

| Addition                                            | Needed for                                                       |
| --------------------------------------------------- | ---------------------------------------------------------------- |
| unprojection that inverts the renderer's projection | drag to rebind, deck scrubbing, any pointer measure in the world |
| `insideFrustum`                                     | stretch vanishing's all-or-nothing clip (§9.1.5)                 |
| render regions: device scissor and depth slice      | panes, embedded views (§11.4, §11.5)                             |
| depth test without depth write                      | translucent pages and faded cells                                |
| `ui::PlaneSet`, with parent planes and soft bands   | cells, labels, badges; glued packs; ghosts and edge heat         |
| a settable matrix and opacity on each `Page`        | every page view                                                  |
| `ui::PaneTree`                                      | splitting the window                                             |

Nothing in this document needs a new shader or vertex format.

______________________________________________________________________

## 14. Performance

| Work                | When                                         | Cost                                                          |
| ------------------- | -------------------------------------------- | ------------------------------------------------------------- |
| toss                | each rebind, origin change, window overflow  | constant (§6.5)                                               |
| bind, group edit    | each edit                                    | the cells edited                                              |
| stretch placement   | cursor, binding, content, pane change        | linear in cells placed; bounded by the pane                   |
| wheel placement     | cursor or binding change                     | the accursed cell's valence, plus its neighbours' for badges  |
| pack derivation     | cursor step, row scrolled into view          | one link read per leaf dimension per new pack                 |
| lane table          | with pack derivation                         | constituents in view                                          |
| coalesce            | active link change                           | `coalesceSteps` × participants²                               |
| deck stagger search | pane, page size, type size or setting change | candidates × `stack.nearPages`                                |
| deck layout         | cursor or catalog change                     | pages emitted; bounded by `stack.minPagePx` plus marked pages |
| measuring           | first sight of content at a width and role   | one `text::fit()`; cached                                     |
| settled frame       | every frame                                  | no layout, no shaping, no upload                              |

Probes, not assertions of speed: `tools/layout-latency-probe` gains a mode per view that lays out
synthetic slices of growing valence and rank length and documents of growing page count, and reports
percentiles, sink growth and toss time; a per-view scene modelled on `tools/ui-text-baseline.cpp`
reports frame time and shaping counts on each backend. "No ceiling" is checked by the curves staying
flat where this table says they are bounded by the pane.

______________________________________________________________________

## 15. Accessibility and text

The rules are the same for every view, so they are stated once.

- A drawn real cell is role `cell`; a drawn page is what it is today. Anything view-only — a pack,
  an empty lane, a ghost, a tail marker, a badge, a label — has another role (`group`, `listitem`,
  `note`, `label`) and is never announced as stored content (V-R45).
- A name is the full text. Fitting, coarse drawing and hiding by legibility change what is painted,
  never the name (V-R43).
- Reading order is structural — axis order, ring order, lane order, page order — never an angle or a
  depth.
- A node's bounds are the projected bounds of the plane that is drawn, and change with the camera
  without the model changing.
- What is not drawn is not in the tree, with one exception: a label hidden only for legibility keeps
  its node.
- Faded content must still meet contrast at the fade floor against the pane's background; the
  floors' defaults are chosen for that and a test checks them against the theme.
- With `view.motion.reduced`, transitions cut and the camera does not animate.
- Every rebind, toss message, pack entry and page transition is announced (§12.2).

______________________________________________________________________

## 16. Testing

### 16.1 Where tests live

A test sits with the code it tests and nothing is tested twice (V-R48).

| Code                                                                                   | Test binary     | Directory     | Links                             |
| -------------------------------------------------------------------------------------- | --------------- | ------------- | --------------------------------- |
| library: unprojection, regions, `PlaneSet`, page matrices, `PaneTree`, `insideFrustum` | `gleditor_test` | `tests/lib/`  | the library only                  |
| view framework and built-in views (engine)                                             | `xuzz_test`     | `tests/xuzz/` | the engine; no library, no device |
| presenter, host, chrome, commands                                                      | `ui_test`       | `tests/ui/`   | both                              |

`xuzz_test` holds xuzz's own code and no more. A view test supplies a fixed measurer and asserts on
records; it does not sweep fonts, scales or backends, because fitting, shaping, projection, focus
and clipping are the library's and are swept in `tests/lib/`. A presenter test asserts that the
right library calls are made with the right boxes and roles, on one font and one size; it does not
re-prove that `text::fit()` fits. The third binary was `zigzag_test` in `tests/zigzag/` until
`apps/zigzag/` was emptied; it is now `ui_test` in `tests/ui/` (VU6).

### 16.2 What is tested

- **View space.** I1 to I6 as §6.4 lists; `verifyViewSpace` after random sequences of bind, group
  edit, derive, move and toss; the toss test of §6.5.
- **Binding.** One dimension on two axes and in two groups; nested groups; cycle and empty-group
  refusals; undo and redo restore `shown()` for every axis; persistence round trip by name,
  including a name that has gone.
- **Binding points.** A configured sixth point gets a slot, a compass arm and movement actions with
  no code change; a point whose role a view does not present still moves the cursor.
- **Selector and ranking.** `rankDimensions` on hand-written histories: decay, the Markov order
  after a given last dimension, the boost for present dimensions, and determinism. The selector's
  layout and cursor for each tier; arming an item and naming a point calls `bind` with that pair;
  group edits in tier 1 are `ViewAxisSet` calls and undo.
- **Each slice view.** Its acceptance list (§9.1.8, §9.2.11, §9.3.11).
- **Each page view.** Its acceptance list (§10.3.4, §10.4.10), over hand-written catalogs.
- **Purity.** `layout()` twice gives equal sinks; a layout with allocation counting on reports none
  after the sink has reached size.
- **No operations.** `expectNoOpsAppended(store, fn)` wraps every move, rebind, view switch and page
  transition and compares the store's operation count and bytes before and after.
- **Golden layouts.** For each view, a few inputs with their records dumped as numbers and as a text
  raster (§8.7) under `tests/samples/view/`. They are regenerated when a layout rule changes on
  purpose, and the raster makes the change reviewable.
- **Presenter.** A settled frame reports zero layout and shaping calls in a `ShapingStatsScope` and
  zero uploads; a shortened label's accessible name is its full text; a stale pick is dropped;
  commands run on the owner thread.
- **Policy.** `apps/common/ui/view/` is added to the files `tools/check-ui-text-policy.py` scans.
- **Backends.** `tools/compare-backends.sh` gains one scene per view and one two-pane scene.
- **Journeys.** `design/ux_workflow_real_work.md` gains journeys for: auditing a cell's connections
  and re-axing by one; reading a table without stepping cell by cell; browsing a pack and pulling
  one cell out; reading two linked documents in the base view; reading a long document as a deck and
  jumping near and far; pages and cells in one scene; two panes; switching views without losing
  place; a third-party view appearing in the palette. Each step is reachable from the interface.

______________________________________________________________________

## 17. Migration

Each step builds and keeps `make test` green, and each can be committed alone. Step 2 is independent
of steps 3 to 5 and can run beside them.

### Step 1: empty `apps/xudu/` and `apps/zigzag/`

Moves only: no behaviour changes. Include spellings, the Makefile's source lists and
`packaging/wasm/build.sh` follow the files.

- **To `apps/common/ui/xanadoc/`** — everything in `apps/xudu/`: `session`, `views`,
  `views_publication_links`, `beams`, `bridge_coordinator`, `batch_orchestrator`, `link_context`,
  `satelloid`, `tenuous_tether`, `kinetic_tether_overlay`, `wireframe_hull`,
  `world_card_presentation`, and the overlays `clasp_link_forge`, `collaborator_overlay`,
  `link_panel_overlay`, `overview_overlay`, `page_break_overlay`, `pouch_drawer`,
  `swarm_telescope_overlay` and `transcopyright_overlay`.
- **To `apps/common/ui/slice/`** — everything in `apps/zigzag/`: `zigzag_visualizer`,
  `zigzag_commands` and `unified_transclusion_engine`.
- **Nothing goes to `apps/common/xanadu/`.** An earlier version of this list sent `link_context` and
  `unified_transclusion_engine` there. Both need the library — the first through `Session`, the
  second because it stages GPU buffers — and the engine is linked by ten targets that do not link
  it.
- **Staying for now**: `apps/xuzz/view_coordinator.*`, until step 6 replaces it.

`XUDU_SRCS` and `ZIGZAG_SRCS` fold into the `apps/common` source lists. One consequence needs care:
the test binary that links `apps/common/ui/` then links the xanadoc components and their libraries
too, where today it links only the slice ones. After this step `apps/xuzz/` holds `main.cpp`,
`cli.*`, `xuzz_app.*` and, for now, `view_coordinator.*`.

*Tests:* unchanged apart from include paths.

### Step 2: the library's world-space work

As [`world-space-rendering-plan.md`](world-space-rendering-plan.md) orders it: unprojection and
`insideFrustum`; render regions with scissor, then depth slices; depth test without write;
`ui::PlaneSet`; page matrices; `ui::PaneTree`. *Tests:* in `tests/lib/` only, with
`compare-backends.sh` scenes for regions and planes.

### Step 3: the view space and the binding model

`ArenaManifold` gains the `release()` guard and `shadowCount()` (§6.5). Add `view.hpp`,
`view_error.hpp`, `view_records.hpp`, `view_manifold.*`, `view_binding.*` and `raster.*`. No
application change. *Tests:* `tests/xuzz/view_manifold_test.cpp`, `view_binding_test.cpp`,
`raster_test.cpp`; the existing arena tests unchanged.

### Step 4: the slice views

`slice_view.hpp`, `pack_rank.*`, `pack_presentation.*`, then stretch vanishing, all-dim walk and the
pack view, each with its acceptance list as a test file and its golden layouts; then
`dimension_ranking.*` and `selector.*`. No application change.

### Step 5: the page model and the page views

`page_view.hpp`, `coalesce.*`, `deck.*`, the base view and the stacked vanishing view, tested over
hand-written catalogs. The base view is first checked for parity with today by giving the strategy
one body per document, which reproduces `LinkBeams`' result, and then switched to pages.

### Step 6: the presenter and the host

`apps/common/ui/view/`: the presenter (including the `PageCatalog` over the library's documents and
the measurer over `text::fit()`), the animation, the chrome with the compass, and the host.
`xuzz_app` builds a `ViewHost`; `ViewCoordinator` goes. `ZigzagVisualizer` and `xanadu::Views` are
each wrapped as a legacy placement, so the unified mode is one scene with two placements and nothing
regresses while the new views become selectable beside them. *Tests:* host tests for split, close,
focus and persistence; presenter tests as §16.2; existing visualizer tests unchanged.

### Step 7: commands, keymap and bindings

`view_commands` registers every action with its default chord, absorbing `zigzag_commands`, and the
binding points come from `layout.bindingPoints`, starting with `x`, `y`, `z`, `u` and `t`.
`ViewAxisBinding` stops being live storage and `DimensionBundle` becomes group presets. *Tests:* the
visualizer's navigation, swap, cycle and bundle tests are adapted to assert the same behaviour
against `ViewAxisSet`.

### Step 8: replace what was wrapped

The visualizer's Cell Content and Topology modes become two more slice views, and `ZigzagVisualizer`
is deleted. The base view takes over arrangement from `LinkBeams`, which keeps only what draws a
beam, as the presenter's edge painter; `xanadu::Views` shrinks to opening, closing and switching
documents. `UnifiedTransclusionEngine`'s private ephemeral slots are replaced by a `ViewManifold`,
leaving one ephemeral-cell mechanism in the tree. *Tests:* each legacy test is re-homed against the
view that replaced the behaviour, or retired with a line saying why.

### Step 9: journeys, probes and goldens

The journeys of §16.2 with their evidence, the probes of §14, and a `compare-backends.sh` scene per
view.

______________________________________________________________________

## 18. Rulings

**V1. A slice placement's view space is two sibling `ArenaManifold`s, and a toss is `release()` to
the mark taken on the empty derived arena.** Why: `ArenaManifold` already has the link
representation, the ephemeral bit and truncation; with no shadows and no trail, its `release()` is a
fixed number of steps and frees the storage at once (§6.5). Price: a one-line guard and one accessor
on `ArenaManifold`; a second arena per placement; refs are reused after a toss, so every held ref
must carry its epoch. Refused: (a) a new storage type — a second copy of a tested invariant; (b) one
arena with nested marks — a stack cannot empty the lower layer first without doing it on the rebind
path; (c) an epoch counter that only hides old cells and reclaims them later — the earlier design;
it was needed only because view links shadowed real cells, and V6 removes the cause.

**V2. `d.pack` and `d.packing` are two dimensions.** Why: containment and the order of constituents
are two relations; on one dimension a nested pack would need two posward neighbours (§9.3.3). Price:
one more link per pack. Refused: one dimension read in two directions, after `d.clone`, which has
one relation to carry.

**V3. A pack step is one lane per member: the cell that many steps along each dimension.** Why: it
is what "every cell connected along a dimension in the group" says; it reads as a table; it costs
one link per dimension per step; and each constituent has exactly one dimension that reached it.
Price: a cell reachable only by mixing dimensions is not in any pack of that rank — the reader
retrieves and continues. Refused: the breadth-first frontier of the earlier draft — it mixes
dimensions, grows as `kⁿ`, and cannot say which dimension a constituent came by.

**V4. Bindings are cells; there is no fixed number of axes.** Why: three named fields and a closed
enum are ceilings. Price: resolving an axis is two reads, not a field. Refused: a longer fixed
array.

**V5. Every view link goes through one function that refuses rather than displaces, and a verifier
checks the whole space.** Why: an invariant each view must remember is a hope. Price: one
indirection per link. Refused: per-view discipline.

**V6. A view arena never shadows a real cell.** Why: a shadow is a stale copy the moment the store
advances, makes `release()` proportional to the shadows, and puts view links on real cells (§6.2).
Price: a view cell cannot be *linked* to the real cell it stands for (V13); `ensureDimension()` is
off limits, so view dimensions are unnamed in the arena. Refused: shadowing only on view-owned
dimensions and filtering every read — the earlier design; it is correct only while every reader
remembers to filter.

**V7. Bindings, groups and ring order persist in `system://layout`, per slice, by name.** Why: they
are how a reader looks, not what the document says, and not a visit. Price: a group does not follow
the reader to another slice (VU1). Refused: the slice's store; the activity store.

**V8. Keeping a pack is explicit and per pack.** Why: a way of looking must not become structure
unasked (R8). Price: the reader asks each time. Refused: keeping on edit, or after a while.

**V9. A pack of one lane, and a lane with nothing in it, keep view-only chrome.** Why: a view cell
must never pass for a stored one. Price: a little more ink. Refused: collapsing them.

**V10. Walking an unbound dimension never changes a binding.** Why: the reader selects a spoke and
walks it, and repeats; nothing needs to be bound for that, and a binding that changes because one
moved is a surprise. Price: the axis keys do not follow an unbound dimension until it is bound,
which is one key. Refused: the earlier draft's temporary bind — an axis that changes under the
reader, plus a stack of bindings to restore.

**V11. Switching view keeps the placement's cursor and bindings.** Why: they belong to the
placement, not to the view. Price: no per-view memory of bindings. Refused: stash and restore.

**V12. Page views are specified here.** Supersedes the earlier ruling that left them to a follow-up.

**V13. A view cell stands for its target by a handle value, not a link.** Why: any link to a real
cell shadows it (V6); a handle is one read to resolve, and it is the cell kind the engine already
uses to name another cell. Price: "what uses this dimension" is a scan of the axes and groups, a
handful of cells; the relation is not walkable as a rank. Refused: (a) occurrences ranked under
their target on a dimension, after `d.clone` — the earlier ruling; it shadows every bound dimension
cell and every packed cell; (b) a direct link from an axis to its dimension — it also caps a
dimension at one axis.

**V14. Text, chrome, focus and legibility come from the fitted UI layer.** Why: the library fits,
shapes, scales and arbitrates focus, tested across fonts and backends; a second implementation would
drift. Price: a layout knows text only as sizes from a measurer. Refused: linking the library into
the engine; a view-owned label size.

**V15. Cells, pages, edges and labels are all in world space.** Why: one camera then moves, turns
and scales everything together, and rotation and depth show structure that a flat picture cannot.
Text is kept readable by facing the camera, by the legibility test and by coarse drawing, none of
which leaves the world. Price: a label can be hidden by something in front of it, and shrinks with
distance until it is not drawn; the library needs placed planes, regions and unprojection. Refused:
screen-space labels at projected anchors — the earlier ruling; they do not rotate, scale or occlude
with what they label, so they stop being part of the structure; and a face-camera bit in the glyph
shader — a per-plane matrix does the same with no shader change.

**V16. A view prepares, then lays out; only preparing may write.** Why: the earlier `layout()` was
called pure and also minted cells. Separating them makes the pure half testable and repeatable, and
makes a drag preview an input instead of a rebind. Price: two entry points. Refused: one call.

**V17. Generic code is the library's, xanalogical code is `apps/common/`'s, and `apps/xuzz/` only
starts the program.** Why: the plain editor shares the library and must never meet a cell; the
language tools share the engine and can then lay out without a window; and a test belongs with the
code it tests, once. Price: the presenter and host are in `apps/common/ui/`, one directory further
from `main()` than is usual. Refused: view code in `apps/xuzz/` — the earlier placement; engine
tests that re-sweep the library's fonts and backends.

**V18. Mixing is scenes and panes.** Why: "together in one world" and "side by side" are different
needs, and today's unified mode is the first. Price: a link across scenes cannot be a beam. Refused:
panes only, which would end beams between pages and cells; one scene only, which would end
independent cameras.

**V19. One record vocabulary for cells and pages, identified by `SubjectId`.** Why: the presenter,
the animation and the raster then serve every view, and a tossed cell fades without being looked up
because its epoch is part of its identity. Price: a page view's records carry fields it leaves
unset. Refused: separate record types per subject.

**V20. The base view moves pages, and coalescing is a strategy whose first form is today's tension
engine at a fixed number of steps.** Why: it is today's behaviour, made replaceable and pure. Price:
"best fit" is whatever the strategy reaches in its steps, as now. Refused: stepping until settled —
not a pure function; whole documents only — it hides the rest of a long document behind one link.

**V21. A deck is two parallel stacks of unrotated pages, and its direction is searched for.** Why:
two tops are both whole; parallel lines give one vanishing point; and which strip of a hidden page
is worth showing depends on the pane, the page and the type. Price: a search of a few hundred
candidates when the pane changes. Refused: a fan of rotated pages — foreshortened text; a fixed
direction — wrong for either wide or tall panes; a second vanishing line for passed pages — it
halves the room.

**V22. Riffle or split is decided by time.** Why: a riffle is one only if each page is seen to turn
and it is over soon; two timings say that, a page count does not. Price: none. Refused: a threshold
in pages.

**V23. The wheel is compact, ordered by a rank, and fixed in the placement.** Why: its fullness is
then the cell's valence; the order is the same everywhere and is the reader's; and orbiting shows it
from another side. Price: a dimension's angle differs between cells that have different neighbours.
Refused: a fixed slot per dimension; a helix or Fibonacci sphere; a camera-aligned figure.

**V24. Derived cells exist only for what is in view.** Why: a long walk would otherwise grow the
arena without bound. Price: a toss and a re-derivation when the window is full. Refused: keeping
everything derived.

**V25. Stretch vanishing places by anchored slide.** Why: it is deterministic, linear, local under
edits, and keeps each cell against the one that reached it. Price: gaps where no slide fits.
Refused: track tables; relaxation; a single skyline, which fills one direction and this view
radiates in four.

**V26. A view has sub-views, and the one showing is an input to its layout.** Why: the spread of a
pack, the depth of neighbours' wheels and the edge heat are variants of one view, not other views;
one key cycles them and the cursor never moves. Price: a descriptor lists them, and a layout must
handle each. Refused: a view kind per variant — it crowds the palette and makes a variant look like
a change of place; a mode kept inside the view — hidden state, and `layout()` is no longer a
function of its input.

**V27. Axes are named binding points with movement keys and a role.** Why: three is not the number;
what limits it is keys and what can be told apart; and a point can mean more than a direction — a
door to another space, or time. Price: roles are a second registry, and every view must say what it
does with each role or leave the point to the compass. Refused: three fixed axes; any number of
anonymous axes — one with no keys cannot be walked, and one with no name cannot be asked for.

**V28. Dimensions are chosen in a three-tier selector in the world, and shown on a compass.** Why:
groups, the dimensions at hand and every dimension are three different questions; putting them round
the accursed cell keeps the reader's place; and naming the point by its key makes a binding three
keystrokes. Price: a modal scope and a second way to lay out dimension cells. Refused: a list dialog
— it hides the cell and has no place for groups and their members together; chrome only — dimensions
are cells and are better shown as cells.

**V29. "Most used" and "most likely" are computed from condensed walk summaries by a first-order
Markov model with decay.** Why: they are facts about what the reader did, so they are replay
products, not settings; first order is what the data can support; and a record per settled run,
carrying its own counts, keeps the activity store to the times the reader stopped somewhere. Price:
the order of steps inside a run is gone, so nothing finer than "this followed that, this often" can
ever be asked of the past; and the summaries need a place in the activity store (VU5). Refused: a
record per step — it buries the store in noise; counters stored as configuration; a higher-order
model.

**V30. A thing in a frame is placed relative to the frame; a page has its own matrix relative to its
document.** Why: a pack must move as one glued thing and a document must carry its pages, and a part
must still be movable on its own; hierarchy says both. Price: a record's position is not its world
position until its frames are composed. Refused: absolute positions for everything — the whole and
its parts then tween separately and drift; a pull-style arrangement object on `Doc`, as first
planned — the page should own its matrix as the document owns its own.

**V31. The edge between packs is a bundle of strands, and a dimension has one colour everywhere.**
Why: a single edge cannot say which lanes continue; strands can, and thin out as lanes end. One
colour per dimension is what lets a strand, a compass arm and a wheel's edge be recognised as the
same thing. Price: a colour per dimension to assign and keep distinct, and it must not be the only
cue. Refused: one thick edge per pair of packs; colours chosen per view.

**V32. A cell the pane would cut is drawn as an empty ghost, and what lies beyond as edge heat.**
Why: content cut in half reads as missing data, but nothing at all makes the edge of the pane look
like the edge of the data. Price: two more kinds of marker. Refused: drawing cut content; drawing
nothing — the earlier text.

**V33. Neighbours' wheels bend and flex by fixed rules, and a wheel loses detail in a fixed order
with names first.** Why: the same input must give the same figure, and names are the widest and
least needed thing on a wheel when its colours and the side list still say which dimension is which.
Price: a wheel that cannot be cleared loses detail where relaxation might have found room. Refused:
spring relaxation; a cap on depth or valence.

**V34. The API has no sentinels, uses the C++26 facilities, and its setters chain.** Why: a reserved
value is a second meaning hidden in a type, and it is the caller who forgets to check;
`std::optional` makes the absence part of the type. A `function_ref` callback allocates nothing, and
an `inplace_vector` says a bound is real. Returning the object lets a caller write a sequence of
changes as one expression, as `ArenaManifold` already allows. Price: an optional index is eight
bytes where a sentinel was four; `inplace_vector` may be used only where the bound is a fact, never
to cap what is the reader's. Refused: `noCell`, `noAxis` and `noIndex` in the API — the earlier
text; `void` setters.

**V35. Movement is recorded as one condensed walk summary per settled run.** Why: a reader passes
cells many times a second, and a record per step would be noise that every later reader of the store
pays for. A run's counts per dimension, and per pair of dimensions, are all the ranking needs.
Price: the order of steps inside a run is not kept. Refused: a visit per step; sampling steps — it
keeps the noise and loses the counts.

**V36. In the base view pages may overlap, and a page may be shown as a window round its passage.**
Why: a link with many ends, or ends on large pages, cannot otherwise be seen in one pane, and seeing
both ends is the point. Price: a windowed page shows little of its context, and a participant may
cover pages that are not taking part. Refused: forbidding overlap, as first written — the reader
would be zoomed out past reading; flying only as many pages as fit and leaving the rest at home —
ends would be out of sight.

______________________________________________________________________

## 19. Open questions

**VU1.** Should a dimension group travel between slices? Settled by: use; a slice-independent key in
`system://layout` can be added without a format change.

**VU2.** What may a third-party view do — mint, read every store, register chords? Settled by: a
trust decision above this document.

**VU3.** Which member should a view other than the pack view follow when an axis shows a group? It
is the first member now. Settled by: use.

**VU4.** When a pack is kept, should its `d.pack` and `d.packing` be the slice's own named
dimensions, shared by every kept pack? Settled by: the first design of kept packs as content.

**VU5.** What should the activity store hold for a slice? Walk summaries (§7.8) are the first thing:
a record per settled run, with its counts. Their shape there — and whether a visit should also
record the view and the bindings, so going back restores how the reader was looking — is that
store's design to make. Either way the answer is more cells on its own dimensions, not a wider
struct.

**VU6.** Decided: the binary that links the library and `apps/common/ui/` is `ui_test`, with its
tests in `tests/ui/`, renamed straight after the relocation. `tests/xudu/` keeps its name for now.

**VU7.** `session` and `batch_orchestrator` mix engine work with library calls. Should each be split
so its engine half reaches `apps/common/xanadu/`? Settled by: the first language tool that wants
one.

**VU8.** Is a stub enough for a link whose other end is in another scene? Settled by: journeys with
two panes.

**VU9.** Is depth as stacked planes the right reading of a third axis in stretch vanishing? Settled
by: use on slices with a meaningful third dimension.

**VU10.** Is a windowed page (§10.3.2) enough when a link has very many ends, or is a smaller unit
still needed — the passage alone, as a card? Settled by: links with dozens of ends.

**VU11.** How deep may subspaces nest before the reader is lost, and is the band of the next space
at the periphery enough to say where one is? The inset and the zoom are decided (§7.3). Settled by:
use on a slice with real sub-clusters.

**VU12.** What does a step on `t` mean with nothing bound? The leading candidate is the whole slice
at the previous hypertime operation. The alternative is the previous version in which the accursed
cell itself changed (`Manifold::historyOf`), which skips operations that did not touch it. Both need
a manifold for another state at hand: `advance()` steps forwards only, so going back means a rebuild
or keeping recent states. Settled by: trying both on a slice with real history, and measuring the
rebuild.

**VU13.** Is the ranking per slice or across slices; do steps and bindings weigh the same; what does
the selector show before there is any history; and when should old walk summaries be folded into one
aggregate per slice so the store stops growing? Settled by: use; the defaults are per slice, equal
weight, ring order, and no folding.

**VU14.** Which keys do `u` and `t` take, and how many binding points can the keymap carry before
they stop being memorable? Settled by: the keymap's conflict report and use.

**VU15.** Is edge heat best as sectors, and is a lower bound honest enough? Settled by: use on dense
slices.

______________________________________________________________________

## 20. Traceability

### The first request

| Clause                                                                              | Met by                |
| ----------------------------------------------------------------------------------- | --------------------- |
| "highly pluggable, highly extensible View system"                                   | V-R1; §8.1, §8.10     |
| "how xanadocs (pages) and slices (cells) should be laid out in a given viewport"    | §8.4 to §8.6; §9; §10 |
| "sit on top of the store"                                                           | §6.1; V-R12; V-R37    |
| "different views for xanadocs and zigzag slices so the two can be mixed freely"     | V-R38 to V-R40; §11   |
| "each cell can only be connected on the two directions of all the bound dimensions" | V-R6; I1; §6.6        |
| "any minted cells used by the view live in the view only"                           | V-R7, V-R8; I2, I3    |
| "quickly tossed in O(1) time as dimensions are rebound"                             | V-R9; §6.5; V1        |
| stretch vanishing: "full content of all cells … always displayed"                   | V-R16; §9.1.3         |
| "drawn very closely together"                                                       | §9.1.3, `stretch.gap` |
| "only the immediate neighbors … completely aligned"                                 | V-R17; §9.1.3 step 2  |
| "sacrificing structural clarity for raw data visibility"                            | §9.1.1, §9.1.6        |
| "cells to the edges of the viewport become less opaque"                             | V-R19; §9.1.5         |
| "partially clipped cells are completely invisible"                                  | V-R18; §9.1.5         |
| all-dim walk: "surrounded by every cell they are connected to on every dimension"   | V-R21; §9.2.2         |
| "a ring that utilizes 3 dimensional space to keep the accursed cell visible"        | V-R22; §9.2.3         |
| "dimension names are used as edge labels"                                           | §9.2.5                |
| "bound dimensions' cells are aligned … like spokes on a wheel"                      | V-R22; §9.2.3         |
| "movement between them works as expected"                                           | §9.2.3, §9.2.6        |
| "one connection in each direction along the bound dimensions"                       | V-R6, V-R23           |
| "visualizing the valence of each cell"                                              | V-R26; §9.2.4, §9.2.5 |
| "rebinding dimensions by dragging an edge to the bound axis"                        | V-R25; §9.2.7         |
| pack view: "dimensions can be grouped to quickly rebind an entire set"              | V-R14; §7.2           |
| "the dimension group can also be bound to a single dimension"                       | V-R14; §7.1           |
| "each real cell is represented as a pack of every cell connected along a dimension" | V-R27; §9.3.2; V3     |
| "extending as far as there is at least one cell to pack"                            | V-R27; §9.3.2         |
| "the pack of cells maintain the movement invariant"                                 | V-R28; §9.3.5         |
| "can be retrieved individually"                                                     | V-R29; §9.3.5         |
| "nested with d.pack (the container) and d.packing (the constituents)"               | V-R30; §9.3.3; V2     |

### The second request

| Clause                                                                                      | Met by                                     |
| ------------------------------------------------------------------------------------------- | ------------------------------------------ |
| "reviewing the view spec for any inconsistencies, design defects, or inefficiencies"        | §21, the entry for this revision           |
| "include the xanadoc view system"                                                           | §8.6; §10; V12                             |
| base view: "baseline that we have today"                                                    | §10.3; V20                                 |
| "pages flow vertically and docs horizontally when no links are shared"                      | V-R32; §10.3.1                             |
| "pages with active links fly in together to align and coalesce for a best fit"              | V-R32, V-R33; §10.3.2                      |
| stacked vanishing: "staggered along the z axis at an angle that maximizes the legible text" | V-R34; §10.4.2, §10.4.3                    |
| "increase in transparency slowly into the distance … to the vanishing point"                | V-R35; §10.4.2, §10.4.4                    |
| "punctuated by fully opaque pages … non formatting links or transclusions"                  | V-R35; §10.4.4, §10.4.5                    |
| "a quick cycling of pages if the new page is near, or a split of the deck with a fly in"    | V-R36; §10.4.7; V22                        |
| "free to develop your own ideas"                                                            | §10.5; the wheel's ring order; lane tables |
| "keep labels, edges and cells in world space"                                               | V-R4; §13.1; V15                           |
| "an implementation plan for world space improvements"                                       | `world-space-rendering-plan.md`; §13.3     |
| "nothing should live in apps/xudu or apps/zigzag anymore"                                   | §1.2; §17 step 1; V17                      |
| "apps/xuzz if it is truly unique … or in apps/common"                                       | §1.2; §5; V17                              |
| "libgleditor must be kept to generic components"                                            | §1.2; §5.3 rule 4; §13.3                   |
| "its own battery of tests that I don't want duplicated in xuzz_test"                        | V-R48; §16.1                               |

### The notes of 2026-10-07

| Note                                                                                       | Met by                                         |
| ------------------------------------------------------------------------------------------ | ---------------------------------------------- |
| "pages should have their own matrices … move relative to the doc as a whole"               | V-R60; §8.4; §10.1; V30; the rendering plan §6 |
| "edges should appear as a bundle of thin strands with consistent coloring per dimension"   | V-R56; §9.3.7; V31                             |
| "drops strands as the pack cells lose constituents"                                        | §9.3.7                                         |
| "strands separate on reaching the pack and … light up or color the faces (or the edges …)" | §9.3.7                                         |
| "on mouse hover … a menu setting and a keybinding (… alternate sub views …)"               | V-R49, V-R57; §8.1; §9.3.7; V26                |
| "the strands pull apart … to give room for dimension labels that fade in"                  | V-R57; §9.3.7                                  |
| "this feature can be reused by other views when dealing with packs"                        | `pack_presentation`, §5.2, §9.3.7              |
| "packs … visually glued together components with clear demarcations"                       | V-R55; §9.3.7; V30                             |
| "binding points are limited only by our available key binds … much larger than 3"          | V-R13, V-R50; §7.3; V27                        |
| "a U binding … separate 3d spaces or sub clusters, and T could walk hypertime"             | §7.3; VU11, VU12                               |
| "the clipped cell at the edges could be minimally drawn … heat map … empty ghost cell"     | V-R18; §9.1.5; V32                             |
| "the neighbors (up to a runtime configurable N steps away …) can display their own wheels" | V-R58; §9.2.8                                  |
| "the wheel bends and flexes in 3d space to not occlude any real cell"                      | V-R58; §9.2.8; V33                             |
| "the wheel can simplify to a summary and or just the valence count … names … first"        | V-R59; §9.2.9                                  |
| "dragging edges … to an axis is one way. A 3d visual dimension selector is another"        | V-R51; §7.6, §7.7                              |
| "tier 1 … dimension groups that radial out their component dimensions"                     | V-R52; §7.7                                    |
| "second tier (… z movement keys) … most used … most likely (… Markov …) … pouch"           | V-R52, V-R53; §7.7, §7.8; V29                  |
| "tier 3 is a stretch vanishing view of dimension cells"                                    | V-R52; §7.7                                    |
| "quick launch keys like 0-9 or traversal … then pick the binding axis by name"             | V-R52; §7.7; §12.1                             |
| "click and dragged to a rose compass in the top left corner"                               | V-R54; §7.9                                    |
| "there can be overlap … to highlight the linked passages in a many endset"                 | V-R32; §10.3.2; V36                            |
| "sentinels are to be eliminated outside of the wire/on-disk"                               | §8, conventions; V34                           |
| "use the c++26 features … in particular function_ref and inplace vector"                   | §8, conventions; §7.7, §7.8, §8.2; V34         |
| "void returns on setters should return the object pointer"                                 | §8, conventions; V34                           |
| "condensed into a smaller number of 'zigzag activity' records"                             | V-R53; §7.8; V35                               |
| "U … zooming into an inset 3d space … a band of the next U on the periphery"               | §7.3; VU11                                     |
| "T … the state of the slice as a whole at the previous hypertime op"                       | §7.3; VU12                                     |

______________________________________________________________________

## 21. Change history

- 2026-10-07 — Initial proposal: the framework, the view space and three slice views.
- 2026-10-07 — Rehomed onto xuzz as the only application.
- 2026-10-07 — Bindings and memberships as occurrence cells, so a dimension may be on several axes.
- 2026-10-07 — Reconciled with the fitted UI layer.
- 2026-10-07 — Rewritten after a full review, and extended with the page views. Corrected:
  - the toss kept garbage and filtered reads; it is now `release()` to an empty mark, constant-time
    with immediate reclamation, because no view arena shadows a real cell (§6.2, §6.5; V1, V6);
  - occurrences linked to their targets, which shadowed them; they now hold handles (V13);
  - `layout()` was called pure but minted cells; preparing and laying out are separate (V16);
  - a pack step was a breadth-first frontier that mixed dimensions and grew exponentially; it is now
    one lane per dimension (V3);
  - the wheel's formula put both neighbours of a dimension on the same side of a cone, gave every
    dimension its own ever-larger ring, and followed the camera; it is now rings of pair slots about
    the hub, fixed in the placement (V23);
  - walking an unbound dimension changed an axis; it no longer does (V10);
  - the stretch fade's band was outside the range of its variable, and the packing rule described a
    skyline for a figure that radiates; both are restated (§9.1.3, §9.1.5);
  - the link choke point checked one end; the verifier counted something the representation cannot
    hold; both are restated (§6.6);
  - the registry was a singleton holding non-owning callables; it is an owned object (§8.1);
  - labels were in screen space; everything is in world space (V15);
  - view code was planned for `apps/xuzz/`, and view tests re-swept the library; placement and
    testing follow §1.2 (V17).
- 2026-10-07 — First batch of notes folded in: pages have their own matrices and everything in a
  frame is placed relative to it (V30); packs are glued, joined by strand bundles, and open into a
  spread (V31, §9.3.7); views have sub-views (V26); axes are named binding points with roles, `u`
  for subspaces and `t` for hypertime (V27, §7.3); a three-tier dimension selector, a ranking from
  activity and a compass (V28, V29, §7.6 to §7.9); cut cells are ghosts with edge heat (V32);
  neighbours show their own wheels, which bend, flex and simplify names first (V33, §9.2.8, §9.2.9).
- 2026-10-07 — Second batch of notes: base-view pages may overlap and be windowed round their
  passages (V36); no sentinels, C++26 facilities and chaining setters throughout the API (V34);
  movement recorded as condensed walk summaries (V35, §7.8); `u` zooms into an inset space with the
  next one at the periphery, and `t` with nothing bound leans to the whole slice at the previous
  operation (§7.3, VU12).
- 2026-10-07 — Step 1's destinations corrected: nothing from `apps/xudu/` or `apps/zigzag/` can go
  to the engine. An implementation plan now orders the work and lists the amendments still to make.
- 2026-10-08 — §16.1: the third test binary is `ui_test` in `tests/ui/` (VU6), renamed by A2.
- 2026-10-08 — Plan G8 absorbed: messages have their own key enumeration, `ViewMessage`, and
  `messageKey()` maps a `ViewError` into it; the refusals §12.2 left unworded have words (§8.11,
  §12.2).
- 2026-10-08 — Plan G13 absorbed: `PlacedEdge` carries a dash class. `SubjectId` factories,
  `placedPose()` and `view_ids.hpp` added to §8.4 and §5.2 with the records (E1).
- 2026-10-08 — §8.1 as built (E3): the registry is seeded with the default keymap, a collision is
  same scope and same canonical chord, `chordHolder()` names the holder, settings are
  `xanadu::SettingSpec`, and `registerBuiltinViews()` is declared with the built-in views.
- 2026-10-08 — §8.7's conventions as built (E6).
