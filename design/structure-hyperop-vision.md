# Beyond Cells: What Else MAKE/CHANGE STRUCTURE MAP Can Carry

**Document Version:** 2.0 — grounded rewrite. **Status:** a vision note and a ranked proposal, not a
ruling. Nothing here changes `ops.hpp`, `CompactOpNode`, or any on-disk format on its own; each
numbered proposal in §5 needs its own design note in the shape of
[`vlog-logic-extension.md`](vlog-logic-extension.md) before code moves. **One exception:** §3
records a live defect found while grounding this note, and its fix is owed regardless of anything
else here.

Version 1.0 of this note ideated before reading the tree and re-proposed several things that were
already built (the Spanfilade, `provenance.hpp`, R4's `GlobalOpRef`, hypertime projected into cells)
or already refused (R8's cursor ruling, enfilade-rank-indexing §3). This version was produced the
other way round: a verified ledger of what exists (§1–§2) first, then a Nelsonian brainstorm and a
systems stress-test against that ledger, then a cross-critique until the two agreed. Where they
still disagreed, §5 says so and records the ruling taken.

## 1. What a Structure operation is today

`OpKind::Structure` (`apps/common/xanadu/ops.hpp`) is OSMIC's sixth hyperop. Four verbs live in
`CompactOpNode::flags` bits 0–2 — `MakeCell = 0`, `SetLink = 1`, `SetValue = 2`, `Splice = 3`,
values 4–7 unused — with bit 3 the link direction, bits 4–6 a `ValueKind` (`None/Double/Bool/Int64`,
values 4–7 unused) and bit 7 unclaimed. There is deliberately no `MakeDim`: a dimension is a cell on
the `d.dims` rank (R2, R12).

A cell **is** an operation. `CellRef` is the ops-spool index of its `MakeCell`; every later verb on
that cell names it by chaining `sourceOpIndex` to the previous operation on the same cell, and the
chain's far end is the `MakeCell` (R7). A cell's content is a run of primedia spans (U3, resolved:
`Splice` edits one piece and leaves every other address alone; `spliceCellSpan` transcludes *into* a
cell). A scalar cell carries both a real span and canonical bits (R6).

Which fields each verb reads (`zigzag/manifold.cpp` `applyStructure`, and the emitters in
`store.cpp`):

| verb       | reads                                                 |
| ---------- | ----------------------------------------------------- |
| `MakeCell` | span, `flags` value kind, `value`                     |
| `SetLink`  | `sourceOpIndex` (subject), `linkId` (dimension), `to` |
| `SetValue` | `sourceOpIndex`, span, `flags` value kind, `value`    |
| `Splice`   | `sourceOpIndex`, `at`, `length`, span                 |

So for the three non-`Splice` verbs, **sixteen bytes of the 64-byte node are idle**: `at`, `length`,
`sourceAt`, `sourceLength`. §5 spends four of them.

Every `Store` structure call — `sliceGenesis`, `makeCell`, `makeScalarCell`, `setScalar`,
`setCellText`, `setLink`, `spliceCell`, `spliceCellSpan`, `setValue`, `makeDimension` — takes a
parent `MicroversionId` and returns the one it produced. **A structure change is a named state in
hypertime**, branchable and scrubbable exactly as an insertion is. `Manifold` is the fold (R9: an
explicitly materialised view with `advance()` and `verifyAgainstFullRebuild()`); `ArenaManifold` is
its ephemeral copy-on-write twin, and R8 draws the persistent/ephemeral boundary at the *type*.

Who writes Structure operations today, outside tests: `system_docs.cpp` (the dominant writer, 34
calls), `zz_xudu_projector.cpp`'s `sliceToStore()`, `unified_transclusion_engine.cpp`,
`arena_manifold.cpp`'s `promote()` (the one road from ephemeral to persistent),
`vortex_host.cpp::promoteAndAttachToStore()`, the VQL and VPL compilers' persist paths,
`dimension_registry.cpp`, and `apps/xudu/batch_orchestrator.cpp`. **Xudu the editor itself emits
none** — the xanadoc editor is text-ops only, which is worth knowing before proposing anything that
assumes a document's structure is authored in xudu.

## 2. What already exists, so it is not proposed again

**Built on Structure.** ZigZag slices (`sliceToStore`/`storeToSlice`, `d.role`/`d.mime`/`d.media`).
The five system xanadocs as configuration-as-structure with ten dimensions — `d.dims`, `d.vars`,
`d.values`, `d.groups`, `d.subgroups`, `d.clone`, `d.notes`, `d.schemas`, `d.alternates`,
`d.default` — with live schema validation and defaults
([system-xanadocs §6.2](system-xanadocs-customization-and-metasystem.md)). The Vortex runtime's
dimensions on the arena (`d.spin`, `d.step`, `d.grab`, `d.contract`, `d.cache`, `d.pinning-cursors`,
`d.stdlib`, `d.clause`, …), with `promoteAndAttachToStore()` persisting a compiled program as real
Structure operations — **and it has a production caller**, the zigzag VQL palette
(`apps/zigzag/main.cpp:201`). VQL's `MultiStoreCoordinator` composing many stores in *one arena* via
a `d.stores` rank, re-minting every foreign cell as a fresh ephemeral ref. Hypertime projected into
cells: `ZigzagVisualizer::adoptDocument` hands the projector's DTO to `sliceToStore`, so
`d.doc`/`d.transclude`/`d.link`/`d.version` cells are minted as persistent operations into an
in-memory store. (`apps/xudu/hypertime_graph.cpp` only *renders*; its one `Structure` mention is a
glyph.)

**Built elsewhere, on purpose.** All five enfilades — Spanfilade (who quotes this span, across
documents and cells), Layoutfilade, Chronofilade with the `EdlTransform` monoid, Holefilade,
Arrayfilade — are ephemeral replay products that mint no operations (R8; their banners say so). The
butterfly `Link` records (type, tier, owner, curator, left and right spans) live in `store.tables`;
an `OpKind::Link` operation carries only the link id. GPG-signed `AUTHORSHIP.yaml`
(`provenance.hpp`) with a scroll-level `quotes` list. `GlobalSpan`/`globalise`/`localise` for
content and `GlobalOpRef{scroll, produces}`/`opRefOf()`/`localiseOpRef()` for operations
(`publication.hpp`). Identity, the Merkle ledger, transcopyright holes.

**Refused already.** R8: only a user-generated update persists; navigation, cursors and view state
never earn a hypertime name; no persistent-side GC. R6 and Non-Goal 2: no canonical scroll of
values. Non-Goal 4: a `Link`'s ends stay spans. R1: `PageBreak` stays a special case. R12: no
privileged dimensions. [enfilade-rank-indexing §3](enfilade/enfilade-rank-indexing.md): an index's
crums are not cells. R11: layout is soft, but 64-byte nodes, cache alignment, 64 KiB pieces and
append-only-ness are hard.

**Designed and not built.** `Manifold::externalCells` (named in R4, never written). The bridge's
"federated link-source interface" for links across independently stored documents
([bridge-next-stage](xudu-zigzag-hypermedia-bridge-next-stage.md), deferred by name). §4.1 preset
sharing (zero code hits for `preset`). U3.4's option (B): framing the other four hyperops in a cell.
Vlog §6. AGENTS.md's line that `UnifiedTransclusionEngine` "has no production caller" is stale —
`zigzag_visualizer.cpp:203` and `:252` construct one.

## 3. A defect found on the way: published Structure operations lose their chain

**Verified.** `Store::setLink` sets `op.source = opsSpool.idOf(previous)` (`store.cpp:484–486`);
`opRecords()` reconstitutes it as a live `MicroversionId` (`store.cpp:1326–1327`); `sealableOps()`
includes Structure operations wholesale (`publication.cpp:803–848`). Then the binary writer's
`BinStructure` case (`binary_ops.cpp:256–266`) writes tag, `produces`, `flags`, `to`, `link`, span
and `value` — **and never `op.source`**, where the `Transclude` case one screen up does
(`binary_ops.cpp:212`). The reader (`:409–430`) leaves `source` zero; `putOp` resolves that to
`sourceOpIndex == 0` (`store.cpp:118`); `applyStructure`'s `SetLink`, `SetValue` and `Splice` all do
`denseOf(0)`, get `noDense`, and refuse (`manifold.cpp:245–250`, `:320–323`, `:307–310`).

So a published slice arrives as isolated cells with no links, and the only trace is `refusedOps()`.
It has gone unnoticed for two reasons: `MakeCell` reads no chain, so a genesis-only slice
round-trips perfectly; and the one round-trip test (`tests/xudu/binary_ops.cpp:390–425`) builds its
`SetLink` with `source` left default-zero and asserts on the other fields, so it passes vacuously.
Nothing in the tree publishes a Structure operation and folds a `Manifold` on the ingested side. The
OSMIC *text* encoding is unaffected (`writeOsmicTextOpsSpool` emits `source`, `at` and `length` as
columns for every kind).

**The fix is `CompactBinaryV4`, and it is smaller than it sounds.** The writer already chains by
*name*, not index, so V4's `BinStructure` record adds `writeMicroversionId(out, op.source)` — the
same call the other kinds make — and `putOp`'s `indexOf()` re-localises it on the reader. That is
R4-correct by construction: the name survives a seal, the index does not. The same record must add
varint `at` and `length` for `Splice` (U3.4 already owes this; the export currently throws rather
than corrupts). **Add the missing test: publish a slice with links, ingest it, fold, assert
`refusedOps() == 0` and `equivalentTo()` the original.**

Two further items were listed here in draft and did not survive the code review in
[`structure-hyperop/5.1-compact-binary-v4.md`](structure-hyperop/5.1-compact-binary-v4.md) §2. The
wire kind field is **not** full: `CompactBinaryV3` already widened it to four bits, so
`BinStructure = 7` is the eighth of sixteen values with eight free after it — the claim was true of
version 2 and was carried forward stale. What *is* full is the tag byte, four kind bits plus four
flags, held by a `static_assert`; a new *flag* needs a second byte, and V4 needs no new flag. And
`storeToLinkPackage()`'s first-span-only defect is a change to `LinkPackage`'s own wire format in
`link_package.cpp`, not to the ops spool — still owed, but not in this bump, because moving two
formats' versions in lockstep couples things that have no reason to agree.

A length-prefixed, skippable extension record was proposed for the same bump, on the ground that
R11's "no compatibility" is about files on this machine rather than a wire two peers negotiate. The
plan recommends deferring it: §5.6's own ruling puts the variable-length part of a cross-store
reference in `store.tables` precisely so the ops wire stays fixed-shape, and nothing could test the
mechanism today.

Every cross-store proposal below is gated on this. So, today, is publishing any ZigZag slice.

## 4. The principle the proposals share

Three observations organise everything in §5.

**A cell is an operation, so any operation is almost a cell.** R7 already means a cell's address is
an operation index and its history is a chain of operations. What the tree does not yet have is the
converse: a way to point *at* an operation that is not a `MakeCell`. An `Insert`, a `Delete`, a
`Transclude`, the making of a link — the atomic acts of authorship — each has a permanent name in
hypertime and no address a link can reach. Nelson's founding ZigZag principle is that every cell is
a link target; here the one thing you cannot point at is an edit.

**The side tables are the authorial acts OSMIC declines to record.** `store.tables` holds the scroll
registry, the local segments, the link records, the author's designated current versions, and the
version annotations. The first two are name-resolution facts a reader needs before it can interpret
any span, and must stay tables (a bootstrapping argument, not a taste). The last two are decisions a
person made — *this* is the French edition; *this* state is called `release-1.0` — and they are
mutable, unattributed, unbranchable and undiffable. Under branching the flat `currentVersions`
vector is already slightly wrong: two futures of one document share one list of heads. There is no
recorded reason for this; `store_tables.hpp`'s only rationale is the R11 consolidation of two
plaintext files, which says nothing about whether a head is an operation.

**A link to a document you do not have yet is normal, not corruption.** A docuverse is mostly
elsewhere. Today the fold has one response to a target it cannot resolve — `refusedOps_++`, whose
documented meaning is "the spool holds something unintelligible." Any cross-store structure needs a
second, visible, non-pejorative state.

## 5. Proposals, in build order

Each entry gives the operations appended, the wire impact, what the fold does when a referenced
store is absent, and the rulings it touches. The order is a prerequisite chain, not a preference
ranking; the Nelsonian weight is stated where it differs.

### 5.1 `CompactBinaryV4`

§3. Prerequisite for 5.6–5.10 and for shipping slice publication at all. No new operations. **Plan:
[`structure-hyperop/5.1-compact-binary-v4.md`](structure-hyperop/5.1-compact-binary-v4.md)** — three
fields appended to the `BinStructure` record (`source` always; `at` and `length` when the verb is
`Splice`, keyed off the verb the reader has already decoded), version 3 refused by number, and no
fixture regenerated, because all seventeen checked-in `ops.export` files are OSMIC text rather than
binary.

### 5.2 `d.hist`: a cell's own past as a rank

**Appends nothing. No wire.** The R7 chain is walked only by the fold today; surface it as a read
API. A cell's earlier states become things you can look at, link to and quote: "quote the third
draft of this paragraph, and show me who changed it." Distinct from the projector's `d.version`
cells, which are a whole-document projection into a throwaway store. R8 is untouched because nothing
is written. Cheapest good idea on the list; build it first. **Plan:
[`structure-hyperop/5.2-cell-history.md`](structure-hyperop/5.2-cell-history.md).**

Two corrections to the draft, from the code review in that plan. **`byRef` does not hold the
chain**: it is an unordered `CellRef -> dense` map, so it answers "which cell does this operation
belong to" and not "what is this cell's chain" — the predecessor links live in each node's
`sourceOpIndex`, in the ops spool, which means the walk needs a `Store` and cannot sit on
`Manifold`'s no-allocation, no-Store-access read path. And the sketched
`historyOf() -> span<const CellRef>` cannot return a span, because no such array is stored; the walk
is a callback in the style of the existing `walkRank`, or it materialises a vector. Enumerating the
chain is also not the same as *reconstructing* a past state: `MakeCell` and `SetValue` carry the
whole content, but `Splice` is a delta, so rendering "the third draft" is a mini-replay and is
scoped as the plan's second half.

### 5.3 ~~Pouch and clasp staging on `ArenaManifold`~~ — withdrawn; refiled after 5.6

**Withdrawn as written.** Review:
[`structure-hyperop/5.3-pouch-staging.md`](structure-hyperop/5.3-pouch-staging.md). Every premise
failed: work package 3's cell-drop path is **built and wired** (`pouch_drawer.cpp:140`, called from
`main.cpp:2912`); there is no hand-rolled arena, only one in-flight `KineticTetherEngine::payload()`
with `cancelDrag()`; and **nothing is written during a drag**, so cancelling is already free and R8
is already satisfied by construction rather than needing the type system's help. `promote()` earns
its keep when an evaluation builds a graph of unknown size; a drop emits two or three operations.

What the section was reaching for is one layer down: **the pouch has no persistent item model.**
Zone definitions are proper system-xanadoc cells, but zone *contents* are a RAM-only
`std::vector<PouchItem>` that nothing rebuilds on load — while every drop appends an `Insert` and a
`VersionAnnotation` nobody reads. That persistence is improvised inside the annotation's string
fields (`alias` = zone, `description` = preview, `tag` = which kind of drop), which makes the pouch
§5.5's second customer. And a dropped ZigZag cell's `originCell` is a bare `std::uint32_t` into
another store's spool — exactly the reference R4 says cannot survive that store gaining a branch.

So the replacement, **pouch items as cells**, depends on §5.6 and is sequenced after it. Nothing
else in the chain depended on this section.

### 5.4 Operation handle cells

### 5.4 Operation handle cells

**One `MakeCell` per handle, minted on demand. Wire: V4.** A handle is an ordinary `MakeCell`
carrying the index of the operation it stands for, marked by its value kind. `sourceOpIndex` keeps
its R7 meaning unbroken: the handle's own chain ends at its own `MakeCell`. (A first draft routed
the handle through `sourceOpIndex` to the referenced `Insert`; that would make `denseOf()` resolve a
text operation to a cell, a category error the fold cannot detect, and was withdrawn.) The
referenced index is local, and is globalised on export exactly as a span is through `globalise()` —
the precedent for "a local address that becomes a global one at the wire" is every content span in
the system. **Plan:
[`structure-hyperop/5.4-operation-handles.md`](structure-hyperop/5.4-operation-handles.md).**

