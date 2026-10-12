# Link forge and pouches: spatial design study

Design proposals, 2026-10-12. These images explore possible interactions; they are not working
screenshots. No application code was written. The accepted loom is now in the
[integrated UI plan](../../3d-spatial-hypermedia-navigation-implementation-plan.md); the forge and
pouch options below remain open for review.

The strongest starting point is a forge with **two endset bays and one central draft tablet**, and
pouches that open **one bounded fan beside a searchable list**. Both fit the liquid-glass world
without demanding that every saved reference stay expanded. Hanging collection docks, bowed
receiving strands and hinged inspectors use depth to expose context. Text remains on quiet faces.

## 1. Pouches as collections of references

Image: [typed pouch beside the loom](01-typed-pouch.png).

“Pressure review” contains 146 saved items. Filtering leaves 18, and six are currently visible.
Those are three different counts. The selected pouch unfolds locally; other pouches stay as compact
named docks. Selecting a card opens its inspector without flying the reader away. A pearl tether can
reveal the selected operation's original loom position, while the source remains fixed.

Cards should declare what they preserve and which actions they support:

| Card                   | Preserved subject                                                    | Main actions                                                     |
| ---------------------- | -------------------------------------------------------------------- | ---------------------------------------------------------------- |
| Passage or span bundle | Exact ordered source ranges, gaps, captured state and occurrence     | Inspect; open source; quote; add selected ranges to an endset    |
| Cell                   | Cell identity and captured slice/state; content selection separately | Inspect cell; enter slice; choose content spans                  |
| Operation              | Exact op identity, authority, owning frame and inspected state       | Inspect before/after; reveal in loom; enter state                |
| Captured page          | Ordered page footprint at a snapshot and layout context              | Inspect captured footprint; follow selected passage              |
| Query or history lens  | Recipe, scope, match/boundary rules and separately recorded results  | Inspect saved results; rerun explicitly; select concrete matches |
| Reader walk            | Reference to a branching set of completed visits                     | Inspect walk; resume a chosen visit                              |
| Link or link draft     | Stored Link identity, or draft's ordered members and destination     | Inspect relation; open forge; resume draft                       |

Current pouches persist individual content cards. Composite span cards, arbitrary-op cards, query
cards, walk cards and durable drafts require extensions. Saving a reader reference writes to the
reader's collection, not the visited source. Naming, moving or dismissing a card does not move or
erase its subject. **Dismissed items** makes the existing dismissal concept recoverable through a
proposed retrieval UI.

Useful pouch ideas to try:

- **Collect first, decide later.** A generous **Save selection** target accepts the reference type
  already selected. It previews the type before release. A dropped op stays an op; it does not
  become a quotation of its preview text.
- **Open one pocket.** Groups by source, kind or purpose become counted wraps. Only the active group
  fans out. The list can sort and filter without changing the reader's authored grouping.
- **Compare two cards.** Pin two inspectors to compare exact source identity, version and context.
  Distinguish “same source,” “matching text” and “explicitly linked.” Only actual sharing gets gold.
- **Carry an investigation.** Keep a gauge cell, its troublesome op, a saved `3.14` lens, selected
  results and an unfinished link together. Different cards invoke different inspectors while the
  collection retains the common task context.
- **Offer a return point.** Entering from a card preserves the previous reading place through the
  activity system. Inspection remains local. A pouch is not another automatically recorded walk.
- **Recover rather than disappear.** Unavailable references retain their identity and source state,
  with retry/removal actions. Preview bytes not held, unresolved authority, locked content and a
  missing state have distinct labels.

Two further options merit experiments: a temporary collection dock attached to a selected scene, and
a saved-query pouch that refreshes only on request. A live result list and a curated collection must
be visibly different. Refreshing a query must not add new members to an authored link.

## 2. Forge as a spatial assembly bench

Image: [disjoint passages and cell content](02-disjoint-link-forge.png).

