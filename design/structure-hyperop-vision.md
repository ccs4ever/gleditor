# Beyond Cells: A Vision for the OSMIC Structure Hyperop

**Status:** speculative — a brainstorm synthesis, not a ruling. Nothing here changes `ops.hpp`,
`CompactOpNode`, or any on-disk format on its own; each category below would need its own design
note (in the shape of [`vlog-logic-extension.md`](vlog-logic-extension.md) or
[`enfilade/enfilade-rank-indexing.md`](enfilade/enfilade-rank-indexing.md)) before any code moves.

`OpKind::Structure` (migration step 12) was built to let a Zigzag manifold be a second replay
product of the same ops spool that produces a document's text. But a "structure map" op is not a
cell-grid feature — it is OSMIC's general mechanism for recording a typed, directional, versioned
edge between two addressable points in permascroll. A Zigzag cell link is one instantiation of that:
the case where both endpoints happen to be dimension coordinates. This note catalogs what else the
same mechanism could instantiate, produced by a tripartite dialectic (Purist thesis, Systems Realist
antithesis, Codebase Expert grounding) and converged into verdicts.

## How to read the table

- **Nelsonian case** — why this is more than convenience; which invariant from Nelson's own writing
  it satisfies.
- **Systems verdict** — cheap/natural, expensive-but-tractable (with mitigation), or wrong data
  structure, argued from the append-only spool, CSR link-run shape, and the render/UI thread budget.
- **Codebase reality** — what already exists, what's greenfield, and which hard invariant
  (`CompactOpNode`'s 64B immutability, the one-writer rule, `ephemeralBit`, `sourceOpIndex`
  addressing) it must respect.

## 1. Transclusion provenance graphs

**Nelsonian case:** Xanadu Green's promise that every quotation is traceable to its origin, made
into queryable data instead of an inference reconstructed from spans.

**Systems verdict:** Cheap and natural. Write-once per transclusion, read-heavy via per-cell CSR
fan-out — exactly the access pattern CSR is built for. Durable by nature (attribution matters
forever).

**Codebase reality:** Partially built. `Manifold` links plus `zz_xudu_projector.cpp`'s `d.role` /
`d.mime` / `d.media` dimensions already encode instance relationships, and
`UnifiedTransclusionEngine` (step 19) folds ops but has no production caller yet. A dedicated
`d.provenance` / `d.derived-from` dimension convention is enough — `SetLink` already suffices, no
`CompactOpNode` change.

**Verdict: build it.** Lowest risk, highest fidelity to the core insight, and the closest to
already-shipped.

## 2. Rhetorical/argument structure

**Nelsonian case:** ZigZag's own founding demonstration was argument mapping — claims linked by
`supports`/`attacks`/`refines` edges on a dedicated dimension. This is not an extension of Zigzag;
it's the use case it was built to prove.