**Ruling taken here:** the marker is a spare `ValueKind`, `ValueKind::OpHandle = 4`, not flags bit
7\. It is a statement about what the cell's value *is*, which is what `ValueKind` is for, and it
leaves bit 7 for something that is not a value.

**Corrected by the plan: the target goes in `value`, not the idle `sourceAt`.** A `ValueKind`
describing a field other than `value` is not the statement the ruling above claims for it, and
`value` wins on every practical count: `applyStructure`'s `MakeCell` case **already** copies both
`valueKind` and `value` into the slot (`manifold.cpp:218-226`), so the fold needs no change at all
and `CellSlot` needs no new field; the manifold can then answer `handleTarget()` off the slot
without the `Store` access `sourceAt` would force; `value` is already on the wire where `sourceAt`
is not; and it is exactly R6's scalar precedent, where `ValueKind` says how to read `value`. The
existing `asDouble`/`asBool`/`asInt64` return `nullopt` on a kind mismatch, so a handle is never
read as a number.

**And it widens §5.1's scope.** `value` travels as a plain varint, and an operation index renumbers
on the reader — so a published handle would arrive naming a *different* operation, silently. That is
R14's failure by a second door, and the fix is the one §5.1 already uses for `source`: when the kind
is `OpHandle`, write the target as a `MicroversionId`. Folded into V4 rather than bumped to a V5,
since neither section is built.

