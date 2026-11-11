# Round 9 critique: Programming Language/Ergonomics Expert

Target: revision 8 of `design/vortex-stdlib-prebuilt-store.md`. Verified absolute root:
`/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan`.
Tally: 0 BLOCKING, 2 MAJOR, 2 MINOR.

Condensed finding record. R8 startup reader/latch, honest resumption, status split, presence
ordering, reload/projection ordering, pane reorder, and signatures are resolved except the following
residual wording.

## MAJOR

### 1. Swap refusal still says it records on the instance

**Problem:** The bindManifold paragraph contradicts initialization-only stdlib status.

**Fix:** State replacement refusal returns only via expected and caller feedback; it does not write
any stdlib loader status.

### 2. Compilation gate does not name its arena domain

**Problem:** Host aggregation would over-refuse: the primary compiler cannot be rolled back by a
mark on a separate worker arena.

**Evidence:** Compiler binds primary core/VM (`vortex_host.cpp:100-103`); every worker owns its own
arena/VM (`worker_context.hpp:59-73`).

**Fix:** Check primary arena `liveMarks()` only for non-replacing compileVQL. Test primary-mark
refusal and worker-only-mark compilation success separately.

## MINOR

1. When an engine exists, attached host must match its manifold/store; independent lifetimes alone
   do not establish cell-space coherence.
1. State whether successful C++ fallback records `lastRefusal` and opens startup UI.

VERDICT: NOT CONVERGED
