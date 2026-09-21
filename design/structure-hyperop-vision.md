# Beyond Cells: What Else MAKE/CHANGE STRUCTURE MAP Can Carry

**Document Version:** 2.14 — dependency-audited rewrite. **Status:** a vision note and a ranked
proposal, not a ruling. Nothing here changes `ops.hpp`, `CompactOpNode`, or any on-disk format on
its own; each numbered proposal in §5 needs its own design note in the shape of
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
`sourceAt`, `sourceLength`. The proposals deliberately leave them idle; handles and external
references use the existing typed `value` plus ordinary cell content.

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

**The fix is `CompactBinaryV4`, after the graph it must carry is settled in §5.2 and §5.5.** The
writer already has stable operation names. V4 writes `source` by `MicroversionId`, and must do the
same for a `SetLink`'s dimension and target and an `OpHandle` target: all are publisher-local
indices today, and all may renumber when `opRecords()` orders records or a foreign history gains a
branch. The same record adds varint `at` and `length` for `Splice` (U3.4 already owes this; export
currently throws rather than corrupts). Ingestion resolves every name through `indexOf()` before
`putOp`. **The required test publishes a branched slice with links and handles, ingests it, folds,
and asserts `refusedOps() == 0` and `equivalentTo()` the original.**

Two further items were listed here in draft and did not survive the code review in
[`structure-hyperop/5.6-compact-binary-v4.md`](structure-hyperop/5.6-compact-binary-v4.md) §2. The
wire kind field is **not** full: `CompactBinaryV3` already widened it to four bits, so
`BinStructure = 7` is the eighth of sixteen values with eight free after it — the claim was true of
version 2 and was carried forward stale. What *is* full is the tag byte, four kind bits plus four
flags, held by a `static_assert`; a new *flag* needs a second byte, and V4 needs no new flag. And
`storeToLinkPackage()`'s first-span-only defect is a change to `LinkPackage`'s own wire format in
`link_package.cpp`, not to the ops spool — still owed, but not in this bump, because moving two
formats' versions in lockstep couples things that have no reason to agree.

A length-prefixed, skippable extension record was proposed for the same bump, on the ground that
R11's "no compatibility" is about files on this machine rather than a wire two peers negotiate. The
plan recommends deferring it: §5.5 stores variable-length reference descriptors as ordinary cell
content, so the ops wire needs only the address names it can test today.

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
registry, local segment locations, link records, designated current versions and version
annotations. Four are document facts or authorial decisions with no hypertime name; §5.4 moves them
to cells after §5.2–§5.3 define their final shapes. Only `localSegments` stays a table, because it
says where this copy's bytes live rather than what the document is. Scroll lookup bootstraps from
local scroll 0 and becomes a replay index, so name resolution remains fast without making the
rewritten table a source of truth. Under branching the flat `currentVersions` vector is already
wrong: two futures of one document share one list of heads.

**A link to a document you do not have yet is normal, not corruption.** A docuverse is mostly
elsewhere. Today the fold has one response to a target it cannot resolve — `refusedOps_++`, whose
documented meaning is "the spool holds something unintelligible." Any cross-store structure needs a
second, visible, non-pejorative state.

## 5. Proposals, in build order

Each entry states what it consumes and what it hands to later steps. The order is a dependency
order, not a ranking of desirability. Full implementation plans live in
[`structure-hyperop/`](structure-hyperop/); where this summary and a plan disagree, the plan is the
source of truth.

### 5.1 `d.hist`: a cell's own past as a rank

**Depends on:** the existing R7 operation chain. **Provides to 5.2 and 5.3:** a read-only history
walk and reconstruction of a cell at an earlier operation. **Appends nothing. No wire change.**

`byRef` answers which cell an operation belongs to; it does not store the chain. The chain walk
therefore follows `sourceOpIndex` through the ops spool and needs a `Store`. Rendering an earlier
state is a mini-replay because `Splice` is a delta. Nothing is persisted, so R8 is untouched.
**Plan:** [`5.1-cell-history.md`](structure-hyperop/5.1-cell-history.md).

### 5.2 Operation handle cells

**Depends on:** 5.1 only for history assertions; the mechanism itself is independent. **Provides to
5.3, 5.4 and 5.12:** a persistent cell naming any exact operation. **Wire completion:** 5.6.

