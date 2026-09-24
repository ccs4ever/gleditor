# Xuzz Unified Link Traversal Vision

**Status:** Proposed interaction vision; Xuzz does not yet implement the complete journey.\
**Audience:** Readers, interaction designers, and engineers building the Xuzz continuum.\
**Companion:** [`ui_workflow_xuzz_navigation.md`](ui_workflow_xuzz_navigation.md) defines the
implementation contract and verification work.

## Reader promise

A link is a place a reader can explore. Selecting it reveals its identity and both complete endsets.
The reader can inspect either side, move among discontinuous spans, enter a chosen occurrence, read
onward through a xanadoc or ZigZag rank, and return with the link still intelligible. A document
passage and a cell's content use the same gestures and the same link context. The interface
preserves where the reader came from even when the destination's shape changes from a page to a cell
neighborhood. Each completed journey becomes part of a reader-owned tree of walks that can be
reopened, branched, referenced, and annotated across sessions.

This follows the stored [`Link`](../apps/common/xanadu/ops.hpp): one ID, type, attribution, and two
ordered lists of `PrimediaSpan`. Left and right are endset names; neither list is intrinsically a
source or destination. A visual strand between two occurrences does not assert that the author
paired those members. Content may recur in many versions or cells, and several links may cover the
same content. The vision keeps those facts visible instead of flattening them into one jump.

## Where Xuzz stands today

Xuzz already embeds a ZigZag presentation in the xudu scene through
[`BridgeCoordinator`](../apps/xudu/bridge_coordinator.cpp). The bridge supplies a manifold view and
cell anchors to beams. Cell picks use the shared renderer; ZigZag contributes an accessibility
source to the shared publisher. The navigation behavior is narrower:

- [`placeLinks`](../apps/common/xanadu/link_layout.cpp) computes a covering extent for an endset in
  each visible document or cell and emits left×right strands. A covering extent can include gaps
  between authored spans; the strands can grow as a Cartesian product.
- [`LinkBeams::picked`](../apps/xudu/beams.cpp) follows a picked strand immediately.
  `LinkBeams::traverse` infers the near side from the caret's document, and cell destinations
  receive a pulse or alignment rather than exact content focus. The accessible action finds the
  first strand with a matching link ID.
- `BridgeCoordinator::activateCell` passes only the first content span.
  [`Views::focusSpan`](../apps/xudu/main.cpp) chooses the first matching document occurrence and can
  fall back to a left end of another link. A cell outside the current ZigZag neighborhood has no
  visible anchor; link-driven materialization is proposed below.
- Xudu's current Back and Forward actions walk document microversions. They do not record where the
  reader traveled. Xuzz has no persistent, branchable record of reading activity.

These are implementation facts, not the intended reader experience. The bridge supplies a shared
scene; the proposed traversal supplies a shared interaction language.

## A journey through one link

Consider link 42, whose left endset has two discontinuous spans and whose right endset has three.
One right member appears in two xanadoc versions; another appears in a ZigZag cell with multiple
content spans.

```text
source page                         link 42                         companion space
Left  [1/2] [2/2 ◉]         Comment · owner · origin         Right [—/3] Choose target
      chosen occurrence             link stays pinned                    no target yet
```

1. **Discover.** Hovering a linked passage or beam gives a brief preview without moving the caret.
   If links overlap, a compact chooser distinguishes their type, attribution, and endset summaries.
   Selecting link 42 from an exact span captures that occurrence as the near-side cursor and origin.
   A beam body with no identifiable member captures only the link identity, then asks the reader to
   choose an endpoint.
1. **Explore.** The reader moves from left member 2 to left member 1 and reads summaries of right
   members 1–3 without choosing one. The two member cursors are independent: after right member 3 is
   chosen, changing the left cursor leaves that right choice intact. Each member can expand into
   occurrences labeled by document/version or cell and intra-cell range. The origin remains marked
   even after focus moves elsewhere.
1. **Cross and enter.** Cross switches the active endset and previews it; focus stays put. With
   several opposite members, an unset cursor opens a chooser with no default. Cross preserves a
   member and occurrence the reader already chose on that side. After selecting right member 3 and
   its cell occurrence, Enter moves focus to that exact range. Selection, crossing, and entering are
   separate actions for pointer, sovereign keymap, and accessibility input.
