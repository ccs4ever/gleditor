# Xuzz Frictionless Navigation: Agentic UI Workflow

## Purpose and grounding

Xuzz presents xanadocs and ZigZag cells in one navigable space. A reader should be able to inspect a
link, move among its left and right spans, cross between document and cell content, and return
without losing the link that made the trip meaningful. This is a design and implementation workflow;
the interaction described below is not yet implemented end to end.

[`xuzz-unified-link-traversal-vision.md`](xuzz-unified-link-traversal-vision.md) describes the
reader journey, compared UI concepts, and experience goals behind this contract.

Nelson's [ZX model](https://www.xanadu.net/zigzag/fw99/XUmodel.html) describes one addressable link
with two lists of content spans, including discontinuous spans, and traversal in either direction.
His [xanalogical overview](https://www.xanadu.net/NOWMORETHANEVER/XuSum99.html) describes
transpointing views with visible connections.
[XanaduSpace](https://www.xanadu.net/XanaduSpace/btf.htm) brings a companion page alongside the
current one, and his [ZigZag description](https://www.aus.xanadu.com/ted/zigzag/xybrap.html) treats
windows as views of the same cells. The controls below are Xuzz design decisions inferred from those
principles, not controls specified by Nelson.

The stored [`Link`](../apps/common/xanadu/ops.hpp) already has an identity, type, and ordered `left`
and `right` lists of `PrimediaSpan`. These lists are **endsets**, not an array of paired edges. A
rendered left-to-right strand is one view of the link; it must not become the navigation identity or
silently imply that its two members were paired by the author. A span may appear in more than one
document version or cell, and multiple links may cover the same content.

## Reader workflow

### 1. Discover and select a link

- The focused span, cell content, margin bracket, beam, and accessible link list all expose the same
  set of link identities. When several links overlap, the reader chooses one by type, owner, and a
  short preview of its two endsets. A beam hit with no unambiguous endpoint selects the link only.
- Selection opens a compact **link context** anchored near the current content. It shows the link
  type, owner or curator, `Left i/N` and `Right j/M`, both endset previews, and the current
  location. All spans belonging to the chosen link remain marked, including those outside the
  visible view (as counts or unresolved markers). Other links recede without becoming inaccessible.
- Selecting is reversible and does not move the caret. A second explicit **Enter endpoint** action
  moves focus. Pointer, keymap, and accessibility actions call the same navigation command; a
  pointer click on a specific endpoint may select it and enter it only when the UI labels that
  action as entry. Hover previews without changing selection or focus.

### 2. Browse both endsets independently

- **Next/previous link** chooses another link touching the current content or visible neighborhood;
  its order is stable by the current view's reading or rank order, then link ID. The active link
  stays pinned until the reader selects another or dismisses it. Moving the editing caret or
  changing a ZigZag dimension does not quietly replace it.
- **Next/previous left member** and **next/previous right member** change separate cursors. Each
  cursor names a stored span and, when that span appears more than once, a particular occurrence.
  Cycling either side leaves the other side, the link identity, and the origin marker unchanged.
- **Cross link** switches the active side and previews its currently selected member. **Enter
  endpoint** moves the reading focus into that member. If no target member has been selected,
  crossing presents the opposite endset for choice; it never picks the first rendered strand.
  Left/right are labels, not a one-way source/destination rule.
- **Activity Back** follows the current visit's parent. **Activity Forward** follows a chosen child;
  when a visit has several children, show their previews and keep every branch available. **Return
  to origin** selects the origin visit recorded for the active link. These actions restore saved
  focus, active side, member cursors, and companion view across document/cell boundaries.
- **Walks** reveals saved roots, branches, references, and annotations. Previewing a visit moves no
  reading focus; Enter restores it. Continuing from an older visit appends another child without
  deleting the futures already there. Existing document hypertime Back/Forward controls remain
  distinct from Activity Back/Forward.

For example, a link with two left spans and three right spans may be read as `Left 2/2, Right 1/3`.
Moving to `Right 3/3` leaves `Left 2/2` selected. Crossing back returns to that left member, with
all five members still represented by the same link context.

### 3. Enter content without a domain boundary

The same select, preview, cross, enter, and activity traversal actions apply to document text and
cell content. Entering a cell targets its exact content span and intra-cell offset, focuses the
cell, and brings its immediate dimensional neighborhood into view. The source document remains as a
companion with an origin marker and a visible connection to the active link. Entering a document
from a cell targets the chosen link span and chosen occurrence in the document; it does not search
for an unrelated first matching link.

The camera and companion views move continuously while the active span stays readable. The
connection remains anchored to content as pages scroll or cell projections change. The reader can
walk ZigZag ranks, switch projection, or read elsewhere on either side while the link context stays
available. If focus leaves a member span, the context says so rather than treating the new focus as
another endpoint. Dismissing the context is explicit.

### 4. Handle unresolved endpoints

Keep the link and both endset counts visible while a target is unresolved. Distinguish a document
page still building, a cell outside the current visible neighborhood, a missing version, and remote
content that cannot currently be fetched. Entering a distant cell requests a temporary view cursor
and neighborhood around that cell; waiting for an ordinary cell anchor cannot make an out-of-radius
cell appear. The request is cancellable, does not block the UI thread, and returns to the prior
focus on failure. Do not substitute another occurrence or another link without the reader's choice.

## Navigation state and command contract

Keep the live link context in view-local state and record completed navigation visits in a
reader-owned `system://activity` store. Each visit has a stable store-scoped ID, one parent, ordered
children, an exact destination, the action that reached it, and a saved view. When link-driven, the
visit records the link authority and ID, selected side, independent left/right member and occurrence
cursors, and origin visit. An occurrence identifies its exact `PrimediaSpan` plus a durable store
authority and version and either document range or cell/range; local store indices and `CellRef`s
alone cannot address targets across sessions. Resolve targets at use time and keep unavailable
visits addressable. A second visit to the same content retains its own path and context.

Record a visit after a successful semantic focus transition: opening a document/version, entering a
link endpoint, moving to another cell on a rank, or jumping to a passage. Hover, endpoint preview,
scroll and camera frames create no visits. Activity Back/Forward and entering a saved visit move
among existing nodes; a new transition from an older node creates a new child. Record view
checkpoints separately from visit edges when needed to resume the current view. A pending or failed
entry records no completed visit.

| Command                  | Effect                                              | Focus moves? |
| ------------------------ | --------------------------------------------------- | ------------ |
| Select link              | Pin link ID and full endsets; disambiguate overlaps | No           |
| Select member/occurrence | Update only that side's cursor and preview          | No           |
| Cross link               | Make the opposite endset active                     | No           |
| Enter endpoint           | Focus the chosen occurrence; append a visit         | Yes          |
| Next/previous link       | Select a neighboring link in stable order           | No           |
| Activity Back            | Restore the parent visit                            | Yes          |
| Activity Forward         | Choose and restore a child visit                    | Yes          |
| Walks / Select visit     | Explore roots and branches; preview a visit         | No           |
| Enter saved visit        | Restore its recorded focus and link context         | Yes          |
| Reference / Annotate     | Attach user-authored context to a visit or walk     | No           |
| Return to origin         | Restore the active link's origin visit              | Yes          |
| Dismiss                  | Remove live link context and temporary view cursors | No           |

The [R8 activity-store extension](store-slice-convergence.md) keeps live cursor and camera movement
ephemeral and appends no operations to the visited document or slice. Completed visits, references,
and annotations belong to the separate activity store. Its history is private by default, survives
restart, and is user data: an unreadable store must be preserved and reported rather than replaced
with generated defaults. New bindings and action names belong in `system://keymap` as Vortex
routines. The same command contract serves pointer and accessibility adapters; no fixed chords are
specified here.

## Implementation workflow

The reusable agent workflow is
[`xuzz-link-navigation`](../.claude/skills/xuzz-link-navigation/SKILL.md). Use it when implementing
the interaction contract below.

The three review roles work in this order for each increment. The **Xanadulogical reviewer** checks
that one link remains addressable through both complete endsets, that spans retain provenance, and
that document/cell navigation uses the same operation. The **systems reviewer** checks bounded work,
stable visual focus, asynchronous resolution, and failure behavior. The **codebase reviewer** maps
those decisions onto the current store, bridge, renderer, Vortex actions, and tests. Each increment
ends with a headless interaction test and an explicit record of what is implemented.

1. **Build a pure link navigator.** Read links through `Store::linkView()` and the functional link
   predicates introduced on `main`. Resolve each stored member into ordered document and cell
   occurrences without merging discontiguous members into a covering range. Keep one link ID across
   all occurrences. Test 2×3 endsets, repeated content, same-document and same-cell overlaps,
   missing ends, and link revision changes.
1. **Route activation through one command boundary.** Make `LinkBeams` picks and accessibility
   actions select a link/member rather than following the first strand. Give source side explicitly
   from the hit target or current session, never infer it from the caret's document alone. Route
   `BridgeCoordinator` cell activation and document focus through the same navigator, carrying link
   ID, member, occurrence, and exact range. Preserve current editor selection during preview.
1. **Record branching activity.** Add `system://activity` as a reader-owned store with stable visit
   IDs, walk roots, parent/ordered-child topology, saved targets and view context, references, and
   annotations. Since a Structure link has one neighbor per direction, represent child fanout with a
   first-child and sibling rank (or an equivalent explicit branch-cell scheme), not multiple direct
   links to one slot. Keep activity branch IDs distinct from document `MicroversionId`. Record
   completed semantic transitions, restore a chosen parent or child without recording a new visit,
   and append a sibling when exploration resumes from an old node. Preserve unreadable activity data
   instead of applying the default system-doc reset path.
1. **Compose companion views.** Focus or materialize distant cells through the shared ZigZag
   presentation surface, then resolve their anchors. Keep the source document or cell as a companion
   and animate toward a side-by-side reading arrangement. Maintain an origin tether and whole-link
   context through independent scrolling and dimension changes. Reuse existing beam and satelloid
   geometry where it represents the selected occurrence accurately.
1. **Bound fanout and expose status.** Index endpoint occurrences by link and visible context; avoid
   constructing every left×right ribbon merely to navigate one link. Stage the active link as one
   grouped visual with individually pickable members. Show explicit pending and unavailable states.
   Provide one accessible node per link with child actions or nodes for each side/member, including
   document/cell names and position counts; avoid duplicate node IDs per strand.
1. **Verify the complete path.** Run pointer, sovereign-keymap, and accessibility commands through
   the same headless scenario. Compare their resulting link ID, side/member, occurrence, focus, and
   visit tree. Test document→cell→document, cell→document→cell, distant-cell materialization,
   cancellation, rapid link switching, and two branches from one parent. Save and reload the
   activity store; verify both futures, last active walk, references, annotations, and unresolved
   targets survive. Measure endpoint indexing and visual staging with large endsets and visible
   manifolds before setting frame-budget thresholds.

Current code provides the bridge presentation and cell anchor callback
([`BridgeCoordinator`](../apps/xudu/bridge_coordinator.cpp)), but
[`LinkBeams::traverse`](../apps/xudu/beams.cpp) chooses one strand and infers direction from the
caret. [`placeLinks`](../apps/common/xanadu/link_layout.cpp) reduces disjoint spans to covering
extents before producing a Cartesian set of strands. `BridgeCoordinator::activateCell()` passes only
the first content span, and [`Views::focusSpan`](../apps/xudu/main.cpp) can fall back to the first
left end of an unrelated link. The steps above replace those navigation decisions without changing
the stored link format. They initially use the primary store (store index 0), which supplies both
the bridge and link beams; federated cross-store links require their own authority design.

## Exit criteria

- A 2×3 link remains one selectable link while either endset is browsed independently; no gap is
  highlighted as linked and no arbitrary pair is presented as authorial intent.
- Pointer, keymap, and accessibility entry into a chosen document or cell occurrence produce the
  same exact range and stable link context. Activity Back and Forward restore the chosen visit and
  its member cursors; taking a new route from an old visit retains all prior branches.
- Crossing between document and cell content keeps both contexts legible, preserves the selected
  link through rank/projection changes, and can reach a cell outside the original focus radius.
- Overlapping links are disambiguated; unresolved content remains visible as an explicit,
  cancellable state. The activity store survives restart and supports exploring, referencing, and
  annotating any saved walk. Visited stores receive no navigation operations, and resolution never
  silently targets an unrelated link.
- Measured high-fanout navigation and rendering have bounded work relative to visible members and
  the selected link, with an explicit benchmark and regression threshold recorded alongside the
  implementation.
