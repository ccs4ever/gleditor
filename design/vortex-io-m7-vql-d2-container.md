# M7-VQL-d2 alternative: `join` as a container of clone views

Status: design, no code. Written at the owner's request before the M7-VQL-d2 (P11) gate closes
([`vortex-io-m7-vql-d2.md`](vortex-io-m7-vql-d2.md)). It evaluates replacing the single-cell `join`
with the repo's documented way of compositing several cells into one container that is read out in
sequence, specifies that design well enough to be judged at its best, and then prices it against
what is built. **Recommendation: (C) keep P11 and do not adopt the container for `join`.** The
container is the right shape for a *rowset*, which the tree already has (section 10); it is the
wrong shape for a *text*, which is what `join` returns. No new core file is changed by either
answer, and no P13 is reserved unless the owner chooses (A) (section 9).

## 1. The proposal

The owner's note (a container cell with the rows linked in along a containment dimension, read out
along it by whoever needs bytes) proposes, in the terms of this repository:

- `join(rows, sep)` mints one **container** cell.
- Each row is linked in as a **`d.clone` view** of the row's clone master, so a row that has its own
  `d.seq` or `d.contains` neighbours is untouched (the view is a different cell, with slots of its
  own) and every member keeps its own store and namespace.
- The members are chained as the canonical many-children encoding: the container's **firstborn on
  `d.contains`**, siblings on **`d.seq`** (`sql-to-zigzag.md` §2.2, §3.1; `GenericDim::Containment`
  and `Sequence`, `anything-to-zigzag-lens.md:259-263`). Any other encoding self-evicts, because a
  cell holds one posward and one negward neighbour per dimension and `ArenaManifold::link` silently
  evicts whatever either end held (`arena_manifold.cpp:2360-2440`).
- One shared **readout** concatenates the members in order for every consumer that needs bytes.

That is the shape the lens layer already uses to present a directory, a query's answers and a
document's paragraphs: `LensRepl::entries` and `printLensTree` walk it (`lens_repl.cpp:46-69`,
`:85-101`), and `VqlResultLens` emits a query's answers exactly so (`vql_result_lens.cpp:57-67`).

## 2. What is built today, and what it costs

`join_step` collects one `JoinPiece` per row and separator; `join_end` decides whether every piece
can share one namespace and mints **one** cell: `makeQuote(space, runs)` for a shared namespace,
otherwise interned text (`join_relation.cpp:159-265`). The namespace rule, the "free piece"
precondition and the text fallback are lines 99-155 and 226-258. Everything downstream sees one cell
whose own content is the joined text, so `textOf`, `VortexCore::render` (`vortex_core.cpp:322-339`),
`readQueryScalar` and the write path all work with no knowledge that it was a join.

P11 itself is small: `arena_manifold.hpp` gained `quotedRun` and a doc change, and
`arena_manifold.cpp` gained a scratch range check in `makeQuote` (`:2213`), `quotedRun` (`:2230`),
`quotedContent` as its filter (`:2257`) and two loop changes in `promoteOrRefuse` (`:3261`,
`:3352`); commit `bed41595` is 34 and 16 lines in the two files. The rest of d2 is application code
(`join_relation`, `result_slice`, `vquery`).

The cost the owner's note names is real: a content run lives in one namespace, so rows of two stores
that each have external scrolls are joined as text (`join_relation.cpp:235-237`). Section 5 shows
that this case is not reachable from any front end today, which bounds what the container could buy.

## 3. The container, specified

**Shape.** For rows $r_1 \ldots r_n$ with $n \ge 2$:

```text
J --d.role--> "vql.join"                 (marker; one cell)
J --d.sep---> S                          (the separator, one scratch text cell)
J --d.contains--> V1 --d.seq--> V2 ... --d.seq--> Vn
Vi --d.clone (negward)--> master(ri)     (Vi is appended at the tail of master(ri)'s clone rank)
```

