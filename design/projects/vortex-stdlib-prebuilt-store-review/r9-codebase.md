# Round 9 critique: Codebase Expert

Target: revision 8 of `design/vortex-stdlib-prebuilt-store.md`. Verified absolute root:
`/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan`.
Tally: 0 BLOCKING, 1 MAJOR, 1 MINOR.

Condensed finding record. Correct 21-module baseline and proposed APIs verified. R8 status split,
exact signatures, hydration caller/API accounting are resolved.

## MAJOR

### 1. VQL reset omits its MultiStoreCoordinator state

**Problem:** Clearing engine maps leaves the coordinator's old arena refs alive.

**Evidence:** Host VQL constructs an owned coordinator over the primary core
(`vql/vql_engine.cpp:121-123`). `MultiStoreCoordinator::initDimensions` caches home and four
dimensions (`multi_store.cpp:40-46`). It also retains `coordinatorHome_`, `storesTail_`, cached
dimensions, and `stores_` (`multi_store.hpp:175-184`); each StoreInfo includes home/store refs
(`:49-57`).

**Fix:** Resync or reconstruct the owned coordinator after core resync, explicitly scope borrowed
coordinators to their owner, account for the touched files, and test home/store/dimension/clone
behavior after swap, not merely missing env entries.

## MINOR

The bindManifold paragraph still says refusal "records on the instance", conflicting with
initialization-only `lastRefusal`. State expected/HUD only, with no status mutation.

VERDICT: NOT CONVERGED