**Systems verdict:** Cheap, low write frequency (an author restructures an argument occasionally,
not per keystroke) — as long as no UI does live drag-to-reorder against it (that pushes toward
category 6's failure mode; keep commits coarse, on release, not per-frame).

**Codebase reality:** Greenfield. Current links carry direction but no edge label beyond the
dimension cell itself — a `Supports`/`Attacks` relation either becomes the dimension cell's identity
(fits today, zero new fields) or wants a real edge-attribute, which competes for the one spare bit
in `CompactOpNode::flags` (see the cross-cutting note below). Prefer the dimension-cell encoding
first; it needs nothing new.

## 3. Hypertime/version DAGs as structure

**Nelsonian case:** version history is content, navigable the same way text is — collapsing "history
browser" and "document browser" into one mechanism.

**Systems verdict:** Expensive but tractable, with a real trap. The DAG a version query wants is
full-spool-scale topology, not one cell's CSR run — re-encoding it as ordinary Structure ops is
redundant with what microversioning already gives for free, and a naive fold would refold the whole
spool for a question CSR isn't shaped to answer.

**Codebase reality:** Not built via Structure today; `osmic-microversioning-and-dag.md`'s own
branching already carries this, and `MicroversionId` is separately `operator<=>`-sortable. Making
the version DAG a `Manifold` would duplicate existing information.

**Verdict: don't build the graph — build the projection.** A `Store::rebuildVersionManifold()`
read-only fold over existing microversion metadata gets the query power (browsable/forkable history,
Nelson's actual insight) without a second source of truth. This is the load-bearing idea of the
whole brainstorm precisely because it's the one that resists being "just another Structure op" — the
right shape is a fold, not new ops.

## 4. Transcopyright royalty/micropayment flow graphs

**Nelsonian case:** royalty obligations must propagate through every transclusion chain; payment
provenance and quotation provenance are the same edge, viewed differently.

**Systems verdict:** Cheap, and the textbook case for "rare update, must be durable." Established
once (or on infrequent renegotiation) per document/scroll, settled in batch/background — never
touches the render loop or the input path.

**Codebase reality:** Genuinely new territory — `publication.hpp/.cpp` and `identity/` have no
`CompactOpNode` involvement today. Needs a `d.royalty` dimension and a deliberate choice: is a
payment *tick* ephemeral (ArenaManifold, since a flow computation shouldn't mint permanent ops per
micropayment event) versus the *agreement* itself, which is durable and belongs in real Structure
ops. `ephemeralBit` already exists for exactly this split.

## 5. Annotation and marginalia graphs

**Nelsonian case:** Nelson rejected inline comments as content pollution — annotation belongs beside
the text, on its own dimension, always two-way traversable, never interleaved.

**Systems verdict:** Cheap if write rate stays human-paced (mouse-up/blur commits, not
per-drag-frame updates).

**Codebase reality:** Largely already the model via the existing `Link`/`LinkType`/`ProminenceTier`
xanalink system — this category is less "greenfield" than "should the existing annotation links and
the Manifold's `d.*` link space be the same mechanism," an architectural unification question rather
than a code gap.

## 6. Live collaborative cursors / per-keystroke provenance

**Systems verdict: fundamentally the wrong data structure.** A cursor position changes at
input-event frequency across N collaborators. Spooling one Structure op per movement means unbounded
append-only growth for data nobody wants past the session, `rebuildManifold()`'s full-refold cost
sitting on a render-adjacent path, and CSR churn on a handful of hot cells — the exact pattern CSR
handles worst. Keep live cursors as transient state-channel messages over the existing BEP10 wire
path; the durable record of "who edited what span when" is already the ops themselves, no new edge
needed.

## 7. Permission/identity graphs

**Nelsonian case:** "who may edit this" answered by the same relationship engine that answers "what
links to this," rather than a parallel, siloed ACL system.

**Systems verdict:** Expensive but tractable, and off the hot path in terms of latency — but a
*dense, global* many-to-many graph (many users × many documents) is the pattern per-cell CSR fan-out
handles worst if cells are documents rather than users.

**Codebase reality — biggest invariant collision on the list.** Identity/permission already lives in
`identity/` (BEP10 plugins, `MerkleLedger`), which is append-only and built for exactly this kind of
consensus data. Recasting it as generic Structure ops would also raise the **one-writer rule**
sharply: `SegmentedOpsSpool::writeSegmentFile()` is single-author-oriented today, and a permission
graph implies multi-writer semantics the current model has no answer for.

**Verdict: don't build it on Structure.** The Merkle ledger is the right home; don't make Structure
a second identity system.

## 8. Enfilade rank indices (open question U1)

**Codebase reality — the design notes already rule this out.**
[`enfilade-rank-indexing.md`](enfilade/enfilade-rank-indexing.md) §3 is explicit: crums as ordinary
cells would make rebalancing an edit to the document, which R8 forbids for the same reason it
forbids path compression. This wants a *third* replay product alongside `Manifold`, with a swappable
per-consumer Wid (count / Bloom summary / span set), not a `d.*` dimension.

**Verdict: explicitly not a Structure-map use case.** Worth restating in this vision doc precisely
so it doesn't get quietly reopened by a future reader skimming the brainstorm out of context.

## 9. Spatial/3D beam layout as structure

**Systems verdict:** ArenaManifold territory, not durable Structure. Beam geometry (spring tension,
anchor preference) is recomputed every frame or on layout invalidation, carries no attribution or
hypertime meaning, and modeling it as permanent ops would mean re-authoring "the same" geometry
forever as the window resizes — spool bloat with zero informational value, and a likely violation of
the hot-rendering-loop zero-allocation rule.

**Codebase reality:** Already implemented outside Structure (`apps/xudu/beams.cpp`, `framing.cpp`,
computed directly from `Store`/`Link` data at render time). If this is ever formalized, it belongs
in `ArenaManifold`'s `mark()`/scratch/`discard()` cycle — the pattern already reserved for exactly
this kind of ephemeral, derived graph.

## 10. Undo/redo as structure

**Systems and codebase verdict: redundant, already better solved.** `CompactOpNode` is immutable
once stored and the spool is append-only, so undo is inherently "replay to an earlier microversion,"
not a separate structure graph — encoding it a second time as Structure ops would duplicate ordering
information the ops spool already gives for free. `ArenaManifold::release()`/`discard()` is already
the ephemeral analog (mark/release = undo-by-truncation). No new machinery needed.

## 11. Cross-document docuverse topology

**Nelsonian case:** the entire point of Xanadu since 1960 — a single addressable literature where
every relationship is visible, two-way, and traversable, and everything else on this list is a
special case of it.

**Systems verdict:** Same shape as category 1 (write-rare, read-via-fan-out, durable), just spanning
`ScrollId` boundaries instead of staying within one store — no new access-pattern risk, only a
scoping question (does a fold need to touch more than one store's spool at once).

**Codebase reality:** Greenfield, and the most consequential kind of greenfield: it's the natural
generalization of category 1's provenance edges once they're allowed to cross scroll boundaries.

## 12. Type/schema-as-structure

**Nelsonian case:** a cell's meaning shouldn't be out-of-band convention — types are literature too,
transcludable and versioned like anything else.

**Systems/codebase verdict:** Cheap; `has-type` as an ordinary `SetLink` to a schema-defining cell
needs nothing new. Mostly a documentation/convention exercise once category 1's plumbing exists.

## 13. Vlog unification and clause selection

Already the most concretely scoped item in this space and already underway, not a new proposal:
[`vlog-logic-extension.md`](vlog-logic-extension.md) has step 21 (`ArenaManifold`) done, with §6
clause selection explicitly gated on U1 (item 8 above). Its ask on the C++ core is deliberately
small — two methods and a 16-byte-per-entry vector on `ArenaManifold`, no `ops.hpp`/`Manifold`/
on-disk format changes. Listed here because it's the existence proof that "generalize Structure
without touching the wire format" is not merely a design-doc aspiration — it's already the pattern
one real feature followed.

## Cross-cutting constraint: the flag-bit budget

`StructureVerb` occupies bits 0-2 of `CompactOpNode::flags` (4 of 8 possible verbs already used:
`MakeCell`, `SetLink`, `SetValue`, `Splice`), `ValueKind` occupies 3 bits (4 of 8 values used:
`None`/`Double`/`Bool`/`Int64`), and there is exactly **one free bit** left in the whole encoding.
Categories 2 and 4 above, if they ever want a genuine new verb or edge-attribute value rather than a
dimension-cell encoding, are pressure on that budget — not free. The fix, if it's ever needed, is a
`ValueKind::Extended` indirection (a small typed side-table keyed by a spare code) rather than
widening `CompactOpNode` past 64 bytes, since the struct's size, alignment, and `value` offset are
each pinned by a `static_assert` for reasons unrelated to this brainstorm (a sealed segment is
mapped `PROT_READ`; the struct's shape is a promise made to already-written files).

## What's essentially Nelsonian vs. merely convenient

Three ideas are the core of this vision, in the sense that they're the least "convenient" and the
most load-bearing:

- **#2, rhetorical structure** — not an extension of Zigzag, Nelson's own founding demonstration of
  it.
- **#3, hypertime as structure** — dissolves the false distinction between "the document" and "its
  history" that most systems never question; the deepest use of "a second replay product," even
  though the systems verdict says build it as a read-only fold, not new ops.
- **#11, cross-document docuverse topology** — the entire point of Xanadu since 1960; everything
  else here is a special case of it.

The rest — royalty flow, annotation, type-as-structure — are important, correctly cheap, and worth
building, but they are Nelsonian principles applied to practical subsystems rather than the core
insight itself. And three ideas are ruled out cleanly enough to be worth stating plainly so a future
reader doesn't reopen them by accident: **live cursors** (wrong cadence for an append-only spool),
**permission graphs** (already has a better home in the Merkle ledger, and collides with the
one-writer rule), and **enfilade rank indices** (explicitly forbidden by R8 in the existing design
note).

## Suggested next design notes, in priority order

1. Transclusion/cross-document provenance (#1 + #11) — lowest risk, `d.provenance` dimension
   convention only, extends what's already shipped.
1. Version-DAG-as-fold (#3) — a read-only `Store::rebuildVersionManifold()`, no new ops.
1. Rhetorical structure (#2) — dimension-cell encoding, no `CompactOpNode` change.
1. Royalty flow graphs (#4) — the first case needing a real ephemeral/durable split decision, good
   proving ground for `ephemeralBit` outside Vlog.
