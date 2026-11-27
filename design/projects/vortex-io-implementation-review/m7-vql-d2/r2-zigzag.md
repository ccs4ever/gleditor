# Round 2 critique: Xanadu/ZigZag Purist (M7-VQL-d2 code)

Target e27693fe (round 1 fixes). Tally: 0 BLOCKING, 0 MAJOR, 1 MINOR.

## Status of round 1 findings

- **M1 (store-less foreign piece raw in text fallback): fixed.** `join_end` now types every piece
  that names a non-scratch byte and is not `isFree` (store-less, not foreign, all local), whether or
  not it has a store (join_relation.cpp, end provider). Scratch-only pieces (separators) are
  inserted raw exactly once; a typed piece's separators ride in its own `textOf`. The shared path is
  unchanged (`makeQuote` admits scratch). The two new JoinHarness tests pin it.
- **m1 (store-less captured join loses identity): fixed.** `quotedSpansOf` (result_slice.cpp) quotes
  the `quotedRun`, else a store-less cell's spans against the capture store; scratch spans are
  rebased into `QuotedSpans::scratch` once per occurrence (start = running size),
  nothing-but-scratch quotes nothing. `vquery resultRows` calls it (main.cpp:144).
- **m2 (free pieces trust the anchor's permascroll): resolved by documentation.** Precondition
  stated at `namespaceOf` and in the design note; accepted, not checkable at the join.
- **m3 (silent degrade to text): fixed.** `GLEDITOR_LOG_DEBUG("vql.join", ...)` present.
- **m4 (no CLI two-store test): accepted.** One-store CLI test added; the two-store case is
  unreachable through a VQL path, agreed.

## Empirical (headless, scratch copies under /tmp/claude-1000/r2z, rebuilt vquery)

- `vquery --permascroll=<copy> xanadoc_a -e 'let $r := ##/+d.dims; return join($r, ", ")' -o out`:
  permascroll 11037 -> 11100. `xudu-dump --section=ops` shows the joined cell as
  `make cell [1083,1089)` then splices `[11067,11069)` ", ", `[1089,1099)`, `[11069,11071)` ", ",
  `[1099,1111)`: original fixture spans, two separators typed once each. The remaining growth is the
  result slice's own scaffolding (`main`, `home`, `d.dims`, `d.result`, `d.source`, the other typed
  rows, `current`), none of it a copy of the rows' bytes.
- `--in-place` on a copy with "|": 11037 -> 11039, ops 128-132 are the original spans plus
  `[11100,11101)` and `[11101,11102)` as separators only.
- 31 relevant xuzz_test suites (Join, Quote, VQueryE2E, Promote, Capture) pass; 2 unrelated skips.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. **`quotedSpansOf`'s capture fallback checks that a cell names a byte, not that the spans are
   local.** When `quotedRun` is nullopt because a foreign cell's space has a null store (the `Space`
   shape `overlay.cpp:402-406` allows) or the arena has no store, a cell carrying an external-scroll
   span is quoted against the capture store and addresses the wrong bytes: the round 1 M1 class,
   reached through the shared helper instead of the join. Unreachable from the CLI (every vquery
   space has a store; captures are local), so not upgraded. Fix: refuse the fallback unless every
   non-scratch span is on the local scroll (`scroll == 0`), and add one unit test with a store-less
   foreign cell.

VERDICT: CONVERGED