1. **Read onward.** The chosen cell and its immediate rank neighborhood come alongside the source
   page. A content-anchored connection and origin marker keep the relationship visible while the
   reader walks dimensions or changes projection. The link context stays pinned until dismissed;
   when focus leaves the linked range, it says so without pretending the new cell is an endpoint.
1. **Return or continue.** Activity Back follows the current visit's parent and restores its saved
   document or cell focus, link context, and view. Activity Forward follows a chosen child. When a
   visit has several children, a branch chooser shows every future and remembers the last one
   followed without hiding its siblings. Returning to an earlier visit and entering a new endpoint
   adds a child while preserving its existing futures. The reader can also cross, choose a document
   occurrence, Enter, or dismiss the pinned link context.

An unambiguous single member and occurrence may be preselected for preview, but Enter remains an
explicit action. When a span occurs in several versions or cells, the reader chooses an occurrence
before Enter. No proximity heuristic silently assigns a target to a many-to-many link.

## Walks are part of the docuverse

The reader's route is a persistent tree of visits, with multiple roots for independent walks. A
visit names the place reached, the action that reached it, and its parent visit. Its children are
all the directions the reader later took from that point. Following a link, moving to another cell
on a rank, opening a document or version, and jumping to a passage create visits after arrival.
Hover, endpoint preview, camera motion, and scrolling stay transient; the current view is captured
when a visit is completed or checkpointed. A failed or cancelled entry does not create a visit.

```text
Walk A:  v1 open page ── v2 enter cell ── v3 follow rank
                         ├────────────── v4 cross link into document
                         └────────────── v5 explore another rank
Walk B:  v6 open another source ── v7 inspect its linked cell
```

Each visit has a stable activity identity, a parent, ordered children, an exact document
version/range or cell/range target, and the active dimension or projection. When a link led there,
the visit also records that link's authority and ID, both endset member/occurrence selections, and
the source companion. The saved view includes enough camera and reading context to make restoration
recognizable. Revisiting the same content along another route creates another visit, since the route
and link context are part of what the reader may want to remember.

Activity Back selects the parent visit. Activity Forward selects a child; a fork opens a chooser
with previews of every child. The last followed child may be highlighted as a convenience, but no
branch disappears. Choosing an old visit previews its place in the tree; Enter restores it without
rewriting history. The next new transition from that visit appends another child. Return to origin
selects the origin visit recorded for the active link, rather than reconstructing a temporary stack.
Existing document hypertime controls still navigate *document versions* and need distinct labels and
sovereign keymap actions from Activity Back and Forward.

A **Walks** view exposes roots, branches, the current visit, and short previews. A reader can open
any walk, resume the last active one after restart, name a walk, bookmark a visit, attach a note,
and refer to a visit or walk from another activity note. The tree's edges describe the reader's
movement; Xanadu link beams describe authored relationships between content. The two use distinct
visual marks, and selecting a visit reveals the link context saved with that visit. A visit whose
target has moved or is unavailable stays in the tree with its original address and a clear recovery
choice; restoration never silently targets current content elsewhere.

### Reader-owned activity store

The proposed `system://activity` is a separate, private-by-default system store, not another
document's hypertime branch. It stores activity visits as persistent structure with stable IDs,
ordered child branches, walk roots, references, annotations, and view checkpoints. Its tree takes
OSMIC's nondestructive branching as an interaction model; activity IDs and edges remain distinct
from `MicroversionId` and from a document's operation ancestry. Durable targets name their store
authority and version as well as the exact span or cell; a session-local store index or `CellRef`
alone is insufficient across sessions or stores.

This is a deliberate extension to [R8](store-slice-convergence.md): live cursor and camera state
remain ephemeral, and visiting content appends nothing to the visited document or slice. Completed
reader transitions append only to the separate activity store. Its record is user data, so an
unreadable activity store must be preserved and reported, not replaced with generated defaults as
configuration stores can be. Recording is local and private until the reader explicitly publishes or
exports a walk. Cross-store references from ordinary xanadocs to activity visits depend on the
federated reference work; references and annotations within the activity store can precede it.

