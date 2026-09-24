# Xuzz Navigation Prototypes

These are proposed interaction prototypes for the
[Xuzz unified link traversal vision](../../xuzz-unified-link-traversal-vision.md). They describe
what a reader should experience and the model each prototype would need. None is implemented end to
end. The [navigation workflow](../../ui_workflow_xuzz_navigation.md) is the shared command and
verification contract.

| Prototype                                               | Reader question                                           | Main technical question                                                              |
| ------------------------------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| [Selected link context](link-context.md)                | What whole link am I exploring?                           | Can one navigation session preserve both ordered endsets and exact occurrences?      |
| [Overlap preview](overlap-preview.md)                   | Which of these links did I mean?                          | Can a hit resolve candidate link identities without activating one?                  |
| [Endpoint search](endpoint-search.md)                   | Where is the particular member or occurrence I want?      | Can search filter large endsets without changing stored order or inventing pairings? |
| [Whole-link overview](whole-link-overview.md)           | How are all these endpoint groups related?                | Can the scene stage clusters without left×right strand growth?                       |
| [Activity walks and branch previews](activity-walks.md) | Where have I been, and which future can I resume?         | Can visit topology stay distinct from activity-store revision ancestry?              |
| [Ambient provenance](ambient-provenance.md)             | Who made this link, and where did this content come from? | Can link attribution and primedia provenance remain separate through transclusion?   |
| [The Flap](flap.md)                                     | Where does this link's content live across versions?      | Can two branch-aware version fans resolve exact document and cell manifestations?    |

## Shared interaction rules

- A selected link has one authority and ID, type, owner or curator, and complete ordered left and
  right `PrimediaSpan` lists. Left and right name endsets, not source and destination. Any line
  between selected occurrences expresses the reader's comparison, never an authored member pair.
- Select, hover, preview, cross, search, and scrub keep reading focus and the editing caret in
  place. **Enter** an exact resolved occurrence changes focus and records one completed visit in the
  reader-owned activity store. Activity Back and Forward restore existing visits.
- Document and ZigZag cell content use the same commands. Repeated manifestations of one span are
  separate occurrences; disjoint members never become one covering extent. A missing target keeps
  its identity and an explicit, cancellable resolution state.
- Pointer, sovereign keymap, and accessibility input invoke the same semantic commands. Visual depth
  or hue can help, but labels and list structure must carry the same meaning without them.

## Prototype order

Build the exact link occurrence query and [selected link context](link-context.md) first. It is the
shared state and command boundary. [Overlap preview](overlap-preview.md) and
[endpoint search](endpoint-search.md) can then exercise ambiguity and scale. The
[whole-link overview](whole-link-overview.md) and [Flap](flap.md) consume the same occurrence
identity for spatial presentations. [Activity walks](activity-walks.md) makes completed travel
durable, while [ambient provenance](ambient-provenance.md) makes its sources intelligible. Every
prototype must report what current Xuzz code actually does before claiming completion.
