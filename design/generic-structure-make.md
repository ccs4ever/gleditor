# Generic Structure Make: Cell, Slice, and Xanadoc Births

## Decision

Rename `StructureVerb::MakeCell = 0` to `StructureVerb::Make = 0`. A Make operation carries a
`StructureKind`: `Cell = 0`, `Slice = 1`, `Xanadoc = 2`; kind `3` is reserved and refused. Kind zero
preserves the meaning and exact flags byte of every present MakeCell, including scalar cells. A
birth operation has a permanent operation identity. Its kind never changes.

Every edit must also carry a context edge that reaches the edited structure's birth on its active
hypertime branch. A birth may in turn name another structure birth as its immediate container, with
arbitrary depth: a Cell may contain a Slice, which may contain more Cells or Xanadocs. The forms
emitted in this milestone remain top-level Slices and Xanadocs with Cells directly in Slices; no
production path emits deeper nesting yet. A missing, broken, cyclic, or wrong-kind chain is refused.
`parentIndex` remains hypertime ancestry; it cannot stand in for this context edge when different
objects' edits interleave in one store. A store is an operation container, not an object identity.

## Wire layout

The existing Structure flags byte has bits 0–2 for the verb, bit 3 for `SetLink` direction, bits 4–6
for `ValueKind`, and bit 7 unused. Direction has no meaning for Make. Reuse bit 3 as the low
Make-kind bit and bit 7 as its high bit, **only when the verb is Make**:

| Make kind | Value | Bit 7 | Bit 3 | Base flags |
| --------- | ----: | ----: | ----: | ---------: |
| Cell      |     0 |     0 |     0 |     `0x00` |
| Slice     |     1 |     0 |     1 |     `0x08` |
| Xanadoc   |     2 |     1 |     0 |     `0x80` |
| Reserved  |     3 |     1 |     1 |     `0x88` |

The `ValueKind` bits retain their present meaning for Cell, including `OpHandle` and `ExternRef`.
Assign the next free value, `ValueKind::Timestamp = 6`, to a typed timestamp Cell. Its 64-bit
`value` is a signed count of nanoseconds since 1970-01-01T00:00:00Z, bit-cast into the unsigned
field as `Int64` already is. It denotes a UTC instant; timezone, locale, and calendar formatting are
presentation choices. Refuse values outside the signed 64-bit nanosecond range rather than clamping,
and do not claim nanosecond measurement precision when the source clock has less. Producers with
microsecond or second resolution may round an instant to the nearest value their clock or input
supports before converting to nanoseconds; use ties-to-even when conversion actually discards finer
precision, including for pre-epoch instants. Multiply only after rounding and check the signed
nanosecond range before storing it. A whole-second reading becomes a nanosecond count divisible by
1,000,000,000; no fabricated subsecond precision is implied.

For Slice and Xanadoc births, `span` names primedia containing that structure's initial local name;
require `ValueKind::None` and zero `value`. An empty span means an empty local name, not an absent
structure. The name is a label, never the identity or a uniqueness key. Require the currently idle
Make fields (`at`, `length`, `to`, `sourceAt`, `sourceLength`, and `linkId`) to be zero for every
birth. `sourceOpIndex` is zero only for a top-level Slice or Xanadoc; on a nested Make it names the
immediate containing Make birth. A Cell birth requires a container. Reject unsupported combinations
at ingestion and during fold; never reinterpret them as Cell. Keep `structureDirectionOf()`
meaningful only for `SetLink`. Add a dedicated `makeStructureFlags(StructureKind, ValueKind)` helper
rather than passing a Make kind through the existing `negward` argument of `structureFlags()`.

This uses no new `CompactOpNode` field: its size, 64-byte alignment, and `offsetof(value) == 56`
stay fixed. The Make kind adds one masked decode to the fold and no per-operation allocation.

## Context edge and replay contract

The context edge is an operation-to-operation reference, not an extra `OpKind::Link` record. Its
target must already exist on the edit's ancestral path. Every Xanadoc or Slice edit refers to the
preceding edit on that structure on the selected branch; the first refers to its Make birth. Cell
content edits and Structure mutations do the same for their Cell. The edit chain stops at that
object's birth; a birth-containment chain continues through its ancestors. Each object has its own
unbroken edit history, even when operations on several objects interleave:

```text
Make(Slice or Xanadoc, top level) -> 0
Make(any kind, nested)            -> immediate containing Make birth
Cell edit                         -> prior Cell edit or Make(Cell) -> container births -> 0
Xanadoc edit                      -> prior Xanadoc edit or Make(Xanadoc) -> container births -> 0
Slice rename                      -> prior Slice edit or Make(Slice) -> container births -> 0

Example: Make(Slice root) -> 0; Make(Cell) -> root; Make(Slice nested) -> Cell
```

Containment is a birth-context relation in hypertime. A Slice born inside a Cell is not bytes in the
Cell's primedia span and does not automatically join any rank. Its eventual home, dimensions, and
presentation remain separate design decisions. The generic edge must preserve this possibility
without inventing current UI behavior for it.

## Local names and renames

`Make(Slice)` and `Make(Xanadoc)` carry their initial local names in their content spans. A rename
uses the existing operation-backed annotation shape: an `OpHandle` Cell names that Make operation,
an alias Cell holds the new local name, and a `SetLink` on `d.alias` attaches the alias to the
handle. The final `SetLink` is the rename event. It carries a separate edit-context predecessor
pointing to the prior edit on the named Slice or Xanadoc; the handle's own Cell-subject chain
remains intact. Each branch can then select its own newest rename event. The old alias Cells and
links remain in hypertime, as does the birth's initial name span.

Resolve the current local name on a selected hypertime branch from the latest valid `d.alias`
annotation event targeting that structure's birth, falling back to its Make span. Other aliases,
notes, tags, and annotations on later operations do not rename the structure. Replacing an alias
link changes the branch's current-name projection, not the birth identity, Xanadoc concatext, Cell
content, or containment. Duplicate local names are valid. Name spans must be persistent, addressable
primedia spans; reject the arena's scratch scroll. `StoreTables::documentId` remains the store's
stable local identifier and does not change on a rename.

`store.tables` no longer persists version annotations: its writer stores only the store ID and local
segments. `versionAnnotations_` and `aliasIndex_` are in-memory compatibility and lookup caches;
`Store::save()` seals pending annotations as Structure operations. `Store::annotateVersion()`
already creates the handle, alias Cell, and `d.alias` link, but its global-latest lookup is not a
branch-local current-name resolver. Extend or wrap that hyperop mechanism for structure names;
derive the canonical name from a fold at the selected version, never from a mutable cache entry. The
rename wrapper must accept an empty alias Cell as a real rename to the empty name: the current
`annotateVersion()` skips empty aliases, and `Manifold::aliases()` filters them out. Distinguish an
absent `d.alias` link from a present link to empty text. A Xanadoc-only store also needs the
annotation dimension substrate before its first rename; initialize it once without changing that
Xanadoc's concatext or treating the metadata Slice as the Xanadoc's identity.

## Timestamp annotations

Use the same OpHandle annotation shape for `d.created` on a Cell, Slice, or Xanadoc birth: the
handle's `value` names the exact birth operation, and a `SetLink` on `d.created` points to a
`ValueKind::Timestamp` Cell. That Cell carries both the signed epoch-nanosecond value bits and a
persistent, addressable primedia span with a canonical UTC RFC 3339 rendering at nanosecond
resolution. A reader uses the bits for ordering and comparison, and the span for ordinary display,
quotation, and provenance. Reject a typed timestamp whose span does not render the same instant;
never infer the typed value by parsing the span during normal reads.