Each `Vi` is minted content-free (as the xanadoc projector's unchanged clones are,
`anything-to-zigzag-lens.md:105-107`). Its content is its master's, which every existing reader
already resolves through `cloneMaster` (`arena_manifold.cpp:1888`, `readQueryScalar`'s clone
dimension argument, `join_relation.cpp:53-64`). Minting a view is the one idiom the engine has for
it: mint a cell, `link(rankTail(source, clone), clone, POS, fresh)` (`vortex_core.cpp:362-370`).

**Separator: a container attribute, not interstitial members.** Chosen: one cell on `d.sep` (an
attribute rank, the `d.role` convention). Interstitial separator cells would need $2n - 1$ chain
members against $n$, make `$j/d.contains/d.seq/count()` answer something other than the number of
rows, and put a presentation detail where a reader walking `d.seq` takes it for content. With the
attribute, the members are exactly the rows. A join with no separator argument cell (the empty text)
simply has no `d.sep`.

**Marker.** A container with no member (the empty join) is indistinguishable from an empty text cell
by structure, and a container with members is indistinguishable from any user's parent cell. Only an
explicit marker (`d.role` = `vql.join`) tells the readout that this cell's text is its members'
readout. This is one more cell per join.

**Empty and one-row.** The empty join stays the empty text cell it is now
(`join_relation.cpp:213-214`): a container of nothing has no use. A one-row join returns the row
itself, as now (`:211-212`, `walk->only`), so the row's identity and what links to it are not
touched. A container exists only for $n \ge 2$.

**Bound.** `join_step` already charges each row's bytes against the 1 MiB bound before the row is
taken and refuses a join past it (`join_relation.cpp:174-179`, `limits.hpp:13`). That stays. The
readout charges the same bound again (section 4), because a view reads its master as it is *at
readout*, not as it was at the join.

## 4. The readout

One function, and every consumer that needs bytes calls it. Home: `apps/common/xanadu/zigzag/` (it
needs the arena and dimension refs only, so it is device-free engine code that `vquery`, `vpl`,
`vprolog` and `xuzz` can share).

```cpp
struct Segment {               // one member's bytes, in its own namespace
  const Store *store;          // nullptr: scratch only
  std::vector<PrimediaSpan> spans;
};
struct Readout {
  std::string text;            // the members' text with the separator between
  std::vector<Segment> segments; // the same bytes as addresses, scratch rebased
  std::string scratch;         // the bytes of the scratch spans, typed on write
};
enum class ReadoutRefusal { NotAContainer, TooLarge, Cycle, Unreadable, Dangling };

[[nodiscard]] std::expected<Readout, ReadoutRefusal>
readOut(const ArenaManifold &arena, CellRef container, const ReadoutDims &dims,
        std::size_t maxBytes);
```

Contract:

1. `NotAContainer` unless the cell carries the marker; the caller then reads the cell as a text, as
   today. A cell that is not a container is never an error.
