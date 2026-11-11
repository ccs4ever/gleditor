# Round 6 critique: System Architect (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 1 BLOCKING, 4 MAJOR, 5 MINOR.

Verified accurate this round, so findings can stay narrow:

- **Seam placement (§5.4/P31).** `session` is built at `apps/xuzz/xuzz_app.cpp:977–978`; `systemStore()` is a session-level accessor (`session.cpp:1744–1754`) over a lazily-created index (`session.cpp:1639–1741`) needing nothing built at/after :1588. `opts.headless` returns at `xuzz_app.cpp:1055–1064`. The first stdlib-bearing context does fire inside `make_shared<ZigzagVisualizer>("")` at :1588 (`zigzag_visualizer.cpp:74 → :215–308 → :306 → :2857–2866 → worker_context.hpp:59–62`). Earlier Vortex machinery is all stdlib-free and never folds: `xuzz_app.cpp:542,889,907`, `lens_file_picker.hpp:28,197`.
- **CAS policy primitive** (`atomic<shared_ptr<const StdlibPolicy>>` + copy-modify-`compare_exchange_weak`) is the correct C++20 pattern. **`shared_ptr<State>`** snapshot-under-mutex correctly protects in-flight grafts across `resetStdlibImageForTesting` swaps.
- **`localImage`** (`localImage.value_or(mode != Mode::Cpp)`, env overrides, base-refusal suppression) is internally coherent (doc :733–740).
- **`bindManifold`** has zero in-tree C++ callers; `attachVortexHost` (:2845–2848) likewise; `outstandingMarks()` is public (`arena_manifold.hpp:750–752`); every choice point takes `core_.arena().mark()` (`vortex_vm.cpp:317–326`). `keyRecord` minus `d.owner` is sound for the image record (genesis 20 dims `vortex_core.cpp:46–73` + `d.mesh.relocation` + `d.params` = ~22); the vagent `varsRecord` (`records.hpp:97–120`) is a separate convention that legitimately keeps `d.owner`. `installWriteGuard` is last-wins (`vortex_vm.cpp:84–95`) and `ownedVm_` is the last installer (`vortex_host.cpp:100–105`, `vpl_engine.cpp:77–86`).

## BLOCKING

### 1. (§5.2.5, line 596) The `ChoiceStackLive` gate misses every shipped host-drop path except `bindXanaduStore`

**Problem:** The doc places the check "at `bindXanaduStore`/`attachVortexHost` entry" (`design/vortex-stdlib-prebuilt-store.md:596–598`, `:1164`, `:777`). But `vortex_host_` is destroyed by `ensureVortexHost`'s **unconditional** replace (`zigzag_visualizer.cpp:2857–2866` — `vortex_host_ = make_shared<VortexHost>(...)` whenever `engine_` is non-null), which is invoked from at least six sites the gate does not cover:

- `adoptDocument` :306 — the document-switch funnel, driven by `populateFallbackStructure` :212, `adoptXanaduStore` :354–359, `adoptXanaduDocs` :424–427, and in xuzz by `xuzz_app.cpp:629`, `:836`, and the render-thread lens-picker sink `:3006–3008`. This is *the same operation class* the gate's recovery contract describes ("the old host, engine, and bound document are all retained", :600–601).
- `translateVQLAndAttachToFocus` :3345, the :3594 command entry, `executeVQLScript` :3618, `executeVPL` :3640, `executeLogicQuery` :3661, `defineMacro` :3690 — each calls `ensureVortexHost()` unconditionally, unlike the correctly null-guarded `vortexHost()` :2851–2853, `dispatchAction` :2870–2871, `compileVQL` :3395–3397. So every command-bar VQL/VPL/logic execution destroys the current host wholesale — today a full re-bootstrap; under this design a cheap refold but the *same silent destruction* of `choiceStack_`/produce cursors the doc itself calls "worse than a refusal" (:594–595). An un-returned produce cursor is an open choice point with a live mark (:1167); the next `vql(...)` keystroke kills it unguarded.

**Evidence:** `zigzag_visualizer.cpp:2857–2866` (unconditional replace), callers at :306,:392,:3345,:3594,:3618,:3640,:3661,:3690; `ensureVortexHost()` grep finds ~11 sites; null-guarded accessors at :2851–2853,:2870–2871,:3395–3397.

