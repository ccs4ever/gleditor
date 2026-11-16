# Round 1 critique: SYSTEM ARCHITECT (performance-architecture-implementation-plan.md)

Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. §11 P1 register omits live VM translation units with rollback-relevant map access

**Problem:** The P1 register is not an exact implementation inventory for the planned map migration
and trail. The plan identifies resume cleanup in its mutator scope, but omits the translation unit
that mutates `cellReferences_`; it also omits a consumer that depends on concrete map/set types. An
implementation cannot preserve the declared ownership and rollback behavior by following the
registered file list alone.

**Evidence:** [§11, P1 row] lists the VM, frames, streams, closures, bindings and contracts, but not
`vortex_resume.cpp` or `vortex_comparison.cpp`. Resume writes and restores/erases `cellReferences_`
around an arena savepoint at `apps/common/xanadu/vortex/vortex_resume.cpp:62-99`. The comparison
path passes `cellReferences_`, `streamSlots_` and `iteratorSlots_` into `comparisonOperand` at
`apps/common/xanadu/vortex/vortex_comparison.cpp:197-200`; its concrete helper types must be adapted
if map types change. The inventory promise appears at [§9.1, lines 225-229], while the exact file
register is [§11, line 323].

**Fix:** Add both translation units to P1's exact file list. Explicitly include resume
mutation/failure rollback in the mutator inventory and differential cases, and require adapting
comparison helper parameters to the selected table interface.

### 2. §7 M3 allocation-failure guarantee conflicts with the noexcept fold interface

**Problem:** M3 promises allocation failure will preserve the prior logical map, but the main
insertion surface, `Manifold::applyStructure`, is `noexcept`. The plan does not specify how failure
during dense/flat map growth is surfaced or prevented. As written, an implementation that allocates
during the fold may terminate on `std::bad_alloc`, violating the promised recoverable semantics.

**Evidence:** [§7, lines 176-180] requires allocation failure to preserve the map; \[§7, lines
185-188\] places all operation-chain insertions in `Manifold::applyStructure`. Its declaration is
`apps/common/xanadu/zigzag/manifold.hpp:792-793`, and the implementation inserts mappings inside the
`noexcept` function (for example `manifold.cpp:342`). Store fold callers consume `FoldResult` at
`apps/common/xanadu/store.cpp:529-542`; that result currently describes semantic refusal, not
allocation failure.

**Fix:** Specify a failure strategy compatible with the interface: for example, prepare/reserve all
mapping capacity before entering the fold so insertions cannot allocate, or revise the interface and
callers to return allocation failure without termination. State which behavior the gate must verify.

## MINOR

None.

VERDICT: NOT CONVERGED
