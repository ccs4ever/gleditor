# Round 3 critique: Codebase Expert (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 5 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2.B1 (Dimension Namespace Falsification Persists): RESOLVED — Replaced fictitious `d.agent` and
  `d.timers` across §1.1, §2.2, Mermaid diagram, and §3.1 with exact 13 dimensions from
  `kAgentDimensionNames` (`runtime_abi.hpp:66–69`), properly placing timers on `$agent`'s `$timers`
  field rank (`kFieldTimers = "$timers"`).
- R2.B2 (Unproposed Engine Delta Store Parent Spools): RESOLVED — Removed unproposed delta store
  parent spools. Proposal P17.1 and §3.1 now specify `zigzag::promoteOverlay` seeding canonical 44
  `VortexCore` and 13 `vagent` dimensions in segment 0 of the checkpoint store, satisfying
  `Store::requireCellOp` (`store.cpp:609`) locally without engine modifications.
- R2.B3 (Hallucinated Line Numbers and File Mismatch in VM Citation): RESOLVED — Updated citation in
  §1.2 to `apps/common/xanadu/vortex/vortex_vm.cpp:655–694` (`OpcodeKind::Jump`, `Branch`, `Call`)
  with `toInt(inputs[0])` cast to `CellRef`, eliminating all phantom citations to
  `vortex_core.cpp:1735..1929`.
- R2.M1 (Non-Existent dimensionNames() in P17.1): RESOLVED — Removed phantom `dimensionNames()` in
  P17.1 in favor of seeding base dimensions; line 426 citation note downgraded to MINOR polish.
- R2.M2 (Infeasible instantiate Construction in P17.2 and Wrong Return Type): RESOLVED — Updated
  `CompiledProgram::instantiate` return type to `std::expected<CompiledAgent, std::string>` matching
  `program.hpp:40–42`, and clarified in P17.3 that `WorkerContext` is constructed over
  `programManifold` by `MeshActorRuntime`.
- R2.M3 (Undefined StateReconciler Signature and Hallucinated Supervision Tree): RESOLVED — Removed
  hallucinated supervision tree from `MeshActorRuntime::upgrade` in P17.4 (replaced with monitors);
  `upgrade` signature properly carries `StateReconciler` callback and
  `std::chrono::system_clock::time_point wallNow`.
- R2.M4 (Handle Kind Contradiction in §1.2): RESOLVED — Descriptive phrasing in motivational
  background section §1.2 reclassified as MINOR non-blocking polish.
- R2.M5 (Mechanically Unspecified Checkpoint Delta Mounting in ArenaManifold): RESOLVED — §8.1 line
  572 clarifies that resume loads checkpoint state cells into the overlay slots of
  `ArenaManifold(&programManifold)` without requiring foreign parent spool lookups or new manifold
  classes.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§5 P17.1, line 426) Stale line citation for `ArenaManifold` constructor

Line 426 cites `arena_manifold.cpp:254`, whereas the constructor
`ArenaManifold::ArenaManifold(const Manifold *base, const xanadu::Store *store)` is defined at
`apps/common/xanadu/zigzag/arena_manifold.cpp:23`. Line 254 is within logging helper
`expectWritten`.

**Fix:** Update citation to `apps/common/xanadu/zigzag/arena_manifold.cpp:23`.

### 2. (§1.1, line 23) Trailing `const` qualifier on `CompiledProgram::instantiate` snippet

The code snippet on lines 21–23 includes a trailing `const`, whereas in
`apps/common/xanadu/vagent/program.hpp:40–42` and `program.cpp:51–52`,
`CompiledProgram::instantiate` is non-const (`override`).

**Fix:** Remove trailing `const` from line 23.

### 3. (§1.2, line 76) `UInt64` handle phrasing in background motivation

Section 1.2 line 76 cites "`d.mesh.relocation`-marked `UInt64` handles (`arena_manifold.cpp:2011`)".
In `ArenaManifold` (`arena_manifold.cpp:1800, 1949–1954`), relocation handles carry
`ValueKind::OpHandle`, and line 2011 is not the remapping logic.

**Fix:** Update to "`ValueKind::OpHandle` handles (`arena_manifold.cpp:1800, 1949–1954`)".

### 4. (§4.2, line 303) Mailbox ownership citation

Line 303 cites "Messages queued in `state.mailbox`". In the repository, `mailbox` is an internal
member of `MeshActorRuntime::Actor` (`apps/common/xanadu/vortex/mesh_actor.cpp:55`), while
`AgentExecutor::State` (`agent_executor.cpp:297–328`) manages turn lifecycle and host events.

**Fix:** Change `state.mailbox` to `actor->mailbox`.

### 5. (§5 P17.5, line 521) P17.5 file list omits `ast.hpp` and `compiler.cpp`

Proposal P17.5 lists `parser.cpp`, `checker.cpp`, `agent_executor.hpp`, and `agent_executor.cpp`,
but implementation of `UpgradeEventDef` and handler lowering will also touch
`apps/common/xanadu/vagent/ast.hpp` and `apps/common/xanadu/vagent/compiler.cpp`.

**Fix:** Add `ast.hpp` and `compiler.cpp` to P17.5.

VERDICT: CONVERGED
