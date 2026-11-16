# Round 2 critique: CODEBASE (performance-architecture-implementation-plan.md)

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Prior findings

1. **MAJOR resolved:** The P1 register and M5a inventory now include `vortex_resume.cpp` and
   `vortex_comparison.cpp`; the resume and VQL comparison tests are registered, the concrete helper
   boundary is called out, and failed resume initialization is part of the gate.
1. **MAJOR resolved:** M3 preserves `applyStructure(...) noexcept` and specifies capacity preflight
   before logical mutation, direct resource refusal outside semantic-refusal accounting,
   non-allocating commit, propagation through `advance`, and typed stop behavior in
   `advanceOrRefold` and both Store rebuild paths. Fault injection and retry/full-rebuild checks are
   included.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Evidence and scope

The registered files and tests match the live resume mutation/helper boundaries at
`vortex_resume.cpp:62-99` and `vortex_comparison.cpp:24-45,197-200`; v2 names both in §9.1 and the
P1 register. The reversible map/set adapter is now explicitly delivered and wired while the snapshot
algorithm remains active, with no-allocation restoration tested before enabling trail rollback
(§9.1). This addresses the live `std::unordered_map` node-restoration constraint.

The P2 plan now separates capacity-preparation failure from authored semantic refusals and says the
prepared insertion commit cannot allocate (§7). The new resource refusal is propagated by `advance`;
callers that rebuild or refold stop with a typed exception rather than continuing from a partial
manifold. That is a workable planned addition to the current `FoldResult`/`AdvanceResult` interfaces
and Store's current refusal-continuation behavior. The remaining unhandled allocation paths inside
the pre-existing `noexcept` replay engine are explicitly outside this guarantee.

No other sections were re-audited in this focused round.

VERDICT: CONVERGED
