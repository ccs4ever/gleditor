# Round 2 critique: C++ Performance Expert (performance-architecture-implementation-plan.md)

Focused follow-up to the C++ performance expert's round-1 finding. This review verifies the M5a
reversible-container ordering and M3 lookup-capacity failure path against v2 and the live fold/Store
integration points. No runtime builds or benchmarks were run; the accepted P1-P5 proposals remain
unchanged.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1 M1 (no-allocation undo precedes a reversible map implementation): RESOLVED. M5a now delivers
  and wires a reversible map/set adapter while retaining the snapshot algorithm, before enabling any
  trail rollback; it specifies retained logically-absent keys, rehash preservation, pre-mutation
  capacity preparation, bulk erase preparation, and allocation-disabled restoration tests
  (`performance-architecture-implementation-plan.md:226-246`). M5c is explicitly limited to tuning,
  remaining maps, and snapshot retirement (`:286-297`). This addresses the live unordered-map/set
  node-reinsertion problem identified in round 1.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Focused verification

- M5a's capacity contract covers live and restorable entries; restoration is by key, not by a
  pointer into movable storage. The planned adapter preserves logical snapshots/comparison and
  absent versus present-unbound states. It is tested independently while the current snapshot
  algorithm is still active, and trail rollback is gated on allocation-disabled erase/clear/rehash
  restoration. This supplies an implementable prerequisite for the stated no-allocation unwind.
- M3 now separates resource exhaustion from semantic refusal with a planned typed refusal and
  exception. It requires all new lookup storage to be prepared before changing fold position, slots,
  content, links, lastOp, mappings, or semantic refusal counters; prepared commits cannot allocate.
  The explicit limitation to new lookup allocations avoids implying that this repairs every existing
  `noexcept` allocation path.
- Store's fold/advance paths are required to propagate or throw on that resource refusal instead of
  continuing past it or returning a partial fold. This matches current rebuild behavior, which
  currently ignores `applyStructure` refusals at `store.cpp:529-561`, while preserving existing
  semantic-refusal accounting. Failure injection at each preparation allocation, logical-state and
  counter comparison, retry, and full-rebuild comparison provide a gate that can establish the
  intended property.

VERDICT: CONVERGED
