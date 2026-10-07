# View System: Implementation Plan

Status: plan, unbuilt. Date: 2026-10-07.

This plan turns [`view-system.md`](view-system.md) (the spec) and
[`world-space-rendering-plan.md`](world-space-rendering-plan.md) (the rendering plan) into ordered
work. It was drafted after four expert reviews against the code — engine, rendering, build and
visual design — and then attacked by three challengers: one for correctness and sequencing, one for
scope and delivery, one for the experience. §9 records what they said and what changed. The seven
reports are kept in [`projects/view-reviews/`](projects/view-reviews/), and
[`projects/start-view-project.md`](projects/start-view-project.md) is the prompt that starts the
work.

## 1. How to use this plan

- §2 lists what the reviews found wrong or missing in the two documents. F1 is corrected in the spec
  already, because building from the wrong table would break the build. Every other item is carried
  by the package named beside it, which amends the spec or the rendering plan in the same commit.
- §3 is the spikes. They are small, they come first, and each one gates a package.
- §4 is the work, in tracks that can run in parallel, with the milestones at which something new can
  be seen and used.
- §5 is the visual and experiential direction, with checks a reviewer applies to captured frames.
- §6 is the gate each commit passes. §7 is the risk register. §8 is what the owner must decide.

Every package keeps `make test` green and is one commit unless it says otherwise. Sizes are of
implementation plus tests: **S** under 300 lines, **M** under 1,000, **L** more, **XL** several
commits. The whole is about 27,000 new lines against 8,000 to 9,000 deleted and 24,500 moved. By
this repository's own cadence — the UI text-fit series was about 19,600 lines in nine batches — that
is 60 to 120 commits.

## 2. What the reviews changed

### 2.1 Corrections of fact

| #   | Finding                                                                                                                                                                                                                                                                                                   | Evidence                                                                             | Consequence                                                                                                                                                                                                                                                                  |
| --- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F1  | Two files the spec sent to the engine cannot go there. `unified_transclusion_engine` includes `gleditor/doc.hpp`, the glyph cache and stream buffers and stages GPU buffers; `link_context` depends on `Session`.                                                                                         | `apps/zigzag/unified_transclusion_engine.hpp:28-30`; `apps/xudu/link_context.hpp:31` | Everything in `apps/xudu/` goes to `apps/common/ui/xanadoc/` and everything in `apps/zigzag/` to `apps/common/ui/slice/`, with no exceptions. Putting either in the engine would break the link of ten targets that do not link the library. The spec's step 1 is corrected. |
| F2  | A page's matrix is not safe to set today. Reflow rewrites every page's matrix from the column formula and reads the previous page's *live* matrix to place the next. A posed page would be snapped back, or would corrupt the column below it, at the next edit. `Page::setModel` is also already public. | `src/doc.cpp:1241`, `:1273`, `:1698-1710`; `include/gleditor/drawable.hpp`           | The page-pose package (L7) must first split a page's *flow* matrix, which only reflow writes and reads, from its *pose*, which overrides it for drawing, and make the pose the single way to move a page.                                                                    |
| F3  | A beam whose run is parallel to world Z has no width: the shader takes its sideways vector from a cross product with Z. The views' own depth axis produces exactly that edge.                                                                                                                             | `assets/shaders/beam.vert.glsl:56-59`                                                | A shader fix is needed after all (L9): take the sideways vector from the view direction. The spec's "no new shader" becomes "one corrected shader, no new vertex format".                                                                                                    |
| F4  | A soft band cannot be drawn by the solid-fill path, which writes one alpha for the whole quad.                                                                                                                                                                                                            | `assets/shaders/glyph.frag.glsl:92-93`                                               | `addBand` is drawn as a textured quad sampling a gradient baked once into the glyph atlas. No shader change.                                                                                                                                                                 |
| F5  | `faceCamera` needs the camera's rotation, which cannot be recovered from the combined world-to-clip matrix.                                                                                                                                                                                               | rendering plan §5.2                                                                  | `PlaneSet::draw` also takes the camera's view matrix.                                                                                                                                                                                                                        |
| F6  | The OpenGL entry-point table has none of `Scissor`, `DepthMask`, `DepthRangef`.                                                                                                                                                                                                                           | `src/render/gl/gl_api.cpp`                                                           | L3 to L5 add them; `DepthRangef` resolves optionally with `DepthRange` as the fallback.                                                                                                                                                                                      |
| F7  | Only Vulkan records batches on worker threads; OpenGL and GLES draw in order, and re-issue every vertex attribute pointer on every draw.                                                                                                                                                                  | `include/gleditor/render/device.hpp:190-195`; `src/render/gl/device_gl.cpp:770-797`  | One draw per plane may cost too much on OpenGL long before 10,000 planes. Spike R1 decides how `PlaneSet` batches before any of it is written.                                                                                                                               |
| F8  | Picking scopes are a global budget of about 8,000 shared with every widget.                                                                                                                                                                                                                               | `include/gleditor/render_state.hpp:61-74`                                            | One persistent scope per `PlaneSet`, with planes told apart by the cluster field, never one per placement or per pack.                                                                                                                                                       |
| F9  | Link occurrences are byte extents. Nothing maps an extent to a page and a height.                                                                                                                                                                                                                         | `apps/common/xanadu/link_occurrences.hpp`                                            | The presenter's `PageCatalog` implementation owns that mapping (U3); it is new work, not a lookup.                                                                                                                                                                           |
| F10 | `ActivityLog` is a closed interface round one payload, `Visit`.                                                                                                                                                                                                                                           | `apps/common/xanadu/link_navigation.hpp:88`                                          | Walk summaries need a second kind of record. E14 adds one to the activity store in the simplest shape that fits; it can be refined when that store is designed.                                                                                                              |
| F11 | `Manifold` has no public way to mint cells.                                                                                                                                                                                                                                                               | —                                                                                    | Test fixtures are built through a small `Store` and `rebuildManifold()`, as `tests/xuzz/two_by_three_fixture.hpp` does.                                                                                                                                                      |
| F12 | The WebAssembly packaging script still assumes separate `xudu` and `zigzag` outputs, and a CI job checks for files it has not produced since the fold.                                                                                                                                                    | `packaging/wasm/build.sh:68-75`; `.github/workflows/packaging.yml:717-718`           | Fixed in the relocation commit, and called out there so the existing breakage is not blamed on the move.                                                                                                                                                                     |

### 2.2 Gaps the spec must close before the package that needs them