The current `annotateVersion()` writes a plain text Cell for `d.created` from a timestamp string.
Update its writer to parse a complete, valid timestamp with a UTC offset, normalize it to the typed
instant, and refuse invalid or out-of-range input. Add a typed timestamp API for callers that
already have an instant, and keep absent timestamp distinct from epoch zero. The annotation is a
claimed time supplied by its author, not proof of when the store appended the operation. A
branch-local reader must find the `d.created` link on the handle targeting the selected birth;
labels on handles targeting other operations remain ordinary operation annotations. Like a rename,
the annotation's `SetLink` retains its OpHandle subject predecessor in `sourceOpIndex` and carries
the annotated structure's edit-context predecessor in `sourceAt`.

## Context encoding and replay

The current `sourceOpIndex` can hold the containment edge for a nested Make and the edit-context
edge for ordinary text operations; a Make birth must no longer be assumed to have
`sourceOpIndex == 0`. For non-Make Structure operations it already names the previous operation on
the **subject Cell**, which must remain intact for the Manifold fold. Put their separate
edit-context predecessor in `sourceAt`, currently unused by Structure. An ordinary Cell edit can
name the same predecessor in both fields; a rename annotation's `SetLink` names the OpHandle
predecessor in `sourceOpIndex` and the Slice or Xanadoc predecessor in `sourceAt`. Permit that
different-subject case only for a validated annotation attachment targeting the named birth. For
`Transclude`, `sourceOpIndex` continues to name the quoted **source version** and its target context
goes in its currently unused `to` field. Expose these choices through typed `contextOf(node)` and
`subjectOf(node)` accessors.

Give `Op` distinct context and source/subject references. Both must survive
`CompactOpNode::fromOp()`/`toOp()`, binary and text exports, publication, and import. No format may
rely on a local spool index as a portable reference: serialize each operation target by microversion
name and localize it on adoption. Validate both references independently. This per-kind encoding
keeps the 64-byte node and avoids stealing scalar `value` bits or the full binary tag byte.

| Operation                                  | Compact edit context | Other reference retained                          |
| ------------------------------------------ | -------------------- | ------------------------------------------------- |
| Top-level Slice or Xanadoc Make            | `sourceOpIndex = 0`  | None                                              |
| Nested Make                                | `sourceOpIndex`      | Immediate containing Make birth                   |
| Structure edit                             | `sourceAt`           | `sourceOpIndex` remains the subject predecessor   |
| Insert, Delete, Rearrange, Link, PageBreak | `sourceOpIndex`      | Existing geometry or link ID                      |
| Transclude                                 | `to`                 | `sourceOpIndex` remains the quoted source version |

The context chain and `parentIndex` ancestry must both be checked. A birth's containment edge must
name an earlier Make birth on the active ancestral path; an edit predecessor must resolve to the
same edited birth. At append and adoption, cache both the edited birth and each birth's immediate
container in an ephemeral index, so long edit and containment chains are not walked afresh for every
keystroke. Cache the outermost root separately if needed for traversal; it cannot replace the edited
birth when selecting a replay product. Resolve the prior edit for the selected object on the
selected branch; an earlier spool index or a global latest pointer does not prove ancestry. Add a
branch-aware Xanadoc and Slice tail lookup alongside `Manifold::lastOp` and `Store::lastOpOnCell()`.
`Manifold::historyOf()` and `byRef` must include Cell-targeted text edits when finding the next
predecessor or reconstructing Cell content.

For a document edit, `at` is a coordinate in the selected Xanadoc. For a Cell edit, it is a
coordinate in that Cell's content. The selected context determines which replay product accepts the
edit: `Version` applies only text operations reaching its Xanadoc birth; `Manifold` applies
Cell-targeted text operations to that Cell's span run along with the existing Structure edits.
`PageBreak` must require a Xanadoc context unless a separate Cell meaning is specified. Keep the
existing `Splice` path for Cell typing while the front end migrates; a Cell-targeted `Insert` must
have a defined fold effect and may not silently enter document concatext. A transclusion has two
independent relations: its target context edge and its quoted source-version reference.

