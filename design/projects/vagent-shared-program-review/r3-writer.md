# Round 3 critique: Technical Writer (`design/vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2.1 (Fictitious Dimensions `d.agent` and `d.timers`): RESOLVED — Reconciled across §1.1, §2.2,
  Mermaid diagram, and §3.1 with the exact 13 dimensions from `kAgentDimensionNames`
  (`runtime_abi.hpp:66–69`), properly separating `VortexCore` genesis dimensions and timers on
  `$agent`'s `$timers` field rank.
- R2.2 (Code Hallucinations in `instantiate` and `vortex_core.cpp`): RESOLVED —
  `CompiledProgram::instantiate` signature updated to `std::expected<CompiledAgent, std::string>`
  and takes `WorkerContext &` and `std::string_view` (`program.cpp:52`). Control-flow execution
  citations updated to `vortex_vm.cpp:655–694` via `toInt(inputs[0])`.
- R2.3 (`checkpoint` function name and `wallNow` type mismatch): RESOLVED — Cited
  `MeshActorRuntime::checkpointView` at `mesh_actor.cpp:924`. Proposal P17.4 uses
  `std::chrono::system_clock::time_point wallNow` matching `mesh_actor.hpp:155, 164, 175`.
- R2.4 (Inaccurate Line Citations for `ArenaManifold` Constructor): PARTLY — P17.1 still contains a
  stale citation to `arena_manifold.cpp:254` (constructor is at line 23), but this is non-blocking
  prose polish. Re-classified as MINOR.
- R2.5 (Mermaid Diagram Dimension Count Mismatch): RESOLVED — Node `DIMS` updated to
  `"(44 VortexCore dims + 13 runtime_abi.hpp dims)"`.
- R2.6 (`restoreState` Function Signature Inaccuracy): RESOLVED — Conceptual prose in §1.2
  accurately describes state transfer; tracked as MINOR polish.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§5, line 426) Stale Line Citation for ArenaManifold Constructor in Proposal P17.1

Line 426 references
`ArenaManifold::arenaDims_ is a private C++ member populated during construction`
`(arena_manifold.cpp:254)`. The constructor `ArenaManifold::ArenaManifold` is defined at
`apps/common/xanadu/zigzag/arena_manifold.cpp:23`, while `arenaDims_` is declared at
`apps/common/xanadu/zigzag/arena_manifold.hpp:746` and populated in `ensureDimension` at line 92.
Line 254 is within `ArenaManifold::denseOf`.

**Fix:** Update the citation to `arena_manifold.cpp:23`.

### 2. (§1.1, line 23) Minor const Qualifier on CompiledProgram::instantiate Snippet

The illustrative snippet on lines 21–23 shows `instantiate(...) const {`. In
`apps/common/xanadu/vagent/program.hpp:40–42` and `program.cpp:51–52`,
`CompiledProgram::instantiate` is non-const.

**Fix:** Remove `const` from the member function header in the code snippet.

VERDICT: CONVERGED
