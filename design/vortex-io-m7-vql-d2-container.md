# M7-VQL-d2 alternative: `join` as a dimensional pack

Status: design, no code. Written at the owner's request before the M7-VQL-d2 (P11) gate closes
([`vortex-io-m7-vql-d2.md`](vortex-io-m7-vql-d2.md)). It evaluates replacing the single-cell `join`
with the container the repo already specifies for compositing cells that are read out in sequence:
the **dimensional pack** of [`view-system.md`](view-system.md) (§3.6 V-R27 to V-R30, §9.3.3, §9.3.9,
decision V2, open question VU4). The first revision of this note specified clone views on
`d.contains`/`d.seq`; the owner pointed to the pack spec, and this revision follows it (section 4
compares the two). **Recommendation: (C) keep P11 for the gate, and build the pack as a rowset
feature on its own merits.** The pack is sound as a representation of `join`; what decides against
it is that it swaps a built, gated 50-line change for an unbuilt one in a different core file and
still needs P11's algorithm at save time (section 10). No P13 is reserved under (C).

## 1. What the pack spec says, and what the join would use

`view-system.md` is a proposal, unbuilt (its status line), so there is no `ViewDim`, `packDim()` or
`packingDim()` in code; they are declared in the spec's `ViewManifold` interface
(`view-system.md:1062-1063`) and nowhere else. What *is* built, and what the pack needs:

- **Occurrence** (V-R29, §6.3 `view-system.md:498-514`): a bare cell whose typed value is
  `ValueKind::OpHandle` naming its target, made with `arena.makeCell()` and
  `arena.setValueBits(cell, OpHandle, target)` (`arena_manifold.hpp:970`,
  `arena_manifold.cpp:2347`), read with `handleTarget` (`arena_manifold.hpp:919`,
  `arena_manifold.cpp:1970`). The relation to the target is a *value, not a link*, so the target's
  slots are not touched and nothing is shadowed.
- **`d.pack` and `d.packing`** (V-R30, V2): the container's posward `d.pack` neighbour is its first
  constituent; the constituents chain on `d.packing`. They are two dimensions because a nested pack
  is both a constituent of its parent and a container of its children and would otherwise need two
  posward neighbours on one dimension (`view-system.md:1990-1993`, `:2976-2979`). This is exactly
  the firstborn-plus-sibling encoding of `sql-to-zigzag.md` §2.2, §3.1 and `GenericDim::Containment`
  and `Sequence` (`anything-to-zigzag-lens.md:259-263`) under the view system's names; a cell holds
  one posward and one negward neighbour per dimension and `ArenaManifold::link` silently evicts
  whatever either end held (`arena_manifold.cpp:2360-2440`), so no other encoding is possible.
- **Keeping a pack** (§9.3.9, `view-system.md:2107-2114`) goes through `zigzag::promote()`. The spec
  cites `arena_manifold.cpp:1838`; that line is now `dimensionsOf` and the handle promotion is at
  `:3201-3215` (checks) and `:3385-3393` (`makeOpHandle`, `makeForeignOpHandle`).

For the join the dimensions are minted in the query's arena with
`VortexCore::findOrMintDimension("d.pack")` and `("d.packing")`, as `LensRepl::entries` mints
`d.contains` and `d.seq` (`lens_repl.cpp:48-49`). A view system's derived arena has its own, so the
two never share a cell; the names say the same relation in both.

## 2. What is built today

`join_step` collects one `JoinPiece` per row and separator; `join_end` decides whether every piece
can share one namespace and mints **one** cell: `makeQuote(space, runs)` or interned text
(`join_relation.cpp:159-265`; the namespace rule and the "free piece" precondition are `:99-155`,
the text fallback `:226-258`). Every consumer sees one cell whose own content is the joined text, so
`textOf`, `VortexCore::render` (`vortex_core.cpp:322-339`), `readQueryScalar` and the write path
work with no knowledge that it was a join. P11 is `quotedRun`, a scratch range check in `makeQuote`
(`arena_manifold.cpp:2213`, `:2230`, `:2257`) and two loops in `promoteOrRefuse` (`:3261`, `:3352`):
34 and 16 lines in the two files of `bed41595`; the rest of d2 is application code.