**Fix:** The gate covers 2 of ~9 replacement sites — the R5-1 defect recurring one level down. The funnel is `ensureVortexHost`: either put the `outstandingMarks()` check at its replace arm (with refusal routing per caller), or make `ensureVortexHost` ensure-if-null and route `adoptDocument`/`bindXanaduStore` through a gated `replaceVortexHost`. The spec must say which; it cannot stay silent on `adoptDocument`.

## MAJOR

### 2. (§6.4/P32, line 1013) §6.4/P32's dev-only scratch cannot mint dev modules as specified

**Problem:** A scratch `ArenaManifold`+`VortexCore`+`VortexVM` compiles fine standalone (`vortex_core.cpp:16` only runs `initGenesis`; `vortex_vm.cpp:74` only installs the guard; precedent `vlace` `driver.cpp:50–53,92–95`), and `promote`/`Store::save` need no stdlib services — but the dev modules can't be built there: every `build*Module` is a private `VortexStdLib` member (`vortex_stdlib.hpp:527,547–567`), and the sole `VortexStdLib` ctor unconditionally `bootstrap()`s (`vortex_stdlib.cpp:1484–1487`) — which under any registered `Mode` either grafts the base image or mints the full ~4,300-cell base, the exact R5-4 defect the clause exists to avoid.

**Evidence:** `vortex_stdlib.hpp:527,547–567` (private `build*Module` decls), `vortex_stdlib.cpp:1484–1487` (unconditional `bootstrap()` in ctor).

**Fix:** P32 must register one more clause: a bootstrap-suppressed construction (a `Mode` arm or ctor tag) plus a public module-builder entry, or an explicit bootstrap-and-subtract generator on a throwaway context.

### 3. (§5.4, line 788) `vortex_stdlib_status` is unreachable in the refusal state it exists to diagnose

**Problem:** The doc specifies the throw "fires only at public use-points — `call()`/`solve()`/routine entries" (:788–789) and that the builtin's "`solveQueryHelper` dispatch arm answers **before** the use-point throw" (:800–801). Those two sentences are call-order-inverted: `solve()` dispatches `solveQueryHelper` at `vortex_stdlib.cpp:4926`, so an entry throw at `solve()` precedes *every* dispatch arm inside the helper — including `vortex_stdlib_status`. For the stated behavior to be implementable, the refusal throw must live inside `solveQueryHelper`'s per-`goalFunctor` dispatch after the status arm (with `call()`'s throw at `vortex_stdlib.cpp:2032/2041` entry), or `solve()`/`solveQuery` must hoist a status-goal pre-check ahead of the entry throw.

**Evidence:** `vortex_stdlib.cpp:4926` (solve→solveQueryHelper dispatch), `:788–789` vs `:800–801` (the inverted claims), `vortex_stdlib.cpp:2032–2039` (`call()` currently has no throw — a proposed change, correctly framed as such).

**Fix:** Pick one: (a) refusal throw inside `solveQueryHelper`'s per-functor dispatch *after* the status arm, with `call()`'s entry throw kept; or (b) `solve()` hoists a status-goal pre-check ahead of its own entry throw. Write it explicitly — the current text reproduces the R5-5 shape (diagnostic unreachable exactly when needed).

### 4. (§11, line 1374) §11's end-to-end gate cannot exercise what it asserts

**Problem:** "`xuzz --headless` boots on the image with `GLEDITOR_STDLIB_SOURCE=image`, `bundle-stdlib` shows the module rank, keymap actions dispatch" (:1374–1376) — but `opts.headless` saves the session and `return 0`s at `xuzz_app.cpp:1055–1064`, *before* the renderer (:1067) and before `ZigzagVisualizer` (:1588) exist. No `VortexHost`, no `ensureCells` fold, no `bundle-stdlib` dispatch runs; the gate passes vacuously.

**Evidence:** `xuzz_app.cpp:1055–1064` (headless early-return), `:1067` (renderer), `:1588` (visualizer).

**Fix:** Use the Makefile's `SDL_VIDEODRIVER=offscreen` windowed boot, or add a scripted command path below :1588 and name it.

### 5. (§5.2.5, line 645) `~VortexVM` unconditionally clears the core's write guard — the "last-wins → `ownedVm_`" note is steady-state false

**Problem:** `~VortexVM` runs `core_.setWriteGuard(nullptr)` (`vortex_vm.cpp:76–82`, funnel consult `vortex_core.cpp:218`, slot `vortex_core.hpp:340`) without checking the installed lambda is its own. Transient VMs sharing a long-lived core die constantly: `vprolog::Compiler`'s `vm_` member (`vprolog/compiler.hpp:76`) is constructed per `solveLogic` (`vortex_host.cpp:935`) and per worker `runOne` (`vortex_host.cpp:1001`); `vquery` builds `VortexVM localVm{coordinator.core()}` (`vquery/main.cpp:488`). After the first solve, `primary_.core`'s funnel is *unguarded*, not `ownedVm_`-guarded — so the grafted-image story's load-bearing claim (a registered op refuses content-writes; `opcodeMap_` can't be desynced, §5.2.5 :616–619) lapses mid-session.

