# Round 7 critique: Programming Language Design & Ergonomics Expert

Target: `design/vortex-stdlib-prebuilt-store.md`. Paths below are relative to the repo root. Tally:
1 BLOCKING, 2 MAJOR, 7 MINOR.

This is the orchestrator-filed finding record from the returned critique.

## BLOCKING

### 1. The document-switch gate runs after the state it promises to retain is gone

**Problem:** A refusal after engine/store replacement leaves new view state with old bookkeeping and
a dangling retained host, which subsequent commands are told to use.

**Evidence:** `zigzag_visualizer.cpp:364,:367-370,:392` orders destruction and rebind before the
gate; `adoptDocument` has the same pattern at `:244-247,:267,:306`. The host captured the old engine
manifold at `:2859`. `xuzz_app.cpp:1829-1841` bookkeeping remains old on refusal.

**Fix:** Check outgoing liveness before any document mutation; retain the command replace-arm check.
Test pointer identity of engine, store, host, and bound store.

## MAJOR

### 2. Refusal feedback is invisible when the command bar is closed

**Problem:** Startup and document-switch refusals are routed to text drawn only in an open command
bar. The external bind lambda also cannot write the private field.

**Evidence:** Rendering is inside `if (commandBarVisible_)` (`zigzag_visualizer.cpp:2393`, feedback
`:2442-2447`); document activation is at `xuzz_app.cpp:1886-1893`; fields expose getters only
(`zigzag_visualizer.hpp:358-362`). The unconditional top HUD is at `:2222-2246`.

**Fix:** Refusing member functions must write feedback and explicitly open the bar, or use a
genuinely always-visible banner/HUD surface with a named draw path.

### 3. std:goal/resume is not an exported routine

**Problem:** Recovery names a path that cannot be resolved.

**Evidence:** Goal exports at `vortex_stdlib.cpp:3608-4155` include no `resume`; the actual API is
C++ `goalResume` at `:4222-4236`, with the results loop at `:4282-4287`.

**Fix:** Name the actual embedding/cursor resumption API, or propose and implement an exported
routine. Do not direct users to a nonexistent path.

## MINOR

1. Rename `navigateCellVQL` to `translateVQLAndAttachToFocus`.
1. Existing bool/vector command results cannot carry expected errors. Use visible feedback and
   existing message fields; verify cursor/command stack coexistence.
1. Put aggregation inside `VortexHost`; choose represented floor accounting or an explicit
   never-hydrated host-arena contract.
1. Pin repeatable `--module` syntax and output replacement semantics. Distinguish optional
   `source=nullopt` from a hypothetical `none` enumerator.
1. Asset-chain levels below the explicit leaf directory are roots: append `vortex-stdlib/` to each.
1. Pin startup logging to INFO in `ensureStdlib`; report the resolved local directory only when its
   chain ran, including honest skipped-mode wording.
1. Disambiguate manual/suppressed construction, `local.state=suppressed`, and `localImage=false`.

## Verified Foundations

The critic verified solver status dispatch and introspection intent, local-state triggers, generator
scratch feasibility and nonzero unresolved-symbol failure, directory pairing and restart workflow,
headless exclusion, offscreen CI reachability, key-record topology, and stdlib-bearing versus
bare-core consumers.

VERDICT: NOT CONVERGED
