# M7-VQL-d2 (P11): a quote that admits scratch spans, and the multi-row join over it

Status: design, written before the code; built, gate passed 2026-11-26 (section 10). Slice d2 of the
[roadmap](vortex-io-m7-vql-roadmap.md) §6.1b; core proposal P11, approved by the owner on 2026-11-26
as its own small slice. It changes one core file,
`apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}`, and nothing else in the core (`store.hpp`,
`ops.hpp`, `compact_op.hpp`, `store_tables.hpp`, `segmented_ops_spool`, `manifold`, `vortex_core`
and `vortex_vm` are untouched, and so is every on-disk format and fixture).

## 1. The problem

`join($rows, sep)` assembles one cell from the spans of its rows with a separator between each pair
(`join_relation.cpp`). A row that quotes text contributes its own spans, so the result stays a
transclusion. A row that is **another store's cell** cannot: its spans are addressed in that store's
scrolls, and the code says so where it gives up (`apps/common/xanadu/vql/join_relation.cpp:24-30`,
`:51-56`): such a row is interned as text, a copy in the arena's scratch buffer, and `promote()`
later spools the copy into the permascroll as new bytes. Every row `vquery` prints from a loaded
store is such a row (a proxy of an attached space, `ArenaManifold::contentOf` answers the foreign
manifold's run, `arena_manifold.cpp:1863-1885`). So `vquery -e 'return join($rows, ",")' -o out` and
`--in-place` lose the rows' transclusion identity exactly where a join is most useful: the saved
cell is a retyped copy, not a quotation of the original spans (`m7-vql-d.md` §8).

Why the arena cannot just hold the foreign spans today:

- A cell's content run is a bare `std::span<const PrimediaSpan>` over `content_`
  (`arena_manifold.hpp:884-886`); a span's `scroll` is meaningful only in one store's scroll table
  (`publication.hpp:490-507`, `canCarry`/`carrySpan`: a local span is the same bytes under every
  store sharing a permascroll, an external scroll id is relative to its own store).
- The namespace of a run is a property of the **cell**, not of each span: `quoteSpace_` maps a cell
  to one space (`arena_manifold.hpp:1239-1241`), `makeQuote(space, spans)` takes one space
  (`:547-548`), and `textOf` and `promote()` read the whole run through that one space
  (`arena_manifold.cpp:2012-2019`, `:3239-3250`, `:3329-3338`).
- `quotedContent()` answers nothing for a run that has any scratch span
  (`arena_manifold.cpp:2239-2243`), because a scratch span has no address outside the arena. A join
  of quotes needs exactly that: the rows' spans with a scratch separator between them. So a quote
  with a separator is "not a quote" to every reader that matters: `promote()` then writes its spans
  uncarried (the namespace of the destination is assumed, not checked), and `vquery`'s result rows
  see no quote and type the text.

## 2. The chosen representation

Two options were named in the approval: "a mixed-namespace content run, or a quote that admits a
scratch separator span, whichever is smaller and sounder."

**Chosen: a quote may hold scratch spans between its quoted spans, and its quoted spans share one
namespace.** A scratch span is already namespace-free: it names bytes this arena holds
(`scratchTextOf`, `arena_manifold.cpp:1981`), and every reader already handles one inside a run
(`textOf` `:2024`, `boundedTextOf` `:2081`, `promote()`'s splice branch `:3388`). The only defects
are the two places that treat "has a scratch span" as "not a quote". Fixing that is a reader change,
not a new data shape:

- `CellSlot`, `content_`, the `Mark`, `PrefixSnapshot` and its codec are unchanged (the snapshot
  already carries `quoteSpaceCount` and the scratch buffer, `arena_manifold.cpp:2653`, `:4033`).
- `quoteSpace_` stays one entry per quote cell.

**A new reader, `quotedRun(ref)`**, answers the store and the *whole* run, scratch spans included.
`quotedContent()` is kept for the callers that want only addressable bytes (`prose_hits`,
`compile_semantic_shape.cpp:185`, `apps/vquery/main.cpp:143`, `apps/vpl/main.cpp:165`, tests): it is
now `quotedRun()` filtered to runs with no scratch span, with identical answers. `promote()` and
`vquery`'s result rows use `quotedRun()`.

**Several stores in one join.** A join of rows of several stores is a quote only when the rows'
spans can share one namespace by `canCarry`'s own rule: every row whose spans include an external
scroll names the same store (the run's *anchor*), and every other row's spans are all local and its
store reads the same permascroll as the anchor. That is the usual `vquery` case (several stores over
one `--permascroll`, all primedia local). Rows that disagree (two stores each with external scrolls,
a store with a different permascroll, a foreign row with no store) are joined as interned text,
which is exactly what slice d does today. The refusal is in the join (it decides what it mints), not
in the arena.

### Refused alternatives

1. **A per-span namespace in the run** (a parallel `space` array beside `content_`, or a wider
   `PrimediaSpan`). It is the literal "mixed-namespace content run". Refused: `PrimediaSpan` is the
   wire and on-disk span (a wider one touches `CompactOpNode` users), a parallel array touches
   `CellSlot` accounting, `setContentAt`, `mark()`/`release()`, `compact()` and the prefix codec
   (each of which copies or truncates `content_`), and `promote()` would carry each span by its own
   space, so the soundness proof grows with every one of those. It buys one thing the chosen shape
   cannot: external scrolls of two stores in one cell. The price of not having it is a text copy in
   that one case, which is today's behaviour, not a regression, and a cell is a quote of one
   document by design (`d.stores` composes cells, not spans).
1. **Rewrite the foreign spans into the destination's namespace at join time.** Refused: the arena
   does not know its destination; carrying registers scrolls in a store (`carrySpan` mutates
   `into`), which an arena, by R8, must never do. `promote()` is the one road and does it.
1. **Make the separator a cell of the quoted store** (a typed separator minted by the join into the
   source store). Refused: that mints operations from a query (R8).
1. **Let `quotedContent()` answer scratch-bearing runs.** Refused: its four callers read the answer
   as "all addressable", and `compile_semantic_shape` rebuilds a quote from `quote->spans`; a
   scratch span there would dangle across a mark and be read as a store span. A second name keeps
   the old contract exact and makes each new use explicit.
1. **`vquery` types the text and the separators together**, as now. It is the defect.

## 3. API diff (`arena_manifold.hpp`)

```cpp
// new, beside QuotedContent (arena_manifold.hpp:550-554)
/**
 * @brief @p ref's whole quoted run: the store its non-scratch spans are
 *        addressed in, and every span in order, scratch spans included.
 *
 * quotedContent() without the refusal of scratch: a join of quotes holds a
 * scratch separator between its quoted spans. Nothing when the run has no
 * non-scratch span (constructed text), no spans, or no store to name.
 */
[[nodiscard]] std::optional<QuotedContent> quotedRun(CellRef ref) const;

// doc change only
CellRef makeQuote(std::uint32_t space, std::span<const xanadu::PrimediaSpan> content);
//   @p content may hold scratch spans of this arena (a separator between quoted
//   spans). A scratch span past the arena's scratch buffer throws
//   std::out_of_range; every other span is read in @p space's scrolls.

// behaviour, same signature
[[nodiscard]] std::optional<QuotedContent> quotedContent(CellRef ref) const;
//   unchanged answers: nullopt for a run with a scratch span.
```

`promote()` and `promoteOrRefuse()` keep their signatures and their refusals.

## 4. How `promote()` maps each kind of span

Per cell (`arena_manifold.cpp:3233-3250`, `:3322-3340`), unchanged in shape:

- **Check before writing** (`UnnameableContent`): for a quote whose store is not the destination,
  every *non-scratch* span must satisfy `canCarry(*quote->store, destination, span)`. A scratch span
  is skipped (it is carried by being typed). Today a run with any scratch span skips the check
  altogether; that is the hazard this closes.
- **Carry**: each non-scratch span becomes `carrySpan(*store, destination, span)`, i.e. a
  transclusion of the same bytes in the destination's coordinates (a local span as is; an external
  scroll registered in the destination by `addScroll`). Foreign spans are therefore **transclusions
  of their own store's permascroll addresses**, never copies.
- **Spool the separator**: the first span makes the cell (`store.makeCell(version, span)` for a
  quoted first span, `makeCell(version, text)` for a scratch first span), and each later span is a
  splice (`spliceCellSpan` for a quoted span, `spliceCell(version, cell, at, 0, text)` for a scratch
  one). The scratch bytes reach the author's permascroll through that call and nowhere else, so a
  saved join grows the permascroll by exactly its separators and constructed numbers.
- **Refusals that stay**: runtime-only state, unnameable foreign op handles, dangling handles, the
  budget, and `CrossStore` for shadowed diffs; a scratch span still reaches a store only through
  `promote()`.
- The ref mapping (`real`, `ephemeralBit`) is not touched: `promote()` still maps every ref and
  trusts none.

## 5. How a mixed run is read

- `textOf`/`boundedTextOf`: a quote cell reads its non-scratch spans through its space's reader and
  its scratch spans from the arena buffer (`arena_manifold.cpp:2012-2031`, `:2056-2092`), already
  true; no change.
- `quotedRun`: the store comes from the same place as `quotedContent` today (`foreignOf` space, then
  `quoteSpace_` space, then the arena's own or its base store), and the spans are `contentOf(ref)`.
- `mark()`/`release()`: `makeQuote` already records the cell in `quoteSpaceOrder_`
  (`arena_manifold.cpp:2216-2217`, `:2730-2733`), and the scratch buffer is truncated by release as
  for any scratch, so a failed branch leaves neither the cell nor its separator.

## 6. `join`, `vquery`, and the saved cell

`join_relation.cpp` keeps each row's spans, the store they are addressed in (`quotedRun`) and
whether they are all local, takes the separator the same way, and decides **at the end** whether the
rows share a namespace (section 2). Shared: one `makeQuote(space, run)` with the separator spans in
place (or a plain cell when the namespace is the arena's own, as today). Not shared: every row that
names a store becomes interned text, as in slice d. A lone row is still returned itself.

`vquery` (`resultRows`, `writeResultSlice`): `ResultRow::quote` (`QuotedSpans`) gains `scratch`, the
bytes of the run's scratch spans (each scratch span's `start` rebased into that string).
`writeResultSlice` writes a scratch span as typed text and each other span as a carried transclusion
(`result_slice.cpp:21-36`, `:48-65`). `--in-place` needs no change beyond the arena: the joined cell
is a quote cell and `promote()` writes it as in section 4.

## 7. Price

- One accessor, a two-line change in `promote()`'s two loops, one range check in `makeQuote`, a
  `scratch` string on `QuotedSpans`. No layout change, no snapshot change, no format change.
- A join over several stores that disagree on external scrolls stays text. Measured as before:
  copies bounded by `maxJoinedTextBytes`.
- The join holds per-row pieces until its end (one small record per row beside the run it already
  held), which is bounded by the same byte bound and cleared at the end; no new leak (tested at 10^4
  rows).
- Hardening, not a regression: `promote()` now carries the non-scratch spans of a mixed run whose
  store is not the destination, and refuses (`UnnameableContent`) one it cannot carry. Before, such
  a run was written raw.

## 8. Acceptance criteria

1. A multi-row join over a loaded store's rows returns a quote cell whose spans are the rows' spans
   with scratch separators; `textOf` equals the joined text.
1. `vquery -e ... -o OUT` and `--in-place` save that cell as the original spans plus separator
   splices: `xudu-dump --section=ops` shows the rows' permascroll spans and the permascroll grows by
   exactly the separator bytes.
1. Mixed foreign + local + scratch (number) rows work; rows of stores that disagree on external
   scrolls join as text; an empty join, a one-row join and a lone foreign row are unchanged.
1. `promote()` still refuses what it refused (runtime-only state, unnameable quotes, cross-store),
   now also a mixed run it cannot carry; scratch bytes reach a store only through it.
1. Every existing `ArenaManifold`, join and `vquery` test passes; `CompactOpNode` and the fixtures
   are untouched; a 10^4-row join adds no live cell after its result is dropped beyond what slice d
   already measures.

## 9. Test plan

- `tests/zigzag` (arena): `quotedRun` answers store and the whole run for a quote with a separator
  and `quotedContent` still answers nothing for it; `makeQuote` rejects a scratch span past the
  buffer; `textOf` of a quote with separator; `release()` drops the cell and the separator;
  `promote()` of a quote with a separator from another store writes carried spans and spools only
  the separator, and refuses (`UnnameableContent`) one whose external scroll the destination cannot
  name; a scratch-first run.
- `tests/xuzz` (join, `vquery`): multi-row join over a `core_hypertext` store's rows through a
  `MultiStoreCoordinator`; `-o` and `--in-place` keep each row's transclusion (ops dump, permascroll
  growth); foreign + local + number; two stores with disjoint external scrolls fall back to text;
  empty and one-row cases; a bounded run.
- **Fail-before**: each new test is run on the pre-change tree (the join returns interned text, the
  saved cell is a typed copy) and its failure recorded.

## 10. As built

Owner decision (2026-11-26, binding): P11 stays for `join` and the d2 gate closes on it. The rule is
"a text is one cell, a collection is a pack". `join` is a snapshot, not live. The dimensional pack
(`d.pack`/`d.packing` of row handles read out in order, `view-system.md` §9.3) is built later as a
separate rowset/collection feature with its own core proposal P13 (`VortexCore::render` reading a
pack out); it is not part of M7-VQL. See
[`vortex-io-m7-vql-d2-container.md`](vortex-io-m7-vql-d2-container.md).

Status: built, **gate passed** in round 2 (zigzag and Vortex critics; see
[`m7-vql-d2/README.md`](projects/vortex-io-implementation-review/m7-vql-d2/README.md)). One core
file changed, `arena_manifold.{hpp,cpp}`; no other core file, no on-disk format, no fixture.

- **`arena_manifold`**: `quotedRun(ref)` (section 3); `quotedContent()` is `quotedRun()` minus
  scratch-bearing runs, answers unchanged; `makeQuote()` throws `std::out_of_range` for a scratch
  span past the buffer; `promote()` uses `quotedRun()` in its two loops and skips scratch spans when
  checking `canCarry` and when carrying. Nothing else (no `mark()`, snapshot or codec change).
- **`join_relation`**: `JoinWalk` holds one `JoinPiece` per row and separator (cell, master, spans,
  store, all-local, foreign). At the end the namespace rule (section 2) decides. Shared: one
  `makeQuote(space, run)` where `space` is the attached space whose store is the anchor, else a
  plain cell as before. Not shared: every piece that names a store is interned as text (`textOf` of
  its master), as slice d did for foreign rows. A store-less, non-foreign, all-local piece
  (`makeCell(span)`, a capture) agrees with any anchor, as it does for `promote()` today.
- **`result_slice`** (`apps/common`, not core): `QuotedSpans::scratch` holds the bytes of the run's
  scratch spans; `writeResultSlice` types those and transcludes the rest.
- **`vquery`**: `resultRows` uses `quotedRun` and rebases scratch spans into `QuotedSpans::scratch`.
  `--in-place` is unchanged (it calls `promoteOrRefuse`). `vpl` still uses `quotedContent`.

### Deviations from the plan

- The arena tests sit in `tests/xuzz/arena_federation_test.cpp` beside the other `ArenaManifold`
  tests (they build against the engine only); the task text said `tests/zigzag`, which holds the UI
  and slice-visualizer tests.
- A join whose rows disagree falls back for **every** piece that names a store, including a
  base-namespace quote that slice d kept as spans. It is text either way and always correct; it is
  the simpler rule to state.
- `promote()` now also carries the quoted spans of a mixed run in a base-namespace quote (hardening,
  section 7). No existing test depended on the old raw write.

### Tests

- `ArenaQuoteSeparatorTest` (8, in `arena_federation_test.cpp`): `quotedRun` and `quotedContent`
  answers, constructed text is no quote, scratch bound (past the buffer, and a wrapping length), a
  quote with a separator vanishes with its mark, `promote()` transcludes the quoted spans and grows
  the permascroll by exactly the separator bytes, a scratch-first run, an unnameable run is still
  refused with the destination untouched, plain scratch is still typed.
- `JoinHarness` (8, `vql_join_quote_test.cpp`): several rows of a loaded store stay quoted; promote
  keeps each row transcluded; two stores over one permascroll are one quote; foreign + local + a
  number; disagreeing external scrolls are text; one row, no rows and the lone foreign row are as
  before; a join of joins stays quoted; 10^4 rows leave only separators in scratch.
- `VQueryE2ETest` (3, `vquery_e2e_test.cpp`): `-o` saves the rows' own spans (equal to the spans a
  control run quotes) with typed separators; `--in-place` grows a copy of the fixture permascroll by
  exactly the separators (4 bytes for `", "` twice) and writes one cell whose run is those spans; a
  lone row and an empty join save as before. `xudu-dump --section=ops` on both outputs shows
  `make cell` over the row's original span `[1083,1089)` and `splice` ops for the separators
  (`[11037,11039)`) and the other rows (`[1089,1099)`, `[1099,1111)`).

### Fail-before

Run against the pre-change `join_relation`, `result_slice` and `vquery` (arena kept): seven tests
fail (five of `JoinHarness`, two e2e: the join is text, the saved cell is a typed copy), and the
three "as before" tests pass. Run with `promote()` reading `quotedContent()` again: the
unnameable-run refusal test fails (the run was written raw). The remaining arena tests cover API
that did not exist.

### Gate round 1 fixes

Findings in
[`r1-disposition.md`](projects/vortex-io-implementation-review/m7-vql-d2/r1-disposition.md).

- **Disagreeing join types every non-free piece.** The fallback in `join_end` interned text only for
  pieces that had a store; a store-less foreign or external-scroll piece kept raw spans that the
  arena then read in its own namespace. It now types every piece that names a byte and is not *free*
  (store-less, not foreign, all local), whether or not the piece has a store. Scratch-only pieces
  (separators, numbers) stay as they are.
- **The precondition of a free piece.** A store-less all-local piece agrees with any anchor on the
  trust that its local spans are on the permascroll the anchor reads. A span carries no permascroll
  identity, so this cannot be checked at the join; it holds because every store-less local cell a
  query meets is a capture of the author's one permascroll. `join_relation.cpp` says so beside
  `namespaceOf`.
- **A diagnostic.** A join that falls back to text logs one `vql.join` debug line with the row count
  (`SPDLOG_LEVEL=debug`).
- **The capture fallback accepts scratch.** `quotedSpansOf` (`result_slice.{hpp,cpp}`, shared by
  `vquery`'s `resultRows`) quotes a store-less cell's non-scratch spans against the capture store
  and rebases its scratch separators into `QuotedSpans::scratch`, so a join of captured cells keeps
  the captures transcluded. A cell holding only scratch quotes nothing.
- **`quotedRun` also answers for a plain mixed cell.** A non-quote cell of an arena that has its own
  store, holding local plus scratch spans, used to give `quotedContent` nothing (any scratch
  refused). Now `quotedRun` returns the arena's store for it, so `promote()` applies `canCarry` to
  its local spans and refuses (`UnnameableContent`) where it once wrote them raw, and `vquery` saves
  it as a transclusion plus typed scratch. Arguably the fix of a latent raw write; pinned by
  `APlainMixedCellIsCarriedLikeAQuoteByItsOwnStore`.
- **Two stores from the command line.** Not reachable: with two stores loaded `##/+d.dims` answers
  the composite's own typed dimension cells and no VQL path yields both stores' content cells in one
  rowset. The two-store case stays at `JoinHarness`; the command line has a one-store join test.

## 11. Change history

| version | date       | change                           |
| ------- | ---------- | -------------------------------- |
| 0.1     | 2026-11-26 | Design for M7-VQL-d2 (P11), gate |
| 0.2     | 2026-11-26 | As built; gate pending           |
| 0.3     | 2026-11-26 | Gate round 1 fixes               |
| 0.4     | 2026-11-26 | Gate passed; owner decision      |
