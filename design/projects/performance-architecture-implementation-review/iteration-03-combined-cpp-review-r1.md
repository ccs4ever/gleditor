# M3 C candidate static review (hybrid byRef + unordered births)

## Verdict

I found no logical failure-atomicity or token-lifetime blocker in the isolated C snapshot inspected.
Its ordering matches the intended contract: prepare the birth node, then prepare the `byRef`
insertion, and only then publish either mapping; typed lookup refusal restores fold position and
does not increment semantic refusal counts. The author has also corrected the unordered-map growth
arithmetic noted below. This remains a static review: I did not compile or execute the candidate,
and the parent/worker must still verify the exact post-fix source and run the tests before treating
this as converged.

## Findings

1. **Growth policy correction is present in the inspected snapshot.** The earlier
   `max(reservedFor_, 64)` seed made the first table reservation request 128 entries, far above the
   intended 1.5–2x live range for small manifolds. It also used a saturating `SIZE_MAX` target after
   doubling overflow, which could reject an otherwise representable `totalEntries`. The current code
   starts at `totalEntries` and clamps geometric growth to `entries_.max_size()`.
   `IncrementalGrowthStartsSmallAndIsGeometric` checks a small first capacity and logarithmic bucket
   growth. This removes the concrete small-table over-reservation issue; the near-maximum arithmetic
   has no direct boundary test, but its current checked/clamped expression is sound by inspection.

1. **Unordered birth insertion is preflighted before publication.**
   `PreparedU32UnorderedMap::prepareInsert` reserves destination buckets and extracts a staged node
   from a same-resource staging map. `commitInsert` checks owner/generation/key, performs a no-throw
   mapped-value assignment, and inserts the same-resource node. The map reserves enough buckets
   before staging, so the commit path has no expected allocation point; unexpected insertion failure
   returns false, and the Manifold caller treats that as an invariant violation.
   `StructureBirthInfo` satisfies the map's no-throw value constraints. The staged token cannot be
   committed into another map or after intervening map mutation, and move/copy operations invalidate
   old generations.

1. **Dense `byRef` preparation preserves contents on allocation failure.** Dense growth allocates
   both replacement arrays into local PMR vectors, copies old values/presence, then swaps only after
   both allocations succeed. A failed allocation therefore leaves the previous logical contents and
   storage intact. Sparse keys continue through the separate `FlatU32Map`; the 0 key is refused
   before subtracting one. Existing mixed dense/overflow lookup and copy/equality logic use the
   public `find` semantics.

1. **Manifold preflight order is correct for current insertion sites.** For cell creation, the birth
   entry and op-index mapping are both prepared before publishing the birth, pushing the cell, and
   publishing `byRef`. For chain operations, the `byRef` token is prepared before the operation's
   semantic mutation. A failed second preparation can leave capacity grown in the first map, which
   the plan permits, but cannot leave an entry published. Store rebuild paths throw
   `ManifoldCapacityUnavailable` on the typed resource refusal instead of returning a partial fold.
   `advance` restores its prior path-local hint on that refusal; successful and semantic-refusal
   steps keep the path count updated.

1. **Copy/move resource handling is consistent.** Copies of the lookup maps allocate using the
   destination resource (copy construction intentionally inherits the source resource); copy
   assignment builds replacement storage before swapping/publishing. Move construction transfers the
   resource with the PMR containers, invalidates tokens on both objects, and leaves the moved-from
   object reusable. The tests cover copying to a destination resource after the source resource
   dies, failure-preserving copy assignment, move token invalidation, and reuse of the moved-from
   maps.

1. **Path-local hint does not page dense storage from the global spool.** Store supplies the
   selected ancestry count, and `ManifoldOpIndexMap` consults a 2x checked bound with a
   4096-operation floor. The vector grows only to the highest eligible operation key actually
   inserted, rather than resizing directly to the hint. Sparse high global operation IDs go to
   overflow. `advance` increments one path step and restores the hint on resource refusal. This
   avoids the earlier global-op-count dense allocation concern.

## Remaining verification / memory notes

- Tests in the snapshot exercise every PMR allocation ordinal for a cold MakeCell preparation and
  verify unchanged logical fold/refusal state followed by successful retry. Generic unordered
  preparation tests cover bucket, staging bucket, and node allocation failures. Other
  chain-operation tests inject a lookup failure and check state preservation. One useful additional
  case would be to fail a **later dense reallocation after an existing dense mapping is live**; the
  current code's local-build-then-swap makes it safe by inspection, but the integration suite mostly
  covers empty-map growth plus sparse overflow refusal.
- The unordered table now doubles its requested bound after a nonzero reservation. Actual bucket
  counts are implementation-specific and can be slightly above the requested target; record actual
  bucket count and capacity/live ratio in the measurement probe. The dense byRef vector also doubles
  geometrically and remains allocated after folding; report actual value capacity, presence bytes,
  and old-plus-new peak during growth. The C probe has suitable per-resource capacity/peak counters,
  but those measurements and the C runtime comparison are still pending.
- This source includes no workload weights or evidence that C is the default winner. The candidate
  should proceed only through the planned structural, failure-injection, ThinLTO, then source-bound
  PGO and held-out workload gates. Use the archived P3 PGO binary as the unchanged baseline only
  after verifying its source/build/profile/library identity; train a fresh profile for C.

No builds, tests, benchmarks, or source edits were performed for this review.