| #   | Gap                                                                                                                          | Resolution, and the package that carries it                                                                                                                                                            |
| --- | ---------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| G1  | `ViewManifold::link` takes a `Layer` and each `ViewCellRef` also carries one; the two arenas' refs can be numerically equal. | The explicit layer says which arena is written; every ref's own layer must equal it or the call is refused (`RealCellInViewLink`). Spike S4, then E4.                                                  |
| G2  | No type for a binding point's role.                                                                                          | The first implementation has three roles in host code — spatial, subspace, hypertime — behind one small `AxisRole` interface declared in E5; the plug-in registry waits for a fourth role to exist.    |
| G3  | `verifyViewSpace` needs each arena's shadow, trail and space counts.                                                         | `ViewManifold` exposes `shadowCount(Layer)`, `trailSize(Layer)`, `spaceCount(Layer)`. E4.                                                                                                              |
| G4  | `promotePack` returns a `Store *` and drops what `promote()` returns.                                                        | It returns `std::expected<zigzag::Promoted, ViewError>`. E11.                                                                                                                                          |
| G5  | Stretch placement and the lane table are written for two in-plane directions.                                                | Each uses the first two spatial points in binding-point order in the plane and the third in depth; further spatial points are on the compass and move the cursor but have no direction there. E9, E11. |
| G6  | What feeds the walk recorder.                                                                                                | Only `AlongAxis` and `AlongSpoke` steps that resolve to a real dimension. Entering, leaving and lane moves are not movement through the slice. E14.                                                    |
| G7  | "Focusing either highlights both" has no stated key.                                                                         | Occurrences are matched by `resolveReal()` over the visible window; spike S5 says whether that needs an index. E11.                                                                                    |
| G8  | Several messages have no `ViewError`.                                                                                        | Messages get their own key enumeration; `ViewError` maps into it. E2.                                                                                                                                  |
| G9  | Replaying saved bindings: nested groups, order, and what is reported.                                                        | Groups are replayed leaves first; one summary message, plus one per unresolved name. E5.                                                                                                               |
| G10 | A dimension's colour is "from a palette".                                                                                    | The rule of §5.2. U1.                                                                                                                                                                                  |
| G11 | Ring order begins by order of first meeting, so it depends on where the reader went first.                                   | It begins as the slice's own dimension order (its `d.dims` rank) and is the reader's from then on. E10.                                                                                                |
| G12 | The wheel's flex clears real cells but not labels, so labels can still collide.                                              | Labels are obstacles to labels: a label that would overlap one already placed slides along its edge, then drops a level of detail. E10, with a test at valence 60.                                     |
| G13 | A second cue beside colour needs a field.                                                                                    | `PlacedEdge` gains a dash class; the presenter maps it. E1.                                                                                                                                            |
| G14 | No chord reorders the ring or edits a group's members.                                                                       | `ring.move(±1)` on the selected spoke, and insert, remove and move inside the group editor's scope. Each ships with its feature (E10, U5).                                                             |
| G15 | The selector has no accessibility contract.                                                                                  | Tiers are groups of list items in ring order; arming is announced; naming a point announces the binding. Written into the spec with E13.                                                               |
| G16 | The motion timings of §5.3 that are new are not settings.                                                                    | Each becomes a `motion.*` setting owned by U6; none is a literal.                                                                                                                                      |

### 2.3 Design changes adopted from the visual review

| #   | Change                                                                                                                                                                    | Why                                                                                    |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| D1  | A ghost's opacity is a fraction of the fade floor (`stretch.ghostShare`, 0.6), not a number of its own.                                                                   | With 0.25 against a floor of 0.15 a ghost outshone the dimmest real cell.              |
| D2  | Fading a box and keeping its text readable are separated: when a cell's opacity would take its text below contrast, its content drops to `Abbreviated` and then `Coarse`. | The stated floor gives about 1.7:1 against the theme, far under what text needs.       |
| D3  | A marked page stays opaque wherever it stands, but its link-type edge tab shrinks and desaturates with distance along the same curve as the fade.                         | A document with links on most pages would otherwise be a wall, with no recession left. |
| D4  | Groups have no colour of their own; a group shows a swatch of its members' colours.                                                                                       | A colour means one dimension, everywhere.                                              |

## 3. Spikes

Throwaway code, headless, a day or less each. They are milestone 1: all of them run after the
relocation and before any other package, because several can change the shape of what follows.

| #   | Question                                                                                    | Method                                                                                                                        | Pass                                                                                                     | Gates                  |
| --- | ------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- | ---------------------- |
| S1  | Is `release()` on a view-shaped arena constant-time once the guard is in?                   | Mint 0, 10³ and 10⁶ cells into a store-less arena with no spaces; time `mark`/`release`; count name lookups.                  | Flat time; zero lookups.                                                                                 | E0, E4                 |
| S2  | Does a fresh engine stepped 25 times reproduce what `LinkBeams` does today?                 | Replay the scenarios of `tests/xudu/tension_layout_test.cpp` and the two-, three- and many-document cases; compare positions. | Within `page.base.levelTolerance`.                                                                       | P2                     |
| S3  | What does `rebuildManifold()` cost for a state a few operations back?                       | Time it on the KJV fixture and a multimedia sample.                                                                           | A number that settles spec VU12.                                                                         | the `t` role (E16)     |
| S4  | Can equal-numbered refs of the two arenas ever be confused?                                 | First test of E4: mint the first cell in each arena and drive every `ViewManifold` call with both.                            | Never a wrong read; a mismatched layer refused.                                                          | E4                     |
| S5  | Is a scan of the visible window enough to match occurrences of one cell?                    | 100 dimensions, a rank several hundred packs long; scan against a small map.                                                  | A number; pick the simpler if it is under a tenth of a millisecond.                                      | E11                    |
| R1  | What does a draw per plane cost?                                                            | 100 to 50,000 synthetic batches on OpenGL (llvmpipe) and Vulkan (lavapipe).                                                   | A curve. Under 2 ms at the plane counts the views produce (§4.6), or `PlaneSet` batches coplanar planes. | L6                     |
| R2  | Does a gradient baked into the atlas give a soft band with no shader change?                | One textured quad.                                                                                                            | It fades; captures match across backends.                                                                | L6                     |
| R3  | Does a beam along Z vanish, and does the fix hold at every angle?                           | One beam; orbit the camera.                                                                                                   | Visible at every angle after the fix.                                                                    | L9                     |
| R4  | Does a page keep a matrix set from outside through a reflow?                                | Set one; edit the document.                                                                                                   | Establishes F2 as a test before L7 changes anything.                                                     | L7                     |
| R5  | Are translucent planes, pages and beams blended in the right order where all three overlap? | One scene with a faded plane, a faded page and a beam crossing both; capture from both sides.                                 | Right from both sides, or the single sorted list of §4.3 is designed before L6 and L7.                   | L6, L7                 |
| V1  | Does the colour rule give distinguishable colours?                                          | Generate 40 swatches by the rule of §5.2; view the strip, and through a colour-blindness simulation.                          | No two neighbours confusable; none in a reserved band.                                                   | U1, and every view     |
| V2  | Do the wheel's formulas look like a wheel?                                                  | A throwaway scene at valence 2, 8, 20 and 60; tune radius, step, tilt and bend by eye.                                        | A reviewer agrees it is a wheel, not a cone or a flower.                                                 | E10's acceptance tests |
| V3  | Does the stagger search choose well?                                                        | Run it on a tall narrow page and a wide one; capture both.                                                                    | Neither reads sideways.                                                                                  | P4's defaults          |