The reader chooses two separated sentences from Lab notes for the left endset, then a review
paragraph and selected content from C17 and C24 for the right. The page companion highlights the two
exact sentences and leaves the intervening paragraph unmarked. An ordered excerpt belt can carry the
two selections as one capture group, while the forge shows their two distinct members.

Each bay has a large drop target, insertion position and ordered member list. It supports inspect,
reorder, remove and undo through pointer and keyboard actions. A draft removal changes the draft; it
never deletes source content. Source companions can reveal the chosen member in its original page or
cell. The central tablet owns the proposed relation, type, prominence and receiving store.

The bench shows **Left 2 / Right 3**, one draft identity and one proposed Link. It does not imply
six separate links. Both sides use the same selected relation hue; left/right labels, ordinal ports
and shapes distinguish the bays. Staged guides are seamed. An authored relation receives the actual
stored Link identity after successful creation.

**Stage → inspect → forge** is the key progression. Dropping references stages eligible spans;
**Forge link** authors the relation. **Preview relation** opens the same two-endset reading model
used to browse an existing link, while retaining an unmistakable draft status. Closing offers **Keep
draft** and **Discard draft** separately. A **Return** action restores the source context.

The central destination is explicit and pinned: receiving store, branch, base state and the intended
authoring mode. Browsing another document or an old source state does not redirect it. Before
implementation, resolve whether forging appends at that writable head or creates a declared child
from an older base; never silently redirect to another structure head. Show the actual author
identity and verification state, rather than the current placeholder `local_author`.

The content-link consequence must be readable: **Links selected source spans; occurrence chooses
entry context.** If those addresses appear in three editions and two cells, the member can have
multiple occurrences. Selecting C17 at V8 chooses where the reader enters; it does not limit the
stored content relation to C17 alone. An occurrence-specific relation would need an explicitly
supported representation.

## 3. Keep a large forge usable

Image: [40 left members and 60 right members](03-dense-endsets.png).

The same bench expands to 100 members through source packs and independent windowed lists. Three
left packs total 40; three right packs total 60. Grouped conduits meet one tablet. All members
remain individually reachable without 2,400 pair connections. A pack's count is not an occurrence
count.

Each bay keeps its own member cursor and filter. Show total, filtered and visible counts. One bay is
active; the other side's remembered selection is quieter. Selecting left member 21 reveals its exact
selected range or bundle and a separate occurrence selector. Its five appearances can be shown as
three previews plus a +2 control. A chosen occurrence changes entry context, not member addresses.
The ordinary list and spatial fan use the same model and commands.

Further density mechanisms:

- Pull one member outward to inspect it, leaving the pack in place. Releasing returns its preview to
  the pack; **Pin inspector** keeps it open. The member's order stays unchanged.
- Reorder within an explicit ordered endset. Source grouping is a view; changing group sort must not
  silently reorder the authored member list.
- Search a bay by excerpt/source/type/state, then reveal the result's true ordinal. A filter does
  not remove hidden members from the draft.
- Expand an excerpt belt to show its disjoint pieces and gaps. Preserve capture-group counts
  separately from endpoint-member counts.
- Inspect one actual relation at a time. Background sharing can stay present at lower brightness
  without turning every possible relation into a bright crossing ribbon.

Rendering is bounded by visible packs, labels and previews; stored membership is not artificially
truncated to fit the screen. Preparation, availability resolution and validation run outside draw
work. Counts in these images are illustrative, not performance claims.

## 4. Save and reopen an unfinished relation

Image: [draft resumption and destination review](04-reopen-draft.png).

A user leaves Draft L7 in “Pressure review.” Reopening restores both ordered endsets, relation,
saved description, prominence, source states, grouping and the chosen destination before previews
arrive. The pictured third member remains C24 contents at V8, including its saved `22 °C` excerpt;
pending validation does not replace it with a different endpoint. A reference whose required
identity validation is pending remains a numbered placeholder with its authority, state and
retry/remove actions. The other members stay intact.