### Reuse OSMIC without confusing its two histories

The activity store's append-only operation spool records **changes to the activity record**:
creating a visit, annotating it, naming a walk, or adding a reference. Its `MicroversionId` names a
state of that store. A visit has its own stable identity and parent visit; its children record the
reader's alternative routes. An annotation added to an old visit changes the activity store but is
not another step in the reader's walk. All walks must coexist in one current activity view even when
a reader resumes from an older visit.

Use Structure operations and the Manifold to persist visit cells, their parent and ordered-child
relationships, notes, and references. A derived, rebuildable walk index can keep roots, child lists,
target-span lookups, ancestor jumps, and lowest common ancestors. That makes questions such as
“where did these walks diverge?” and “which walks passed through this passage?” practical. The index
is an acceleration of the visit graph, not a second durable source of truth. Its ancestor index can
borrow Chronofilade's binary-lifting idea, but must follow *visit parents*.

The existing [`Chronofilade`](../apps/common/xanadu/enfilade/chronofilade.hpp) follows *operation
parents* and rebuilds document edit-decision-list versions. Structure operations are text replay
no-ops, so it does not itself reconstruct or navigate the activity visit tree. It remains useful for
the activity Store's own version history where text EDLs are involved. The activity store needs its
own lossless Structure replay and derived visit index, with activity `MicroversionId`s kept distinct
from visit IDs. Notes belong in the reader's permascroll; the Store carries their addresses, never
primedia bytes. An unreadable activity store is preserved and reported, because the walks and
annotations are user data.

## The visible language

The selected link has one compact context anchored to the viewport so it remains available while
content moves. It shows identity, type, owner or curator, the origin, and each endset's count. A
chosen member reads `Left i/N` or `Right j/M` with its occurrence; an unset side reads
`Right —/M · Choose target`. Content-anchored tethers point from the context to visible occurrences.
When both sides have chosen occurrences, they receive full marks and one clear connection
representing the reader's current comparison, not an authored pair. Other members appear as grouped
marks and counts that can be expanded on demand. This gives the reader a whole-link view without
drawing every possible left×right strand.

Documents and cells share the same selected-span treatment, link context, and entry action. A cell
reveals its local ranks as the destination comes forward; the source remains a legible companion
rather than disappearing behind a new mode. Motion maintains spatial orientation, while a
reduced-motion preference uses short fades and immediate stable placement. Link context and focus
remain readable during either transition.

The same context is available to a screen reader as one link with left and right member groups,
occurrence labels, and selection and entry actions. Moving accessibility focus to it leaves the
editing caret untouched. Pending and unavailable states are announced alongside the chosen member.
The sovereign keymap binds the same semantic actions without fixing their chords in the interface
design.

The live link context is view-local; its selected state is captured in completed activity visits.
Link content and attribution remain distinct: endpoint spans address primedia, while type, owner,
and curator stay with the link identity. Explicit stored links retain their own visual identity;
transclusion prisms and format links do not open a link traversal session merely because they are
hovered.

## Concepts considered

| Concept                                    | Useful quality                                               | Cost                                               | Decision                                                                         |
| ------------------------------------------ | ------------------------------------------------------------ | -------------------------------------------------- | -------------------------------------------------------------------------------- |
| Direct jump with a linear breadcrumb       | Fast for a one-to-one link                                   | Revisiting a prior point can discard other futures | Use explicit Enter, with every completed route retained in the activity tree     |
| Spatial overview of endpoint groups        | Makes topology apparent at small scale                       | Dense links can still obscure text                 | Offer an optional overview of groups and the reader's current comparison         |
| Two endset lists beside the active content | Keeps the whole link available while either side is explored | Needs careful sizing and a stable origin marker    | Adopt as the default context, emphasizing the chosen two occurrences plus counts |

The [prototype collection](ui/prototypes/README.md) expands the reader walkthrough and technical
model for each idea within that default context:

- **The Flap:** Fan the document and cell versions that still manifest the selected link's spans
  into two branch-aware onion-skin wings. Keep the link as a fixed hinge while each side scrubs
  independently. The [prototype](ui/prototypes/flap.md) defines the reader journey, exact
  membership, activity behavior, and prototype questions.