## 4. The work

### 4.1 Tracks and milestones

```text
 A  relocation ......... A1
 L  library ............ L1 L2 L5 L8 | L9 | L10 | L6 | L7 | L3+L4
 E  engine, slices ..... E0 E1 E2 E3 E4 E5 E6 E8 | E9 | E7 E11 | E10 E12 | E13 E14 | E16
 P  engine, pages ...... P1 P2 P3 | P4 P5
 U  presentation ....... U0 U1 U2 U5a U6a U7a | U4a U5b | U3 U6b | U5c | U4b
 X  cut-over ........... X0 | X1 X2 | X3 | X4
```

| #   | Milestone                 | Packages                                    | What a reader gets                                                                                                                                                                                                 | Risk it retires                                                 |
| --- | ------------------------- | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------- |
| M0  | The tree in its new shape | A1, A2                                      | nothing                                                                                                                                                                                                            | the build, tested in isolation before anything is stacked on it |
| M1  | Spikes                    | §3                                          | nothing                                                                                                                                                                                                            | the assumptions most likely to be wrong                         |
| M2  | The spine                 | E0 to E6, E8, E9, U0, U1, U2, U5a, U6a, U7a | A second slice presentation, chosen from the palette beside today's: stretch vanishing with ghosts. A compass that shows what each point is bound to. Binding by a typed command. Sub-views listed in the palette. | every layer of the design, end to end, on primitives that exist |
| M3  | Packs                     | E7, E11, U7b                                | Groups made and bound by command; the lane table, strands and the spread.                                                                                                                                          | pack semantics; the derived arena under real use                |
| M4  | The wheel                 | L1, L5, L6, L9, L10, E10, E12, U4a, U5b     | All-dim walk in three dimensions; drag an edge to an axis or to the compass. The first thing a reader notices unprompted.                                                                                          | placed planes and their cost; the look of the wheel             |
| M5  | Pages                     | L7, P1 to P3, U3, U6b                       | The base view as a view: first at parity with today, then page by page with ghosts and tethers.                                                                                                                    | page poses against reflow; parity                               |
| M6  | Choosing dimensions       | E13, E14, U5c                               | The selector's second and third tiers and "most used"; binding in three keys.                                                                                                                                      | whether the binding model is usable past three points           |
| M7  | Decks and many ends       | P4, P5, windowed pages in P3                | The stacked vanishing view; links with many ends shown as windows.                                                                                                                                                 | translucency; the stagger search                                |
| M8  | Panes and scenes          | L3, L4, L8, U4b                             | Several panes; pages and cells in one scene through the host.                                                                                                                                                      | regions on three backends; rewiring the application             |
| M9  | Cut-over                  | X0 to X4                                    | The legacy presentations gone.                                                                                                                                                                                     | —                                                               |

**The first release is M0 to M6.** Everything after it is separable.

**The spine does not wait for the library.** Stretch vanishing and the pack view are flat: every
cell lies in one plane per depth layer. The presenter draws a flat placement with the canvases and
beams that exist today — the visualizer already uses a world canvas, a canvas for secondary text and
a screen canvas for chrome, and the presenter keeps that split. So M2 and M3 prove the whole path —
registry, view space, bindings, prepare, layout, records, presenter, picking, accessibility — while
the library track proceeds. `PlaneSet` is first needed by the wheel, page poses by the page views,
regions by panes.

**Nothing is present without a way in.** The compass is displayed from M2, so what is bound is
always visible; binding is a typed command from M2, a drag from M4 and the selector from M6; the
palette lists views and their sub-views from M2. The `u` and `t` points are not configured, and have
no keys, until the package that gives them meaning (E16) lands.

**Deferred beyond the first release**, each on its own, none blocking another:

| Deferred                                                                     | Until                                  | Cost of waiting                                                |
| ---------------------------------------------------------------------------- | -------------------------------------- | -------------------------------------------------------------- |
| edge heat                                                                    | after M2                               | ghosts alone say "more this way" without saying how much       |
| neighbours' wheels beyond depth 0, and the lower levels of the detail ladder | after M4                               | the wheel shows one cell's connections only                    |
| nested packs; keeping a pack                                                 | after M3                               | a group cannot contain a group in a pack; packs cannot be kept |
| "most likely" (the Markov order)                                             | after M6                               | the selector ranks by use and by ring order only               |
| editing a group's members inside the selector's first tier                   | after M6                               | groups are edited by command and in the group editor           |
| the `u` and `t` roles                                                        | E16, after the first release (decided) | five points become three until then                            |
| the stagger search                                                           | M7 ships a fixed direction first       | a deck may peek the wrong way in an odd pane                   |
| embedding; other deck sources                                                | after M8                               | —                                                              |

The two things a reviewer proposed cutting — edge heat and the Markov order — are deferred, not cut:
both were asked for.

### 4.2 Track A: relocation

| #   | Package                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | Files        | Tests and gate                                                                                                                                                                                                                                                                                                | Size |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---- |
| A1  | Empty `apps/xudu/` and `apps/zigzag/`. `git mv` all of `apps/xudu/` to `apps/common/ui/xanadoc/` and all of `apps/zigzag/` to `apps/common/ui/slice/`. Rewrite the `"xudu/…"` and `"zigzag/…"` include spellings (about twenty files). Delete `XUDU_SRCS`, `ZIGZAG_SRCS` and their object lists; fix `ALL_OBJS`, the `xuzz` link line and the `zigzag_test` link line (`Makefile:606-607`, `:623-624`, `:702-706`, `:841-843`, `:902-903`). Fix `packaging/wasm/build.sh` and the stale check in `packaging.yml` (F12). Update the paths cited in `AGENTS.md`, the README, the skills and `tools/code-quality-audit.py`. | moves only   | Full gate (§6). `find apps/xudu apps/zigzag -type f` prints nothing: the `find`-based source lists would silently go on compiling a file left behind. The test binary that linked the slice components now also links the xanadoc ones; confirm its libraries. Format with clang-format 19, not the system's. | M    |
| A2  | Rename `zigzag_test` to `ui_test` and `tests/zigzag/` to `tests/ui/`: it is the binary that links the library and all of `apps/common/ui/`. Makefile, CI, `AGENTS.md`, the README.                                                                                                                                                                                                                                                                                                                                                                                                                                       | renames only | Full gate; the same tests pass under the new name.                                                                                                                                                                                                                                                            | S    |

A1 is one commit: it is behaviour-preserving, and half a move is worse than none. A2 is a second.

### 4.3 Track L: the library

Generic; every test in `tests/lib/`; nothing here names a cell or a view. Details and signatures are
in the rendering plan, as amended by §2.

