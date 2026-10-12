# 3D Spatial Hypermedia Navigation: Implementation Plan

Status: design plan; the additions below are unbuilt. Updated 2026-10-12. This revision integrates
the accepted Hypertime Loom into the uploaded *Implementation Plan — 3D Spatial Hypermedia
Navigation Overhaul*. It preserves the rest of that overhaul's spatial direction. No application
code is changed by this design update.

The implementation target recorded by the original plan is `.worktrees/3d-spatial-hypermedia`,
branch `feat/3d-spatial-hypermedia`; both exist as of this review. This document is the repository
copy of the revised plan. Coordinate its packages with
[`view-system-implementation-plan.md`](view-system-implementation-plan.md), particularly Track H.
The existing M0–M6 first-release boundary in that plan is unchanged.

## 1. Confirmed direction

Xuzz presents documents, ZigZag cells, explicit links, shared content, and history in one spatial
scene. The visual language is liquid glass, physical depth, bowed conduits, mechanical unfolding,
and legible tablet faces. The main elements are:

- A Master Link Tablet between two endset clusters, retaining one link identity and both complete
  ordered endsets while either side is explored.
- Liquid-glass conduits, flying occurrence stacks, and a spline-following traversal camera.
- A dimensional slice tear revealing selected cells and their actual rank neighbors.
- A suspended Hypertime Loom: coiled operation tracks, branched exits, hanging and outward-bowing
  strands, and gold transclusion connections crossing the volume.
- Temporary parallel history lenses drawn from the loom by tugging a container thread or selecting a
  history command. The same operation can appear in several contexts with correspondence back to its
  original position.

The loom replaces the earlier proposal's flat light table and its “paper and light, not glass and
plasma” direction. The approved examples establish spatial coils and liquid glass as the history
presentation. They are concept images, not working captures or performance measurements.

References: [ten-path loom](mockups/hypertime-loom-2026-10-12/10-scenario-3-ten-branches-v2.png),
[density and expansion](mockups/hypertime-loom-2026-10-12/compaction-and-expansion.md),
[helical packing](mockups/hypertime-loom-2026-10-12/helical-packing.md),
[scenario notes](mockups/hypertime-loom-2026-10-12/scenario-notes.md), and
[six history journeys](mockups/hypertime-loom-journeys-2026-10-12/journeys.md).

### Camera and tablet decisions retained

Explicit link entry (`std:xuzz/link_enter`) follows a beam spline with expressive Frenet–Serret
banking and roll. Cross remains a side/member preview under the retained navigation contract; this
corrects the original plan's travel hook on `std:xuzz/link_cross`. Preserve the requested full-tilt
mode. Provide `spatial_nav.clamp_camera_roll` in `system://settings`, with the planned CLI binding,
for stabilization. Spike continuous framing at straight sections, near-zero curvature and
inflections: a singular frame must not cause a sudden flip. Reduced motion provides an immediate or
stabilized arrival with the same selected destination. Inspection itself does not trigger travel.

The Master Link Tablet's provenance wing unfolds left, microversion/lineage wing below, and
transcopyright wing right. Wings are hinged spatial surfaces, with separate picking and focus
scopes. Display actual available author and verification data; the original plan's named author was
an example. Unknown, unverified, locked and unavailable states remain distinguishable. A shared
source relation and an explicit content link have different identities and presentations.

The slice tear reveals a placement envelope computed by a view, not an inherent Euclidean volume of
the manifold. Its approximation hull describes the displayed neighborhood and coverage. Selected
cells and their immediate rank neighbors project forward through the aperture. Preserve the
one-neighbor-per-direction-per-dimension topology; history branches are not extra neighbors on one
ZigZag rank.

## 2. Interaction and identity contract

Follow [`ui_workflow_xuzz_navigation.md`](ui_workflow_xuzz_navigation.md) and R8 in
[`store-slice-convergence.md`](store-slice-convergence.md).

1. **Select → inspect → enter.** Selection pins a subject; inspection exposes its context; entering
   changes the reading place. Completed reader visits belong to the private activity store. Camera
   motion, unfolding, filtering, and tugs append no operations to visited stores.
