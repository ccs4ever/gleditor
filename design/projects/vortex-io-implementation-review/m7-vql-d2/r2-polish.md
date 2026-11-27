# Round 2 polish (M7-VQL-d2 gate)

Targets: [`r2-zigzag.md`](r2-zigzag.md) (0/0/1, converged) and [`r1-vortex.md`](r1-vortex.md)
(0/0/2, converged in round 1).

## zigzag r2 MINOR 1: fixed

`quotedSpansOf`'s capture fallback (`apps/common/xanadu/result_slice.cpp`) now requires every
non-scratch span to be local before it quotes a store-less cell against the capture store; a cell
carrying an external-scroll span returns `std::nullopt`, so the caller saves it as typed text (the
convention of the function's other refusals). Test:
`JoinHarness.AStoreLessCellWithAnExternalSpanIsNotQuotedAgainstCapture`; it fails with the fallback
as it was and passes with the fix.

## Vortex r1 MINORs

1. Store-less foreign row joined as raw spans: fixed in round 1 (same defect as zigzag M1), see
   [`r1-disposition.md`](r1-disposition.md).
1. Free pieces trust the anchor's permascroll (vortex m2): documented in the design note, pinned by
   a test; accepted as not checkable at the join.