A handle is an ordinary `MakeCell` with `ValueKind::OpHandle`; its local target index lives in
`value`. The handle has its own unbroken R7 chain. On publication the target travels by
`MicroversionId`, because a local op index renumbers on ingestion. Handles make edits, link-making
acts and explicit breaks addressable without changing what `sourceOpIndex` means. Link cells are not
minted merely for addressability here; 5.4 later mints them for hypertime identity. **Plan:**
[`5.2-operation-handles.md`](structure-hyperop/5.2-operation-handles.md).

### 5.3 The editions rank

**Depends on:** 5.1 history and 5.2 handles. **Provides to 5.4:** the final operation-backed shape
that replaces `currentVersions` and `versionAnnotations`.

An edition is a persistent cell. Its designated state is a handle, aliases and descriptions are
ordinary cells, and branches may designate different heads without sharing one mutable array.
`currentVersions` remains only as a transitional compatibility cache until 5.4 removes the table; no
separate table-format bump lands in this step. System stores retain their one-edition policy as a
consumer concession, not a restriction in the model. **Plan:**
[`5.3-editions-rank.md`](structure-hyperop/5.3-editions-rank.md).

### 5.4 The store's tables as cells

**Depends on:** 5.2 handles and 5.3 editions. **Provides to 5.5 and 5.12:** rank-backed scroll,
annotation, edition and classic-link identities plus replay indexes. **Keeps:** `localSegments`,
because it describes where this copy lives rather than what the document is.

Four rewritten side tables move into ordinary Structure: annotations attach to handles; editions
replace designated-version rows; scroll cells form `d.scrolls`; and butterfly links become cells
whose endpoint cells retain their spans. A scroll cell contains its global key; `ScrollId` remains a
derived replay index and is never persisted as identity. Many references to one scroll live on a
per-scroll `d.scroll-refs` rank, because a degree-two ZigZag dimension cannot connect every
placeholder directly to the same registry cell. Fast lookup is a replay product rather than a
mutable source of truth. This lands before extern refs so no temporary `externals` table is built.
**Plan:** [`5.4-tables-as-cells.md`](structure-hyperop/5.4-tables-as-cells.md).

### 5.5 Persistent references to cells in other stores

**Depends on:** 5.4 scroll cells; shares 5.2's `ValueKind` mechanism. **Provides to 5.6–5.12:** the
persistent boundary cell, `GlobalOpRef` lookup, and `GlobalDocumentState` snapshot descriptor.

A placeholder is an ordinary cell with `ValueKind::ExternRef`, filed on its scroll cell's
`d.scroll-refs` rank. Its canonical `MicroversionId` is ordinary descriptor content: it cannot fit
in the 64-bit `value` field. A replay index interns `(ScrollId, MicroversionId)` and maps in both
directions. The fold records unresolved placeholders but never performs I/O; resolution above the
fold may load and fold a foreign store. Missing bytes are `NotFetched` or `Absent`, not malformed
operations. `GlobalDocumentState{scroll, version}` separately names a whole snapshot for pouch and
overlay provenance. **Plan:** [`5.5-extern-refs.md`](structure-hyperop/5.5-extern-refs.md).

### 5.6 `CompactBinaryV4`: carry every Structure address by name

**Depends on:** 5.2's handle target and 5.5's final persistent graph. **Provides to 5.7–5.12:** a
stable publication boundary. **No new operations.**

The current binary record loses `source`; it also writes a `SetLink` dimension and target as local
indices. All three may silently retarget after `opRecords()` ordering or branch insertion. V4 writes
the `MicroversionId` of `source`, `SetLink` dimension and target, and an `OpHandle` target; it also
writes `Splice`'s `at` and `length`. `OpRecord` carries wire-only names and ingestion resolves all
of them through `indexOf()` before `putOp`. Version 3 is refused by number. The text OSMIC form
remains a local/debug encoding, not a portable publication path. **Plan:**
[`5.6-compact-binary-v4.md`](structure-hyperop/5.6-compact-binary-v4.md).

### 5.7 Arena federation: the composition substrate

**Depends on:** 5.5 global identities and 5.6 stable publication. **Provides to 5.9, 5.10 and
5.12:** identity-preserving foreign spaces, provenance-bearing bound dimensions, and explicit edge
scopes.

Each attached store has one arena store cell on `d.stores`; its many lazy proxy cells live on a
per-store `d.store-refs` rank. A proxy carries `(space, operation index)` only ephemerally and keeps
foreign identity without copying a manifold. Promotion maps a proxy through 5.5 rather than trusting
an ephemeral ref. `EdgeScope::Explicit` plus `permitEdge()` lets a consumer expose an induced
subgraph without traversal escaping into the whole foreign store. Bound dimensions record their
provenance: explicit and `NameMatch` modes are generic here; 5.11 later supplies `SharedIdentity`
evidence without becoming a prerequisite of federation. **Plan:**
[`5.7-arena-federation.md`](structure-hyperop/5.7-arena-federation.md).