| #   | Package                                                                                                                                                                                                                                                                                                                       | Depends            | Tests                                                                                                                                                        | Size |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---- |
| L1  | `projectToViewport`, `unprojectToRay`, plane, quad and segment tests, `windowToPixels` in `spatial.hpp`                                                                                                                                                                                                                       | —                  | `spatial_unproject_test.cpp`: round trip over poses, fields of view and aspect ratios; sub-viewports; content scales                                         | S    |
| L2  | `insideFrustum` in `draw_budget.hpp`                                                                                                                                                                                                                                                                                          | —                  | cases in `draw_budget.cpp` for each of six planes                                                                                                            | S    |
| L3  | Render regions with scissor, on OpenGL, GLES and Vulkan in one commit; the new loader entries (F6)                                                                                                                                                                                                                            | —                  | `render_region_test.cpp` on mocks; scene in `compare-backends.sh`                                                                                            | M    |
| L4  | Depth slices in regions, all three backends                                                                                                                                                                                                                                                                                   | L3                 | the far-glyph-in-a-thin-slice scene; the embedded-in-front scene                                                                                             | S    |
| L5  | `PipelineDesc::depthWrite`                                                                                                                                                                                                                                                                                                    | —                  | blend-order cases                                                                                                                                            | S    |
| L9  | Beam width from the view direction (F3)                                                                                                                                                                                                                                                                                       | R3                 | a beam along each world axis is visible from four camera angles; existing beam captures unchanged                                                            | S    |
| L10 | `tools/world-scene-probe`: a generic headless tool that builds a scene of regions, planes and beams from a small description, captures it and reports frame time, draws and uploads. `compare-backends.sh` drives it. It gains regions when L3 lands.                                                                         | —                  | used by L3, L4, L6, L9                                                                                                                                       | M    |
| L6  | `ui::PlaneSet`: retained planes, parents, per-plane state as uniforms, legibility per plane, one persistent pick scope (F8), soft bands from the atlas (F4), camera view matrix for `faceCamera` (F5), batching as R1 decides (F7)                                                                                            | L1, L3, L5, R1, R2 | `plane_set_test.cpp`: one range uploaded per repaint; nothing uploaded or shaped on motion; order; picks; parents                                            | L    |
| L7  | Page poses and bands. First split flow from pose (F2) with no behaviour change, and close `Page::setModel` to everything but the document's own flow, so a pose is the only way to move a page; then `setPose`, `animatePoseTo`, `clearPose`, bands with their own strip of paper; then depth-sorted translucent page batches | L5, R4             | `doc_page_pose_test.cpp`: a posed page survives reflow; the column below it is unaffected; caret and pick follow the pose; existing document tests unchanged | L    |
| L8  | `ui::PaneTree`                                                                                                                                                                                                                                                                                                                | —                  | `ui_pane_tree_test.cpp`                                                                                                                                      | S    |

Translucent draws of different kinds are not sorted against each other today (pages, planes and
beams each sort their own). L6 and L7 hand their translucent batches to one list the renderer sorts
by depth, since pages and planes are both glyph batches; beams are drawn after, depth-tested and not
written. A scene that forces the three to overlap is part of L7's gate, because the fault is
invisible in every scene that does not.

### 4.4 Track E: the engine, slices

`apps/common/xanadu/view/`; tests in `tests/xuzz/`, linking the engine only.

| #   | Package                                                                                                                                                                                                          | Depends    | Tests                                                                        | Size |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------- | ---------------------------------------------------------------------------- | ---- |
| E0  | `ArenaManifold`: the `release()` guard and `shadowCount()`                                                                                                                                                       | S1         | in `tests/xudu/arena_manifold_test.cpp`; federation tests unchanged          | S    |
| E1  | `view_records.hpp`: `SubjectId`, records with the dash class (G13), `LayoutSink`, `PaneFrame`, `Measure`                                                                                                         | —          | sink grows and logs, never drops; identity and epoch                         | M    |
| E2  | `view_error.hpp` and the message keys (G8)                                                                                                                                                                       | —          | every enumerator has a key                                                   | S    |
| E3  | `view.hpp`: descriptor with sub-views, registry                                                                                                                                                                  | E2         | duplicate kind and chord collision refused                                   | S    |
| E4  | `view_manifold`: two arenas, mint, occurrence, `link` with the layer rule (G1), toss, epoch, counts (G3), `verifyViewSpace`                                                                                      | E0, S4     | I1 to I6; random sequences then verify; the toss test                        | L    |
| E5  | `view_binding`: binding points and `AxisRole` (G2), occurrences, groups, ring order seeded from `d.dims` (G11), pouch, undo, replay by name (G9)                                                                 | E4         | doubled bindings; nested groups; cycles; undo; replay with a missing name    | L    |
| E6  | `raster`                                                                                                                                                                                                         | E1         | fixed records give a fixed grid                                              | S    |
| E7  | `pack_rank` and `pack_presentation`: lanes, steps, glue, seams, strands, spread, added to the shared chrome helper                                                                                               | E4         | both worked examples; strand per shared lane; spread emits lane labels       | L    |
| E8  | `slice_view.hpp`: cursor, inputs, `move`, `cellAt`                                                                                                                                                               | E5         | default movement; cursor survives a toss                                     | S    |
| E9  | Stretch vanishing, with ghosts and the two-axis rule (G5, D1, D2); the shared chrome helper for ghosts, view-only marks and the focus mark, as its own file from the first view; edge heat follows as a sub-view | E8         | §9.1.8 as a test file; goldens as rasters                                    | L    |
| E10 | All-dim walk: wheel, slots, labels that avoid labels (G12), ring reordering (G14); first at depth 0 and the top three levels of detail                                                                           | E8, V2     | §9.2.11; no label overlaps at valence 60                                     | L    |
| E11 | Dimensional pack view, `promotePack` (G4), occurrence highlighting (G7)                                                                                                                                          | E7, E8, S5 | §9.3.11                                                                      | L    |
| E12 | `view_gesture`: drag to rebind and to reorder the ring, as a state machine                                                                                                                                       | E10        | every transition; cancel changes nothing                                     | S    |
| E13 | `selector`: tiers, cursor, layout                                                                                                                                                                                | E5, E9     | each tier; arming and naming a point binds                                   | M    |
| E14 | `dimension_ranking`: `WalkRecorder`, summaries kept in the activity store, ranking (G6)                                                                                                                          | —          | decay; Markov order; slips dropped; determinism; summaries survive a restart | M    |
| E15 | `builtin_views`: one registration call. Each view's descriptor, with its own settings and chords, ships in that view's package, so a view is reachable the day it lands.                                         | E3         | the keymap conflict test passes as each view is added                        | S    |
| E16 | The `u` and `t` roles' cursor rules                                                                                                                                                                              | E5, S3     | step in and out of a subspace; a step on `t` against two built states        | M    |

E9 and E10 are independent once E8 lands. E14 depends on nothing and can be done at any time.

About a hundred settings come with these views. They are added in the package of the view that reads
them, not in one sweep, through a small table helper so each is one line: the existing pattern is a
hand-written case per setting. Adding a setting does not touch the fixtures under `tests/samples/`,
which never include the system xanadocs.
`SystemDocsTest.DefaultKeymapGivesEachChordOneActionPerScope` is the conflict gate for every new
chord.

