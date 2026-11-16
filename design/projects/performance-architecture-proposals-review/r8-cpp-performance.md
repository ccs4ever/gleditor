# Round 8 critique: C++ Performance Expert (performance-architecture-proposals.md)

Paths below are relative to the repository root. This focused follow-up reviews v7 against the
round-7 findings. Performance experiments remain implementation acceptance gates; this review did
not run benchmarks or establish new speedups.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R7 B1 (quote occurrence value kind is mutable): RESOLVED — P3 retains occurrence exclusion inside
  the `ExternRef` branch, preserves missing-dense/base delegation, and specifies differing/absent
  local-edge and rollback regression cases (`design/performance-architecture-proposals.md:279-305`).
- R7 M1 (per-mutation handle snapshots can force quadratic COW): RESOLVED — P1 defines first-write
  snapshots per active rollback boundary, retained before uniqueness checks, with private touched
  sets, nested-boundary lifecycle and allocation-failure ordering. Subsequent writes still honor
  external aliases without creating another undo owner per mutation. Tracking and retained-version
  costs are priced (`design/performance-architecture-proposals.md:130-163,190-215`).
- R7 M2 (instrumented self time presented as release saving): RESOLVED — The baseline and P4 now
  distinguish instrumented observations from unmeasured release effects, remove the promised
  1.4-second saving, and require independent release inline/LTO comparisons, inlining evidence,
  executable-text size and build/link costs (`design/performance-architecture-proposals.md:19-22`,
  `design/performance-architecture-proposals.md:311-338,479-487`).

## Verification of nested mutable-payload boundaries

The specified rule preserves the required restoration states without allocation on commit:

- If the parent has already touched a key, its original undo handle remains sufficient. A child
  snapshots the child's entry handle independently; retaining the child's records after commit
  preserves reverse replay through the child state back to the parent's original state.
- If the parent has not touched a key, the child's retained record still names the parent's entry
  value. The parent's first later write may additionally snapshot the post-child value; reverse
  replay restores that value and then the child's entry value. Dropping the child's touched set
  without merging into the parent avoids an allocating commit and does not lose either undo record.
- Child rollback restores its entry state and removes its own records/set. Root eviction retains
  surviving sets and reclaims only records below all remaining boundary watermarks. Existing COW
  checks continue to account for aliases outside the trail.

These are design-level invariants to exercise in the implementation gates at
`design/performance-architecture-proposals.md:451-464`; convergence does not certify an unbuilt
trail implementation or imply that it is faster than the current map snapshots.

## Status of previous MINOR findings

- R7 m1 (wing calls are not allocations): RESOLVED — Empty-wing behavior is corrected; eight is a
  provisional spill threshold with arity, allocation, spill and alternative-buffer measurements
  (`design/performance-architecture-proposals.md:347-350,389-395`).
- R7 m2 (prefix compaction costs): RESOLVED — O(live records + choices) movement/rebasing and
  retained vector capacity are explicit; repeated-steal measurements are required
  (`design/performance-architecture-proposals.md:211-215,463-464`).
- R7 m3 (dense key range is not useful-mapping density): RESOLVED — P2 now separates key-range facts
  from the storage-policy hypothesis and requires text/branch/structure-heavy folds with
  initialization, reserved-space and physical-memory measurements
  (`design/performance-architecture-proposals.md:260-266,465-467`).

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED
