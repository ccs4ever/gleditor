# Round 9 Disposition

Target: revision 8 -> revision 9 of `design/vortex-stdlib-prebuilt-store.md`. All six verified the
correct absolute worktree. Tally: 0 BLOCKING, 3 MAJOR, 14 MINOR. Writer, Vortex, Architect, and
ZigZag converged; Codebase and Ergonomics did not.

## Accepted Major Findings

1. **Coordinator cache lifetime (codebase M1):** VQL's map reset does not reset its coordinator's
   home, dimension, tail, or mounted-store refs. Revision 9 adds chainable
   `MultiStoreCoordinator::onArenaSwapped()` and its header/cpp to P30. It clears mounts/tail and
   calls existing `initDimensions()` against the resynced core. Host-owned VQL invokes it in place;
   borrowed coordinators remain their external owner's responsibility. Replacing `ownedCoordinator_`
   is not used because the engine's `coordinator_` reference would dangle. Swap tests cover anchors,
   named stores, dimensions, and clone dereference.
1. **Refusal status contradiction (ergo M1, codebase minor):** Remove residual "records on the
   instance" wording from bindManifold. Pre-swap liveness errors travel only via returned
   expected/caller feedback, never loader status.
1. **Compilation mark domain (ergo M2):** Non-replacing compileVQL gates on the primary arena only.
   A separate worker's marks cannot invalidate primary compiler cells. Tests require primary-mark
   refusal and worker-only success. ZigZag minor 1's aggregate suggestion is refused for this
   precise reason; host replacement still correctly checks all arenas.

## Minor Dispositions

- Writer: split invariant/hydration/compiler paragraphs; explain the historical shared refusal-type
  name. Citation-only helper range polish is non-blocking.
- Vortex: pin swap service order inside initHostServices: genesis, VM clearing, both stdlib
  ensure/rebinds, engine/coordinator cache resets, then actions/workers. Ownership-field versus
  cache-field citation drift is non-blocking.
- Architect: specify failed hydration fails the spark rather than running it unhydrated; distinguish
  adjusted arena uint32 count from host size_t aggregate. Complete compileVQL range is already cited
  at the behavior contract.
- Ergonomics: an attached host must match an existing engine's manifold/store; independent host
  lifetimes are allowed only with no engine. Successful C++ fallback is not failed initialization:
  lastRefusal stays empty, source is cpp, rejected-image cause goes to warning logs.
- ZigZag: add the sole-seed stale-Mark assertion test. Existing worker genesis before hydration
  relies on snapshot-compatible layout; changing that order is outside the signature/counter fix and
  is not required for this migration.

## Next Gate

Revision 9 requires focused re-review, particularly coordinator reference stability, primary-only
compilation gating, and loader-status consistency. No implementation or final convergence is claimed
yet.