Resolve context against the selected version's ancestral path, not the whole spool. A birth on a
sibling branch is not a valid target. A Cell can be shown in several slices through quotation or
projection, but its birth edge names one immediate container; focus on a quoted occurrence must
explicitly select the origin Cell for editing or create a new Cell in the focused structure. Span
equality, rank, or operation order never chooses that context. Focus changes append no operation.

## Implementation sequence

1. **Define the encoding and context API.** In `apps/common/xanadu/ops.hpp` and `ops.cpp`, rename
   the verb, add `StructureKind`, flag accessors, a Make-specific constructor, and names for dumps.
   Add `ValueKind::Timestamp = 6`, a signed nanosecond accessor, and typed Cell construction that
   writes matching value bits and a canonical UTC span. Make `d.created` annotations use that type.
   Add a typed context reference to `Op` and a `contextOf()` accessor for compact nodes. Audit every
   `StructureVerb::MakeCell` comparison, including Store, Manifold, external reference resolution,
   publication, and tools. Each site must say whether it means *any birth* or *a Cell birth*.
   Centralize validation of the target's kind, ancestry, and chain termination; an unresolved
   context may not be coerced to zero by `Store::putOp()`. The generic Make API accepts an optional
   containing Make birth for any kind. Current producers use zero for top-level Slice and Xanadoc
   births and a Slice birth for Cells; no producer creates deeper nesting yet. Accept a persistent
   name span on Slice and Xanadoc Make, and reject scalar value bits on those births.
1. **Make the fold type aware.** `zigzag::Manifold::applyStructure()` should mint a `CellSlot` only
   for `Make(Cell)`. Record recognized Slice and Xanadoc births in a typed, version-scoped structure
   forest, or expose them through an equivalent operation-index query. Track the branch-local name
   span for each Slice and Xanadoc there. They must be observable without becoming `CellRef`s or
   silently increasing `cellCount()`. `Store::putOp()` and `indexGenesisCells()` must count only
   Cell births for the current home and `d.dims` bootstrap. Before production root births are
   emitted, replace the assumption that *the first two cells in the whole spool* are always those
   bootstrap cells. Define genesis as one ancestral sequence of `Make(Slice)`, `Make(Cell: home)`,
   and `Make(Cell: d.dims)` operations and index that triplet by its Slice birth through the Cell
   birth context edges. Reject incomplete or interrupted genesis rather than guessing from unrelated
   cells. A branch must not inherit a root born only on another branch.
1. **Introduce births and wire every edit.** Have `sliceGenesis()` emit `Make(Slice)` before its
   home and `d.dims` cells, carrying the Slice's initial local name, and make its current Cell
   producers name that Slice. Have xudu's `Session::createNewStore()` and
   `importFileToTemporaryStore()` emit `Make(Xanadoc)` before their first text operation, carrying
   the Xanadoc's initial local name even for empty documents; audit import branches, transcluded
   documents, system documents, fixtures, and direct Store writers. Hybrid system documents need
   both a Xanadoc birth for their text and a Slice birth for their settings cells, even when those
   operations interleave. Document edits chain through the prior edit on the selected Xanadoc to its
   birth; Cell text operations and Structure mutations chain through the prior operation on that
   Cell to its birth. Add a typed `renameStructure()` wrapper over the OpHandle, alias Cell, and
   `d.alias` annotation path. Its final `SetLink` carries the named structure's prior edit in
   `sourceAt`, so the rename joins that structure's edit chain while `sourceOpIndex` continues to
   name the handle Cell subject. Require a context argument or typed editing handle in Store's edit
   APIs instead of inferring a root from the latest operation or store. Top-level Slice and Xanadoc
   births alone may have zero context, even when a later birth has a nonzero `parentIndex`. Validate
   that handle and its branch before `UserPermascroll::append()` so a rejected edit leaves no
   orphaned primedia. Audit code that treats a birth's zero `sourceOpIndex` as its chain terminator:
   an object's edit history stops at its own birth, while containment validation follows birth edges
   to the top-level Slice or Xanadoc.