### 5.8 Pouch items as cells

**Depends on:** 5.2 handles, 5.4 rank-backed storage and 5.5 global provenance. **Provides:** a
persistent pouch model; nothing later depends on it.

Drag staging already writes nothing and needs no arena redesign. The real defect is that pouch
contents are RAM-only while drops emit annotations nobody reconstructs, and a dropped ZigZag cell
stores a bare foreign index. Each item therefore becomes a cell on its zone's `d.items` rank, with
optional `GlobalDocumentState` and `GlobalOpRef` origin descriptors. Preview text is derived;
dismissal moves the item to `d.dismissed` rather than erasing its authorship. **Plan:**
[`5.8-pouch-items.md`](structure-hyperop/5.8-pouch-items.md).

### 5.9 Cross-store ranks: the anthology

**Depends on:** 5.5 persistent references, 5.6 publication and 5.7 federation. **Provides to 5.10
and 5.12:** the pin-and-refresh policy for authored references to foreign states.

An anthology is an ordinary local rank whose members are persistent placeholders. Rendering attaches
each referenced store and uses its federation proxy; it never copies the member into the anthology.
A member pins a state. Refresh is an authored repoint whose previous value remains in history, never
an automatic move to another author's latest state. `d.stores` remains an ephemeral federation
workspace and is not the persistent anthology model. **Plan:**
[`5.9-anthology-ranks.md`](structure-hyperop/5.9-anthology-ranks.md).

### 5.10 Quoted structure: adopting somebody else's shape

**Depends on:** 5.5 references, 5.6 publication and 5.7 explicit federation scopes. **Provides to
5.11:** vocabulary adoption and the general shape-transclusion mechanism.

A quotation names a deterministic selector (`cell`, `rank`, declared-dimension `closure`, or
versioned VQL `query`), not merely a rank. The resolver folds the pinned foreign state, attaches it
with `EdgeScope::Explicit`, exposes proxies for the answer set, and permits only edges whose two
ends are selected. The persistent fold never performs I/O and no foreign cell or span is copied.
Operations and small reference descriptors are required to resolve shape; selected content remains
lazy. Overrides key on `GlobalOpRef`, and splicing preserves the local rank tail. **Plan:**
[`5.10-quoted-structure.md`](structure-hyperop/5.10-quoted-structure.md).

### 5.11 Published vocabularies

**Depends on:** 5.5 persistent term references, 5.7 bound-set provenance and 5.10 quotation.
**Provides to 5.12:** `SharedIdentity` dimension binding.

Publishing and adopting a vocabulary use ordinary cells and a `rank` quotation. A term becomes a
usable local dimension only when its foreign identity is filed as a 5.5 placeholder on local
`d.dims`; equal rendered names never imply identity. A release operation pins the catalogue state,
while the term's birth operation remains its stable identity. Federation groups per-space dimensions
by that shared `GlobalOpRef` and records `SharedIdentity`, distinct from a reader-chosen
`NameMatch`. **Plan:**
[`5.11-published-vocabularies.md`](structure-hyperop/5.11-published-vocabularies.md).

### 5.12 Plural structure maps: overlays

**Depends on:** 5.2 handles, 5.4 link cells, 5.5 snapshot and boundary descriptors, 5.6 stable
publication, 5.7 federation and 5.11 dimension identity. **Provides:** the final composition layer
for Structure and classic xanalink claims.

An overlay is a separate signed store. Its `d.overlay-targets` rank pins one or more
`GlobalDocumentState`s; its `d.overlay-claims` rank holds handles to exact `SetLink` acts,
preserving authored subjects and explicit breaks that a final manifold would erase. A reader
attaches target and overlay spaces, validates snapshot ancestry, translates boundary placeholders
through federation, and arbitrates both directional slots before materialising winners. Intrinsic
target structure wins by default; every losing candidate and its provenance remain inspectable.
Trust tier comes from the reader's verified graph, never a field the overlay author self-assigns.
Because 5.4 already made classic links cells, they join this layer after the Structure path is
established, without a second overlay format. **Plan:**
[`5.12-plural-structure-maps.md`](structure-hyperop/5.12-plural-structure-maps.md).

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