### 4.5 Track P: the engine, pages

| #   | Package                                                                                                                                                                                                   | Depends | Tests                                                                  | Size |
| --- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------- | ---------------------------------------------------------------------- | ---- |
| P1  | `page_view.hpp`: `PageRef`, `PageCatalog`, `LinkEnd`, cursor, inputs                                                                                                                                      | E1, E3  | a hand-written catalog                                                 | S    |
| P2  | `coalesce`: strategy and the tension strategy, cold start. Fixed steps if S2 passes; if not, stepped until velocities fall under a threshold or a cap of steps is reached, which is still a pure function | P1, S2  | pure; level within tolerance; parity cases from S2                     | M    |
| P3  | Base view: rest, coalescing, ghosts, tethers, windows, motion hints                                                                                                                                       | P2      | §10.3.4; first with one body per document for parity, then per page    | L    |
| P4  | `deck`: stagger search, opacity, tab fade (D3), riffle or split                                                                                                                                           | P1, V3  | the score is maximised; hysteresis; the rule is exactly the inequality | M    |
| P5  | Stacked vanishing view                                                                                                                                                                                    | P4      | §10.4.10                                                               | M    |

This track shares no file with track E after E1 and E3 and can be a second stream of work from the
start.

### 4.6 Track U: presentation and host

`apps/common/ui/view/`; tests in the binary that links the library and `apps/common/ui/`.

The application is wired to the two presentations directly: `apps/xuzz/xuzz_app.cpp` calls into the
visualizer in about 55 places and into `Views` in about 116, and each is a frame contributor, a pick
observer, an accessibility source and a focus scope with no seam between those roles. Wrapping them
inside a new host in one step would be the largest and riskiest package of the plan. So the host
arrives last (U4b), and until then the new presentation stands *beside* the old behind the seam the
application already uses.

| #   | Package                                                                                                                                                                                                                                                                                                                                                                                                                                | Depends             | Tests                                                                                                                                      | Size |
| --- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ | ---- |
| U0  | Extract the seam. `SlicePresentation`: the calls `xuzz_app` and `ViewCoordinator` make on the visualizer, as an interface the visualizer implements. No behaviour change.                                                                                                                                                                                                                                                              | A1                  | every existing test unchanged                                                                                                              | M    |
| U1  | Dimension colour: the generation rule, the dash and glyph cue, reader overrides in `system://ui` (§5.2)                                                                                                                                                                                                                                                                                                                                | V1                  | no colour in a reserved band; stable when a dimension is added; a cue for every colour                                                     | S    |
| U2  | The new slice presentation: a second implementation of `SlicePresentation` over the view framework, with one placement. Presenter for flat placements on the existing canvases and beams; the `text::fit` measurer; picks; accessibility; retained by stamp. `ViewCoordinator` shows the legacy or the new one by `layout.slice.<id>.presentation`; the legacy one is the default. An action the new one does not yet support says so. | U0, U1, E9          | a settled frame shapes and uploads nothing; a shortened label keeps its name; commands run on the owner thread; the legacy tests unchanged | L    |
| U5a | Compass, display only; view palette listing views and sub-views                                                                                                                                                                                                                                                                                                                                                                        | U2                  | every configured point is shown with what it is bound to                                                                                   | S    |
| U6a | Animation by `SubjectId`: tweens, the toss fade from a snapshot, the key-repeat rule (§5.3), the `motion.*` settings (G16)                                                                                                                                                                                                                                                                                                             | U2                  | a tossed cell fades without being looked up; a held key never queues; reduced motion cuts                                                  | M    |
| U7a | Commands for movement, view and sub-view switching and typed binding, each with its chord                                                                                                                                                                                                                                                                                                                                              | U2                  | every chord dispatches; nothing is handled in code                                                                                         | S    |
| U7b | Commands for each later feature, in that feature's package                                                                                                                                                                                                                                                                                                                                                                             | —                   | the same                                                                                                                                   | —    |
| U4a | Presenter over `PlaneSet`, for a placement in three dimensions, still in one pane                                                                                                                                                                                                                                                                                                                                                      | L6, U2              | zero uploads on motion; picks on planes; labels hidden by legibility keep their names                                                      | M    |
| U5b | Compass as a drop target; the group editor; the wheel's side list                                                                                                                                                                                                                                                                                                                                                                      | U5a, E12            | drops bind; focus order                                                                                                                    | M    |
| U3  | The page presentation: the seam for the arrangement that `LinkBeams` does today; `PageCatalog` over the library's documents with the extent-to-page mapping (F9); pages through poses                                                                                                                                                                                                                                                  | L7, P3              | passages map to the right page and height across a reflow; with the base view off, today's behaviour is unchanged                          | L    |
| U6b | Motion hints: sworph, riffle, split                                                                                                                                                                                                                                                                                                                                                                                                    | U6a, P3             | hint delays and durations are honoured                                                                                                     | S    |
| U5c | The selector's input scope and its accessibility (G15)                                                                                                                                                                                                                                                                                                                                                                                 | E13, U5a            | the selector binds in three keys; a screen reader hears the tiers                                                                          | M    |
| U4b | The host proper: scenes, panes over regions and `PaneTree`, edges between placements, embedding. `xuzz_app` is rewired to the host and `ViewCoordinator` goes.                                                                                                                                                                                                                                                                         | L3, L4, L8, U4a, U3 | split, close, focus; a link across placements is one edge; a stub across scenes; every mode of today's `--view` reproduced                 | XL   |

Plane counts that R1 must hold for: a stretch pane, about 300 cells in a handful of planes; a wheel
at depth 2 with valence 20, about 1,300 planes; a deck, 30 to 60 pages; a selector's third tier, a
few hundred.

### 4.7 Track X: cut-over

| #   | Package                                                                                                                                                                                   | Depends    | Tests                                                                             | Size |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------- | --------------------------------------------------------------------------------- | ---- |
| X0  | Carve out of the visualizer what is not layout — the command bar, the palette, the Vortex and VQL palettes, the cell editor — into components both presentations use. Can start after M2. | U0         | the visualizer's tests unchanged; the new presentation gains each as it is carved | XL   |
| X1  | The visualizer's Cell Content and Topology modes as two slice views                                                                                                                       | M4, X0     | the visualizer's layout tests re-homed against them                               | M    |
| X2  | `ViewAxisBinding` and `DimensionBundle` reduced to presets; the legacy navigation tests asserted against `ViewAxisSet`                                                                    | E5, X1     | same behaviour, new storage                                                       | M    |
| X3  | **Go or no-go** (below). Then delete `ZigzagVisualizer`, the arrangement in `LinkBeams`, `BridgeCoordinator` and `zigzag_commands`; reduce `Views` to documents                           | X1, X2, M8 | each legacy test re-homed or retired with a reason                                | L    |
| X4  | Journeys, probes, a `compare-backends.sh` scene per view                                                                                                                                  | all        | the journeys of the spec's §16.2                                                  | M    |

