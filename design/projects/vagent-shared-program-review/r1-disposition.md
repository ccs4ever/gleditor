# Round 1 Disposition: Vagent Shared Program Manifold & Live Upgrade Spec

Date: 2026-10-24 Review Target: `design/vagent-shared-program-and-live-upgrade.md` Orchestrator:
Lead Editor

______________________________________________________________________

## 1. Summary of Round 1 Findings and Verdicts

| Critic Role                                | Blocking | Major  | Minor  | Round 1 Verdict            |
| :----------------------------------------- | :------: | :----: | :----: | :------------------------- |
| **Technical Writer (`writer`)**            |    3     |   5    |   3    | `VERDICT: NOT CONVERGED`   |
| **Vortex & Spool-Mesh Expert (`vortex`)**  |    4     |   4    |   2    | `VERDICT: NOT CONVERGED`   |
| **Codebase Expert (`codebase`)**           |    5     |   5    |   2    | `VERDICT: NOT CONVERGED`   |
| **System Architect (`architect`)**         |    1     |   4    |   1    | `VERDICT: NOT CONVERGED`   |
| **PL Design & Ergonomics Expert (`ergo`)** |    3     |   4    |   2    | `VERDICT: NOT CONVERGED`   |
| **Xanadu / ZigZag Purist (`zigzag`)**      |    3     |   3    |   2    | `VERDICT: NOT CONVERGED`   |
| **TOTAL**                                  |  **19**  | **25** | **12** | **OVERALL: NOT CONVERGED** |

______________________________________________________________________

## 2. Cross-Cutting Themes and Architectural Decisions

1. **Opcode Output Routing vs. Runaway COW Shadowing (Arch-1, Codebase-2, Vortex-7)**:
   - *Issue*: Placing opcode output slots in the base manifold triggers `ArenaManifold::shadow` on
     every instruction result, ballooning heap allocations and making turn-end scratch cleanup
     (`arena.release(turnMark)`) impossible because it wipes live state.
   - *Disposition*: **ACCEPTED**. The compiled program in the base manifold contains immutable code
     graphs and static read-only operands. Opcode outputs write directly to ephemeral overlay
     registers / frame slots (`d.vars`/`d.values`) allocated in the agent overlay arena. Base opcode
     output wings are not shadowed across turns.
1. **Dimension Identity & Namespace Alignment (Writer-1, Codebase-4, ZigZag-8)**:
   - *Issue*: Fictitious `d.vagent.*` dimension names contradict the live 10 canonical dimensions in
     `apps/common/xanadu/vagent/runtime_abi.hpp` (`d.agent`, `d.vars`, `d.values`, `d.skill`,
     `d.behavior`, `d.intention`, `d.subgoal`, `d.outbox`, `d.timers`, `d.stack`).
   - *Disposition*: **ACCEPTED**. Update all prose, diagrams, and tables to use the exact dimensions
     defined in `runtime_abi.hpp:67-87`.
1. **Store Invariants & Checkpoint Delta Promotion (Codebase-1, Vortex-2)**:
   - *Issue*: `Store::setLink` enforces `requireCellOp` on its own `opsSpool`. Standalone checkpoint
     stores cannot link along base dimensions without dimension definitions existing in that store's
     spool.
   - *Disposition*: **ACCEPTED**. Clarify that an agent state checkpoint is an overlay delta store
     whose header names `baseRef` as its parent segment; when promoted standalone, dimension
     declarations are resolved against the base spool, or re-declared in the delta segment header.
1. **Vagent Surface Language Syntax and `on Upgrade` Event (Ergo-1, Ergo-2, Writer-7)**:
   - *Issue*: `String` is not a vagent type (vagent uses `Str`); `on Upgrade(from_version)` must be
     typed as an event record `event Upgrade { from_version: Str, old_fields: Dict<Str, Any> }` so
     removed fields can be inspected without breaking version 2 static agent field checking.
   - *Disposition*: **ACCEPTED**. Fix type syntax to `Str`. Define
     `event Upgrade { from_version: Str, dropped_fields: Any }` in `prelude.hpp`.
