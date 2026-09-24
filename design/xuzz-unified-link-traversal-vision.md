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
neighborhood.

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
1. **Return or continue.** Back within the pinned context reverses the last Enter or link selection
   and restores its saved camera, document or cell focus, active side, and both member cursors.
   Preview and member changes do not add a location to that history; ordinary rank walking retains
   its own local navigation. Return to origin always goes to the selection point. The reader can
   instead cross, choose a document occurrence, Enter, or dismiss the context while leaving reading
   focus where it is.

An unambiguous single member and occurrence may be preselected for preview, but Enter remains an
explicit action. When a span occurs in several versions or cells, the reader chooses an occurrence
before Enter. No proximity heuristic silently assigns a target to a many-to-many link.

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

The context is view-local and ephemeral. Browsing, camera movement, rank changes, and Back append no
operations under R8 of [`store-slice-convergence.md`](store-slice-convergence.md). Link content and
attribution remain distinct: endpoint spans address primedia, while type, owner, and curator stay
with the link identity. Explicit stored links retain their own visual identity; transclusion prisms
and format links do not open a link traversal session merely because they are hovered.

## Concepts considered

| Concept                                    | Useful quality                                               | Cost                                                                  | Decision                                                                         |
| ------------------------------------------ | ------------------------------------------------------------ | --------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| Direct jump with a breadcrumb              | Fast for a one-to-one link                                   | A many-to-many link's other members disappear at the moment of travel | Use only as the explicit Enter action after an exact occurrence has been chosen  |
| Spatial overview of endpoint groups        | Makes topology apparent at small scale                       | Dense links can still obscure text                                    | Offer an optional overview of groups and the reader's current comparison         |
| Two endset lists beside the active content | Keeps the whole link available while either side is explored | Needs careful sizing and a stable origin marker                       | Adopt as the default context, emphasizing the chosen two occurrences plus counts |

The following ideas merit prototypes within that default model:

- **Overlap preview:** A small stack of link summaries at a shared span helps a reader choose a link
  before its full context opens. Test whether type and attribution are enough to distinguish links
  without filling the margin.
- **Endpoint search and grouping:** For a large endset, group occurrences by document/version or
  cell neighborhood and allow a text search inside the context. Preserve stored member order when
  the search is cleared.
- **On-demand whole-link overview:** Temporarily pull back to show all member clusters and the
  reader's current comparison. Keep the link context in place so overview never becomes a second
  navigation mode.
- **Reading trail:** Show a small reversible sequence of entered occurrences. The trail expresses
  the reader's path, while the endsets express the author's link; keep those meanings distinct.
- **Ambient provenance:** Let an endpoint reveal its author, version, and primedia source on demand
  without adding permanent chrome to every page and cell.

## Missing and distant targets

An endpoint remains listed when its view is absent. The context names the reason: a document page is
building, a cell is outside the current visible radius, a version cannot be resolved, or remote
content is unavailable. Enter on an unresolved occurrence starts a cancellable pending request. A
distant cell requests a temporary neighborhood around its exact cell; waiting on an ordinary
visible-cell anchor would never bring it into range. Resolution performs no blocking lookup on the
UI thread, and focus changes only when that exact target is ready. Cancellation or switching links
invalidates the pending request so a late result cannot redirect focus. Retry and
choose-another-member actions preserve the same link ID. No failure path substitutes the first
available occurrence.

## Roadmap and evidence

1. **Recover authored members.** Build an occurrence index that preserves each stored span, link ID,
   side, member order, version, and document or cell range. A 2×3 link remains one link even when
   the visible occurrences multiply.
1. **Unify actions.** Route beam, margin, text, cell, keymap, and accessible input through one
   select/choose/cross/enter/back command boundary. Use one stable accessible node per link with
   side, member, and occurrence choices; focusing that node never moves the editing caret.
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
visible and cancellable; rapid link switching does not redirect a pending entry. These journeys are
the acceptance stories for the companion [workflow](ui_workflow_xuzz_navigation.md).

The first implementation uses the primary store (store index 0), which currently supplies both the
bridge and link beams. Federated cross-store links need their own authority design. The vision
requires no change to the stored `Link` shape or `CompactOpNode` format.
