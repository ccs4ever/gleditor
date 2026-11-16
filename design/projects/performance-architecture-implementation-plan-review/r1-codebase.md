# Round 1 critique: CODEBASE (performance-architecture-implementation-plan.md)

Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. §11 P1 register omits live VM translation units with rollback-relevant map access

**Problem:** The P1 file register is not complete enough to serve as the promised exact
implementation inventory. `vortex_resume.cpp` directly mutates `cellReferences_` while coordinating
resume-token operations with arena savepoints. `vortex_comparison.cpp` passes the concrete map/set
types into `comparisonOperand`. A migration that changes the map types or trails all writes cannot
safely be scoped to the registered TUs without auditing or changing these interfaces.

**Evidence:** [§11, P1 row] lists `vortex_vm.{hpp,cpp}`, frames, streams, closures, bindings and
contracts, but neither omitted file. Live mutations are in
`apps/common/xanadu/vortex/vortex_resume.cpp:62-99` (find, insertion through `writeCellReferences`,
and restore/erase on failure). `apps/common/xanadu/vortex/vortex_comparison.cpp:197-200` passes
`cellReferences_`, `streamSlots_` and `iteratorSlots_` to `comparisonOperand`; that helper's
concrete aliases are in `vortex_comparison.cpp` near lines 24-45. The register itself says consumer
inventory is to be confirmed, but this is a known integration point, not merely a hypothetical
caller.

**Fix:** Add `vortex_resume.cpp` and `vortex_comparison.cpp` to P1's exact file list, and explicitly
require adaptation of comparison helper parameter types. Include the resume failure path in the
mutator/savepoint inventory and differential cases.

### 2. §7 P2 allocation-failure guarantee conflicts with the `noexcept` fold interface

**Problem:** M3 promises that map growth allocation failure preserves the prior logical map, but the
primary insertion surface, `Manifold::applyStructure`, is `noexcept`. Its `FoldResult` can report
semantic refusal, not `std::bad_alloc`; an allocation from the new dense/flat structures therefore
cannot be propagated as a recoverable fold refusal as currently described. The plan does not choose
a compatible failure mechanism, so implementers cannot meet both the failure promise and current
interface.

**Evidence:** [§7, first and third items] requires allocation failure to preserve the logical map
and covers all chain insertion paths. The declaration is
`apps/common/xanadu/zigzag/manifold.hpp:792-793`; the definition is
`apps/common/xanadu/zigzag/manifold.cpp:321-323`. The live fold inserts into `byRef` within this
`noexcept` function (for example `manifold.cpp:342`), and Store fold callers consume only
`FoldResult` at `apps/common/xanadu/store.cpp:529-542`.

**Fix:** State how the fold handles capacity preparation and allocation failure within its
`noexcept` contract. For example, pre-reserve all required mapping capacity outside the fold and
make fold-time insertion non-allocating, or revise the interface/callers so allocation failure can
be surfaced without terminating. Specify which behavior the gate verifies; do not leave it as an
implicit helper implementation choice.

## MINOR

None.

VERDICT: NOT CONVERGED
