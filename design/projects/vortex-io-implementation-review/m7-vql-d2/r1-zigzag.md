# Round 1 critique: Xanadu/ZigZag Purist (M7-VQL-d2 code)

Target 5f973a10 (diff 7ac9ed6f..5f973a10). Tally: 0 BLOCKING, 1 MAJOR, 3 MINOR.

## Verified clean (empirical, headless, scratch copies of the fixtures)

- Transclusion identity: `vquery --permascroll=<copy> xanadu_a -e 'let $r := ##/+d.dims; return join($r, ", ")' -o out`
  and `xudu-dump --section=ops --permascroll=...` show `make cell [1083,1089)` then splices
  `[11067,11069)` ", ", `[1089,1099)`, `[11069,11071)` ", ", `[1099,1111)`: the rows' original
  fixture spans, only separators new. `--in-place` with "|": permascroll 11037 -> 11039 bytes (two
  separators, nothing else), ops 128-132 are the original spans plus `[11037,11038)` and
  `[11038,11039)`. Not a retyped copy.
- Scratch policy: `makeQuote` is the only door for scratch into a quote; past-buffer and wrapping
  lengths throw `out_of_range` (arena_manifold.cpp:2215-2221); `quotedContent()` still answers
  nothing for scratch-bearing runs; `promote()` types scratch only via `makeCell`/`spliceCell`, and
  checks `canCarry` for every non-scratch span before writing (3258-3270). `compact()` does not
  touch `scratch_`.
- Hardening deviation (`UnnameableContent` for a mixed run): sound; it closes a raw-write of foreign
  spans. 70 tests (ArenaQuoteSeparator, JoinHarness, VQueryE2E, Promote) pass on the rebuilt binary.
- R8: join mints no operation; 2-degree rule, `CompactOpNode`, fixtures and every other core file
  untouched (`git diff --stat` shows only `arena_manifold.{hpp,cpp}`).
- Namespace rule for stored pieces (anchor = first external-scroll store, others same store or
  local-only on the same permascroll object) matches `canCarry`; two external stores, or a different
  permascroll, fall back to text.

## BLOCKING

None.

## MAJOR

### M1. Disagreeing join leaves a store-less foreign piece's spans raw in the text fallback

- **Problem**: `namespaceOf` makes `shared=false` for a foreign piece with no store (comment at
  join_relation.cpp:100-108 says such rows "are joined as text"). The fallback loop converts only
  `piece.store != nullptr` (join_relation.cpp, end provider `if (!shared.shared && piece.store != nullptr)`).
  A foreign (or external-scroll, store-less) piece has `store == nullptr` (`quotedRun` returns
  nullopt when the space's `store` is null, arena_manifold.cpp:2240-2250), so its foreign spans are
  copied raw into a plain arena cell, in the arena's own namespace.
- **Failure scenario**: a space attached with `store = nullptr` (the `Space` type allows it,
  overlay.cpp:402-406 passes `target.store` unchecked) whose cells carry an external-scroll span,
  joined with a normal store row. Slice d interned every foreign row as text; now `textOf` of the
  result reads scroll id N of the arena's own store (wrong bytes or empty), and `promote()` writes
  it as a local span. Static finding: I could not drive it from the CLI (every vquery space has a
  store).
- **Evidence**: join_relation.cpp:101-125 (rule and comment), end-provider fallback loop
  (`piece.store != nullptr`), arena_manifold.cpp:2233-2251.
- **Fix**: in the fallback, intern as text every piece that is not scratch-only and not `free`
  (`!shared.shared && !(free(piece))`), i.e. test the same predicate namespaceOf used; add a
  JoinHarness test with a store-less space.

## MINOR

1. **A store-less all-local join loses capture identity at the CLI.** Joining captured cells
   (`makeCell(span)`, no store) gives a plain cell of local spans plus scratch separators; with no
   store `quotedRun` is nullopt, and `resultRows`' capture fallback refuses any run containing
   scratch (apps/vquery/main.cpp:165-178), so it is saved typed. Read from code, not run. Either
   let the capture fallback type only the scratch spans (as `QuotedSpans::scratch` now allows) or
   record the limit.
1. **"Free" pieces are trusted to share the anchor's permascroll** (join_relation.cpp:106-108): at
   library level a store-less local span from permascroll P1 joined with a store over P2 is quoted
   in P2's namespace. The CLI has one permascroll, so unreachable there; state the precondition.
1. **One outlier row degrades the whole join to text silently** (deviation 2). Documented; but a
   `vquery a b` over a differing-permascroll or external-scroll store gives a typed copy with no
   notice. Consider a one-line diagnostic (`GLEDITOR_LOG_DEBUG`, category `vql.join`).
1. **No CLI-level two-store join test.** `##/+d.dims` over two stores returns the composite's own
   dimension cells (typed), so the "two stores, one permascroll" path is covered only by the
   JoinHarness unit test; I found no vquery query that exercises it through the binary.

VERDICT: NOT CONVERGED
