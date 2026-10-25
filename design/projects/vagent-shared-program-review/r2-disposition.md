# Round 2 Disposition: Vagent Shared Program Manifold & Live Upgrade Spec

Date: 2026-10-24 Review Target: `design/vagent-shared-program-and-live-upgrade.md` Orchestrator:
Lead Editor

______________________________________________________________________

## 1. Summary of Round 2 Findings and Verdicts

| Critic Role                                | Blocking | Major  | Minor  | Round 2 Verdict            |
| :----------------------------------------- | :------: | :----: | :----: | :------------------------- |
| **System Architect (`architect`)**         |    0     |   0    |   2    | `VERDICT: CONVERGED`       |
| **Technical Writer (`writer`)**            |    2     |   2    |   2    | `VERDICT: NOT CONVERGED`   |
| **Vortex & Spool-Mesh Expert (`vortex`)**  |    3     |   2    |   2    | `VERDICT: NOT CONVERGED`   |
| **Codebase Expert (`codebase`)**           |    3     |   5    |   3    | `VERDICT: NOT CONVERGED`   |
| **PL Design & Ergonomics Expert (`ergo`)** |    0     |   3    |   3    | `VERDICT: NOT CONVERGED`   |
| **Xanadu / ZigZag Purist (`zigzag`)**      |    1     |   2    |   2    | `VERDICT: NOT CONVERGED`   |
| **TOTAL**                                  |  **9**   | **14** | **14** | **OVERALL: NOT CONVERGED** |

______________________________________________________________________

## 2. Core Architectural Resolutions

1. **Exact 13 `vagent` Dimensions (`runtime_abi.hpp:66–69`)**:

   - The live code defines exactly 13 dimensions in `kAgentDimensionNames`: `d.behavior`, `d.skill`,
     `d.capacity`, `d.space`, `d.member`, `d.holon`, `d.intention`, `d.subgoal`, `d.outbox`,
     `d.belief`, `d.pending`, `d.source`, `d.extends`.
   - `d.vars`, `d.values`, and `d.stack` are `VortexCore` genesis dimensions (`core.dims()`).
   - Timers live on the `$agent` cell's `$timers` field rank, not a dedicated dimension.
   - All occurrences updated across §1.1, §2.2, and Mermaid diagrams.

1. **Accurate Code Citations & Signatures**:

   - `CompiledProgram::instantiate` (`WorkerContext &`, `string_view`) returns
     `std::expected<CompiledAgent, std::string>` (`apps/common/xanadu/vagent/program.cpp:52`).
   - Control-flow dispatch (`#Jump`, `#Branch`, `#Call`) lives in
     `apps/common/xanadu/vortex/vortex_vm.cpp:655–694` via `toInt(inputs[0])`.
   - Checkpoint creation lives in `MeshActorRuntime::checkpointView` (`mesh_actor.cpp:924`).
   - P17.4 takes `std::chrono::system_clock::time_point wallNow` matching `MeshActorRuntime`.

1. **Checkpoint Store Promotion & Dimension Seeding**:

   - `Store::setLink` (`store.cpp:609`) enforces `requireCellOp` locally on its `opsSpool`.
   - Standalone checkpoint stores cannot invent an unproposed parent spool field in
     `OpsSegmentHeader`.
   - Instead, `promoteOverlay` seeds the canonical 44 `VortexCore` and 13 `vagent` dimension cells
     in segment 0 of the promoted checkpoint store so all intra-state links validate locally.

1. **Skill Instance Topology & Rule V12**:

   - Skill instances hold mutable field cells and therefore **must live in the ephemeral overlay**,
     never in the frozen base manifold.
   - The base manifold holds only immutable capacity definitions and compiled method graphs.
   - When an agent mounts a skill, the overlay allocates the skill instance and links it into the
     agent's `d.skill` rank.

1. **Live Upgrade Context & State Transcription**:

   - `WorkerContext` is non-movable and `ArenaManifold::base_` is immutable.
   - Upgrading constructs a fresh `WorkerContext` over `programManifoldV2`.
   - `StateReconciler` deep-transcribes preserved fields, beliefs, and activation frame locals from
     the old overlay into the new overlay.
   - `actor->cursor` is re-initialized to the continuation entry in $V_2$.

1. **Language Surface (`on Upgrade` & Delegation Cancel)**:

   - `event Upgrade { from_version: Str }` eliminates the non-existent `Any` type.
   - Dropped fields are accessed via a dedicated introspection function
     `dropped_field(name: Str) -> Str`.
   - Non-native skills support `on upgrade` handlers to migrate private skill state.
   - Aborting delegated intentions stages a `$cancel` message to the delegate peer (§7.1).

______________________________________________________________________

## 3. Dispositions by Finding

- **Writer (W1, W2, W3, W4)**: ACCEPTED. Corrected dimensions, citations, signatures, and API types.
- **Vortex (V1, V2, V3, V4, V5)**: ACCEPTED. Replaced delta store parent spools with segment-0
  dimension seeding; clarified `WorkerContext` reconstruction and cursor setup; aligned `wallNow`
  type.
- **Codebase (C1, C2, C3, C4, C5)**: ACCEPTED. Fixed `kAgentDimensionNames` citations,
  `vortex_vm.cpp` citations, `instantiate` signature, and `promoteOverlay` mechanics.
- **Ergo (E1, E2, E3)**: ACCEPTED. Removed `Any`, added skill `on upgrade` hooks, added `$cancel`
  performative emission for aborted delegations.
- **ZigZag (Z1, Z2, Z3)**: ACCEPTED. Moved skill instances to overlay, preserved Rule V12, aligned
  diagram with §5.7 ranks (`d.vars`, `d.values`, `d.clause`, `d.spin`, `d.step`).