What it buys, in one mechanism: commentary on an edit ("why was this deleted" — nothing else in the
tree can answer it, and the link table cannot address an operation at all); a review rank of edits;
links-to-links, because *making a link is itself an `OpKind::Link` operation*, so a handle to that
act delivers Nelson's commentable link without touching the `Link` record or Non-Goal 4; and version
annotations — an alias, a description, a tag become the content of the handle cell for the operation
that produced the state, on `d.notes` like any other cell's notes. That retires the
`versionAnnotations` table.

**Refused alongside:** a slot per operation. A 32-byte `CellSlot` plus its hash entry for every
operation is roughly +75% resident over the node for a table nobody queries; one cell where a
comment exists is the sparse reality. Also refused: a "link handle" that names a `linkId` — a
durable name for a mutable table row is half a solution, and the handle to the link-making
*operation* is the whole one.

### 5.5 The editions rank

**One `MakeCell` plus one `SetLink` per edition; a `SetValue` to repoint. No wire.** The author's
designated current versions become cells on `d.editions` off `home`. The cell's content is the
edition's name; it is linked on `d.edition-of` to the operation handle (5.4) for the state it
designates, so 5.5 is a special case of 5.4 and needs no encoding of its own. Declaring "this is the
French edition, as of now" then has an author, a hypertime name, a branch and a diff.
`storeTablesFormatVersion` goes to 4 and drops `versionAnnotations`; `scrolls`, `localSegments` and
`links` stay. **Plan:
[`structure-hyperop/5.5-editions-rank.md`](structure-hyperop/5.5-editions-rank.md).**

