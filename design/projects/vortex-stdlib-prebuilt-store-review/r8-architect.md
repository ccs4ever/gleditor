# Round 8 critique: System Architect

Target: `design/vortex-stdlib-prebuilt-store.md`, revision 7. Absolute worktree verified:
`/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan`.
Tally: 0 BLOCKING, 1 MAJOR, 5 MINOR.

This is the orchestrator-filed condensed finding record. All R7 findings are resolved; the new
lifetime registry and allocator accounting are appropriately layered and priced.

## MAJOR

### 1. Hydration must reject live marks before replacing arena state

**Problem:** The synthetic-floor flag assumes its seed is oldest, but no checked precondition
prohibits hydration while real Marks still exist. Hydration stomps the count and replaces
mark-covered state.

**Evidence:** `hydratePrefix` unconditionally seeds count one (`arena_manifold.cpp:2168-2194`);
later eviction removes the oldest floor. Today's workers are fresh (`or_parallel.cpp:345-349`), but
the API is public.

**Fix:** State and enforce a no-outstanding-marks precondition before any hydration mutation, and
tie flag clearing to removal of the seed. The original report's suggestion that old floors remain in
the vector needs correction: the method actually clears `floors_`; live external Mark objects would
nevertheless become stale.

## MINOR

1. Resolve `makeWorkerContext`'s explicit bootstrap alternative: remove it because constructor-time
   ensure already covers the context.
1. Pin expected-result handling for internal tail ensures and null-host callers.
1. Specify incoming attached-host arena/base lifetime requirements.
1. `reloadStoreVersion` must precheck before `store_->setCurrentVersions` at
   `zigzag_visualizer.cpp:415`, not merely before its nested bind.
1. Distinguish constructor fallback adoption from direct lens-surface callers.

VERDICT: CONVERGED

## Orchestrator Gate Note

The returned verdict conflicts with the nonzero MAJOR tally. Under the workflow's zero-MAJOR gate
this report is treated as NOT CONVERGED until disposition/re-review.