**Coexistence.** Old and new are both selectable at run time from M2; there is no `#ifdef` and no
build flag. The legacy presentation is the default, and its tests go on running, which is what keeps
it from rotting while it is still what a reader gets.

**Go or no-go.** X3 does not happen by default. The orchestrator of the work decides, and decides go
only when a review finds that the new views pass every journey the legacy ones pass, that the checks
of §5.5 hold, and that the frame-time probes are no worse. If not, the legacy presentation stays the
default, nothing is deleted, and the new views remain a choice. The cost of that is carrying both:
X0's carving is what keeps it small, since after it the two share everything but layout and drawing.

**What becomes of everything A1 moves.** The view system replaces layout and arrangement. Most of
what lives in `apps/common/ui/` after A1 is neither.

| Component                                                                                                                                                                                                              | Fate                                                                                                                                                                                      |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `session`                                                                                                                                                                                                              | Stays. `ReadingPlace` gains each placement's view, sub-view and cursor, so a session resumes in the view it was left in (U2).                                                             |
| `views`, `views_publication_links`                                                                                                                                                                                     | Document opening, switching and the publication interface stay. Alongside and onion skin stay, and are candidates to become page views (spec §10.5). Arrangement and framing leave at X3. |
| `beams`                                                                                                                                                                                                                | Beam drawing stays, as the presenter's edge painter. Its alignment of documents is replaced by the base view (P3) and deleted at X3.                                                      |
| `bridge_coordinator`                                                                                                                                                                                                   | Replaced by edges between placements in a scene (U4b); deleted at X3.                                                                                                                     |
| `tenuous_tether`, `kinetic_tether_overlay`, `satelloid`, `wireframe_hull`, `world_card_presentation`                                                                                                                   | Stay as world overlays. The tether of a flown page comes from the base view's records from M5; the satelloid's alignment of a proxy overlaps windowed pages and is reviewed at M7.        |
| the overlays (`clasp_link_forge`, `collaborator`, `link_panel`, `overview`, `page_break`, `pouch_drawer`, `swarm_telescope`, `transcopyright`), `hypertime_graph`, `quotation_builder_overlay`, `store_object_manager` | Unaffected until M8. Each is its own focus scope or overlay. With panes, one that anchors to a place in a document takes its projection from the host (U4b).                              |
| `batch_orchestrator`                                                                                                                                                                                                   | Unaffected.                                                                                                                                                                               |
| `zigzag_visualizer`                                                                                                                                                                                                    | Carved (X0), replaced (X1), deleted (X3).                                                                                                                                                 |
| `zigzag_commands`                                                                                                                                                                                                      | Absorbed by `view_commands` as each command gains a view-system form; deleted at X3.                                                                                                      |
| `unified_transclusion_engine`                                                                                                                                                                                          | Stays; it stages GPU buffers for cell content. Its private ephemeral slots give way to a `ViewManifold` at X3, and what still uses it then is reviewed.                                   |

## 5. Visual and experiential direction

### 5.1 Principles

Each settles an argument an implementer would otherwise have.

1. **Real is hard-edged; view-only is soft-edged and seamed.** A stored cell is a sharp rectangle,
   as today. A pack, a ghost, a badge, an empty lane has rounded corners or seams. Never the
   reverse.
1. **Depth always means something.** A step on a bound point, a ring's lean, a deck position, a lift
   towards the reader. Depth is never decoration, and never the only cue.
1. **The thing to follow moves first and longest.** Everything else moves later and for less time,
   as `sworphSubject` leads `sworphRow` today.
1. **The anchor stays still.** The accursed cell and the anchor page do not move to make room; the
   camera does.
1. **Colour is identity, never state.** A dimension's hue does not change on hover, focus or
   selection; those change brightness, weight or outline.
1. **A placeholder never outshines what it stands in for.** A ghost is dimmer than the dimmest real
   thing on screen.
1. **Detail falls in steps.** Text gives way to a shorter form, then to bars, then to a count, with
   hysteresis. Nothing shrinks continuously until it is unreadable.

### 5.2 The dimension colour system

- **Reserved hues.** Five bands already mean something and are not given to dimensions: gold and
  amber (transclusion, focus), cyan and blue (accent, comment links), purple and violet (authorship,
  link ribbons), green (quotation), red (disagreement). The legacy cyan, emerald and amber of `x`,
  `y` and `z` are not carried over.
- **Generation.** The *n*-th dimension seen takes the hue at the fractional part of *n* times the
  golden ratio's conjugate, mapped onto the open arcs only. Adding a dimension never changes an
  existing one. After a first pass at one lightness and chroma, later dimensions take a second and a
  third band — vivid, deep, pale.
- **A second cue, always.** Each colour comes with a dash pattern and a small glyph, cycling
  independently of hue, used on strands, edges and the compass. A `PlacedEdge` carries the dash
  class.
- **Groups** show a swatch of their members' colours on a neutral ground (D4).
- **Overrides** are `ui.dimension.<name>.colour` and `.dash`, the reader's to set.

V1 checks the rule before anything depends on it.

### 5.3 Motion

Three families, so the program moves as one thing: *chrome* is quick, *content* reuses the timings
documents already move with, and the *camera* is last and slowest.

| Transition                   | Duration                                | Shape                    |
| ---------------------------- | --------------------------------------- | ------------------------ |
| step along a dimension       | 120 ms                                  | ease out                 |
| toss: old view cells         | 90 ms, fade in place, no travel         | ease in                  |
| toss: new view cells         | 160 ms, fade with a slight scale-in     | ease out                 |
| view switch                  | 220 ms cross-fade                       | ease in-out              |
| sub-view switch              | 150 ms                                  | ease in-out              |
| pack spread                  | 220 ms, slight overshoot                | ease out                 |
| wheel depth change           | 240 ms                                  | ease in-out              |
| selector open / close / tier | 180 / 120 / 140 ms                      | out / in / in-out        |
| page fly-in and return       | subject 620 ms; row 450 ms, 90 ms later | as today's sworph        |
| riffle, per page             | 60 to 120 ms, the run under 420 ms      | ease out, shallow arc    |
| split                        | block 320 ms; target 80 ms later        | in-out; out              |
| pane split or close          | 150 ms                                  | ease in-out              |
| subspace zoom                | 450 ms, slight overshoot                | ease in-out              |
| camera reframe               | 700 ms, 140 ms later                    | as today's camera settle |
| reduced motion               | all of the above: a cut                 | —                        |

Never animated: the field of view, a ghost's position, text at the level of glyphs.

**Under a held key.** A step is accepted the moment it arrives; input never waits for a tween. Each
repeat retargets what is moving from where it is now, so a held key reads as a glide at a steady
speed and not as a stutter of queued animations. Derived view cells that would appear and vanish
within one repeat interval are not faded in at all.