The limit the owner named: rows of two stores that each have external scrolls are joined as text
(`join_relation.cpp:235-237`). Section 6 shows no front end reaches it.

## 3. The pack, specified

For rows $r_1 \ldots r_n$, $n \ge 2$:

```text
K  (container; content: empty)
 | d.pack
 v
C1 --d.packing--> S1 --d.packing--> C2 --d.packing--> S2 ... --d.packing--> Cn

Ci  an occurrence: OpHandle -> clone master of row i      (no link to the row)
Si  a separator: a plain cell whose content is the separator's scratch span
```

**Constituents are occurrences of the rows' clone masters** (V-R29): `Ci` holds the master's
`CellRef` as a value. A row that is a proxy, a foreign cell, a quote or a capture is named the same
way, and nothing in its slots changes.

**Separator: interstitial constituents, chosen.** The options were a constituent between each pair
of rows, or the separator as the container's content (the view system gives a group its name as
content, `view-system.md:646`). Chosen: constituents, each a cell holding the one interned separator
span. Reasons: the pack then says exactly what is read out, in order, with no hidden rule "put the
separator between"; the readout is a plain concatenation; a nested pack composes with no gap logic;
and a consumer unaware of packs reads a container with *empty* content, a failure that is at least
detectable, where a separator as container content makes it read `","` and pass it on as the join.
Price: $2n - 1$ constituents instead of $n$, and `count` over the pack counts both. The container
content option is the cheaper in cells (one cell, not $n - 1$); constituents are chosen for the
failure mode.

**Container marker: none.** A cell with a posward `d.pack` neighbour is a pack container by
definition (V-R30). A cell with none is not, so the empty and one-row joins need no container.

**Empty and one-row.** The empty join stays the empty text cell (`join_relation.cpp:213-214`); a
one-row join returns the row itself (`:211-212`, `walk->only`), so its identity and what links to it
are not touched.

**A join of joins** is a pack whose constituent `Ci` is an occurrence of another container, so the
same pack may be a constituent of two parents without two cells on one `d.packing` rank (a row or
container named twice is two occurrence cells naming one target).

**Bound.** `join_step` keeps charging each row's bytes against the 1 MiB bound before it takes the
row (`join_relation.cpp:174-179`, `limits.hpp:13`); the readout charges it again.

## 4. `d.clone` views versus occurrences

The first revision used `d.clone` views, the repo's other way to give a cell another cell's content.
The comparison, from the code:

| question                         | `d.clone` view                                                                                                                   | occurrence (`OpHandle` value)                                                                                |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| touches the row's slots          | yes: appended at the master's clone rank tail (`vortex_core.cpp:362-370`), a shadow for a proxy (`arena_manifold.cpp:2375-2440`) | no: a value in the occurrence's own slot (`:2347-2358`)                                                      |
| cost to make                     | `rankTail` walks the master's whole clone rank, $O(k)$ for the $k$-th join of a row; a foreign tail mints proxies (`:1803`)      | $O(1)$                                                                                                       |
| deleting or reclaiming a row     | refused while views exist (`CloneGuard`, `cell_deletion.hpp:88`; `HoldsViews`, `arena_manifold.hpp:274`)                         | row may go; the occurrence then reads `Dangling` (loud)                                                      |
| `Vlog::bind` on a row afterwards | splices behind the join's views (`vlog.cpp:86-96`)                                                                               | unaffected                                                                                                   |
| `promote()` of a base cell's row | grafts a `d.clone` edge onto the user's cell (a shadowed cell is `holdsOwn`, `:3463-3500`)                                       | no edge on the row                                                                                           |
| `promote()` of a proxy's row     | the edge is lost: proxies are skipped (`:3463`), the master is an extern placeholder (`:3308-3345`)                              | refused loudly: an ephemeral handle target must be a relocation handle, else `DanglingHandle` (`:3205-3207`) |
| read by an unaware consumer      | a view reads as its master's text (readers resolve `cloneMaster`)                                                                | `readQueryScalar` refuses it as `UnsupportedHandle` (`query_scalar.cpp:255-258`)                             |
| P12 root rule (links outside)    | a `d.clone` edge from outside makes the container region a root forever                                                          | a value is no link; d3 must treat `OpHandle` values as references (view-system I4, `:603`)                   |

