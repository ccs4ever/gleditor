# Round 1 critique: Vortex Expert (M7-VQL-d2 code)

Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR. Target `5f973a10` (diff `7ac9ed6f..5f973a10`).

Method: read the full diff of `arena_manifold.{hpp,cpp}`, `join_relation.{hpp,cpp}`, `result_slice.*`,
`apps/vquery/main.cpp`; traced mark/release/discard/compact/promote against the new scratch-in-quote
state; ran (headless, scratch XDG dirs under /tmp/claude-1000) a copy of the existing `xuzz_test`
with `--gtest_filter='JoinHarness.*:ArenaQuoteSeparator*:*Arena*:*Join*:*Quote*'`: 322 tests, 321
passed, 1 skipped (SwarmTest, unrelated). No new probe binary was built (read-only lane, and the
tree was mid-build by another process).

Core-file check: `git diff --stat 7ac9ed6f..5f973a10` over `vortex/`, `store.*`, `ops.hpp`,
`compact_op.hpp`, `store_tables.hpp`, `segmented_ops_spool.*`, `zigzag/manifold.*` is empty. Only
`arena_manifold.{hpp,cpp}` changed in core, as approved.

## Verified clean (no finding)

- Mark/release/discard: a quote's `content_` slot, its `quoteSpace_` entry (`arena_manifold.cpp:2224`,
  truncated `:2750`) and the scratch bytes it names (`scratch_.resize(m.scratchSize)`, `:2908`) are
  all rolled back together. The new bound check (`:2216-2221`) requires the span to lie inside the
  buffer at mint time, so a quote can only name scratch minted before it; any release that truncates
  that scratch also truncates the quote. `AQuoteWithASeparatorVanishesWithItsMark` covers the slot
  reuse.
- `compact()` (`:3089`) rewrites only `content_`; scratch is never compacted, so spans stay valid.
  `namedByArenaState` (`:2974`) keeps quote cells from being freed.
- Copy-on-write over a base `Manifold`: scratch is arena-owned only (a base has none), so shadowing
  cannot carry a scratch span from a base.
- Snapshot/restore refuse any arena holding quotes (`:3760`, `:3855`), unchanged.
- `quotedContent` keeps the old answer for every caller: pure-scratch run gives nullopt in
  `quotedRun` (all_of), mixed run gives nullopt in the `quotedContent` wrapper (any_of); runs with no
  scratch are identical. Callers (`vpl/main.cpp:165`, `compile_semantic_shape.cpp:185`, tests) see no
  change.
- `promote()`: scratch spans are skipped by the carry validation (`:3259-3268`) and the carry loop
  (`:3352-3354`), then typed through `scratchTextOf` (first span `:3398`, later spans `:3412-3414`).
  Refusal for an uncarryable non-scratch span is preserved
  (`ARunTheDestinationCannotNameIsStillRefused`). Ref mapping and `ephemeralBit` rules untouched.
- `textOf`/`boundedTextOf` already read scratch spans from the arena before the space reader
  (`:2025`, `:2082`).
- `join_relation`: pieces hold one span vector per row plus separator; memory is O(spans), freed by
  `std::exchange` at `join_end`; 10^4 rows is exercised by `AWideJoin...` (2n-1 spans, scratch =
  n-1 separator bytes). Owner-thread only (no worker access). Provider registration without undo is
  unchanged from slice d (`position == 0` resets the walk); no new live-cell pins.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. **A store-less foreign row is joined as raw spans, not text as the comment promises.**
   Problem: `namespaceOf` returns `shared=false` for a piece with no store that is not "free"
   (`join_relation.cpp:~118-123`), and the doc comment says such rows "are joined as text". But the
   `join_end` loop converts to text only when `piece.store != nullptr`
   (`join_relation.cpp:~211`); a foreign store-less piece falls to the `else` branch and its spans
   are copied raw into a plain cell, whose reader is the arena's own store. Slice d joined every
   foreign row as text, so this is a regression on that corner.
   Scenario: a `Space{.manifold=..., .reader=...}` with `store == nullptr` (test-only today; every
   production attach in `quoted_structure.cpp:534`, `compile_history.cpp:157`, `vql_engine.cpp:1355`
   sets `.store`). Rows from two such spaces join into bytes read against the wrong namespace.
   Fix: in the `!shared.shared` branch also take text when `piece.foreign` (or when the piece quotes
   and is not `free`); add a test with a store-less space.
2. **`quotedRun` widens the promote carry check to plain mixed cells (undocumented).**
   Problem: a non-quote arena cell with local plus scratch spans previously returned nullopt from
   `quotedContent` (any scratch), so `promote` neither checked nor carried it; now `quotedRun`
   returns the arena's own store for it, so `promoteOrRefuse` applies `canCarry` to the local spans
   (`arena_manifold.cpp:3259`, `:3352`) and may return `UnnameableContent` where it formerly wrote
   the spans raw. Likewise `vquery`'s `resultRows` now emits such a cell as a transclusion plus typed
   scratch instead of typed text.
   Scenario: `makeCell()` + `setContent(local span, scratch separator)` in an arena whose store
   reads a different permascroll than the promote destination: refused now, silently raw before.
   This is arguably more correct (the old behaviour could write unrelated bytes) but the design doc
   says refusal paths are unchanged except as documented, and no test pins it.
   Fix: document the change in `vortex-io-m7-vql-d2.md` and add a test for the plain mixed cell,
   or restrict `quotedRun` to cells with a quote space.

VERDICT: CONVERGED