Every number in the table that is not already a setting becomes one (`motion.*`, G16).

### 5.4 Component notes

The full guidance for each component is the visual review's report
([`projects/view-reviews/04-aesthetics.md`](projects/view-reviews/04-aesthetics.md)); these are the
points most likely to be got wrong.

- **Compass.** The eye lands on what is bound, not on the rose: arms are thin, labels carry the
  weight. An unbound arm is present and faint, so the reader sees there is somewhere to drop.
- **Selector.** One tier is in focus and the others recede in depth and opacity; three tiers at
  equal strength is noise. The armed item is the only thing at full brightness until a point is
  named.
- **Stretch vanishing.** The gap is small and constant; variety comes from the content. Ticks on the
  immediate neighbours are the one place structure is drawn, so they are crisp.
- **The wheel.** Spokes of bound points are heavier than ring edges. A child wheel is visibly a
  smaller wheel that has turned away, not a second hub competing with the first.
- **Packs.** One outline, hairline seams, and the header quieter than any constituent. The spread
  opens along the lane axis only. A strand's tint on a face is a wash, not a fill.
- **Base view.** The tether is the faintest thing on screen that is still visible. A windowed page
  has paper edges top and bottom that say "there is more of this page".
- **Decks.** Paper darkens slightly with distance as well as fading, so the line reads as depth and
  not as a gradient. A marked page is solid, not glowing.
- **Panes.** The focused pane is told by its divider and its compass, not by dimming the others.

### 5.5 Acceptance checks for the eye

Applied to captured frames and recordings at each milestone. **A** can be asserted from layout
records; **H** needs a person.

| #   | Check                                                                                              | Kind |
| --- | -------------------------------------------------------------------------------------------------- | ---- |
| 1   | No label overlaps another label or a cell's text.                                                  | A    |
| 2   | A ghost is never brighter than the dimmest real cell in the frame.                                 | A    |
| 3   | Nothing view-only has square corners; nothing real has rounded ones.                               | A    |
| 4   | Every strand, compass arm and wheel edge of one dimension has one hue and one dash.                | A    |
| 5   | No hue in a reserved band is used for a dimension.                                                 | A    |
| 6   | Text is never drawn below the readable size; it has stepped down instead.                          | A    |
| 7   | The wheel reads as a wheel at valence 2, 8, 20 and 60.                                             | H    |
| 8   | In a riffle no more than two pages are in the air at once.                                         | A    |
| 9   | The anchor page and the accursed cell do not move during a coalesce, a rebind or a spread.         | A    |
| 10  | A deck of a link-dense document still recedes.                                                     | H    |
| 11  | A pack is seen as one object at a glance, and as parts when spread.                                | H    |
| 12  | In every transition the eye follows one thing.                                                     | H    |
| 13  | Contrast of drawn text is at least 4.5:1 at every opacity at which text is drawn.                  | A    |
| 14  | With motion reduced, nothing tweens.                                                               | A    |
| 15  | At valence 60 no label on the wheel overlaps another.                                              | A    |
| 16  | While a movement key is held, the cursor's position is never more than one step behind the input.  | A    |
| 17  | At every milestone, every feature present can be reached from the palette, a chord or the pointer. | H    |

### 5.6 Where the eye is consulted

- **At M1, with the spikes:** V1, the colour strip.
- **With E9, the first view:** ghosts, the focus mark and view-only chrome are written once, in a
  small shared helper; E7 adds seams and strands to it. Three views built one after another must not
  each invent their own.
- **Before E9's goldens are fixed:** read the rasters of stretch layouts over varied content, not
  only the numbers.
- **Before E10's tests are fixed:** V2.
- **At P3's parity step:** capture old and new side by side on one fixture; numbers matching is not
  the same claim as looking the same once pages can window.
- **Before P4's defaults are fixed:** V3.
- **At each milestone from M2:** the checks of §5.5 on a set of captured frames and one recording of
  the milestone's journey, inspected by a subagent whose only job that is, never by whoever wrote
  the code. At M2, M4 and M6 one task is given to a reader who has not seen the feature: bind
  another dimension and read a rank; find which dimension connects two cells; bind a dimension that
  is not on screen.

## 6. Gates

| Change                                         | Gate                                                                                                                                               |
| ---------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| A1, U2, U4, X3, and anything touching a device | `make -j$(nproc)`; `make test`; `make format-check lint` with clang-format 19; `xvfb-run … ./tools/compare-backends.sh` and the captures inspected |
| Tracks E and P                                 | `make -j$(nproc) lib xuzz_test`; `./build/xuzz_test` and `./build/xudu_test` headless; `make format-check lint`                                    |
| Track L, header-only (L1, L2, L8)              | `make -j$(nproc) gleditor_test`; `./build/gleditor_test` headless                                                                                  |
| Track L, device (L3 to L7, L9)                 | the full gate, with the new scene                                                                                                                  |
| Every package                                  | the eye's checks that apply (§5.5); no file under `apps/xudu/` or `apps/zigzag/`; `tools/check-ui-text-policy.py` over `apps/common/ui/view/`      |

The publication-network tests take most of a full `make test`. Engine packages need not wait on
them: they cannot be affected by code no program yet links. The full gate runs at every milestone.

`make test` ends with the rootless swarm tests, which need the `veth` kernel module. Without it the
run ends non-zero *after* every test binary has passed. Read the `[  PASSED  ]` lines before calling
a gate red, as `AGENTS.md` says; loading the module is the machine owner's to do.

A green fast gate proves only that the engine code is right by its own tests: until a presentation
links it, nothing proves it is usable. That is why M2 is as small as it is.

## 7. Risks

| Risk                                                          | Likelihood      | Cost                          | Handling                                                               |
| ------------------------------------------------------------- | --------------- | ----------------------------- | ---------------------------------------------------------------------- |
| A draw per plane is too slow on OpenGL                        | medium          | `PlaneSet` redesigned         | R1 before L6; flat views do not use `PlaneSet` at all                  |
| Page poses break reflow, the caret or picking                 | high without F2 | corrupted documents on screen | L7 splits flow from pose first, behind tests, with no behaviour change |
| The base view at fixed steps does not match today             | medium          | visible regression at M4      | S2; parity step in P3 with captures                                    |
| Three views drift apart in look                               | high            | rework across views           | shared chrome with E7; milestone reviews                               |
| The relocation breaks packaging or a target not built locally | medium          | red CI                        | F12 fixed in the same commit; the emptiness check; the full gate       |
| Legacy and new paths diverge while both live                  | medium          | bugs found late               | both are view kinds; legacy tests run until X3                         |
| Translucent order is wrong where kinds overlap                | medium          | visible only in some scenes   | one sorted list; a scene that forces overlap in L7's gate              |
| Settings and chords: a hundred entries, by hand               | certain         | tedium and slips              | a table helper; added per view; the conflict test                      |
| The `t` role needs a manifold per state                       | unknown         | a cache, or `t` deferred      | S3; E16 is last in its track and nothing depends on it                 |