1. Members are the `d.contains` firstborn and its `d.seq` chain, in order, iteratively (no
   recursion; a member that is itself a container, which a join of joins produces, is expanded on an
   explicit stack). The walk is bounded by `traversalBound` and a visited set, so a cyclic `d.seq`
   is `Cycle`, never a hang (two-degree makes a cycle possible only by a user's own `link`).
1. Each member is resolved by `cloneMaster(view, clone)`; its bytes are `contentOf(master)`. A
   foreign master answers its store's spans and `foreignOf(master)` its store, so a segment's
   namespace is the member's own. Adjacent segments with the same store are merged.
1. The size of every member is summed from span lengths *before* any byte is read, plus the
   separator once per gap, and compared with `maxBytes`; past it, `TooLarge` and nothing is read.
   Each member also calls `arena.consumeWork()` once, so the Vortex fuel budget bounds a hostile
   container (`query_scalar.cpp:27-37` is the idiom).
1. Pure: it mints no cell, writes no link, spools no byte (R8), and answers the same twice unless a
   master changed between the calls.
1. A member whose master is gone, or whose bytes the reader cannot supply, is `Dangling` or
   `Unreadable`, never an empty segment.

This is, deliberately, the same vocabulary as `QuotedSpans` (`result_slice.hpp:22-26`) plus one
level. `quotedSpansOf` is today's single-cell readout (`result_slice.cpp:14-42`); `readOut` is its
container-valued twin.

## 5. Every consumer, and what the container does to it

Today every row below calls `textOf`, `render` or `readQueryScalar` on one cell and gets the text.
Under the container each gets **the empty string**, silently, unless it is changed to call `readOut`
first. That is the central cost: the failure is not a refusal but a successful empty read.

| consumer                     | today                                                                                                                     | with the container                                                                                                         |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| write source                 | `sourceBytes`: no I/O tag, so `arena.textOf` then `render` (`script_runner.cpp:1862-1880`)                                | writes **zero bytes** with no diagnostic unless `sourceBytes` calls `readOut` before `textOf`                              |
| REPL and `-o` rows           | `resultRows` calls `core().render(cell)` (`apps/vquery/main.cpp:104`)                                                     | prints an empty row; `resultRows` must call `readOut` and fill `text` and `quote`                                          |
| `string()`, comparisons      | `readQueryScalar` (`vortex_string.cpp:51`, `vortex_comparison.cpp:39`)                                                    | empty; `readQueryScalar` is a zigzag file (not core) and can call `readOut`                                                |
| everything else that renders | about 60 `.render(` call sites outside the UI (`vql/*`, `vortex_stdlib.cpp`, `vql_engine.cpp`)                            | empty at each, unless `VortexCore::render` (core, `vortex_core.cpp:322`) reads containers out: that is a core change (P13) |
| `join` of a join             | a quote of quotes stays one run (`JoinHarness.AJoinOfJoinsStaysQuoted`)                                                   | a container member that is a container; `readOut` recurses on an explicit stack                                            |
| persist exposure accounting  | `expose()` records payload ranges where a payload is read (`script_runner.cpp:825`, `:2129`), keyed by range, not by cell | unchanged: a join neither adds nor removes an exposure; capture identity is kept if the master is a capture quote          |
| `match` and `io_payload`     | the matched text arrives as a cell through `payloadText`; join is downstream                                              | no interplay, except that `sourceBytes` takes a join through its text branch (`:1855-1865`, a container has no I/O tag)    |
| `-o`, `--in-place`, `:save`  | `quotedSpansOf` and `promoteOrRefuse` over the one cell                                                                   | see section 6                                                                                                              |

The "two stores, one rowset" case, the only one where the container's per-member namespace would pay
off, is not reachable from the command line: with two stores loaded `##/+d.dims` answers the
composite's own dimension cells, and no VQL path yields both stores' content cells in one rowset
(`r1-disposition.md`, zigzag m4). It stays a `JoinHarness` unit case.

## 6. Persistence: verified against `promote()`

`--in-place` calls `promoteOrRefuse` on each result (`apps/vquery/main.cpp:999`); `-o` and `:save`
call `writeResultSlice` over `resultRows`. The question the owner asked is whether `promote()` turns
a container of clone views into a transclusion of each master across stores. Read from the code:

- **A view of a foreign master does not survive.** A proxy master is minted as an extern-ref
  placeholder (`arena_manifold.cpp:3308-3345`: `registerScroll`, `makeExternRef`,
  `placeholderForExtern`), so the saved cell is a reference to the foreign operation, not the
  master's spans; it reads empty without that store loaded. Worse, the link loop skips every proxy
  (`:3463`: `if (!from.holdsOwn(arena) || from.isProxy(arena)) continue;`) and only emits each own
  cell's *posward* edges, so the master-to-first-view edge on the proxy is never written and the
  view's own edge is the negward one: **the `d.clone` link from a saved view to its master is lost
  and the view is an empty cell.** This is from reading, not from a run; if (A) is chosen it is the
  first fail-before test (section 9).
- **A view of a local ephemeral master does survive**, as an empty cell linked to a master cell
  re-spooled with its content (the master's posward edge is emitted); but `promote()` "follows
  `d.clone` edges like any other link and re-spools each reachable cell's content"
  (`anything-to-zigzag-lens.md:836-842`, U-L4), so a row that is joined twice is saved twice.
- **A view of a persistent base cell writes into the user's document.** The view is appended at the
  tail of the master's clone rank, which shadows the base cell (`link`, `:2370-2440`); the shadowed
  cell is `holdsOwn`, so promote expands it and its changed posward `d.clone` edge is emitted
  (`:3463-3500`, the "net-state diff"). Saving a join would graft a clone link onto the user's rows.
  R8 holds during evaluation (the arena mints no operation) and is broken by the save.
- **`sliceToStore`** (`xanadu_projector.hpp:152`) mints a slice DTO, with no clone-view resolution
  and no span carrying; it is not a route.

So the container cannot be saved by `promote()` as it stands. It needs either a change to
`promote()` (a view of a proxy or of a base cell is carried as its master's spans, scratch
separators typed: which is P11's `quotedRun` carrying, in the same file, for a larger case) or an
application-level writer that bypasses `promote()` for containers. The second needs no core change:
the per-cell code in `writeResultSlice` (`result_slice.cpp:76-100`: first span `makeCell`, the rest
`spliceCellSpan`, scratch typed) factors into a function over `Readout::segments`, with
`canCarry`/`carrySpan` per segment instead of one store. `--in-place` would call it instead of
`promoteOrRefuse`, and would have to reproduce `promote()`'s refusals (`UnnameableContent`,
runtime-only state) itself.

## 7. Identity, fan-in and the two-degree rule

**Identity across stores.** A member keeps its own namespace, which is the container's real
advantage over P11: `Readout::segments` is per-member, so rows whose external scrolls disagree are
carried span by span (`carrySpan` registers each scroll in the destination) instead of typed. P11
refused that as a "mixed-namespace run" (`d2.md` §2, refused 1) because the cost was in `CellSlot`,
`mark()`, `compact()` and the prefix codec. The container pays it in the readout and the writer
instead, which are application code. It is a real gain with no reachable user (section 5).

**The two-degree rule holds, but only as a chain.** Many views of one master do not fan in: a cell
has one posward and one negward neighbour on `d.clone`, so views of one master are a *chain*
`master -> V -> V' -> ...`, each appended at the rank's tail (`vortex_core.cpp:362-370`), and
`cloneMaster` of any of them walks negward to the master. The container's own `d.contains` and
`d.seq` chain are on different dimensions, so a view in one container and one `d.clone` rank is
two-degree on three dimensions and fine. Consequences, each a price:

- **Cost grows with the rank.** `rankTail` and `cloneMaster` are linear walks (`cell_views.hpp:258`,
  `arena_manifold.cpp:1888`). A row joined in every query of a REPL session gains a view per query,
  so the $k$-th join costs $O(k)$ for that row and a session $O(k^2)$. For a proxy the tail walk
  mints a proxy for every foreign clone neighbour (`linkedMinting`, `:1803`) and the link takes a
  savepoint and a shadow (`:2370-2395`). This is a ceiling users work around (a session that slows
  until restarted).
- **The master's identity rank is polluted by presentation.** The views are not clones a user made;
  they sit in the master's rank beside the user's `><` clones. `Vlog::bind` links an unbound
  variable's rank onto a term's rank tail (`vlog.cpp:86-96`), so unifying a row after a join splices
  it behind the join's views, and `CloneGuard` refuses to delete a master that holds views
  (`cell_deletion.hpp:88`, `HoldsViews`, `arena_manifold.hpp:274`): after `join($rows, ...)` a
  script cannot `delete` a row without first deleting the container.
- **The same row twice** in one join is two views in one chain, which is fine, and it is the only
  place the chain shows up as a design question.
- **Live, not snapshot.** A view reads its master as it is now. Today's `join` is a snapshot (a
  quote's spans are addresses, immutable). With views a script that joins, edits a row, then writes
  the join writes the *edited* bytes, and `sourceBytes`' "frozen now"
  (`script_runner.cpp:1634-1642`) holds only if `readOut` is called at that moment. The query
  language's purity (vlace's premise, R8) is the snapshot; the container trades it away.

## 8. R8, bounds and the d3 plan

**R8 holds during evaluation:** the container, its views, the marker and the separator are arena
cells, links on ephemeral cells and shadow edges on proxies are arena state, and the readout mints
nothing. It is broken only by `promote()` over a base cell (section 6).

**Bounds at $10^4$ rows.** A view is a `CellSlot` of 32 bytes plus 12 per dimension it links
(`AGENTS.md`, `zigzag::CellSlot`), here `d.clone`, `d.seq`, and the first one `d.contains`: about 56
bytes a view, so $10^4$ rows is about 0.56 MB of slots plus $10^4$ new `d.clone` entries on the
masters (and, for proxies, $10^4$ shadow entries and savepoints) against P11's one cell and one run
of $10^4$ spans. The 1 MiB bound is applied twice (join and readout), and the readout must be
bounded in fuel as well as bytes (section 4, rule 4). A saved container is about $2n$ operations
against P11's $n$ splices.

**d3.** P12's root rule is "a link on any dimension the VM does not claim to a cell outside the
region is the user's structure and a root" (`roadmap` §5, §6.1c, item 1), and `d.clone` is one of
the dimensions whose user structure a round-3 reclaim destroyed. A join container is reached from
each row through a `d.clone` edge from outside its region, so under that rule the whole container
region is a root and is never reclaimed. The d3 acceptance pins `ReturningAJoin` and `BindingAJoin`
(live cells bounded at $10^4$ runs; `m7-vql-d.md` §8c lists today's baseline of 266 and 115) would
fail by $n + 3$ cells per run until d3 grows a special case for join containers, and the REPL's
prior-row freeing (item 4) would have to unlink views from masters first (the same `HoldsViews`
guard). P11's result is one cell plus scratch that `release()` truncates.

## 9. Does this make P11's `arena_manifold` change unnecessary

**No, not as a replacement of the join; and the part that is avoidable is avoidable only by moving
the same work to application code.** Three reasons, in order of weight:

1. **Something must carry spans and type scratch at save time.** Either `promote()` learns it for
   views (a larger version of P11 in the same file), or an application writer does it from the
   readout. The latter needs no `arena_manifold` change, but it is the same algorithm as P11 over a
   different input, written again.
1. **The container has no consumer-visible win that is reachable** (section 5), and it adds the
   silent-empty failure to about 60 call sites, the clone-rank costs of section 7, a d3 conflict and
   a semantic change from snapshot to live.
1. **P11 is built, tested and one finding from converged** (round 1: zigzag 0/1/4, vortex 0/0/2, the
   major fixed). Its core footprint is about 50 lines in one file.

### Options

- **(A) Adopt the container, revert P11.** Requires: the readout (section 4); `join_relation`
  rewritten to build containers (lines 99-258 replaced); the readout called from `sourceBytes`,
  `resultRows`, `readQueryScalar` and either a P13 on `VortexCore::render` or a hand edit of the
  other 60 sites; a writer over segments (section 6) or a P14 on `promote()`; the d3 special case; a
  fail-first run of section 6's three claims. **What to revert,** from `bed41595` and the r1 fixes
  (`af2fb3f6`, `19cd3735`, `55755f4c`, `d867cca5`, `983ec275`): in `arena_manifold.hpp` the
  `quotedRun` declaration and the `makeQuote` doc; in `arena_manifold.cpp` the scratch check in
  `makeQuote`, `quotedRun` (restore `quotedContent`'s body, with its refusal of scratch), and
  `promoteOrRefuse`'s two `quotedRun` loops back to `quotedContent` (this also restores the latent
  raw write of a plain mixed cell that r1 closed, `d2.md` "Gate round 1 fixes");
  `QuotedSpans::scratch` and `quotedSpansOf`'s scratch rebasing in `result_slice`; `resultRows`'
  `quotedSpansOf` call. **Tests that change:** `ArenaQuoteSeparatorTest` (nine) deleted with the
  API; `JoinHarness` (eleven) rewritten to assert container shape and `readOut`;
  `VQueryE2ETest.AJoinOfALoadedStoresRowsSavesTranscludedPlusSeparators` and
  `ALoneAndAnEmptyJoinAreSavedAsBefore` kept as black-box tests and expected to pass only once the
  writer exists; new tests for every row of section 5 (each fails before: empty read) and the d3
  pins. Price: a second gated slice with a harder gate than d2's, and d3 delayed.
- **(B) Keep P11 and add the container.** Two representations of one value, each needing every
  reader; the container's only win is unreachable. Refused.
- **(C) Keep P11 only. Recommended.** Cost: the one limit the owner named (rows of two stores with
  disagreeing external scrolls are typed, now logged at debug, `join_relation.cpp:221-224`), which
  no front end reaches. The container stays available as a structural *view* of a rowset (section
  10).

### Core changes

None under (C). Under (A), and only then: **P13** `VortexCore::render` reads a marked container out
(vortex core, so its own approval); **P14** `promote()` carries a clone view of a proxy or base cell
as its master's spans instead of dropping the edge (`arena_manifold.cpp:3308`, `:3463`). Both are
avoidable only by the call-site and writer work above. Not proposed.

## 10. Where the container is right, and a refused-alternatives list

A *rowset* is a collection and wants the container: `VqlResultLens` already presents a query's
answers as `d.contains`/`d.seq` members (`vql_result_lens.cpp:41-67`), and a viewer should see rows
as cells it can follow, not a joined string. A *text* is a value and wants one cell: it has one
address range, one `textOf`, and nothing to read out. `join` produces the second. If the owner wants
the first (a `collect($rows)` that returns a container of the rows themselves, with no separator and
no bytes), that is a new function over the lens shape with no `join` semantics to preserve, and it
would be proposed on its own merits, not as the join's representation.

Refused, with what each would have bought:

1. **Members as per-row quote cells (`makeQuote` per row), not clone views.** Needs no core change,
   keeps each member's namespace, and promotes with today's carrying. Refused: it gives up the cell
   identity the owner asked for (a member is a quotation of spans, as the single cell already is),
   still has the silent-empty readers and the d3 and $n + 3$ cell costs, and saves as a subtree that
   `readResultSlice` (one row, one cell) cannot read back as text.
1. **Members as `OpHandle` or `ForeignOpHandle` cells** (`promote` maps them,
   `arena_manifold.cpp:3385-3393`). Avoids touching the master's clone rank. Refused: a handle is a
   pointer, not content; every reader derefs, the handle of a proxy is refused as
   `UnnameableContent` when its store is not nameable, and a "view" that carries no content defeats
   the `cloneMaster` resolution that all readers share today.
1. **Members as `quoteOccurrence` cells** (`arena_manifold.cpp:686-714`), the arena's own "light,
   view-local presentation cell". Avoids the pollution of `d.clone` (it chains on `d.quote-origin`).
   Refused: it exists for proxies only, is one occurrence per (view, proxy) so the same row twice
   would put one cell twice in a `d.seq` chain, and promote does not handle occurrences.
1. **Interstitial separator cells.** Doubles the member count and falsifies `count()`; the attribute
   (section 3) is smaller.
1. **`d.ref` members** (one cell per row pointing at the row). A row has one negward `d.ref`, so a
   row in two joins evicts the first (section 7); the clone rank at least chains.
1. **Flatten at write only; keep the join as it is.** That is (C): P11 already is the flattened
   readout, computed once at the join instead of at every read.

## 11. Open questions for the owner

1. Is "a text value is one cell, a collection is a container" (section 10) acceptable as the rule
   that keeps `join` as is? If not, which of the eight consumers in section 5 is the one the
   container must serve, since each of them currently works?
1. If a rowset container is wanted, is it a lens (as `VqlResultLens` is), or a VQL function
   (`collect`)? Either is a separate slice; neither touches P11.
1. Is live-readout semantics (section 7: the join follows a changed master) wanted for any consumer?
   If it is, it argues for (A) on its own and the cost list in section 9 is the price.
1. Should the `d.clone` rank be allowed to hold presentation views at all? The question is wider
   than `join` and bears on `quoteOccurrence`, the `Vlog::bind` hazard (`cell-shape-spec.md:394`)
   and the d3 root rule.

## 12. Change history

| version | date       | change                                                                  |
| ------- | ---------- | ----------------------------------------------------------------------- |
| 0.1     | 2026-11-26 | Evaluation of the container alternative for the d2 gate; recommends (C) |