1. **Keep the source identity.** A tug produces a parallel projection, not a new authored op or a
   reparented history. Store authority, exact microversion, owning frame/object, and occurrence
   accompany references. Missing data never substitutes the latest state.
1. **Make relations explicit.** Solid silver strands show stored ancestry; pearl tethers show the
   same op in different presentations; seamed guides show filtered ordering. Gold identifies actual
   shared primedia addresses. Text matches, numeric equality, or suggested meaning do not create
   gold identity. Explicit xanalinks retain their type and identity colors.
1. **Declare scope and coverage.** Store, xanadoc, slice, and cell are real scopes where supported;
   own edits, contained edits, indirect topology effects, and shared-source occurrences are distinct
   relations. Ordinary pagination has a captured footprint rather than an invented permanent page
   identity. Show included branches, frames, pending content, and completed query coverage.
1. **Retain exact selections.** Disjoint ranges retain order and gaps. Source addresses, the seed
   occurrence, inspected state and layout context are different fields. Whole-object cell references
   and selected cell-content ranges support different actions.
1. **Offer equivalent access.** Pointer tugs and drops have menu, keymap and accessibility actions.
   A synchronized list and plan view expose the same records, scopes, branch exits and commands.

## 3. Prerequisites and spikes

Data services precede presentations that depend on them. The existing spool, Chronofilade,
Spanfilade and cell subject chains do not constitute an all-branch effect or historical text index.

| Package | Work and gate                                                                                                                                                                               |
| ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| D1      | Exact selection descriptor: ordered ranges, grouping/gaps, source authority, snapshot, frame and occurrence. Prove mixed page/cell selections and repeated appearances remain distinct.     |
| D2      | Branch/frame-scoped history enumeration and correspondence records. Prove a cell's subject chain is not mistaken for all contained or indirectly affecting history.                         |
| D3      | Asynchronous before/after effect footprints: insertion gaps, deletion-before ranges, rearrangements, and reciprocal structure changes. Validate in each op's parent state and owning frame. |
| D4      | Versioned occurrence text/numeric search with explicit rules and coverage. Include text assembled across span boundaries and historical occurrences later removed.                          |
| D5      | Typed durable reference descriptors and restore/availability dispatch for op cards and saved lenses. Existing content pouches persist; arbitrary op/query/draft cards require new support.  |
| S1      | Coiled layout and picking: stable forks/exits, visible branch counts, occlusion, local expansion and an equivalent list. Compare software OpenGL, GLES and Vulkan where available.          |
| S2      | Retained geometry and query cancellation: bounded visible work, stale-result rejection, cold-load coverage, and no preparation during draw. Measure budgets before choosing settings.       |
| S3      | Camera frame continuity, render-region unprojection, depth/mask reset, and tablet/wing focus. Use the library's backend conventions, including Vulkan reversed Z.                           |

Put graphics-free history/selection services in `apps/common/xanadu/`; spatial presentation and
input belong in `apps/common/ui/`. Generic placed-plane, picking and render-region support belongs
in the library. Do not introduce Xanadu dependencies into the plain editor. Register configuration
through the system-document schema and defaults; do not bake numeric LOD, pitch, duration or GPU
budgets into the presentation.

## 4. Implementation phases

### Phase 1: Shaders and visual physics

Extend `assets/shaders/beam.frag.glsl` for caustics, edge dispersion/refraction and conduit pulses.
Add the planned slice-tear and slice-hull vertex/fragment shaders and tablet-glass material. Use the
same material family for loom conduits, op inspectors and unfolded tablets. Keep text on calm,
contrast-preserving faces; animated highlights and refraction belong outside readable glyphs.

Spike compositing, masking, depth range and picking before multiplying translucent surfaces. Quality
settings reduce optical effects independently of history detail. A cheaper material must retain the
same subject, edges and actions. Compile shaders through `make -j$(nproc) shaders`.

### Phase 2: Core spatial geometry and mechanics

The original planned classes remain under `apps/common/ui/spatial/`:

