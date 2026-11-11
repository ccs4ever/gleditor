# Round 7 critique: Xanadu/ZigZag Purist (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 5 MINOR.

This is the orchestrator-filed finding record from the returned critique. The critic re-verified the
live tree after the rate-limit retry; source was changing concurrently. Unlike the codebase critic's
earlier reading, this reading found `kLastContentDecodableOpcode`, `kLastOpcodeKind`, and
`isRegistrableOpcode` at `vortex_vm.hpp:145-157`, the `d.lineage` skip at `arena_manifold.cpp:1780`,
and all 21 module creations at `vortex_stdlib.cpp:1508-1530`.

## BLOCKING

None.

Rank-filed dimensions, private holder ranks, unscoped key-record containment, bootstrap-suppressed
construction, and bits-only scroll-size restatements are sound. The scalar restatement precedent is
`vagent/lineage.cpp:407-410`.

## MAJOR

### 1. The public-accessor-only gate cannot enumerate private worker contexts

**Problem:** The proposed aggregate includes `extraContexts_`, but neither `arena()` nor
`primaryContext()` exposes them.

**Evidence:** `extraContexts_` is private at `vortex_host.hpp:350`; `workerContextCount()` at
`:115-117` returns only a count. Real worker arenas are created by `solveLogicBatchParallel`
(`vortex_host.cpp:1025-1028`) and can carry marks.

**Fix:** Propose host-side `VortexHost::liveMarks()` summing the primary and all extra arenas
internally. Include the new declaration in P30 rather than asserting no new API.

### 2. The floorBaseline has no representation and hydration's seed is evictable

**Problem:** Synthetic and real floors have the same representation; a permanently subtracted one is
not valid.

**Evidence:** `hydratePrefix` seeds a floor and count at `arena_manifold.cpp:2186-2192`.
`evictOldestMark` at `:2196-2203` can remove that seed. The caller,
`VortexVM::stealNearRootChoicePoint` (`vortex_vm.cpp:397-409`), runs on exactly the OR-parallel
arenas that hydrate (`or_parallel.cpp:345-349`).

**Fix:** Either track a `baselineMarks_` member and update it when eviction removes the seed, or
make never-hydrated host arenas the explicit v1 contract. In the latter case the host aggregate
simply counts `outstandingMarks()`; floor-aware arithmetic is unnecessary. `makeWorkerContext`
creates fresh, unhydrated arenas (`vortex_host.cpp:107-115`).

## MINOR

1. `overlay_` is partially restored by `release()` via `shadowOrder_`
   (`arena_manifold.cpp:1663-1669`); `spaceStoreRefsTail_` is also resynchronized (`:1678-1693`).
   Distinguish these from untrailed `arenaDims_`, `boundDimensions_`, and `storesRankTail_`.
1. Refresh relocation-reader citations by symbol: this reading found executor probes at
   `vagent/executor.cpp:564,:634,:2924` and bare marker mint at `mesh_evaluator.cpp:743`, not the
   revision-6 ranges.
1. `navigateCellVQL` does not exist; use `translateVQLAndAttachToFocus`
   (`zigzag_visualizer.cpp:3339-3391`). `compileVQL`'s `:3396` ensure is null-guarded.
1. `findOpHandle`, `versionAnnotation`, and `aliases` scan `Manifold::slots`, not `ArenaManifold`.
   The accepted conflation risk applies after later promotion and folding, not directly to grafted
   arena scans. Registration does not consult them.
1. `~VortexVM` clears only under `guardInstalled_`, not unconditionally. The real last-installer
   hazard remains: an earlier installer's destructor can clear a later guard.

## Purity Check

Runtime bootstrap flags, transient feedback, mark counts, and any arena-internal baseline accounting
are appropriate C++ runtime state, not missing manifold records. Rank-filing dimensions improves
manifold grounding; `std::expected` matches house rules.

VERDICT: NOT CONVERGED