1. **Scope replay and focus.** Give `Store::rebuild()` a selected Xanadoc identity and make it
   ignore edits belonging to other roots on the same ancestral path. Have the Manifold fold apply
   Cell-targeted text operations only to their target Cell and validate its containment edge;
   incremental `Manifold::advance()` must see those non-Structure operations too. Preserve a
   combined, context-aware store view for Xuzz rather than splitting objects onto sibling branches
   solely to obtain replay isolation. Resolve the current Slice or Xanadoc name from the selected
   version's annotation fold; a rename's final `d.alias` SetLink updates that name projection, not
   document `Version` or the named structure's birth span. Include the selected edited birth in
   Chronofilade replay/cache keys and audit occurrence and link discovery for the same partition.
   Carry the focused birth and its validated containment path in `Session::OpenView` and the
   ZigZag/Xuzz view state. Keyboard, pointer, accessibility, paste, drag, and scripted edit entry
   points must resolve an ephemeral `FocusTarget` carrying the exact Xanadoc or Cell birth before
   appending primedia or an operation; refuse absent or ambiguous focus. Select current heads per
   birth rather than using `Store::latest()` as a document head. A contextual `OpKind::Link` records
   the authoring focus; its span endpoints can still be visible in other objects.
1. **Make every serialization path honest.** `ops.nodes` currently has
   `opsSegmentFormatVersion = 1`. Bump it to 2 before writing non-Cell Make records so old readers
   refuse the changed meaning. Compact `ops.export` currently carries raw flags as V4; bump to V5
   and retire the V4 reader. V5 must carry the context target by microversion name for every edit,
   alongside the independent transclusion source name. `Store::opRecords()` and `adoptOpRecords()`
   must include context in dependency ordering and localize it before append. Version or explicitly
   refuse the human-readable OSMIC export when it contains the new Make kinds or context edges,
   since its present unversioned columns would silently discard context. `store.tables` stays
   version 3 unless its shape changes. Update `xudu-dump --section=ops` to show kinds and context
   chains, initial names and rename annotation events, and report unknown or reserved kinds as
   invalid.
1. **Regenerate and verify.** The version bump invalidates every `ops.nodes` fixture even though the
   node remains 64 bytes. Run both `tools/create-sample-xanadocs.sh` and
   `tools/create-floating-image-sample.sh` in that order. Diff `xudu-dump --section=ops` output
   before and after, accounting for operation indices and microversion names shifted by the new
   births, and check that text, links, and cell meaning survive. Confirm the system-doc typed
   refusal path in `Session::systemStoreIndex()` still regenerates old system documents.

## Acceptance checks for this first milestone

- Round-trip flags for all three kinds, Cell scalar kinds, and `SetLink` direction. Reject reserved
  kind 3 and invalid root payloads.
- Put Slice and Xanadoc births before, between, and after Cell births; verify the fold's cell count,
  home and `d.dims`, structure forest, and incremental `advance()` agree with a full rebuild. Every
  edit must resolve through its context chain to the right birth, including after branch, save/load,
  and import. Reject zero, dangling, future, sibling-branch, cyclic, and wrong-kind references.
- Interleave two Xanadocs' edits in one ancestry and verify rebuilding either produces only its
  text. Interleave Cell edits with Xanadoc edits and verify Cell content and document concatext stay
  separate, including in a hybrid system document. Test direct `Insert` into a Cell, transclusion
  with different source and target roots, and rejection of a Cell-context `PageBreak`.