`d.clone` is not required. Its one real advantage, that unaware readers resolve a view to the row's
text, does not help the join, because the unaware reader never reaches a member: it reads the
container, which has no content either way. Everything else that made the first revision's
recommendation firm is a cost of `d.clone` and disappears. The occurrence is the spec's choice and
is adopted.

## 5. The readout

One function over `d.packing`, and every consumer that needs bytes calls it. Home:
`apps/common/xanadu/zigzag/`, device-free engine code that `vquery`, `vpl`, `vprolog` and `xuzz`
share.

```cpp
struct Segment {                    // one constituent's bytes, in its own namespace
  const Store *store;               // nullptr: scratch only
  std::vector<PrimediaSpan> spans;
};
struct Readout {
  std::string text;                 // the constituents' text, in order
  std::vector<Segment> segments;    // the same bytes as addresses, scratch rebased
  std::string scratch;              // the bytes of the scratch spans, typed on write
};
enum class ReadoutRefusal { NotAPack, TooLarge, Cycle, Unreadable, Dangling };

[[nodiscard]] std::expected<Readout, ReadoutRefusal>
readOut(const ArenaManifold &arena, CellRef container, const PackDims &dims,
        std::size_t maxBytes);
```

1. `NotAPack` unless the cell has a posward `d.pack` neighbour; the caller reads it as text, as
   today. A cell that is not a pack is never an error.
1. Constituents are the `d.pack` firstborn and its `d.packing` chain, in order, iteratively, with a
   visited set and `traversalBound`: a cyclic rank is `Cycle`, never a hang. A constituent that is
   an occurrence is resolved by `handleTarget`; its target is then `cloneMaster`'d, and if the
   target is itself a pack it is expanded on an explicit stack (no recursion), else its bytes are
   `contentOf(master)`. A plain constituent (a separator) reads its own content.
1. A foreign master answers its store's spans and `foreignOf(master)` its store, so a segment's
   namespace is the constituent's own; adjacent segments with one store merge.
1. Sizes are summed from span lengths and compared with `maxBytes` *before* any byte is read; past
   it, `TooLarge` and nothing is read. Each constituent calls `arena.consumeWork()` once, so the
   Vortex fuel budget bounds a hostile pack (`query_scalar.cpp:27-37` is the idiom).
1. Pure: mints no cell, writes no link, spools no byte (R8); answers the same twice unless a target
   changed between the calls.
1. A target that is gone is `Dangling`, bytes the reader cannot supply `Unreadable`; never an empty
   segment.

It is `QuotedSpans` (`result_slice.hpp:22-26`) with one level more; `quotedSpansOf`
(`result_slice.cpp:14-42`) is today's single-cell readout and `readOut` is its pack-valued twin.

## 6. Every consumer

Without a change each reads a pack container as the empty string, successfully. The members refuse
loudly (`UnsupportedHandle`) when read as scalars, so only the container is silent.

| consumer                             | today                                                                                                               | with the pack                                                                                                        |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| write source                         | `sourceBytes`: no I/O tag, so `arena.textOf`, and when empty `core.render` (`script_runner.cpp:1862-1880`)          | `textOf` is empty, then `render`: correct **if** `render` reads packs out, else a zero-byte write with no diagnostic |
| REPL and `-o` rows                   | `resultRows` calls `core().render(cell)` (`apps/vquery/main.cpp:104`)                                               | same: the text is right only through `render`; `quote` also needs `readOut` (section 7)                              |
| `string()`, comparisons, `contracts` | `readQueryScalar` (`vortex_string.cpp:51`, `vortex_comparison.cpp:39`, `contracts.cpp:201`)                         | empty; `readQueryScalar` is zigzag code, not core, and can call `readOut`                                            |
| every other `render` site            | about 60 outside the UI (`vql/*`, `vortex_stdlib.cpp`, `vql_engine.cpp`)                                            | empty at each unless `VortexCore::render` (`vortex_core.cpp:322`) reads packs out                                    |
| join of joins                        | a quote of quotes stays one run (`JoinHarness.AJoinOfJoinsStaysQuoted`)                                             | a constituent that is a pack; `readOut` expands it                                                                   |
| persist exposure accounting          | `expose()` records payload ranges where a payload is read (`script_runner.cpp:825`, `:2129`), by range, not by cell | unchanged: a join neither adds nor removes an exposure                                                               |
| `match`, `io_payload`                | matched text arrives as cells through `payloadText`; the join is downstream                                         | no interplay; `sourceBytes` takes the pack through its text branch (`:1855-1865`: a pack has no I/O tag)             |
| `-o`, `--in-place`, `:save`          | `quotedSpansOf`, `promoteOrRefuse` over the one cell                                                                | section 7                                                                                                            |