| Rewiring the application to the host | high | the largest single change | deferred to M8 behind
the seam of U0; done in several commits; every `--view` mode reproduced first | | The new views are
not good enough to replace the old | possible | two presentations carried | the go or no-go at X3;
X0 keeps the shared part large and the duplicated part small | | Two streams collide in
`system_docs.cpp`, the keymap table and the Makefile | high | merge friction | settings and chords
live in each view's descriptor, not one file; the Makefile's source lists are globs and need no edit
| | Scope: 60 to 120 commits | certain | nothing usable for a long time | the first release is M1 to
M6; M2 and M3 use only what exists; the deferrals of §4.1 |

## 8. Decisions made

The owner answered the five questions the first version of this plan left open.

| Question                                            | Decision                                                                                                                   |
| --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| Relocation now, as its own change?                  | **Yes, and first** — before the spikes, so it is tested in isolation (M0).                                                 |
| Are `u` and `t` in the first release?               | **No.** Deferred with E16.                                                                                                 |
| Who gives the go or no-go, and who inspects frames? | **The orchestrator** decides, at every milestone and at the cut-over. **A specialised subagent** inspects captured frames. |
| Where do walk summaries live?                       | **Wherever fits for now**, in the activity store; refine later (E14).                                                      |
| The third test binary's name?                       | **`ui_test`**, with `tests/ui/`, renamed straight after the relocation (A2).                                               |

## 9. Challenges and responses

Three challengers attacked the first draft. What each said that mattered, and what was done.

### 9.1 Correctness and sequencing

| Challenge                                                                                                                     | Response                                                                                                                                                                                                                             |
| ----------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| The plan said the spec's relocation table was corrected; it was not, and following it breaks ten link targets.                | **Accepted.** The spec's step 1 is corrected in the same change as this plan (F1).                                                                                                                                                   |
| The wheel was a milestone without the package that draws planes.                                                              | **Accepted.** The presenter over `PlaneSet` is split out (U4a) and is in M4.                                                                                                                                                         |
| Colour, animation and commands were in no milestone, so "first light" could not be driven.                                    | **Accepted.** U1, U5a, U6a and U7a are in M2.                                                                                                                                                                                        |
| Wrapping the visualizer and `Views` in a host was sized as one package; `xuzz_app.cpp` reaches into them in about 170 places. | **Accepted, and the approach changed.** The new presentation stands beside the old behind an extracted seam (U0, U2); the host and the rewiring come last (U4b, XL).                                                                 |
| The cut-over named a fate for three classes and nothing else.                                                                 | **Accepted.** §4.7's table gives every moved component its fate, and X0 carves the visualizer's other responsibilities before it can be deleted.                                                                                     |
| `Page::setModel` stays public, so reflow and a pose can race.                                                                 | **Accepted.** L7 closes it.                                                                                                                                                                                                          |
| The base view's solver is asserted pure and equal to today's.                                                                 | **Accepted in part.** S2 tests it; P2 names the fallback. The engine is cleared and rebuilt on each activation today (`apps/xudu/beams.cpp`, the block before `tensionEngine_.step`), so a cold start is what the code already does. |
| The full gate cannot be run without the `veth` module.                                                                        | **Accepted.** §6 carries the caveat.                                                                                                                                                                                                 |

### 9.2 Scope and delivery

| Challenge                                                                                                      | Response                                                                                                                                                                                          |
| -------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| The plan gave no size.                                                                                         | **Accepted.** §1: about 27,000 new lines, 60 to 120 commits. E4 and E7 are L.                                                                                                                     |
| Spikes should all come first.                                                                                  | **Accepted.** Milestone 0.                                                                                                                                                                        |
| Stretch vanishing is not what a reader will notice; call the first milestone what it is.                       | **Accepted.** M2 is "the spine"; the wheel is the first thing a reader notices.                                                                                                                   |
| The binding model is useless without a way to bind, and the selector was last.                                 | **Accepted.** A typed bind and a displayed compass are in M2; the selector moves ahead of decks and panes.                                                                                        |
| Cut edge heat and the Markov order.                                                                            | **Refused as cuts; accepted as deferrals.** Both were asked for; neither is in the first release.                                                                                                 |
| Defer `u` and `t`, deeper wheels, nested packs, keeping a pack, windowed pages, the stagger search, embedding. | **Accepted.** §4.1's table.                                                                                                                                                                       |
| No spike covers translucent order across pages, planes and beams.                                              | **Accepted.** R5.                                                                                                                                                                                 |
| What if the new views are not good enough?                                                                     | **Accepted.** The go or no-go at X3.                                                                                                                                                              |
| The tracks are not as independent as claimed: `system_docs.cpp`, the keymap, the Makefile.                     | **Accepted in part.** Settings and chords move into each view's descriptor; the source lists are globs. `xuzz_app.cpp` remains a point of contact, which is why it is touched in U0 and U4b only. |

### 9.3 Experience

| Challenge                                                                                                                               | Response                                                                                                                                                                          |
| --------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Features would exist with no way in: bindings before a compass, sub-views before a palette, `u` and `t` keys before they mean anything. | **Accepted.** "Nothing is present without a way in" (§4.1) and check 17.                                                                                                          |
| A view's chords waited for all three views.                                                                                             | **Accepted.** Each view ships its own descriptor (E15).                                                                                                                           |
| The wheel's flex does not keep labels apart.                                                                                            | **Accepted.** G12 and check 15.                                                                                                                                                   |
| The colour system promises a dash class no record carries.                                                                              | **Accepted.** G13.                                                                                                                                                                |
| No chord reorders the ring or edits group members.                                                                                      | **Accepted.** G14.                                                                                                                                                                |
| The selector has no accessibility contract.                                                                                             | **Accepted.** G15.                                                                                                                                                                |
| New motion timings are literals.                                                                                                        | **Accepted.** G16.                                                                                                                                                                |
| Nothing says what a held key does.                                                                                                      | **Accepted.** §5.3.                                                                                                                                                               |
| The points where the eye is consulted have no artifact and no owner.                                                                    | **Accepted.** §5.6 names frames, a recording, a second inspector and a task; who that is, is the owner's to say (§8).                                                             |
| "Real is hard-edged" against pages, and "the anchor stays still" against a walk.                                                        | **Clarified.** A page is real and hard-edged; its ghost is not. The anchor stays still *during a transition that is not a move*: a walk moves the cursor, and the camera follows. |

## Change history

- 2026-10-07 — Initial plan, from four expert reviews.
- 2026-10-07 — Revised after the challenge round: milestones reordered, the host deferred behind an
  extracted seam, deferrals and the go or no-go added, gaps G12 to G16, spike R5.
- 2026-10-07 — The owner's decisions recorded (§8): relocation first, then spikes; `u` and `t`
  deferred; the orchestrator decides go or no-go and a subagent inspects frames; walk summaries in
  the activity store as they fit; the third test binary becomes `ui_test`.
