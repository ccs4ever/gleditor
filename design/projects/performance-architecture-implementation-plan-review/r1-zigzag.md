# Round 1 critique: XANADU/ZIGZAG PURIST (performance-architecture-implementation-plan.md)

This review focuses on M2 quote routing, M3 R7/replay and dense/presence lookup semantics, and M4/P1
host metadata and topology boundaries. The accepted P1-P5 proposal is treated as settled. No runtime
work or benchmarks were performed.

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§7, lines 176–198) M3 does not make allocation failure atomic with the noexcept replay fold

**Problem:** M3 correctly requires an explicit presence bitmap, keeps dense slot zero valid, maps
every operation in an R7 chain, and says the new map preserves its prior logical contents on
allocation failure. But the fold itself is `noexcept` and mutates the materialized manifold as it
applies each operation. A map helper's local strong guarantee is insufficient if dense/overflow
capacity or a `structureBirths_` insertion fails after cell/content/link state has already changed.
The plan does not say how fold-time allocation is prevented or how that failure can be represented
as a refusal. Implementers cannot satisfy both the atomicity language and the live fold contract
from this sequence alone.

**Evidence:**
[§7](/data/git/gleditor-restore/design/performance-architecture-implementation-plan.md:176) requires
map growth failure to preserve logical contents; lines 181–187 require mapping insertions across
`Manifold::applyStructure`. `applyStructure` is declared `noexcept` in
`apps/common/xanadu/zigzag/manifold.hpp:790-793` and defined at
`apps/common/xanadu/zigzag/manifold.cpp:321-323`. It changes `foldedThrough_` at line 323; the
non-Structure chain paths mutate content and `lastOp` before inserting into `byRef` (for example
lines 336–343 and 355–385); Make mutates `structureBirths_` and slots before later mapping work
(lines 446–492). Allocation failure inside this API therefore cannot simply propagate as an
exception, and a partially applied operation would break the incremental/full-rebuild honesty
contract in R9. The plan's gate checks replay equality/refusal counts but does not inject or define
this failure case.

**Fix:** Specify an atomic fold-time policy. For example, preflight/reserve all mapping and
birth-table capacity needed for an operation before mutating the manifold, then make its commit
phase non-allocating; alternatively revise the fold result/API so allocation failure is a typed
refusal and explicitly roll back every preceding mutation. State the guarantee and add a
fault-injection gate that verifies the manifold still equals its pre-operation state and a
subsequent full rebuild remains honest.

## MINOR

None.

VERDICT: NOT CONVERGED