So the pack needs **one core change to be safe by default: `VortexCore::render` reads a pack out**
(proposal P13, `vortex_core.cpp:322-339`, about ten lines, one function). It repairs `sourceBytes`,
`resultRows`' text and the 60 sites at once, because every one falls through to `render`. Without it
each of those is edited by hand and a new one is silently empty.

The case the pack's per-constituent namespace would serve, rows of two stores in one rowset, is not
reachable from the command line: with two stores loaded `##/+d.dims` answers the composite's own
dimension cells and no VQL path yields both stores' content cells (`r1-disposition.md`, zigzag m4).

## 7. Persistence, checked against `promote()`

`--in-place` calls `promoteOrRefuse` on each result (`apps/vquery/main.cpp:999`); `-o` and `:save`
call `writeResultSlice` over `resultRows` (`:1030`, `:1312`). Read from the code (not run):

- **A pack of handles to proxy rows is refused**, which is loud and correct. A handle cell with an
  ephemeral target is accepted only if it is marked as a cell-relocation handle (a link to the
  `d.mesh.relocation` marker, `foreign_op_handle.hpp:24-29`) and its target is in the walk, else
  `DanglingHandle` (`arena_manifold.cpp:3205-3207`); a proxy is ephemeral. Marking would reuse the
  mesh's relocation meaning for a join and, even then, the target is minted as an *extern
  placeholder* (`:3308-3345`: `registerScroll`, `makeExternRef`), so the saved cell is a reference
  to the foreign operation, not the bytes: not a transclusion of spans, and unreadable without that
  store.
- **A pack of handles to real cells promotes**, since a non-ephemeral target only has to exist
  (`getCompactOp`, `:3213`): container, occurrences as `OpHandle` cells naming the originals
  (`:3385-3390`), separators typed, `d.pack` and `d.packing` linked. That is §9.3.9's "keeping a
  pack" and works today for rows that are cells of the destination store.
- **So `join`'s saved form cannot be the pack.** `-o` and `:save` write *one cell holding the joined
  text, transcluded*, because `readResultSlice` reads one cell per row (`result_slice.cpp:123-152`)
  and a result row is text. That form is the flattened readout: `Readout::segments` into the
  per-cell code of `writeResultSlice` (`:76-100`: first span `makeCell`, the rest `spliceCellSpan`,
  scratch typed), with `canCarry`/`carrySpan` per segment rather than one store. It needs no core
  change and gains per-constituent namespaces. `--in-place` would call the same writer instead of
  `promoteOrRefuse` and reproduce `promote()`'s `UnnameableContent` refusal itself.

The flatten-and-type step is P11's `quotedRun` carrying and `promote()`'s scratch splicing
(`arena_manifold.cpp:3352-3400`) written again over a different input.

**VU4 for this use.** The question: when a pack is kept, are its `d.pack` and `d.packing` the
slice's own named dimensions, shared by every kept pack (`view-system.md:3185-3186`)? The join does
not keep a pack (it saves the flattened cell), so it does not decide VU4. If a kept pack is wanted
(for real-cell rows, per the second bullet), the answer from this use is **yes, named and shared**:
`promoteOrRefuse` mints a dimension as an ordinary arena cell (`:3300-3395`, `discover(edge.dim)`),
so a second keep would mint a second cell named `d.pack`, and a reader asking `dimensionNamed`
(`manifold.hpp:570`; `result_slice.cpp:126` is the idiom) would find one and miss the other. One
named pair per slice, looked up before it is minted (the `d.result` idiom, `result_slice.cpp:67`),
is what keeps two packs in one store two packs on one rank pair. The check that a second keep reuses
rather than re-mints is a fail-first test owed by whoever builds §9.3.9; I did not run it.