- `CameraSplineDriver`: Hermite/Catmull–Rom paths, arc-length sampling, banked frame continuity,
  stabilization and destination easing. Retarget or cancel traversal rather than queue camera trips.
- `MasterLinkTablet`: negative-space placement between endsets, actual relation/author metadata,
  independent member and occurrence controls, hinged provenance/lineage/rights wings, and picking.
- `DimensionalSliceTear`: view-derived envelope, aperture, selected-cell relief and actual neighbor
  selection. Its coverage is visible; a coarse hull cannot imply unseen topology is loaded.
- `FlyingOccurrenceStack`: stable occurrence identities, bounded visible stack, source-to-tablet
  docking, and cancellable motion. Selecting an occurrence previews it; entering is separate.

Use the view plan's PlaneSet, page-pose and render-region seams instead of building a second
unprojection or placement system. A scene host is introduced at U4b/M8; earlier presentations can
use U4a/U3. Store-backed records, ephemeral view placement and GPU geometry remain separate.

### Phase 3: Liquid-glass beam trunks and conduits

Refactor `apps/common/ui/xanadoc/beams.hpp` and `beams.cpp` to support tributaries converging on a
shared trunk, camera waypoints, tablet clearance bounds and material parameters. One explicit Link
retains both complete ordered endsets. Member counts and windowed lists preserve individual access
without drawing every left/right pair. Keep shared-content gold distinct from explicit-link ribbons.
Do not replace exact anchors with the covering extent of disjoint selections.

### Phase 4: Hypertime Loom and investigation lenses

This replaces the original Phase 4 limited to a 2D DAG's masking and continuation badges.

**4A — Suspended, coiled history.** Replace `hypertime_graph`'s primary presentation with a retained
spatial loom over D2. Long runs first wind into configurable corkscrews to gain density; at greater
distance they become counted skeins. Local pitch opens around an inspected op while adjacent turns
remain compact. Compaction preserves selected subjects, forks, all branch exits, counts,
continuation access and original ancestry. Late-arriving branches do not reshuffle the reader's
anchor. Hanging and bowing strands express spatial connections; depth supplements labels and shapes
rather than carrying the only identity cue.

**4B — Container tugs.** A thread back to an op's actual owning context can pull reachable history,
including branches, into a parallel track. Keep each source op in its original loom position and
connect its additional appearance by a neutral correspondence tether. Expose scope and relation
controls. For a cell, own edits and indirectly affected topology require different enumeration; for
a slice, contained content and connections are separately filterable. A tug does not assert a
universal containment hierarchy across ZigZag.

Ship own/contained scopes first; enable **Affected topology** only after D3/4C supplies its complete
before/after effect records. Do not present a subject-chain subset as a completed effect query.

**4C — Span and content lenses.** Drop an exact selected range bundle on **History lens · Touches**.
D3 explains inserted gaps, removed pieces from before-states and moved fragments. **Original
source** freezes ranges; **Evolving passage** declares insertion/boundary rules. **This occurrence**
and **All uses of this source** differ. Unrelated offset shifts are optional context. Results carry
reasons, parent-state previews, branch badges and counted filtered gaps.

D4 supplies **Content history** with separate literal, whole-token, numeric and related-idea modes.
For `3.14`, math prose and a pressure-gauge reading remain separate contexts. Related ideas require
explained rules/aliases or identified suggestions. Numeric normalization and units stay visible.
Counts distinguish occurrences from affecting operations; partial searches report found-so-far
coverage. Query tracks are not stored parent chains.

**4D — Save, resume and recover.** D5 enables an arbitrary-op reference in a pouch, with exact
identity, state and optional occurrence. Opening inspects before entering. Save lens recipes with
scope/rules separate from result snapshots; rerunning is explicit. A page lens captures ordered
contents and snapshot/layout context, or follows a chosen passage. Reflow is not automatically an
authored page edit.

Recovery distinguishes the source state from the target branch base. Preview the selected source
passage against the full chosen base, then explicitly author a separately identified recovery
branch. Preserve original futures and source identity. Inspection never creates that branch.

