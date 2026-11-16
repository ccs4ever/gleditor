# Round 3 critique: Vortex & Spool-Mesh Expert (performance-architecture-proposals.md)

Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (fixed-capacity `inplace_vector` without heap fallback): RESOLVED — `OperandList` now
  specifies inline storage plus unbounded heap spill.
- R2.2 (nested savepoint commit truncates enclosing rollback records): RESOLVED — commit clears the
  trail only when both choice depth and savepoint depth are zero.

## BLOCKING

*None.*

## MAJOR

### 1. (§2.1, lines 114–117) The priced trail entry cannot represent the VM state it must restore

Full title: `VMTrailEntry`'s 16-byte payload cannot restore several trailed map values, and the
rollback contract has no ownership or no-throw rule

**Problem:** P1 prices a 24-byte entry whose union can hold only `uint64_t`, `CellRef`, or
`shared_ptr`. But the VM tables named in P1 have values requiring more than a single `shared_ptr` or
scalar: `ActivationStack` is a vector of shared pointers; `IteratorState` contains a
`RankIteratorState` with an owning `unordered_set`, or a `SequenceIteratorState` with a vector; and
runtime guard arguments and frame data also own vectors and strings. P1 does not specify whether the
undo entry moves these values out, clones them, or stores indirect payloads. Those choices change
its stated size and allocation cost. The required reverse unwind also needs a non-throwing
restoration path; the design does not establish that.

**Evidence:** The target defines the 24-byte union as holding only those three payload forms at
`design/performance-architecture-proposals.md:114-117`. Existing owning values include
`ActivationStack` at `apps/common/xanadu/vortex/vortex_vm.hpp:503-518`, `IteratorState` and
`StreamMap` at lines 453-472, and guard stacks at lines 364-432. Backtracking currently restores
whole map snapshots by move at `apps/common/xanadu/vortex/vortex_vm.cpp:1461-1475`, after releasing
the arena mark at line 1464; the proposal changes that ordering but does not specify a
representation that can restore all those values safely.

**Fix:** Define the undo representation and ownership transfer for every trailed table value,
including how erased and replaced entries are restored. State which operations are `noexcept` during
unwind and how any indirect payload allocations are priced. Revise the 24-byte estimate and its
resource bound to match that representation.

### 2. (§2.1, lines 105–110) Generation-tag resets can retain owning payloads without bound

Full title: Generation-tag invalidation skips destruction of values in open-addressed VM tables

**Problem:** The proposal selects generation tags for O(1) reset but gives no cleanup rule for
occupied slots from an expired generation. Several specified tables hold owning values: `streams_`
and `iterators_` hold `shared_ptr`s, while `cursorPC_` and `opcodeMap_` hold ordinary values. If
resetting only advances the generation, stale entries containing shared pointers retain their
referents until overwritten or table destruction. Repeated resets can therefore accumulate retained
stream and iterator state, defeating the proposal’s bounded-resource claim.

**Evidence:** P1 specifies generation-tagged open-addressed tables and O(1) reset at
`design/performance-architecture-proposals.md:105-110`. The current owning map types are `StreamMap`
and `IteratorMap` at `apps/common/xanadu/vortex/vortex_vm.hpp:470-472`; `VortexVM` stores these
alongside `cursorPC_` and `opcodeMap_` at lines 872-885.

**Fix:** Specify a reset and slot-reuse lifecycle that promptly destroys stale owning values, or
restrict generation-tag reset to tables whose values are trivially destructible and use another
reset strategy for owning tables. Price cleanup work and retained capacity.

## MINOR

*None.*

VERDICT: NOT CONVERGED