## 8. Identity, fan-in, snapshot

- **Identity across stores.** Each constituent names its own cell in its own store; the readout is
  per-constituent. That removes `Namespace`, the "free piece" precondition and the text fallback of
  `join_relation.cpp:99-155`, `:226-258`, which is the real gain.
- **Two-degree and fan-in.** Occurrences are values, so many occurrences of one row put nothing on
  the row's rank: no chain, no tail walk, no eviction. Each occurrence has one pos and one neg on
  `d.packing`; a nested pack is a constituent (neg/pos on `d.packing`) and a container (pos on
  `d.pack`) on two dimensions, which is V2's point. This holds for fan-in, for the same row twice
  (two occurrence cells) and for a pack in two parents (two occurrences of the container).
- **Live, not snapshot.** An occurrence names a cell, and the cell's content is read at readout. A
  script that joins, edits a row, then writes the join writes the edited bytes, and `sourceBytes`'
  "frozen here" (`script_runner.cpp:1634-1642`) holds only because `readOut` is called there.
  Today's join is a snapshot of immutable span addresses. This is a semantic change, arguably the
  pack's point, and the owner's call (section 11).

## 9. R8, bounds, d3

**R8 holds** during evaluation and at save: a pack is arena cells and links on cells it minted; the
readout mints nothing; the writer spools only the separators through `makeCell`/`spliceCell` as
`writeResultSlice` does now. The base-cell graft of the clone design does not occur.

**Bounds at $10^4$ rows.** $2n - 1$ constituents plus one container and one interned separator span:
a `CellSlot` is 32 bytes plus 12 per dimension (`AGENTS.md`), here one dimension each, so about 44
bytes a constituent and about 0.9 MB (an estimate, not measured), against P11's one cell and a run
of $10^4$ spans. The 1 MiB bound applies at the join and again at the readout, with fuel. Saved
through the flatten writer the operation count is the same as P11's: one `makeCell` and splices.

**d3.** The occurrences are not links, so P12's root rule ("a link on any dimension the VM does not
claim to a cell outside the region is the user's structure", roadmap §5, §6.1c item 1) does not make
a pack a root. Two things d3 must add: an `OpHandle` value naming a row is a reference (a freed row
would leave a `Dangling` occurrence; view-system I4 `:603` already says a handle must name a live
cell), and the pins `ReturningAJoin` and `BindingAJoin` (bounded live cells at $10^4$ runs;
`m7-vql-d.md` §8c baselines 266 and 115) count $2n$ cells per join where P11 leaves one cell and
scratch. A REPL that keeps its rows (`lastResults`, item 4) keeps all of them until freed.

## 10. Does this make P11's `arena_manifold` change unnecessary?

**Not unless something else carries spans and types scratch at save time, and the only thing that
can do it without `arena_manifold` is application code that is P11's algorithm again.** The pack
removes the clone-view costs and keeps the sound part of the container idea. It does not remove the
need, and it adds its own.

### Options