- **Decomposing `Link` records into cells merely to make the act addressable.** §5.2 handles that
  narrower need. Section 5.4 nevertheless moves links to cells for a different reason: a
  cross-author link set needs hypertime identity before it can participate in §5.12's overlay model,
  and pays the fold cost explicitly.
- **A persisted `Manifold` checkpoint.** The fold is estimated at 4–10 million operations per second
  (from its component costs; §7), so a 60,000-operation slice folds in tens of milliseconds. A
  checkpoint is a derived artefact that can disagree with the spool — exactly what
  `verifyAgainstFullRebuild()` exists to catch — bought against fifteen milliseconds.
- **Glyph, layout or Spanfilade state as Structure.** Per-frame or derived; the pinned arena island
  is already the answer, and putting a derived index into operations is enfilade §3's crums-as-cells
  mistake in a second costume.
- **A slot per operation** (see 5.2) and **a stored reader-local foreign index** (see 5.5).
- **Dimension genealogy** as a category: "which dimension superseded which" is an ordinary link
  between two dimension cells and works today.
- **Live cursors, permission graphs, rank indices** — refused in 1.0 and still refused, for R8, the
  Merkle ledger, and enfilade §3 respectively.

## 7. Open, and measured before believed

- **No fold-rate measurement exists.** `tests/xudu/manifold_test.cpp` times a hop (R12 §12.5:
  9.9–11.3 ns per CSR hop), never a fold. The figures in §6 are estimates from `applyStructure`'s
  component operations and should be replaced by a benchmark before 5.7–5.12 add foreign cells.
- **`compact()` is the scaling risk, not the fold.** `linkFor()` triggers it when
  `links.size() > 2 * liveLinks + compactionSlack`, and it is linear in all links, so a build
  pattern that alternates relocation and compaction on a large manifold is quadratic. Benchmark it.
- **U1 is unchanged.** Nothing here needs random access along a rank; 5.10 keys overrides by
  `GlobalOpRef` specifically to stay out of it.
- **Overlay arbitration is specified and unmeasured.** Section 5.12's two-slot selection prevents
  asymmetric ranks, but its claim-index and composition costs need measurements before production
  discovery is enabled.
- **Multi-author structure over one document within one store** was not resolved: the one-writer
  rule on `ops.nodes` makes it an overlay (5.12) or nothing, and whether that is the right answer
  for live collaboration is a question for the collaboration design, not this note.

## 8. The chain, in one picture

```text
5.1 cell history
  └── 5.2 operation handles
        └── 5.3 editions
              └── 5.4 tables as cells
                    └── 5.5 extern refs and document states
                          ├── 5.8 pouch items (leaf)
                          └── 5.6 CompactBinaryV4
                                └── 5.7 arena federation
                                      ├── 5.9 anthology ranks
                                      └── 5.10 quoted structure
                                            └── 5.11 published vocabularies
                                                  └── 5.12 plural structure maps

Additional direct edges into 5.12: handles (5.2), link cells (5.4), stable wire (5.6), and
federation (5.7).
```

## Appendix: Change History

Implementation plans live in [`structure-hyperop/`](structure-hyperop/), one per §5 subsection,
written in dependency order. Each is grounded in the current code and the related design notes, so
where an older historical label disagrees with a current filename or dependency, the current plan is
authoritative.

- **2.14** — Re-audited all twelve plans as one build. The order is now history, handles, editions,
  tables, extern refs, binary V4, federation, pouch/anthology side branches, quotation,
  vocabularies, then overlays. Tables precede extern refs so there is no temporary externals table;
  V4 follows the final address-bearing graph and carries every Structure address by operation name;
  federation precedes quotation and exposes an explicit edge scope so a selector cannot leak into a
  whole foreign graph; vocabulary identity extends federation rather than forming a dependency
  cycle; and overlays follow both link cells and published dimension identity. The audit also adds
  per-owner membership ranks required by ZigZag's degree-two geometry, gives variable-length
  `MicroversionId`s ordinary descriptor content instead of a 64-bit field, and shares
  `GlobalDocumentState` between pouch provenance and overlay targets.
- **2.13** — Added the plural-structure-map plan and separated arena federation from overlay
  precedence. This revision's renumbering supersedes the numbers recorded in that historical plan.
- **2.12 and earlier** — Grounded the individual proposals against the implementation, correcting
  operation handles, edition semantics, cross-store references, selector-based quotation, published
  dimension identity and arena proxies. Their substantive findings are incorporated into §5 and the
  current plans; superseded numbering is intentionally not repeated here.
