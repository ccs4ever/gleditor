# Hypertime Loom: journeys that test the concept

Design study only. No application code is written or changed. The images depict proposed
interactions using invented histories, labels, and result counts. They are not captures of working
features or evidence of performance.

The loom works best as an overview of authored history with readable, temporary investigations
pulled out of it. The source ancestry stays visible; a lens changes which operations are presented
in detail. A result appearing in two contexts is the same operation, not a new stored operation.
Solid silver carries ancestry, pearl tethers carry correspondence, and seamed guides carry filtered
view order. Gold is reserved for actual shared source content. Matching text, numeric equality, and
suggested meaning do not establish that identity.

**1. Keep this operation and return to it later.**

The writer notices an unexpected deletion in Draft B, selects its operation chip, and chooses **Save
to pouch** or drags its reference into “Check later.” The original chip stays where it is. The pouch
receives an operation reference card, distinct from a quotation card or a saved query. It carries
the exact operation identity, originating store/object, inspected state, and optional selected
occurrence. Saving belongs to the reader's pouch; it does not edit the visited document.

On reopening, the card first inspects that operation with before/after companions. **Enter state**
is a separate action. The current reading position stays available as the return point. A closed
store or pending content leaves a named reference with its availability and retry action; it never
substitutes the newest state. Dismissing the card removes it from the active pouch, not the source
history. This journey requires a new arbitrary-operation pouch item type; a Delete op cannot be
passed off as a cell-creation reference.

Image: `01-save-and-reopen-op.png`.

**2. What happened to this exact passage?**

The reader selects `ABCDEF` at a particular occurrence and drops it onto the generous **History lens
· Touches** target. The same command is available from the selection menu and keyboard. The subject
card preserves the ordered source ranges, originating snapshot, occurrence, and branch scope.
Discontinuous ranges keep their gaps; repeated appearances of the same source remain separately
selectable.

The investigation finds three kinds of effect:

- An insertion makes `ABCDEF` appear as `AB[xy]CDEF`. The new `xy` addresses do not overlap the
  original bytes: this is an insertion into the selected occurrence, splitting its presentation.
- A deletion changes `ABxyCDEF` to `ABxyEF`. The removed `CD` is recovered from the operation's
  before-state and remains inspectable in history.
- A rearrangement changes `ABxyEF` to `EFABxy`. The result identifies the recorded move and shows
  exactly which source fragments moved.

Each result carries a reason and an exact before/after preview. Candidate operations are examined in
their own parent state and owning frame; the coordinates of a delete or move cannot be treated as
immutable source addresses. An unrelated insertion earlier in the document merely shifts the
passage's offset and is excluded unless **Context shifts** is enabled.

**Original source** freezes the seed ranges. **Evolving passage** is an explicit alternative that
also includes newly inserted material according to visible boundary rules. The original query is
never silently widened. **This occurrence** and **All uses of this source** are separate choices:
moving one of two quotations must not make both look moved.

The parallel result track is filtered order, with counted gaps and branch badges. It does not
connect results as if they were direct stored parents. Selecting opens a preview; entering the state
remains separate. A zero-result answer is conclusive only for the displayed completed scope.

Image: `02-span-effects.png`.

**3. Find the same text in different contexts.**

The reader enters `3.14` in **Content history**. The illustrated query uses **Text · Whole token**,
searching current and historical occurrences across the chosen store branches. It finds “Pi is
approximately 3.14” in Math notes and “3.14 bar” in a pressure-gauge cell. These remain distinct
results, with object, branch/state, exact occurrence, and surrounding text or dimensional context.
Neither meaning nor equal spelling gives them a gold sharing strand.

The match rule is visible and changeable:

| Choice        | Question answered                                | Important boundary                                                                                 |
| ------------- | ------------------------------------------------ | -------------------------------------------------------------------------------------------------- |
| Literal text  | Where does this character sequence occur?        | Substring `3.14` also matches the prefix of `3.14159`.                                             |
| Whole token   | Where is this complete textual token used?       | Token boundaries are part of the declared query rule.                                              |
| Number        | Where is this numeric value represented?         | Typed values or explicit parsing/normalization are required; unit context stays visible.           |
| Related ideas | What other expressions might represent the idea? | `pi`, `π`, or approximate values require explained aliases, links, rules, or inferred suggestions. |