- **Overlap preview:** A small stack of link summaries at a shared span helps a reader choose a link
  before its full context opens. Test whether type and attribution are enough to distinguish links
  without filling the margin. See [Overlap preview](ui/prototypes/overlap-preview.md).
- **Endpoint search and grouping:** For a large endset, group occurrences by document/version or
  cell neighborhood and allow a text search inside the context. Preserve stored member order when
  the search is cleared. See [Endpoint search](ui/prototypes/endpoint-search.md).
- **On-demand whole-link overview:** Temporarily pull back to show all member clusters and the
  reader's current comparison. Keep the link context in place so overview never becomes a second
  navigation mode. See [Whole-link overview](ui/prototypes/whole-link-overview.md).
- **Branch previews:** Show a child's destination, link type, and note in the Forward chooser. A
  compact preview gives rapid access to a known route while the Walks view shows the full tree. See
  [Activity walks](ui/prototypes/activity-walks.md).
- **Ambient provenance:** Let an endpoint reveal its author, version, and primedia source on demand
  without adding permanent chrome to every page and cell. See
  [Ambient provenance](ui/prototypes/ambient-provenance.md).

## Missing and distant targets

An endpoint remains listed when its view is absent. The context names the reason: a document page is
building, a cell is outside the current visible radius, a version cannot be resolved, or remote
content is unavailable. Enter on an unresolved occurrence starts a cancellable pending request. A
distant cell requests a temporary neighborhood around its exact cell; waiting on an ordinary
visible-cell anchor would never bring it into range. Resolution performs no blocking lookup on the
UI thread, and focus changes only when that exact target is ready. Cancellation or switching links
invalidates the pending request so a late result cannot redirect focus. Retry and
choose-another-member actions preserve the same link ID. No failure path substitutes the first
available occurrence. A saved visit to unavailable content remains addressable and annotatable in
Walks even while its destination cannot be opened.

## Roadmap and evidence

1. **Recover authored members.** Build an occurrence index that preserves each stored span, link ID,
   side, member order, version, and document or cell range. A 2×3 link remains one link even when
   the visible occurrences multiply.
1. **Unify actions.** Route beam, margin, text, cell, keymap, and accessible input through one
   select/choose/cross/enter command boundary. Use one stable accessible node per link with side,
   member, and occurrence choices; focusing that node never moves the editing caret.
1. **Persist branching walks.** Add a reader-owned `system://activity` store and a visit recorder
   that appends successful focus transitions, preserves siblings when a reader branches, and
   restores the last active walk after restart. Build Activity Back/Forward, branch selection,
   Walks, references, and annotations over stable visit IDs. Keep this store out of
   default-regeneration recovery and separate its actions from document hypertime.
1. **Complete the bridge journey.** Carry exact occurrence identity through `BridgeCoordinator`,
   materialize out-of-radius cell neighborhoods, and preserve the source companion and pinned link
   context across document/cell transitions and dimension changes.
1. **Bound presentation and validate feel.** Stage the chosen connection and visible member groups
   without enumerating every pair. Measure large-endset and radius-change behavior before fixing a
   frame budget. Check transition state, cancellation, and reduced-motion behavior with interaction
   tests; inspect headless backend captures for visual continuity.

The implementation is successful when a reader can select one of several overlapping links, browse
both sides of a discontinuous 2×3 link independently, choose an exact repeated occurrence, enter a
cell and return through a document, and still identify the same link and origin at every step.
Pointer, keymap, and accessibility paths must reach the same occurrence. Missing targets remain
visible and cancellable; rapid link switching does not redirect a pending entry. From an earlier
visit, taking a new route must create a sibling while both old and new futures survive restart. The
reader must be able to preview, revisit, reference, and annotate either branch. These journeys are
the acceptance stories for the companion [workflow](ui_workflow_xuzz_navigation.md).

The first implementation uses the primary store (store index 0), which currently supplies both the
bridge and link beams. Federated cross-store links need their own authority design. The vision
requires no change to the stored `Link` shape or `CompactOpNode` format.
