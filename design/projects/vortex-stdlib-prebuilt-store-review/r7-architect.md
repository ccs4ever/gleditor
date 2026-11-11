# Round 7 critique: System Architect (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 5 MAJOR, 8 MINOR.

This is the orchestrator-filed finding record from the returned critique.

## BLOCKING

### 1. Adoption destroys retained state before the replacement gate

**Problem:** The replace-arm check cannot promise retention of state that adoption already
destroyed. Proceeding on a retained host after refusal can use a freed base.

**Evidence:** The engine owns `Manifold manifold_` (`unified_transclusion_engine.hpp:441`, accessor
`:187`); the arena base is a raw pointer. `ensureVortexHost` binds `&engine_->manifold()`
(`zigzag_visualizer.cpp:2859`). `bindXanaduStore` resets the engine at `:364` and rewrites
store/focus/view state at `:366-391`, then ensures at `:392`. `adoptDocument` rewrites state at
`:219-305`, including replacing the engine at `:267`, then ensures at `:306`. The design's
retained-copy caveat does not excuse retaining the visualizer's own dangling member.

**Fix:** Gate at entry before any mutation in `bindXanaduStore`, `adoptDocument`, and direct
`attachVortexHost`, retaining the replace-arm command gate. Explain retained-host command execution
only under the unchanged engine/store invariant.

## MAJOR

### 1. Private worker contexts require a host-level aggregate API

**Problem:** Public primary accessors cannot enumerate `extraContexts_`.

**Evidence:** `vortex_host.hpp:350` is private; public `workerContextCount` returns only a scalar.
Real execution contexts are created at `vortex_host.cpp:675,:1028`.

**Fix:** Add a public host mark-count method implemented inside the host; include its declaration
and implementation in P30.

### 2. bindZigzag cannot write private feedback fields

**Problem:** The xuzz lambda is specified to assign visualizer-private fields.

**Evidence:** `commandBarFeedback_` and its error flag are private
(`zigzag_visualizer.hpp:791-792`); `:358-361` exposes getters only. HUD rendering at
`zigzag_visualizer.cpp:2393-2446` is a read, not a writer.

**Fix:** Make the refusing visualizer member write feedback before returning `unexpected`, or
propose a public setter. Name the writer explicitly.

### 3. Command return channels and converted callers are unspecified

**Problem:** Commands do not have the claimed "own expected result".

**Evidence:** Existing results are bool (`navigateVQL`, `defineMacro`, translation), `ScriptResult`
(VQL/VPL), vector (logic), and `CompilationResult` (`compileVQL`). `reloadStoreVersion` calls
`bindXanaduStore` but returns void (`zigzag_visualizer.cpp:409-422`). Boot/adoption/bind lambdas and
visualizer tests also consume the converted APIs. P30's location list omits these changes.

**Fix:** Keep command result types and surface replacement refusal through the HUD, or enumerate a
full expected-signature migration. Include every converted caller, wrapper, header, and affected
test in the implementation inventory.

### 4. The hydration baseline is neither represented nor permanent

**Problem:** `floors_.size()` is parallel to the raw mark count, not a synthetic-floor count.
Eviction can remove the hydration seed.

**Evidence:** `mark`, `discard`, `release`, and `evictOldestMark` push/pop the floor stack and count
together (`arena_manifold.cpp:1589-1602,:1695-1697,:2196-2203`). `hydratePrefix` seeds an otherwise
indistinguishable floor at `:2186-2192`.

**Fix:** Pin never-hydrated host arenas with enforcement, or propose explicit baseline state updated
by all floor-lifetime operations under a numbered proposal. Delete the false equivalence between an
untracked subtraction and the scope restriction.

### 5. Guard lifetime needs one specified mechanism with complete file accounting

**Problem:** Destructor clearing is install-conditional, not unconditional; historical installation
is not current slot ownership. An installer-aware remedy changes the core header, which P30 does not
list.

**Evidence:** `vortex_vm.cpp:76-95` uses `guardInstalled_` and last-wins installation;
`vortex_core.hpp:200` stores a bare function. Transient compiler VMs are constructed at
`vortex_host.cpp:935,:1001`; the vquery VM is `vm` at `vquery/main.cpp:487`.

**Fix:** Pin a concrete reinstall-at-resync contract with documented limits, or fully specify an
installer token/generation and its core-header cost. Do not leave the choice unresolved in a
normative design.

## MINOR

1. `attachVortexHost` directly assigns the host (`zigzag_visualizer.cpp:2844-2848`); it does not
   pass through `ensureVortexHost`. Give it its own gate.
1. Remove `overlay_` from the wholly untrailed list: release erases post-mark shadows.
1. `views.documentActivated` calls `keyboardPane.leaveZigzag(false)` before bind
   (`xuzz_app.cpp:1889,:1892`). Move it after successful bind.
1. Correct the translation function name and relocation/VM citation drift; optional `source` absence
   should be called `nullopt`, not an ambiguous `none` enumerator.
1. Skip focus restoration after refused adoption at `xuzz_app.cpp:632-637,:836-844`.
1. The `buildModule` facade must mint/find the module cell before calling a private
   `build*Module(CellRef)` method.
1. Null-host constructor/lazy/const compilation callers still must consume expected results
   explicitly, even where liveness refusal is impossible.
1. `adoptXanaduStore` and `adoptXanaduDocs` have no current callers; distinguish those API wrappers
   from the actually exercised `reloadStoreVersion`.

## Verified Foundations

The critic verified host replacement order, WorkerContext construction, swap and mark mechanics,
goal cursor marks, suppressed-builder feasibility, scalar restatements, image-holder topology,
packaging, headless ordering, and configuration-loading seams.

VERDICT: NOT CONVERGED
