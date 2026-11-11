# Round 7 critique: Technical Writer (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 4 MAJOR, 9 MINOR.

This is the orchestrator-filed finding record from the returned critique, whose heading incorrectly
called this Round 1.

## BLOCKING

### 1. The gate fires after the teardown it claims to prevent

**Problem:** Refusal inside `ensureVortexHost` cannot retain already replaced document state.

**Evidence:** `bindXanaduStore` destroys the engine, repoints the store, and rebuilds the view
(`zigzag_visualizer.cpp:364-391`) before ensuring at `:392`. `adoptDocument` replaces the owned
store at `:244-246` and engine at `:267`, then ensures at `:306`. A retained host's arena base
points into the freed engine. `reloadStoreVersion` inherits the defect via its bind at `:416`.

**Fix:** Add pre-mutation checks at the top of adoption/bind operations and retain the replace-arm
command check. Test actual pointer identity retention.

## MAJOR

### 1. attachVortexHost bypasses the claimed funnel

**Problem:** Direct assignment is not protected by an ensure-only check.

**Evidence:** `zigzag_visualizer.cpp:2844-2848` assigns `vortex_host_` directly.

**Fix:** Specify its own outgoing-host check before assignment.

### 2. The expected-to-feedback writer is missing

**Problem:** `bindZigzag` cannot write private feedback fields.

**Evidence:** Fields are private at `zigzag_visualizer.hpp:791-792`, with getters only at
`:358-362`. `xuzz_app.cpp:2393-2446` is unrelated pick handling.

**Fix:** Have the refusing visualizer member write feedback internally or propose a setter. Compose
`bindZigzag`'s lens-surface branch explicitly.

### 3. Public primary accessors cannot aggregate private worker arenas

**Problem:** The stated aggregate is not implementable through existing accessors.

**Evidence:** `extraContexts_` is private (`vortex_host.hpp:350`); `workerContextCount()` is only a
scalar; `primaryContext()` exposes only primary state.

**Fix:** Propose host-side `liveMarks()` or a context enumeration API.

### 4. call entry-throw contract contradicts itself

**Problem:** Adjacent sentences both prohibit and require a `call()` entry throw.

**Evidence:** `call(path)` resolves and delegates (`vortex_stdlib.cpp:2032-2039`); the cell-ref
overload is a distinct entry. Status is solver-goal-only.

**Fix:** Name each overload and specify resolve failure versus cell-ref use-point behavior without
contradictory generic `call` wording.

## MINOR

1. Correct the six unguarded command sites: translation is `translateVQLAndAttachToFocus`;
   `compileVQL` is separately null-guarded.
1. Commands return bool, ScriptResult, vector, or CompilationResult, not expected. Specify their
   actual refusal channel and converted wrapper/constructor callers.
1. `overlay_` has `shadowOrder_`-tracked rollback; do not list it as wholly untrailed.
1. Refresh marker and worker-context citations to `mesh_evaluator.cpp:743,:936` and executor probes
   `:564,:634,:2924` for this source reading.
1. Cite `ArenaManifold::link` (`arena_manifold.cpp:1344-1378`) and `rankContains` (`:1381-1421`) for
   graft-time behavior, rather than only the base-class twins.
1. Describe install-conditional guard clearing accurately; `guardInstalled_` is historical, not
   current ownership. Reset `oracleCursor_` on swap.
1. Include hydration's seed tail in the prefix citation (`arena_manifold.cpp:2126-2194`).
1. Refresh lazy accessor ranges, call the vquery VM `vm` at `main.cpp:487`, and use full
   `packaging/windows/build-msys2.sh` and `packaging/smoke-test.sh` paths.
1. Attached hosts are replaced by the next engine-bearing ensure, including commands, not just the
   next document bind.

VERDICT: NOT CONVERGED
