# Round 7 critique: Vortex/Spool-Mesh Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 3 MAJOR, 4 MINOR.

This is the orchestrator-filed finding record from the returned critique, whose heading incorrectly
called this Round 1. The critic disclosed that the common brief and R6 disposition were available
only through inherited context and grep excerpts; some caller and exclusion-list checks were not
repeated end-to-end.

## BLOCKING

### B1. The replacement gate runs after adoption has destroyed retained state

**Problem:** Checking inside `ensureVortexHost` cannot retain the old engine or document when its
callers have already replaced them. A retained host can hold a dangling base.

**Evidence:** `bindXanaduStore` resets `engine_` at `zigzag_visualizer.cpp:364`, rebinds stores at
`:366-369`, constructs an engine at `:370`, and rewrites view state at `:372-391` before
`ensureVortexHost` at `:392`. `adoptDocument` similarly mutates document, store, mapping, engine,
and focus state before its `:306` ensure. The host captured the old engine manifold at `:2859`.
Command-only callers do not replace the engine before ensuring the host.

**Fix:** Add pre-mutation entry checks to `bindXanaduStore`, `adoptDocument`, `attachVortexHost`,
and the already specified `bindManifold`; retain the replace-arm check for command callers. Refusal
must precede all mutations, not only host assignment.

## MAJOR

### M1. Worker mark aggregation needs a new public host method

**Problem:** Existing public accessors expose only the primary arena.

**Evidence:** `extraContexts_` is private (`vortex_host.hpp:350`); `arena()` and `primaryContext()`
reach only `primary_` (`:82-85,:101-104`).

**Fix:** Propose `VortexHost::liveMarks()` or an explicit context enumeration API.

### M2. Swap clearing and write-guard ownership remain incomplete

**Problem:** The clear list omits `choiceStack_` and `oracleCursor_`. The guard flag means
"installed before", not "currently owns the slot".

**Evidence:** `ChoicePoint.mark` is an arena snapshot (`vortex_vm.cpp:317-326`); `backtrack`
releases it (`:343-345`). `oracleCursor_` indexes the cleared oracle path (`vortex_vm.hpp:309`).
`~VortexVM` clears under `guardInstalled_` (`:76-82`); `installWriteGuard` is last-wins (`:84-95`).
The primary VM and VPL-owned VM share a core.

**Fix:** Clear both omitted members (or prove the stack empty through the gate); specify the VM
whose registration map backs the post-resync guard. Pin a concrete ownership or reinstall mechanism
rather than treating the flag as current ownership.

### M3. Selective module generation needs a dependency rule

**Problem:** A suppressed builder can emit a structurally valid but dependency-broken module unless
dependency checking is explicit.

**Evidence:** `bootstrap` builds logic before goal because goal depends on logic
(`vortex_stdlib.cpp:1516-1518`). The public facade is proposed over private builders.

**Fix:** Specify dependency closure or an explicit supported self-contained module set, and reject
unresolved dependencies before publishing a local image.

## MINOR

1. Rename `navigateCellVQL` to `translateVQLAndAttachToFocus`; separate guarded `compileVQL` from
   unconditional command ensure sites. Refresh executor and mesh-evaluator citations by symbol.
1. The evictable hydration seed has no `floorBaseline` mechanism. The never-hydrated host-arena
   scope is a viable v1 alternative; OR-parallel hydration is outside it.
1. Add `xuzz_app.cpp`, command callers, and tests to P30's touched-file accounting.
1. State rollback exclusions positively: no `ensureDimension`, dimension binding, or `attach` inside
   the graft mark. Overlay entries are partially mark-restored; the three dimension/store side maps
   are untrailed.

## Verified Foundations

The critic verified constructor/driver factoring, builder access through a proposed facade,
bits-only scalar restatement, image-op registration, per-context marks, Store/permascroll save
order, the read-only configuration gap, consumer coverage, and packaging anchors. These do not
require a redesign.

VERDICT: NOT CONVERGED
