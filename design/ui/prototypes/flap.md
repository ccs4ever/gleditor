# The Flap: a Link's Living Versions

**Status:** Tentative interaction prototype, not implemented.\
**Context:** [Xuzz unified link traversal](../../xuzz-unified-link-traversal-vision.md) and its
[workflow](../../ui_workflow_xuzz_navigation.md).

## The gesture

With one butterfly link selected, **Flap** opens its left and right endsets like wings around a
stable hinge. Each wing fans out the document and ZigZag cell versions in which its linked primedia
spans still appear. Nearby leaves are translucent onion skins; the chosen leaf is fully legible.
Ancestors recede behind the hinge, descendants open toward the reader, and hypertime forks spread
into smaller fans. The two wings can be scrubbed independently while the complete link, its
attribution, and the reader's origin remain visible.

```text
       LEFT WING                    HINGE                    RIGHT WING
  parent / fork / child      link ID · type · owner      parent / fork / child
      [page v2]  [page v3]    left 2/2 | right 1/3    [cell v7]  [page v9]
          selected occurrence ╲ reader comparison ╱ selected occurrence
```

The hinge is the stored link, not a particular rendered strand. Its left and right lists stay
ordered and complete. A line between two previewed leaves shows a reader-chosen comparison; it never
claims that the link author paired those members. A wing can hold several endset members, documents,
cells, and occurrences. The other wing does not change when one is scrubbed.

## One reader journey

1. Select an exact link occurrence and invoke **Flap**. The current content remains in focus. Its
   leaf is marked on the appropriate wing; the other wing opens with its endset visible and no
   arbitrary target chosen. Selecting a beam without an exact near member opens both wings
   unselected.
1. Choose a right member. The wing shows its manifestations by document or cell, then by OSMIC
   branch. Fan an ancestor backward to see the passage in an earlier edition; open a descendant fork
   forward to see how that same primedia survived beside new material. A version with two quotations
   of the same span offers two exact occurrences inside one leaf.
1. Pin that right leaf and scrub the left wing independently. The two chosen spans align visually
   while their surrounding pages or cell neighborhoods remain readable. Labels keep version, branch,
   source, link owner, and full or partial span coverage clear.
1. **Enter leaf** moves reading focus to the chosen exact occurrence, including its version and
   intra-cell range. The link hinge and opposite wing remain available. Entry creates one child in
   the reader's activity walk; merely opening, scrubbing, or pinning the flap creates none.
1. Before entry, Escape closes Flap and restores the prior live arrangement. After entry, Close
   removes the overlay around the new reading focus; Activity Back can return to the earlier visit.
   The saved entry visit can restore the chosen wings, and another route from it creates a branch in
   the activity tree without deleting this one.

## What belongs on a leaf

A leaf is a *manifestation of one stored endset member*, identified by link authority and ID, side,
member index and `PrimediaSpan`, store authority, document or cell identity, `MicroversionId`, and
exact occurrence range. Store indices, current page offsets, and `CellRef` alone are insufficient
durable cross-session addresses. Repeated quotations of one span are distinct occurrences within the
same version; several members in one version remain separately selectable. No same-text search can
substitute for matching primedia addresses.

The first query should classify candidate versions as follows:

| State      | Meaning                                                       | Presentation                                       |
| ---------- | ------------------------------------------------------------- | -------------------------------------------------- |
| Full       | One occurrence preserves the complete linked primedia span    | Normal leaf, eligible for exact entry              |
| Fragment   | The version contains only an intersecting piece of the member | Faded boundary leaf with the surviving range named |
| Absent     | No piece of that member occurs in the version                 | Omit from the fan; show a gap at a relevant fork   |
| Unresolved | Version or primedia cannot currently be read                  | Keep an addressable placeholder and retry action   |

This is a content-addressed query over versions. The current `Link` table sits beside versions, so
an older version may contain the span even if it predates the operation that registered the link.
Such a leaf is a manifestation of the linked *content*, marked “before link declaration” when that
provenance can be resolved. A later version without the span is absent even when the link record
still exists. The selected link record and its endsets are held fixed during a flap; a future link
revision model would need to label revisions explicitly. Cell leaves require the cell to exist at
that version and its content runs to contain the member's address range.