**Evidence:** `vortex_vm.cpp:76–82` (unconditional `setWriteGuard(nullptr)` in `~VortexVM`), `vortex_core.cpp:218` (funnel consult), `vortex_core.hpp:340` (the slot), `vprolog/compiler.hpp:76` + `vortex_host.cpp:935,1001` + `vquery/main.cpp:488` (transient-VM constructors).

**Fix:** P30's resync should name re-installing the guard, or `~VortexVM` should clear only its own; at minimum this is a named-boundary line beside :645–647.

## MINOR

### 6. (§5.2.5, line 597) `vortex_host_->primary_.arena.outstandingMarks()` names a private member

`private:` at `vortex_host.hpp:337`, `primary_` at :346. The public accessors reach the same object: `arena()` (:82–85) or `primaryContext()` (:101–104); the r5-disposition itself wrote `arena()`. The doc's `vortex_host_ &&` null guard is already correct.

### 7. (§5.4, line 727) "every level is taken-as-given (the `assetDir()` convention, `paths.cpp:41–48`)" mis-cites

`assetDir()` takes *only* `GLEDITOR_ASSET_DIR` as given (`paths.cpp:45–48`); the portable/installed/`GLEDITOR_DATADIR` levels are `isDirectory`-guarded fall-throughs (`paths.cpp:50–75`). The doc's stricter chain is self-consistent and arguably better (a bad `DATADIR` naming itself in `ImageMissing`), but drop or reword the parenthetical before an implementer reproduces fall-through semantics.

### 8. (§3.1, line 285) `keyRecord`'s shape already exists file-local

`varsRecord` at `vagent/fragment.cpp:39–49` (identical three-line mint+link+`rankTail` append, minus `d.owner`). Name it as the shared helper rather than describing the shape abstractly. Also `varsRecord` mints `d.owner` unconditionally even on reads (`vagent/records.hpp:101`) — the ~21–22-dim claim holds for the scratch image arena, but a *live* registry gains `d.owner` on any vagent record call; one clause prevents misreading.

### 9. (§5.4, line 760) The consumer enumeration omits `vlace`/`vlacec`

`vlace` drives `runMeshCommand` → per-job `WorkerContext`s (`vlace/main.cpp:108` → `mesh_actor.cpp:191,325`, `spool_mesh.cpp:113`) which fold like every consumer, while `vlace::Driver`/`vlacec` run `VortexCore`+`VortexVM` stdlib-free (`driver.cpp:50–53,92–95`) — simultaneously a missing consumer line and a citable P32 precedent. Same class worth one confirming line: bare-`VortexCore` helpers `xuzz_app.cpp:542,889,907` and `LensFilePicker` (`lens_file_picker.hpp:28,197`) never fold, so "once per process" is per-`VortexStdLib`, not per-`VortexCore`.

### 10. (§5.2.5, line 717) "the ctor-path host is discarded by `bindXanaduStore`'s `engine_` replacement at :370"

The manifold the host wraps dies at `engine_.reset()` (:364), not the `make_unique` (:370) — `vortex_host_` holds `&engine_->manifold()` on a destroyed engine from :364 until `ensureVortexHost` replaces it at :392. No deref occurs between them today, but the death line is :364, and §5.2.5a's gate reasoning keys on exactly this ordering.

VERDICT: NOT CONVERGED