- **(A) Adopt the pack, revert P11.** Requires: `readOut`; `join_relation` rewritten to mint packs
  (lines 99-258 replaced); P13 on `VortexCore::render` (or hand edits of the sites in section 6);
  `readQueryScalar` calls `readOut`; `QuotedSpans` becomes segments and `resultRows` and
  `writeResultSlice` follow; `--in-place` writes through the segment writer and not `promote()`; d3
  treats `OpHandle` values as references. **What to revert**, from `bed41595` and the r1 fixes
  (`af2fb3f6`, `19cd3735`, `55755f4c`, `d867cca5`, `983ec275`): in `arena_manifold.hpp` the
  `quotedRun` declaration and the `makeQuote` doc; in `arena_manifold.cpp` the scratch check in
  `makeQuote`, `quotedRun` (restore `quotedContent`'s body with its refusal of scratch) and the two
  `quotedRun` loops in `promoteOrRefuse` back to `quotedContent` (this also restores the latent raw
  write of a plain mixed cell that r1 closed); `QuotedSpans::scratch` and `quotedSpansOf`'s
  rebasing. **Tests:** `ArenaQuoteSeparatorTest` (nine) deleted with the API; `JoinHarness` (eleven)
  rewritten to assert pack shape and `readOut`; the two join cases of `VQueryE2ETest`
  (`AJoinOfALoadedStoresRowsSavesTranscludedPlusSeparators`, `ALoneAndAnEmptyJoinAreSavedAsBefore`)
  kept as black-box tests and expected to pass only when the writer exists; new tests for each row
  of section 6 and the d3 pins, each fail-first. **Core:** one proposal, P13 (`vortex_core.cpp`),
  replacing the built P11; no change to `arena_manifold` or `promote()`.
- **(B) Keep P11 and add the pack.** As a layering this is coherent: the pack is the structure, the
  P11 quote is its frozen flattened value (`readOut` then `makeQuote` with the run). It is not worth
  doing *for `join`*: the pack would be built and then flattened by the join's own return.
- **(C) Keep P11 only.** Recommended for the gate. Cost: the one limit the owner named, typed
  fallback logged at debug (`join_relation.cpp:221-224`), unreachable from any front end.

### Why (C), priced

1. **A swap, not a removal.** (A) trades a built, tested, round-1-fixed 50-line change in
   `arena_manifold` for an unbuilt one in `vortex_core` plus a rewrite of `join`, `resultRows`, the
   writer and `--in-place`, and a second gate. "Prefer no core change" is not served.
1. **The pack cannot be the saved form** (section 7), so the flatten-and-type algorithm exists
   either way; P11 has it, tested, in the arena where `promote()` already needs it for every plain
   mixed cell (`d2.md` "Gate round 1 fixes").
1. **The pack's one gain has no user** (section 6) and its semantic change (live) is the owner's to
   want, not an implementation detail.
1. **d3 is the next slice** and (A) would put pack-awareness and a $2n$-cell pin into it.

What would change this answer: the owner wanting live, structurally navigable joins (section 11,
question 2), or a front end that reaches two stores in one rowset. Either makes (A) one slice with
P13 and no P14; the first revision's P14 (`promote()` carrying clone views) is not needed.

## 11. Where the pack is right, refused alternatives, open questions

A *rowset* is a collection and wants the pack: `VqlResultLens` already presents a query's answers as
`d.contains`/`d.seq` members (`vql_result_lens.cpp:41-67`), and `LensRepl` and `printLensTree` walk
that shape (`lens_repl.cpp:46-69`, `:85-101`). A pack-valued function over a rowset (no separator,
no bytes) is a new function with no `join` semantics to preserve, and it is where §9.3.9's kept pack
and VU4 belong. A *text* is a value and wants one cell.

Refused:

1. **`d.clone` views** (first revision), section 4.
1. **Separator as container content.** One cell instead of $n - 1$; an unaware reader then reads
   `","` as the join (silent and wrong). Reconsider if `render` reads packs out (P13), which removes
   unaware readers.
1. **Members as per-row `makeQuote` cells.** No core change and per-member namespace; gives up the
   cell identity the spec requires (V-R29 says "never a copy").
1. **`d.ref` members.** A row has one negward `d.ref`; a row in two joins evicts the first.
1. **`quoteOccurrence` members** (`arena_manifold.cpp:686-714`). Proxies only, one per (view,
   proxy), so the same row twice would put one cell twice on a rank.
1. **Flatten at write only, keep the join as it is.** That is (C).

Open questions for the owner:

1. Is "a text is one cell, a collection is a pack" an acceptable rule for keeping `join` as is?
1. Should `join` be live (follow a changed row) for any consumer? If yes, that argues for (A) on its
   own, and the cost is section 10's list with one proposal, P13.
1. Is P13 (`VortexCore::render` reads a pack out) acceptable as the one core change a pack needs,
   independent of `join`? It is also what makes a pack safe for the view system's kept packs.
1. VU4: confirm "named and shared per slice" for kept packs (section 7), and that the check is owed
   to §9.3.9's slice and not to d2.

## 12. Change history

| version | date       | change                                                                              |
| ------- | ---------- | ----------------------------------------------------------------------------------- |
| 0.1     | 2026-11-26 | Container of `d.clone` views on `d.contains`/`d.seq`; recommends (C)                |
| 0.2     | 2026-11-26 | Follows the pack spec: `d.pack`/`d.packing`, occurrences, VU4; (C) kept, (A) priced |
