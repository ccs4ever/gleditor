# Round 8 critique: Xanadu/ZigZag Purist

Target: `design/vortex-stdlib-prebuilt-store.md`, revision 7. Absolute worktree verified:
`/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan`.
Tally: 0 BLOCKING, 1 MAJOR, 3 MINOR.

This is the orchestrator-filed condensed finding record. All R7 findings are resolved: aggregation,
seed accounting, rollback exclusions, owner-keyed guards, folded-scan scope, command skipping, and
selected-module generation.

## MAJOR

### 1. The categorical mark invariant overclaims coverage of runtime registration

**Problem:** `mintOpcode` writes the untrailed map without a mark check; non-replacing `compileVQL`
can compile on a marks-live host and bypass the ensure gate.

**Evidence:** `vortex_vm.cpp:157-159` registers unconditionally; `vql/compiler.cpp:244` mints
through it; `zigzag_visualizer.cpp:3395-3397` only ensures when the host is null. Releasing marks
can truncate cells but retain map entries.

**Fix:** Scope the invariant to image ensure/graft/rebind, explicitly identifying the pre-existing
runtime-registration exposure, or add a compilation/mint gate with complete proposal accounting.

## MINOR

1. Implicit arena moves copy scalar count/flag but move the floor vector. Specify moved-from
   accessor restrictions or reset moved-from mark state explicitly.
1. Arena has no clear/reset member. Name actual state replacement paths: hydrate clears/reseeds
   floors, move assignment replaces them, destruction ends them.
1. Remove obsolete `guardInstalled_` and its historical single-slot comment.

VERDICT: NOT CONVERGED