## Space, time, and control

The central leaf on each wing is the reader's chosen occurrence. Depth expresses ancestry relative
to that leaf, while lateral spread expresses forks and different documents or cells. Versions on
incomparable branches keep visible branch labels; a timestamp may help sort them but cannot turn
them into one historical line. The hinge stays still while a wing moves, so the reader can compare
left v3 against right v9 and then right v12 without losing left v3.

The first prototype can show a bounded window of leaves around each selected branch point, with
counts and an expand action for hidden leaves. The selected leaf and its exact linked range remain
opaque and readable; other leaves fade without obscuring their provenance. A reduced-motion mode
uses two branch-aware lists with the same commands and no depth-dependent information. Pointer,
sovereign keymap, and accessibility expose **Open/Close Flap**, **Choose side/member**, **Choose
branch/version/occurrence**, **Preview**, **Pin comparison**, and **Enter leaf**. Scrubbing
previews; it does not change the editing caret or append activity.

Loading a distant cell or unavailable version is cancellable. Focus changes only after the exact
leaf resolves; failure leaves the flap and prior reading focus intact. The saved activity visit
records the chosen link, both wing selections, exact target, and useful view arrangement so it can
be restored across sessions. Link versions and activity visits are separate trees.

## Technical explanation

The flap query begins with one selected link authority and ID and its fixed ordered endsets. For
each stored member, enumerate candidate Store versions by OSMIC ancestry around the chosen anchor,
then resolve document `Version` runs and version-specific ZigZag cell content runs against that
member's primedia address. Preserve every exact occurrence; a version can carry several quotations
of one member. `Version::occurrencesOf()` identifies candidate overlaps, but full coverage requires
checking the underlying address sequence. One member's leaf never implies a pair with a member on
the other wing. Link metadata lives beside versions, so a content manifestation may predate the link
declaration and needs that provenance label.

Represent each leaf by side, member index, store/document or cell authority, `MicroversionId`,
occurrence range, coverage state, and resolution generation. The version fan is a projection of the
OSMIC DAG: parent/child ancestry defines depth, while sibling branches remain distinguishable.
Chronofilade can help rebuild document versions and jump through their operation ancestry;
version-specific cell manifolds need their own incremental or bounded replay path. The current
Spanfilade covers indexed open views and cells, not every historical version, so a lazy historical
membership index or bounded candidate scan is needed. Remote and missing versions keep placeholders
and cancellation tokens.

The view holds two independent wing selections and a fixed link hinge. Stage only a bounded window
of leaf geometry, and keep the selected exact span readable. Existing Xudu onion-skin transforms and
opacity animation can be reused for document leaves, while the current flat open-document cycling
cannot choose the flap's membership or forks. Cell leaves require version-specific content views and
exact anchors through Xuzz's shared presentation. Reduced motion uses branch-aware lists with the
same leaf identities. The actions Open/Close Flap, Choose wing/member/branch/version/occurrence, Pin
comparison, Preview, and Enter leaf belong to the sovereign keymap and accessibility command
surface. Preview and pin add no activity; successful Enter records one visit with both wing states.

## Prototype questions and proof

Begin with a pure document-leaf query and a small branch-aware fan, then add cell leaves through the
shared Xuzz presentation boundary. Keep resolution and version rebuilding off the UI thread. The
existing Xudu hypertime graph can supply branch labels and selection, while its onion-skin mode
supplies only presentation mechanics.

The first prototype should answer these questions with a 2×3 discontinuous link, a transcluded
duplicate, an OSMIC fork, one cell, a partial remnant, and one unavailable version:

- Does a fragment belong in the main fan, or only at the edge where full coverage is lost?
- Is ancestry depth intelligible when several document and cell histories coexist under one wing?
- Should a pinned comparison survive closing Flap as an activity checkpoint, or only after Enter?
- How many translucent leaves remain readable before the view should collapse to branch summaries?

Success means the reader can fan either wing backward and forward, identify precisely why every leaf
is present, compare two independently chosen manifestations, enter either exact span, and return
without losing the link or either branch of their activity walk.
