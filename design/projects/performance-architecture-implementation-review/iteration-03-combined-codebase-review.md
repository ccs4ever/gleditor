# M3 C arm codebase review

Scope: static review of `/tmp/perf-m3-hybrid-unordered` against the archived M3 source variants. A
is dense `byRef` plus flat births (`c427`); B is both maps using reserved standard unordered maps
(`b777`); C is dense `byRef` plus unordered births. Branch-local count hints are common M3 behavior
across A/B/C, not part of the accepted P3 baseline. No source edits or builds were made.

## Verdict

No C-arm-specific correctness blocker found in the requested call paths. Relative to A, the isolated
change is narrowly scoped: it retains A’s dense `byRef` map and changes `structureBirths_` from A’s
flat map to `PreparedU32UnorderedMap<StructureBirthInfo>`. Store fold entry points, typed capacity
failure propagation, and common M3 branch-local count behavior remain coherent.

## Findings

- **Typed capacity errors:** both Store full-fold paths check
  `FoldRefusal::LookupCapacityUnavailable` and throw `ManifoldCapacityUnavailable` before returning
  a folded manifold. `Manifold::advanceOrRefold` does likewise; direct `advance` returns
  `AdvanceError::Refused` with the refusal intact. The new birth-map prepare errors are translated
  to the typed refusal; duplicate birth remains `DuplicateCell`. I found no call site converting
  lookup exhaustion into a partial successful fold.
- **Both Store paths:** indexed rebuild uses `walkAncestralWithCount`, sets the selected path count
  before visitor folding, and checks every fold result for lookup exhaustion. Fallback rebuild
  computes count over the requested version path, sets the hint, then folds the same path and
  performs the same refusal check. The fallback does an extra path scan; this is bounded policy
  overhead, not a semantic divergence.
- **Branch hints:** the indexed walker calls the count callback only after collecting a complete
  ancestry path, so its hint is branch-local and not a store-wide operation-index maximum. The
  fallback counts only the selected version path. Incremental `advance` adds one to the previous
  ancestry count and restores that hint if lookup preparation fails. In C, the hint still drives
  dense `byRef` sizing; the birth map grows independently, so it is not incorrectly treated as a
  birth count.
- **Structure-chain insertion preflight:** each new cell prepares its birth entry and `byRef` entry
  before publishing the birth or cell slot. If the second preparation fails, the staged first token
  is destroyed and the logical fold position, birth lookup, and slot set remain unchanged.
  Slice/Xanadoc births prepare their single birth-map entry before commit. Existing chain/edit
  operations prepare any new `byRef` index before changing their logical target and commit the
  staged mapping afterward. The post-preflight commit failure is treated as an invariant violation
  (`terminate`), consistent with the map’s reserved same-resource node publication contract.
- **Map and Manifold value semantics:** the map copy constructor keeps the source PMR resource; copy
  assignment builds a replacement using the destination resource before swapping, so allocation
  failure preserves the destination map. Move operations invalidate outstanding prepared tokens and
  the tests cover moved-from reuse. The Manifold copy/move operations propagate both lookup maps
  alongside other fold state; equivalence compares birth contents independent of unordered iteration
  order. `structureBirths_` iteration sites collect/sort keys where deterministic ordering is
  needed.
- **Resource ownership:** Store supplies the PMR resource to each newly rebuilt Manifold, and
  Manifold constructs both lookup maps with that resource. As with the existing `byRef` map, callers
  must keep an injected resource alive for the Manifold’s lifetime. No new config/store
  serialization path was added; these maps remain derived fold state.
- **Test evidence:** the isolated draft contains direct failure-injection coverage for birth-map
  reservation/staging failures, retry after refusal, move/token invalidation, copy assignment into
  the destination resource, and Store indexed/fallback refusal. The syntax logs exist in the
  isolated directory, but I did not run tests and do not claim a test pass.

## Non-blocking observations

- The path hint reserves/chooses dense sizing for `byRef`; C’s birth map is not path-pre-reserved.
  This is the intended C split, but means birth-table cold-growth behavior is distinct from B’s
  path-capacity preflight and should remain represented in its measurements.
- `walkAncestralWithCount`’s boolean completion result is not checked by the Store caller. This
  pattern is also present in the prior `walkAncestral` use and is unrelated to the C map
  substitution, but it means malformed/unreadable ancestry handling is not made stricter by this
  work.
- The commit-after-preflight guarantee relies on equal PMR resources for the staged and destination
  unordered maps and enough destination bucket capacity. The helper uses the same resource for both
  maps and reserves before staging; keep its allocation-free commit tests coupled to this contract.

No accidental configuration regeneration or production edits were observed; this review inspected
the isolated C tree and archived source only.
