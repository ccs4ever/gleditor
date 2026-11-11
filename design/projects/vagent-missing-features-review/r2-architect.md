# Round 2 critique: System Architect (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Layer Inversion in std:actor:spawn_image): RESOLVED — Dynamic actor image spawning has been
  completely removed from `vortex_stdlib.cpp` and relocated to the VAgent runtime layer
  (`apps/common/xanadu/vagent/adapter.cpp` and `executor.cpp`), preserving the strict unidirectional
  dependency `vagent -> vortex`.
- R1.2 (Cross-Arena Raw CellRef Leaks in Distributed Swarm): RESOLVED — The compilation pipeline is
  cleanly bifurcated into Pattern A (`behavior StagedCompiler` executing synchronously in a single
  actor turn within one `ArenaManifold`) and Pattern B (`agent CompilerCoordinator` operating at
  module granularity, exchanging sealed, immutable `MicroversionId` strings across the mesh with
  zero cross-arena `CellRef` leaks).
- R1.3 (Missing Mutable Store Binding and Capability Bounding in promote): RESOLVED — Proposal P27
  updates `boundStore_` in `VortexStdLib` to a mutable `xanadu::Store*`, derives `parentVersion`
  from `boundStore_->primaryCurrentVersion()`, passes `d.dims` in `extraRoots`, executes P16 slot
  restatements, bounds promotion budget by the caller's `maxCells` capability grant, and returns
  typed `Result<Str, ZigzagError>`.
- R1.4 (Hallucinated consumeWork Signature & ValueKind::String): RESOLVED — Parameterless
  `core_.arena().consumeWork()` is used for cycle fuel accounting, ingestion memory allocation is
  metered against arena scratch allocation ceilings, and primedia text updating is cleanly separated
  into `set_text` (`ArenaManifold::setContent`) versus scalar bits `set_value`
  (`ArenaManifold::setValueBits`).
- R1.5 (Unbounded Actor Lifecycle Leak in Swarm Coordinator): RESOLVED — The un-reaped 4-worker
  per-phase pipeline is eliminated; intra-file compilation is consolidated into single-turn
  `StagedCompiler`, and the swarm coordinator delegates to a single `CompilationWorker` per unit.
- R1.6 (Missing .vagent Metacircular Parity and Macro Desugaring Alternative in P30): RESOLVED —
  Proposal P30 now includes all sovereign stdlib compiler components (`stdlib/lexer.vagent`,
  `stdlib/ast.vagent`, `stdlib/checker.vagent`, `stdlib/emitter.vagent`), and pattern matching is
  specified to lower into deterministic decision trees (`#Equal`, `#Branch`, `#ExtractField`),
  eliminating choice stack leaks and trail destruction.

## BLOCKING

(None.)

## MAJOR

(None.)

## MINOR

### 1. (§4.2, lines 351–368) Worker Actor Termination Lifecycle in CompilerCoordinator

Full title: Clarify orderly termination or pooling of `CompilationWorker` instances

**Problem:** In `agent CompilerCoordinator` (§4.2, line 360), a worker is dynamically spawned for
each compilation request: `let worker := spawn CompilationWorker()`. While moving from 4 workers per
sub-pass to 1 worker per file significantly reduces actor sprawl, spawning one actor per file
without an explicit `stop` percept or pooling pattern can still accumulate dormant `Actor` entries
in `MeshActorRuntime::actors_` during large multi-file builds.

**Fix:** In §4.2, add a clarifying note or show `worker` terminating cooperatively upon sending
`tell UnitCompiled(...)` (via an orderly `stop` percept as implemented in `VagentAdapter:356–385`),
or recommend reusing a pooled worker set.

### 2. (§3.2, lines 237–246, §7 P27) Capability Authorization Guard in `promote`

Full title: Explicitly verify caller's `ActorGrant.immutable` before initiating promotion

**Problem:** While §3.2 and §7 P27 now properly bind a mutable `Store*` and check promotion budgets,
§3.2 does not explicitly mention testing `grant.immutable == true` before calling `zigzag::promote`.
If a sandboxed actor with an immutable grant calls `std:zigzag:promote`, the call must immediately
fail with `ActorError::CapabilityRefused` before touching the store.

**Fix:** Add an explicit requirement to §3.2 and Proposal P27 stating that `std:zigzag:promote`
checks the caller's `ActorGrant` and returns `Err(ZigzagError:CapabilityRefused)` if
`grant.immutable` is set.

______________________________________________________________________

VERDICT: CONVERGED
