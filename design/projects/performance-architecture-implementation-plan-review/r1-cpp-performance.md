# Round 1 critique: C++ Performance Expert (performance-architecture-implementation-plan.md)

This bounded review checks implementation order, release ThinLTO/PGO lifecycle, mechanical sympathy,
VM undo/COW feasibility, lookup and operand costs, and measurement gates. No runtime builds or
benchmarks were run. The accepted P1-P5 choices are treated as settled; this report does not request
reapproval.

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

This is the first review round for the implementation plan; there are no prior plan findings.

## BLOCKING

None.

## MAJOR

### 1. (§9.1, lines 231–241) No-allocation undo is ordered before a reversible map implementation

**Full title:** M5a cannot guarantee allocation-free restoration of erased unordered-map entries as
sequenced

**Problem:** M5a requires an undo path that never allocates and says erased map entries retain a
restorable bucket, but its deliverables introduce only trail/savepoint helpers. M5c, later, is where
the plan introduces reversible open-addressed VM maps. In the live implementation, these state
containers are `std::unordered_map` and `std::unordered_set`; erasing an element destroys its node.
Reserving bucket capacity does not reserve a node for a future insertion, so ordinary reinsertion
while unwinding can still allocate. A failed allocation during restoration violates the stated
noexcept savepoint/scoped-abort contract. The M5a allocation-failure gate cannot demonstrate this
property unless it specifies how erased nodes survive before the M5c migration.

**Evidence:** The plan stages the trail first
(`performance-architecture-implementation-plan.md:231-245`) and postpones open-addressed reversible
maps until M5c (`:279-296`). Its requirement to retain restorable erased buckets is at `:233-241`.
Current VM state uses unordered maps/sets in `apps/common/xanadu/vortex/vortex_vm.hpp:872-884`;
existing frame rollback erases absent-before keys at
`apps/common/xanadu/vortex/vortex_frames.cpp:411-415`, and current choice restoration relies on
whole-map swaps at `apps/common/xanadu/vortex/vortex_vm.cpp:491-505`. Those swaps avoid allocating
new nodes precisely because the old map is retained wholesale.

**Fix:** Make the M5a sequence explicitly provide a reversible-entry mechanism before production
trail rollback is enabled: for example, retain extracted node handles per typed container and
pre-reserve any bucket growth needed to reinsert them, or introduce the reversible table primitive
in M5a and defer only its performance tuning/complete map migration to M5c. State how absent keys,
rehash, multi-key erase/clear, and allocation failure preserve those retained nodes. Keep the
snapshot oracle until the no-allocation unwind property passes against the chosen mechanism.

## MINOR

None.

VERDICT: NOT CONVERGED