**Preview not held** and **Reference unresolved** are different. Known valid addresses may be
sufficient for a link even while preview bytes are absent, depending on the declared authoring
policy. Disable commit when required reference or destination validation is incomplete, not merely
because an optional thumbnail is missing. Never substitute a newer state or same-worded passage.

After resolution, preview the complete relation and confirm the pinned receiving store/branch/base
and authoring mode. One forge action creates one Link identity, potentially through multiple
Structure operations. Current transfer preflight covers both endsets, but complete rollback for
every mid-commit storage failure is not established. The implementation needs a defined atomic or
recoverable protocol, with failure preserving the draft and preventing duplicate links on retry.

Ephemeral staging should keep destination deployment changes separate until an explicit authoring
action. Current staging can adjust private scroll bindings, even though it authors no Link. Saving a
durable draft deliberately writes to the reader's pouch. Those are different storage boundaries.

## 5. More journeys worth testing

| Intent                                  | Proposed mechanism and boundary                                                                                                                                                                                 |
| --------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Quote a passage, then comment on it     | **Quote** transcludes selected source into an authored destination. Then forge an explicit comment link to selected commentary. Quotation itself creates no extra xanalink.                                     |
| Pour a pouch into a forge bay           | Open a chooser showing eligible concrete content selections, order, count and exclusions; explicitly add them. An op, query or walk is not a content endpoint by default.                                       |
| Link search results                     | Select exact known ranges from saved or rerun results. Commit freezes that selection. Later matches can suggest an explicit revision; they do not change the stored Link.                                       |
| Link an empty cell                      | Explain that a content link requires spans; do not manufacture primedia from the cell's label. Whole-cell structural relations are a separate design question.                                                  |
| Re-drop the same passage                | Reveal the existing exact member and offer an explicit duplicate policy. Equal text at different addresses is distinct; overlapping selections are shown before any merge.                                      |
| Share one reference between two pouches | Distinguish collection-card membership from subject identity. The source is shared; renaming or dismissing one card must not secretly affect another collection.                                                |
| Revise a stored link                    | Open a clearly labelled revision draft, show endset additions/removals against the selected state, then author the declared new state. Do not treat a draft edit as an in-place overwrite of immutable history. |
| Resume a collection offline             | Restore reference identities and saved context first; fetch previews later. Preserve known, pending and unavailable states, with cancellation and retry.                                                        |
| Collect computed results                | Retain a query reference or explicitly persist scratch output before using it as durable content. Ephemeral arena addresses cannot silently become stored span members.                                         |
| Compare the gauge reading with pi       | Keep the two text contexts separate, inspect query rules, then choose whether to create an explicit relationship. Matching `3.14` is not shared source or mathematical equivalence.                             |

## 6. Existing foundation and proposed work

The inspected main branch and spatial worktree use the same forge/pouch semantics. Current
[`ClaspLinkForge`](../../../apps/common/ui/xanadoc/clasp_link_forge.cpp) stages two ordered vectors
of span cards, creates one Link with full endsets/type/tier, and chooses the destination from the
active caret. Its bench is not serialized and its preview is mainly the last item plus a count.
Current [`PouchItem`](../../../apps/common/xanadu/pouch_zone.hpp) preserves an individual span and
source context. A multi-span quotation run can be received, but currently becomes separate cards;
retained composite capture identity is new work.

Proposed device-free prerequisites are the shared exact-selection descriptor, typed durable
reference schema/dispatch, ephemeral draft model, explicit destination/base contract, complete
preflight and defined commit/retry behavior. These precede new bays, pouches and animations. Reuse
the [existing navigation contract](../../ui_workflow_xuzz_navigation.md) for the draft relation's
member/occurrence inspection and eventual authored-link browsing.

All four images use the built-in image-generation tool. The [prompt record](prompts.md) preserves
exact generation and edit prompts, inputs and final assets. The final visual review corrects gold
reference tethers, relation hue, occurrence labels, exact range visibility and close/return actions;
the text defines the full behavior beyond what fits in a single frame.