1. **Engine C++ Citations & Type Realism (Writer-2, Codebase-3, Vortex-3, Vortex-4)**:
   - *Issue*: Non-existent files cited (`core.cpp` -> `vortex_core.cpp`), hallucinated `ActorGrant`
     fields (`epoch`, `seq`, `actorPid`), non-movable `WorkerContext`.
   - *Disposition*: **ACCEPTED**. Correct all repository paths, line numbers, and API structures to
     match the live C++23 codebase.
1. **House Style Completeness (Writer-4, Writer-5, Writer-6, Arch-6)**:
   - *Issue*: Missing Refused Alternatives section, missing Open Questions table, unpriced proposal
     register (`P17.1`–`P17.5`).
   - *Disposition*: **ACCEPTED**. Add formal Refused Alternatives (§6), Open Questions (§7), and
     full cost/pricing analysis in the Proposal Register (§5).

______________________________________________________________________

## 3. Dispositions by Finding

### Technical Writer (`writer`)

- **W1 (`d.vagent.*` dimensions)**: ACCEPTED. Align with `runtime_abi.hpp`.
- **W2 (C++ source files and signatures)**: ACCEPTED. Correct all file citations to
  `apps/common/xanadu/vortex/vortex_core.cpp`, `mesh_actor.hpp`, etc.
- **W3 (Hallucinated Actor records)**: ACCEPTED. Align with `ActorRecord` in `mesh_actor.hpp`.
- **W4 (Refused Alternatives)**: ACCEPTED. Add Section 6.
- **W5 (Open Questions Table)**: ACCEPTED. Add Section 7.
- **W6 (Proposal Pricing & Fallbacks)**: ACCEPTED. Add explicit byte/cycle/link costs and fallback
  paths to `P17.1`–`P17.5`.
- **W7 (String vs Str)**: ACCEPTED. Replace `String` with `Str`.
- **W8 (Mermaid diagram dimension count)**: ACCEPTED. Fix diagram to show canonical dimensions.
- **W9 (Typo `d. Next`)**: ACCEPTED. Fix to `d.next`.
- **W10 (Line citations)**: ACCEPTED. Update citations.
- **W11 (Inbound mailbox queue naming)**: ACCEPTED. Use `mailbox` instead of `inbound`.

### Vortex & Spool-Mesh Expert (`vortex`)

- **V1 (Cross-arena CellRef leaking)**: ACCEPTED. Specify handle-based migration with explicit copy
  into the new overlay arena.
- **V2 (Checkpoint delta store invariants)**: ACCEPTED. Detail `Store::setLink` parent-segment
  linking model.
- **V3 (ActorGrant fields)**: ACCEPTED. Match `ActorGrant` (`grantId`, `programRef`, `baseRef`,
  `options`, `expiresAt`, `issuedAt`).
- **V4 (WorkerContext non-movability)**: ACCEPTED. Keep `WorkerContext` pinned or recreate per
  turn/epoch.
- **V5 (Opcode names)**: ACCEPTED. Use valid `OpcodeKind` names.
- **V6 (Actor fields)**: ACCEPTED. Remove non-existent fields.
- **V7 (Turn mark & -d.grab slot reset)**: ACCEPTED. Discard COW scratch shadowing in favor of
  overlay register routing.
- **V8 (SpanReader in WorkerContext)**: ACCEPTED. Include `SpanReader` in construction.
- **V9 (readScalarValue / core.cpp)**: ACCEPTED. Use `readValue` and `vortex_core.cpp`.
- **V10 (OpHandle in render)**: ACCEPTED. Clarify handle resolution via `resolveHandle`.

### Codebase Expert (`codebase`)

- **C1 (Overlay promotion and setLink)**: ACCEPTED. Document segment header parent linkage.
- **C2 (mark/release live state destruction)**: ACCEPTED. Same as V7 / Arch-1.
- **C3 (core.cpp hallucination)**: ACCEPTED. Correct to `vortex_core.cpp`.
- **C4 (runtime_abi.hpp dimension alignment)**: ACCEPTED. Same as W1.
- **C5 (P17.4 layer boundary conflation)**: ACCEPTED. Separate `MeshActorRuntime` distributed
  concerns from `AgentExecutor` vagent adapter.