**4E — Coexistence and scale.** Retain a readable list/plan presentation over the same models. While
the old DAG coexists, fix background-beam occlusion, depth-mask resets, edge clipping and
earlier/later continuation badges. Spatial continuation must expose folded branches as well as trunk
overflow. Use bounded visible labels, hysteresis and local expansion. Cancellation, unavailable
content and empty completed scopes have different states. Performance gates are measured on actual
stores; concept counts are fixtures, not established throughput claims.

### Phase 5: Application integration and cutover

Coordinate `apps/xuzz/xuzz_app.cpp`, `Session` and the shared view coordinator. The planned
`SpatialNavigatorScene3D` contains tablet, tear, occurrence stacks, camera and loom presentations.
Route selection and explicit entry through the existing link navigation contract and activity store.
Restore scope, selection, lens subjects and reading return points with exact identities.

Do not delete the old navigation path until equivalent pointer, keyboard, list and accessibility
journeys pass, unavailable sources remain honest, and backend frames pass review. With panes, all
anchors project through the shared host rather than a private camera model.

## 5. Acceptance journeys and verification

The three density fixtures are required visual and interaction checks:

1. A xanadoc trunk with 100 ops and three 20-op branches at the start, middle and end: preserve
   chronological ancestry, three fork positions, branch access and exact counts through coiling,
   aggregation and expansion.
1. Thirty interleaved slice/xanadoc ops: combined view and tugged slice history show the same ops in
   their original positions, reachable branches, scope and neutral correspondence.
1. Ten paths with exits and many crossing transclusions: each path and exit is selectable; actual
   shared source remains distinguishable from ancestry and filtered results.

Run all [six journey checks](mockups/hypertime-loom-journeys-2026-10-12/journeys.md): op
save/reopen, span insertion/deletion/rearrangement, historical `3.14` search, reciprocal topology
effects, captured page footprint versus reflow, and recovery from a separately selected base. Repeat
with partial coverage, disjoint/repeated spans and cancelled queries. The reviewed mockups establish
design intent; implemented behavior must be validated through actual user interfaces.

Automated tests belong with their owning model or UI component, in a target that links that code. Do
not put generic camera/rendering tests in the engine-only ZigZag target merely because a tear shows
cells. Cover history identity, branch scope, before/after effect reasons, selection grouping,
coverage/cancellation, restore, picking, frame continuity and explicit authoring boundaries.

Use `make -j$(nproc)` and `make -j$(nproc) test`; outside make use all six headless/XDG variables
from AGENTS.md. Compare backends headlessly using `tools/compare-backends.sh` under Xvfb. Capture
offscreen frames showing glass trunks, unfolding wings, slice relief, camera banking, and dense loom
overview/expansion/tugs. Capture reduced-motion and list equivalents as well. Measure settled
frames, cold history queries and large visible windows before fixing budgets. Never report mockup
rendering as implementation verification.

## 6. Link forge and pouches: next design study

The [forge and pouch study](mockups/link-forge-pouches-2026-10-12/concepts.md) explores two endset
bays around one draft tablet, dense typed reference collections, preserved disjoint selection
groups, explicit receiving store/base, and draft resumption. These are proposals for review, not
newly approved implementation packages. They reuse D1/D5 and the tablet/collection presentation
language, but require separate staging, destination and commit semantics.

Existing forge code authors one Link with two ordered span lists; existing pouches persist
individual content cards. Arbitrary object/op/query/walk cards, composite span cards, durable drafts
and explicit destination/base controls are additions. One Link can require several Structure
operations. Resolve failure recovery and branch targeting before promising atomic commits or
forbidding all staging storage changes. Do not silently convert a saved operation or a whole cell
into a content endpoint.

## Change history

- 2026-10-12 — Repository revision of the uploaded spatial overhaul: integrated the accepted coiled
  Hypertime Loom, six history journeys, data prerequisites, scale/accessibility gates and
  coordinated Track H. Preserved camera banking, mechanical tablet wings, tear, occurrence stacks
  and beams.
- 2026-10-12 — Added a separately identified forge/pouch brainstorming study; no application code
  changed and no concept image treated as a completed feature.