- For one Xanadoc, assert that consecutive Insert, Delete, Rearrange, PageBreak, Transclude, and
  authored Link operations each point to that Xanadoc's preceding edit, with the first pointing to
  `Make(Xanadoc)`. Interleave a second Xanadoc and fork from an earlier version; each branch must
  choose its own prior edit and terminate at its own birth after save/load and export/import.
- Verify Slice and Xanadoc Make spans expose their initial local names. Rename each through an
  OpHandle, alias Cell, and `d.alias` annotation, then fork before a rename. Each branch must
  resolve its own latest valid name while the birth IDs, document text, Cell content, and
  `StoreTables::documentId` remain unchanged. Include duplicate and empty names, a nested structure,
  a Xanadoc-only store's first rename, and save/load plus export/import round trips. Verify the
  annotation `SetLink` keeps both its handle-subject predecessor and the named structure's
  edit-context predecessor.
- Annotate Cell, Slice, and Xanadoc births on `d.created` with typed timestamps. Verify pre-epoch,
  epoch-zero, and fractional-second instants, canonical UTC spans, bitwise and publication round
  trips, ordering across branches, microsecond and second producers with nearest-value rounding,
  ties-to-even at positive and negative half-unit boundaries, and refusal of malformed or
  out-of-range input. Distinguish no annotation from a present zero-valued timestamp; verify the
  SetLink's two predecessor edges.
- Save/load and V5 export/import preserve both the context edge and transclusion source along with
  birth identities and kinds; old `ops.nodes` and binary export versions fail by version. Text
  export either round-trips with an explicit new version or refuses the new operations.
- Test focus switching between a document page and Cell content in Xudu, ZigZag, and Xuzz; typing
  and paste must reach the selected birth, while navigation alone creates no operation. Missing or
  ambiguous focus must cause no primedia append or op.
- Verify a Cell content edit followed by `SetLink` or `SetValue` retains one Cell history, and
  `historyOf()` stops at `Make(Cell)` while containment validation continues through all birth
  ancestors.
- Construct a nested `Slice -> Cell -> Slice -> Cell` chain through the low-level Make API, plus a
  nested Xanadoc. Verify birth identities, exact edit targets, save/load and export/import round
  trips, and refusal of a missing, non-birth, or off-branch container. No UI or fixture generator
  needs to emit nested births in this milestone.
- Create a normal xanadoc and slice through their public creation paths and verify unchanged
  rendered text and cell topology. Run the four headless test binaries and the full
  `make -j$(nproc) test` gate, accounting for the documented `veth` prerequisite of the final swarm
  test.
- Measure `Manifold::advance()` and full rebuild on the existing large-slice benchmark before and
  after. Keep the new structure index compact and bounded by the number of births; optimize only if
  the measurement shows a regression.

## Remaining work for full multiple-object use

`sliceToStore()` already accepts a second DTO in a store, but it reuses the single home, `d.dims`,
and named dimensions; it does not create an independent second Slice. The context edge introduced
here gives each birth an immediate container and each edit a target, but nested Slice geometry, root
selection controls, and persistent per-birth current-version designations still need design before
the UI offers full multi-object creation and switching. Define how links, transclusion discovery,
publication, and Xuzz navigation resolve each typed root across stores and branches. Keep
`GlobalOpRef` as the portable birth identity; a local spool index is only a local handle. Do not
infer ownership from operation order, a shared span, or a cell's presentation rank.

## Architectural basis

This keeps cells distinct from pages and respects the two replay products in
[`store-slice-convergence.md`](store-slice-convergence.md). Nelson describes a
[rank as an ordering of cells along a dimension](https://xanadu.com.au/mail/zzdev/msg01894.html) and
[cells as includable in larger objects](https://www.xanadu.com.au/mail/zigzag/msg00015.html). Those
ideas support typed, independently addressable births. They do not make a Slice or Xanadoc
equivalent to a rank or to an [enfilade crum](https://xanadu.com/tech/). Xuzz should be able to
reach both typed roots through structure while preserving Xanadoc concatext and ZigZag topology as
distinct views.