`3.140` may qualify through declared numeric normalization; pi is not exactly equal to `3.14`. The
reader can group or exclude gauge readings after seeing their contexts. Historical matches show the
state in which the occurrence existed, even if it was later removed. Equal text from different
authors is not silently deduplicated. Repeated unchanged appearances can be compacted into state
runs, but each exact occurrence remains recoverable and occurrence counts stay distinct from counts
of affecting operations.

The result list reports matches found so far and branch/content coverage. Searching raw permascroll
bytes alone is insufficient: it would include unrelated material and miss phrases assembled across
span boundaries. This needs a versioned occurrence search, not merely the current text search with a
new picture.

Image: `03-content-search-v2.png`.

**4. My cell's text is unchanged; why did its neighbor disappear?**

The reader pulls C17's container thread to reveal its history beside the store loom, or chooses
**Show history** from the cell menu. The slice companion remains visible. The lens separates **Own
edits**, **Contained edits** where applicable, and **Affected topology**.

Before the event, C17 is followed by C18 on `d.reading`. An operation authored on C24 connects it
after C17. Afterwards C17 is followed by C24 and C18 is detached; C17 still reads “3.14 bar.” The
effect result says **Edited C24 · affected C17**, and the inspector shows all changed connection
ends. A scan of C17's own edit chain would miss the culprit.

Slice scope includes its contained cell edits, with content and connections separately filterable.
Authored containment, membership through shared content, and indirect effects are different
relations. A scope picker can expose those relations without imposing one universal hierarchy on
ZigZag. A forked history remains a labelled branch projection, never fan-out disguised as a single
ZigZag rank. Each cell has at most one neighbor in each direction on each dimension.

Image: `04-cell-topology.png`.

**5. What happened to this page after the document grew?**

From Page 4 at V12, the reader can ask about **Captured page contents** or **Follow selected
passage**. The former freezes that page's ordered content footprint and snapshot/layout context. The
latter follows the selected content through later document states. They are different subjects, not
two names for whatever currently occupies Page 4.

At V19, an earlier insertion makes “Pressure stayed stable” cross Pages 5 and 6. Its source identity
survives. Enabling context shifts explains the reflow, while direct content effects stay separate. A
changing font or viewport can also repaginate without an authored text operation; the inspector
distinguishes layout-context changes from document edits. Investigating another configuration store
is an explicit scope expansion.

There is no invented permanent page identity or page-creation operation for ordinary pagination. The
saved subject names the document, captured state, ordered ranges and occurrence/layout context.
Returning to the captured view recovers the original footprint, even when today's page numbers
differ.

Image: `05-page-footprint.png`.

**6. Compare alternatives, then recover a passage into a new future.**

The writer selects Draft A and Draft B from the ten-path overview. Two compact comparison tracks
open beside the fixed loom, keeping their actual common ancestor available. Only the active
before/after preview carries full detail; the others can be parked or saved. Source-correspondence
shows whether a paragraph moved, was removed, or was independently retyped. Identical final text
does not erase different histories.

The writer selects a historical passage to recover, chooses a destination and base state, and
inspects an uncommitted recovery preview. The proposed path is visibly seamed, with **No change
saved**, **Create recovery branch**, and **Cancel**. Commit authors a new recovery branch rooted at
the chosen base, transcluding the exact selected historical occurrence. Existing futures remain
reachable. This is a selected content recovery; it does not promise an automatic merge or replay
arbitrary old operation coordinates against a different state.

The pictured passage source is Draft A at V27; the new branch's base is Draft B at V31. The branch
action deliberately creates a separately identified recovery branch, even if its base has no
existing child.

Image: `06-branch-recovery.png`.

More journeys worth testing after these six:

| User goal                                                     | Mechanism to test                                                                                                              |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| Keep an investigation running as the store grows              | Save a lens recipe separately from an exact op bookmark; retain the saved result snapshot and make refresh explicit.           |
| Find every use of a quotation                                 | Search shared addresses, grouped by exact occurrence; offer text search separately for independent retyping.                   |
| Reach the next relevant change in a huge trunk                | Next/previous match and next fork work through the synchronized list; open one local coil without expanding every branch.      |
| Find which edit changed a scalar reading                      | Cell value/content effects retain type and units; distinguish a value change from a display-format change.                     |
| Return after closing the store or losing a source             | Restore named identities, scope and companions first; show pending, unavailable, locked or unverified content distinctly.      |
| Resume a route through documents and cells                    | Save the reader's walk in reader-owned activity; do not depict that walk as an authored document branch.                       |
| Explain an apparently unchanged document that looks different | Inspect rendering context and authored formatting separately; expose configuration dependencies when the reader expands scope. |
| Check who changed something                                   | Show available attribution and its verification status beside the actual op; unknown attribution remains unknown.              |

The interaction contract should remain small: **Select → Inspect → Enter**, **Show history**,
**Expand/compact**, **Save reference**, **Park/close**, and explicit authoring commands. Dragging is
a shortcut to those commands, not their only entry point. Selecting and inspecting do not mutate the
visited store. Saving a pouch item is an explicit write to the reader's pouch; authoring a recovery
is an explicit write to the chosen destination.

Every lens keeps its subject, relation, branch scope and coverage visible. Large drop targets and a
synchronized ordinary list handle occluded chips and keyboard/accessibility input. Compaction
preserves branch exits, selected identities, counts and gaps. Camera reframing leaves source and
selection positions stable in the scene; new matches do not reorder the selected item. Opening
detail is bounded by the reading area, with staged content and cancellable queries. Reduced motion
cuts to the same final state. Exact drawing and query budgets require later measurement.

The concept holds up when the loom remains the orientation map and the lens is the declared
question. It breaks down if every match becomes a bright ribbon, if hidden branches are treated as
searched, or if the reader must infer identity from spatial proximity. The ten-path view needs focus
isolation, occurrence grouping, and a list just as much as it needs coils.

The visual review also found refinements for a working interface: inactive panel rims should be
quieter than selected gold content, background strands should dim around the active inspector, and
provenance/coverage text needs a dependable readable size and dark backing. The raster studies do
not prove contrast, target sizes or motion behavior. Background micro-labels and decorative beam
placement remain illustrative; the named subjects, effect diagrams and text above define each
journey's intended meaning.

Current foundations and proposed work:

- Authored births/context paths and branch replay exist in
  [Store](../../../apps/common/xanadu/store.hpp) and
  [Chronofilade](../../../apps/common/xanadu/enfilade/chronofilade.hpp). The span-effect lens is a
  proposed derived query, not an existing Chronofilade API.
- [Spanfilade](../../../apps/common/xanadu/enfilade/spanfilade.hpp) indexes shared-address
  occurrences. It does not by itself identify every operation affecting a selected occurrence.
- [Pouch items](../../../apps/common/xanadu/pouch_zone.hpp) already carry content and origin
  references. An arbitrary-operation card and saved-lens recipe remain proposed.
- [VQL find](../../../apps/common/xanadu/vql/vql_engine.cpp) searches loaded current documents and
  cells. Historical/deleted occurrence search and related-concept expansion remain proposed.
- [Manifold](../../../apps/common/xanadu/zigzag/manifold.cpp) demonstrates reciprocal/displaced link
  effects. The affected-topology history query remains proposed.

The conceptual distinctions are grounded in Nelson's separation of addressed content from version
appearances in the [Xanadu model](https://xanadu.com/zigzag/fw99/XUmodel.html), and multiple views
and dimension-neighbor rules in the
[ZigZag axioms](https://www.xanadu.com/zigzag/tutorial/zzAxioms.html). The journeys above are our
design proposals inferred from those principles and the current code.