- **C6 (arenaDims\_ and dimensionNames())**: ACCEPTED. Correct to actual `ArenaManifold` methods.
- **C7 (attachBase post-construction)**: ACCEPTED. Construct `ArenaManifold` directly with `base`
  pointer.
- **C8 (Hallucinated filenames)**: ACCEPTED. Fix to existing files.
- **C9 (Handle kind OpHandle vs UInt64)**: ACCEPTED. Clarify `OpHandle` structure.
- **C10 (Error types and signatures)**: ACCEPTED. Use `std::expected<void, ActorError>`.
- **C11 (Line numbers)**: ACCEPTED. Fix line numbers.
- **C12 (Multi-agent compilation scope)**: ACCEPTED. Specify program-wide unit compilation.

### System Architect (`architect`)

- **A1 (COW-shadowing & broken turn cleanup)**: ACCEPTED. Resolved via overlay registers.
- **A2 (C++ vs Vortex reconciliation)**: ACCEPTED. Specify Vortex standard library helper for state
  mapping, invoked during upgrade turn.
- **A3 (Base manifold retention bounds)**: ACCEPTED. Add LRU cache bounds on
  `LocalHost::programManifolds_`.
- **A4 (Magic sentinel zero)**: ACCEPTED. Use `std::nullopt` or explicit unlinked state instead of
  zero cell ref.
- **A5 (Proposal signatures & error types)**: ACCEPTED. Fix signatures to match `ActorError`.
- **A6 (Arbitrary ceilings)**: ACCEPTED. Expose configurable limits.

### PL & Ergonomics Expert (`ergo`)

- **E1 (on Upgrade syntax and typing)**: ACCEPTED. Use `on Upgrade as u` with `Str` version.
- **E2 (Removed field access in V2)**: ACCEPTED. Provide `u.dropped_fields` dictionary for
  historical state migration.
- **E3 (Plan lifecycle & tried list across upgrades)**: ACCEPTED. Clear `tried` list when plan graph
  hash changes.
- **E4 (In-flight mesh requests across upgrades)**: ACCEPTED. Requests remain tracked by correlation
  ID; replies match new interface.
- **E5 (Maintain goal restorer lifecycle)**: ACCEPTED. Re-evaluate `holds` condition on upgrade
  completion.
- **E6 (Subgoal cancellation)**: ACCEPTED. Specify cascade cancellation if parent goal plan changed.
- **E7 (Skill upgrade hooks)**: ACCEPTED. Add `upgrade` lifecycle handler for native and regular
  skills.
- **E8 (Semver vs hash versioning)**: ACCEPTED. Clarify semver string in `on Upgrade` and
  cryptographic hash in `programRef`.
- **E9 (achieve vs adopt in non-suspending ceiling)**: ACCEPTED. Clarify that `adopt` is
  non-suspending, whereas `achieve` suspends.

### Xanadu / ZigZag Purist (`zigzag`)

- **Z1 (Base-to-overlay linkage & Rule V12)**: ACCEPTED. Base cells never link to overlay cells;
  overlay links point inward or across overlay cells.
- **Z2 (Dimension overloading & fan-in in root table)**: ACCEPTED. Maintain strict 2-degree 1D rank
  invariants.
- **Z3 (d.clone and d.capacity preservation)**: ACCEPTED. Preserve clone and capacity ranks in base
  program manifold.
- **Z4 (Permascroll transclusion model)**: ACCEPTED. Detail sealed program store as an EDL over the
  deployment permascroll.
- **Z5 (Suspension table ZigZag rank modeling)**: ACCEPTED. Model suspension table as cells along
  `d.step` rank.
- **Z6 (Structural plan navigation)**: ACCEPTED. Navigate plans structurally along `d.plan` rank
  instead of delimited strings.
- **Z7 (Mermaid fan-out labeling)**: ACCEPTED. Label all ranks in diagrams.
- **Z8 (Dimension naming)**: ACCEPTED. Aligned with `runtime_abi.hpp`.
