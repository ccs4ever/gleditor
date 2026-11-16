# Round 1 critique: Vortex & Spool-Mesh Expert (performance-architecture-implementation-plan.md)

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### M1. Preserve the VM's current Dewey path across trail migration

**Problem:** M5 inventories the ten snapshotted maps/sets plus `cursorPC_` and `streamCursor_`, but
does not classify `currentDeweyPath_` or explicitly retain `ChoicePoint::deweyPath` as choice
metadata. This path is live OR-parallel branch state, and M5c's retirement of production snapshots
could remove or stop restoring it without an obvious regression in ordinary VM state comparisons.

**Evidence:** The inventory at `design/performance-architecture-implementation-plan.md:225-241` does
not mention either path field. In the live VM, choice creation derives `altDewey`/`nextDewey` and
updates `currentDeweyPath_` (`apps/common/xanadu/vortex/vortex_vm.cpp:1393-1433`); backtrack
restores it from `cp.deweyPath` (`vortex_vm.cpp:1478`); scoped abort restores it from the saved
choice (`vortex_vm.cpp:1575`). Cut callbacks also derive branch prefixes from these paths
(`vortex_vm.cpp:1528-1543`). OR-parallel export consumes the oldest choice's `deweyPath` and `altOp`
to construct a spark (`apps/common/xanadu/vortex/or_parallel.cpp:224-258`).

**Fix:** In M5a, classify `currentDeweyPath_` and `ChoicePoint::deweyPath` as branch-routing
metadata, state whether they remain boundary-owned outside the undo trail, and add differential
coverage for nested choice/backtrack, cut callbacks, scoped abort, and OR-parallel root stealing.
Ensure M5c retires only the whole-map snapshot state, not this path metadata.

## MINOR

None.

VERDICT: NOT CONVERGED