**The section is right, and the code has drifted away from it.** `currentVersions` is an array *on
purpose* — the author designates the current French version, the current English version, and so on,
and the UI presents them; paired with version aliases, that was the plan for making editions real.
The alias half is built and on screen (`hypertime_graph.cpp:281-300` renders each as a label beside
its node). The system-store concession — exactly one current version, because a config document
presents one chosen set of values to the system — is intact, `repointCurrentVersion()` being guarded
by `isSystem()` at every call site.

What drifted is the array. `Session::syncCurrentVersions()` (`session.cpp:849-872`), called
unguarded from `save()`, *derives* the designations from whatever versions happen to be open in
windows. In the common case the two coincide, which is how it survived; they diverge the moment
anyone opens a third state to compare, and saving then silently redesignates the document.
`batch_orchestrator.cpp:765-774` now reasons from the drifted behaviour as settled ("interactive
multi-view sessions retain all heads") and works around it on the export path.

**Fixing the drift is not enough, which is what keeps this section a migration.** An edition today
has no identity: "French is at X" is `X ∈ currentVersions` joined to
`versionAnnotations[X].alias == "French"` by microversion id, so repointing means three edits across
two tables — and **the alias moves with the key**, so nothing remembers French was ever at X. As a
cell, the identity is the cell, the name is its content, the designation is one `SetLink` to a §5.4
handle, its own history is its R7 chain (§5.2), and per-branch heads come free because editions are
folded per state. `currentVersions` stays in the table as a *cache* of the rank, for the same
bootstrapping reason as `scrolls`.

### 5.6 Persistent references to cells in other stores

The one design that serves 5.7–5.10, and the point where the Purist and the Realist disagreed. The
Purist wanted the foreign reference in the node's idle bytes with no side table, on the ground that
a side table is "an authorial act filed where hypertime cannot name it." The Realist's objection is
mechanical and decisive: reader-side indices after `historyFromSeal()` are deterministic for a given
seal but **renumber when the publisher later adds a branch** (`opRecords()` sorts by
`MicroversionId`, `store.cpp:1329`), so a stored local index would have to be rewritten on re-seal —
and the node it lives in is in a `PROT_READ` segment (R10). Only `GlobalOpRef` survives, and R4
already says it cannot fit in a node.

**Ruling taken:** the *act* stays in hypertime and only the *name resolution* is tabled — the status
the scroll registry already has.

- `store.tables` gains a section `externals`, whose index *i* corresponds to a local **placeholder
  cell**. Bencode, where variable-length things already live. **Corrected by the plan:** it holds a
  *local* `ExternOpRef{ScrollId, produces}`, not a `GlobalOpRef` with a scroll-key string — the same
  relation `PrimediaSpan` has to `GlobalSpan`, converted at the publication boundary by
  `globalise`/`localise`, which is what R4 meant by localising "through the identical
  `Store::externals`/`scrollKey` path". Forty references into one foreign store then share one
  registry entry instead of carrying forty copies of its key, and the registry already holds the
  segments needed to fetch it.
- The placeholder is an ordinary `MakeCell` with `ValueKind::ExternRef = 5` and the `externals`
  index **in `value`, not `sourceAt`** — the same correction §5.4 took, for the same reason: the
  fold already copies `valueKind` and `value` into `CellSlot`, so it does not move. It has an
  operation behind it, so it is **not** ephemeral: R8's refusal and `refusedOps_` keep their
  meaning.
- A link to a foreign cell is an ordinary `SetLink` whose `to` is the placeholder. **The 64-byte
  node is untouched and no new verb is needed.**
- The fold mints the placeholder like any cell — `spanCount == 0`, no value — and counts it in a new
  `unresolvedExternals()`, distinct from `refusedOps()`. Absent store: the rank dead-ends at a
  visible placeholder rather than vanishing.
- Resolution is on demand: `localiseOpRef()` against the foreign store when it is loaded, memoised
  in an arena, never written back. Re-localisation on re-seal is therefore a cache invalidation, not
  a segment rewrite.
- Wire: V4 carries the `GlobalOpRef` for placeholder `MakeCell`s only, via `writeMicroversionId`.
  Merkle cost is zero — `store.tables` is not in the piece stream, and `ops.nodes` remains a run of
  64-byte appends, 1,024 per piece.

This is `Manifold::externalCells` built, and the bridge's federated link source made concrete.
**Plan: [`structure-hyperop/5.6-extern-refs.md`](structure-hyperop/5.6-extern-refs.md)**, which also
drops the floated `SetExternLink` verb as unnecessary — the link is local; only its target stands
for something remote — and states the cost the bullets above understate: **resolution is not a
lookup.** Turning a placeholder into a foreign cell means finding the store behind a `Scroll`,
loading it (possibly over BitTorrent), knowing its `sealedAs`, calling `localiseOpRef()` for an
index, and then *folding a manifold on it*, because an index is not a cell. That needs a
foreign-store cache nothing in the tree has, and it splits a placeholder's status three ways —
resolved, not fetched yet, permanently absent — where a spinner and a dead link must not look alike.
The fold itself resolves nothing: it is `noexcept` and allocation-free, and resolution is I/O.

### 5.7 Cross-store ranks: the anthology

**Per foreign member, one placeholder `MakeCell` (5.6) and one `SetLink`. Wire: V4.** A rank that
threads through cells in several documents. A syllabus, a compilation, an issue of a journal is then
a structure map — not a copy, not a list of addresses in prose. The docuverse as one address space
where a compilation quotes rather than reproduces is the anthology argument of *Literary Machines*.
Distinct from `d.stores` in `multi_store.hpp`, whose cells represent stores, not cells inside them.
**Plan: [`structure-hyperop/5.7-anthology-ranks.md`](structure-hyperop/5.7-anthology-ranks.md)** —
thin by design, since §5.6 was built to carry it, and `walkRank` needs no change because a
placeholder is an ordinary local cell.

Two decisions the section leaves open, and one question it can close. The `d.stores` distinction is
sharper than "cells represent stores": `importManifold()` **copies** every cell, dimension, link and
span into a composite arena, so `d.stores` *reproduces* where an anthology *quotes* — the tree
currently has only the reproducing half. **Ruling taken: an anthology pins a state and does not
track a moving one.** `ExternOpRef` names a specific microversion, and tracking "whatever Alice's
current edition is" would make a document's content change because someone else edited theirs,
breaking the property that a document is what its own EDL says. The price is that an anthology goes
stale and refreshing it is an authorial act — a `SetLink` with an author and a diff, which is the
same answer transclusion already gives for content. And **U1 does not bite here**: an anthology is
tens of members walked from a cursor, not a scrollbar over five thousand cells, which is the
condition U1's own experiment names for the concession holding.

### 5.8 Quoted ranks: transclusion of a shape

**Two operations to quote, two to override. Wire: V4.** `spliceCellSpan` transcludes content into a
cell; nothing yet transcludes a *rank*. Quoting appends a `MakeCell` for the quotation head and a
`SetLink` from it to the placeholder for the foreign rank's head; traversal falls through at fold
time. Overriding one position appends a `MakeCell` with the local content and a `SetLink` to the
placeholder for the foreign cell it shadows — **keyed by that cell's `GlobalOpRef`, not by
ordinal**, which keeps U1 out of it. With the foreign store absent the quote is a visibly empty
rank.

A first draft called this "ArenaManifold's copy-on-write overlay made durable," which sounded like a
crossing of R8. It is not: R8's boundary is authorship versus navigation, and adopting someone's
shape is authorship; every appended operation is a user-generated update. The COW overlay is the
same *mechanism*, and mechanism is not regime.

This is the honest implementation of two things already wanted: §4.1 preset sharing (adopt a
published keymap by quoting its rank, then override one key) and schema adoption outside the system
stores. A published sequence — a reader's alternate path through an author's text — is also an
application of this rather than a mechanism of its own.

### 5.9 Published vocabularies

**No operations of its own; rides 5.8.** A set of dimension cells — `supports`, `refutes`,
`qualifies`; Toulmin's roles; a discipline's citation types — published as a store and adopted by
quoting its `d.dims` rank. Arguments across documents become comparable because they share a
vocabulary with an author, a version and a citation, instead of each author minting private
dimension names. `LinkType::Disagreement` is already Nelson's example; this makes disagreement a
publishable frame. Deferred behind 5.8 by dependency, not by weight.

### 5.10 Plural structure maps: overlays

**Appended in the overlay author's own store. Wire: V4.** Another author publishes Structure
operations *over your document's cells* — an outline, an argument map, a translator's alignment —
addressed through 5.6, folded in as an overlay at a `ProminenceTier`, never touching your spool. The
one-writer rule holds because an overlay is a separate store folded beside yours, never a second
writer to `ops.nodes`. An overlay is sparse by nature (an outline over ten thousand cells touches
dozens), so it holds boundary references, not a shadow of the base, and it records which sealed
state of the base it was written against so renumbering is a non-issue.

"THE AUTHOR'S LINKS ARE NO DIFFERENT FROM ANYONE ELSE'S" is quoted in `ops.hpp`; a structure map is
a link set and deserves the same pluralism, or the reader is back in a walled document. Last in the
order only because it needs an overlay *fold mode* — composing two manifolds at a tier — which
nothing in the tree has yet.

### Ordinary, needing no new mechanism

- **A second reading order** over a document's own cells is a second dimension on cells the
  projector already mints (R12); content-addressed, so unlike `PageBreak` it travels with a
  quotation (R1). Two operations per piece. Fine, and not a category.
- **Schema islands in any store.** The `d.schemas`/`d.alternates`/`d.default` pattern generalises
  today with zero new machinery. The validator runs as a pinned arena island on the write path,
  behind `Store::put*`, and never in `applyStructure`, which is `noexcept` and allocation-free.
- **Keymap macro *definitions* as Structure; invocations never.** R8 drawn on the right seam, and
  `promoteAndAttachToStore` already does it. One condition the Purist adds and this note adopts: a
  promoted definition must be a cell with readable content, not an opaque compiled artefact — a
  program nobody can read is not literature, and readability is the whole justification for
  persisting it.

## 6. Refused this round

- **Decomposing `Link` records into cells.** Four to eight times the bytes for a structure whose
  access pattern is point lookup by id and bulk scan by the Spanfilade. Wrong data structure; the
  table stays, and 5.4 makes the *act* of linking addressable instead.
- **A persisted `Manifold` checkpoint.** The fold is estimated at 4–10 million operations per second
  (from its component costs; §7), so a 60,000-operation slice folds in tens of milliseconds. A
  checkpoint is a derived artefact that can disagree with the spool — exactly what
  `verifyAgainstFullRebuild()` exists to catch — bought against fifteen milliseconds.
- **Glyph, layout or Spanfilade state as Structure.** Per-frame or derived; the pinned arena island
  is already the answer, and putting a derived index into operations is enfilade §3's crums-as-cells
  mistake in a second costume.
- **A slot per operation** (see 5.4) and **a stored reader-local foreign index** (see 5.6).
- **Dimension genealogy** as a category: "which dimension superseded which" is an ordinary link
  between two dimension cells and works today.
- **Live cursors, permission graphs, rank indices** — refused in 1.0 and still refused, for R8, the
  Merkle ledger, and enfilade §3 respectively.

## 7. Open, and measured before believed

- **No fold-rate measurement exists.** `tests/xudu/manifold_test.cpp` times a hop (R12 §12.5:
  9.9–11.3 ns per CSR hop), never a fold. The figures in §6 are estimates from `applyStructure`'s
  component operations and should be replaced by a benchmark before 5.7–5.10 add foreign cells.
- **`compact()` is the scaling risk, not the fold.** `linkFor()` triggers it when
  `links.size() > 2 * liveLinks + compactionSlack`, and it is linear in all links, so a build
  pattern that alternates relocation and compaction on a large manifold is quadratic. Benchmark it.
- **U1 is unchanged.** Nothing here needs random access along a rank; 5.8 keys overrides by
  `GlobalOpRef` specifically to stay out of it.
- **The overlay fold mode** 5.10 needs is unspecified.
- **Multi-author structure over one document within one store** was not resolved: the one-writer
  rule on `ops.nodes` makes it an overlay (5.10) or nothing, and whether that is the right answer
  for live collaboration is a question for the collaboration design, not this note.

## 8. The chain, in one picture

```text
5.1 CompactBinaryV4 ───────────────────────────────────────────┐
  (source, at, length, kind width, extension record, spans)    │
                                                               │
5.2 d.hist            zero ops, read API                       │
                                                               │
5.4 operation handles ── 5.5 editions rank (tables v4)         │
        │                                                      │
        └── 5.6 extern refs (externals table + placeholder) ◄──┘
                  │
                  ├── 5.3 pouch items as cells (refiled here)
                  ├── 5.7 anthology ranks
                  ├── 5.8 quoted ranks ── 5.9 vocabularies
                  └── 5.10 overlays (needs a fold mode)
```

## Appendix: Change History

Implementation plans live in [`structure-hyperop/`](structure-hyperop/), one per §5 subsection,
written in build order. Each is grounded in a fresh reading of the code, so where a plan contradicts
this note the plan is right and this note is corrected to match.

- **2.6** — Plan for 5.7 written; thin, as expected, since §5.6 carries the mechanism. Adds a ruling
  (an anthology pins a state, never tracks a moving one, because tracking would let someone else's
  edits change your document), sharpens the `d.stores` distinction to copying versus quoting, and
  closes U1 for anthology-scale ranks. Notes in passing that U1's experiment text points at
  `assets/zigzag/`, which no longer exists.
- **2.5** — Plan for 5.6 written. R4 turns out to have ratified the side table and its price
  already, so the ruling stands. Two corrections: the table holds a *local*
  `ExternOpRef{ScrollId, produces}` rather than a scroll-key string, mirroring `PrimediaSpan`
  against `GlobalSpan`; and the index goes in `value`, as in 5.4. The floated `SetExternLink` verb
  is dropped as unnecessary. The section's real cost is named: resolution requires loading *and
  folding* the foreign document, so phase 3 is a subsystem and should be scoped separately from the
  persistent model in phases 1–2.
- **2.4** — Plan for 5.5 written. The section stands: `currentVersions` is an array so the author
  can designate a current version per edition, aliases name them, and system stores take one by
  concession. The **code** has drifted — `syncCurrentVersions()` derives the designations from open
  windows on every save, silently redesignating a document whenever someone opens a third state to
  compare. An earlier draft of the plan read that drift as the design and proposed collapsing the
  array to one; that was wrong, is recorded in the plan, and would have destroyed the edition model.
  The migration to cells stands because an edition needs an identity the two tables cannot give it.
- **2.3** — Plan for 5.4 written. Its encoding corrected: the handle's target belongs in `value`,
  marked by `ValueKind::OpHandle`, not in the idle `sourceAt` — the fold already carries both halves
  into `CellSlot`, so the fold does not move. This widens §5.1's V4 record by one conditional field,
  without which a published handle silently names a different operation.
- **2.2** — 5.3 reviewed and **withdrawn as written**: its cell-drop path is already built, its
  "hand-rolled arena" is one drag payload, and nothing is written during a drag, so the arena buys
  none of the three things claimed. Refiled after 5.6 as "pouch items as cells", the pouch having no
  persistent item model and holding a bare cross-store `CellRef` R4 forbids. Build order updated.
- **2.1** — Plans for 5.1 and 5.2 written, and this note corrected against them. §3: the wire kind
  field is not full (V3 already widened it) and `storeToLinkPackage` is a different format, so both
  leave the V4 bump; the extension record is deferred with its reasoning. §5.2: `byRef` does not
  hold the chain, the walk needs a `Store`, and a past *state* needs a replay rather than a lookup.
- **2.0** — Full rewrite after a grounded tripartite pass. Removed every proposal that was already
  built or already refused; added the §3 defect and its verification; replaced the flat idea list
  with a prerequisite chain; recorded the two rulings taken in 5.4 and 5.6.
- **1.0** — Initial brainstorm. Superseded; its errors are listed in the preamble.
