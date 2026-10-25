# Round 2 critique: Technical Writer (`design/vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 2 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Fictitious `d.vagent.*` Dimensions Contradict Runtime ABI): PARTLY — `d.vagent.*` removed,
  but replaced with an erroneous 10-dimension list inventing `d.agent` and `d.timers` while
  contradicting `runtime_abi.hpp:66–70`.
- R1.2 (Non-Existent C++ Source Files and API Signatures): PARTLY — `core.cpp` replaced with
  `vortex_core.cpp`, but fake `instantiate` snippet at lines 20–25 remains and lines 66–68 cite
  non-existent lines (1735+) in 585-line `vortex_core.cpp`.
- R1.3 (Hallucinated Actor Records and Move-Assignment): RESOLVED — `ActorEntry`, `MeshFlight`, and
  `WorkerContext` move-assignment removed; P17.4 specifies clean in-place transition.
- R1.4 (Missing Refused Alternatives Section): RESOLVED — Added §6 with complete comparison table
  and priced penalties.
- R1.5 (Missing Open Questions Table): RESOLVED — Added §7 with structured open questions table.
- R1.6 (Proposals P17.1–P17.5 Lack Pricing and Fallbacks): RESOLVED — All proposals in §5 now
  include explicit Pricing and Fallback sections.
- R1.7 (Surface Type Inconsistency: String vs Str): RESOLVED — Replaced `String` with `Str`
  throughout; `String` only appears in §6 as a refused alternative.
- R1.8 (Mermaid Diagram Dimension Count and Duplicate Stack): PARTLY — Removed duplicate
  `d.stack.*`, but diagram label retains incorrect "10 runtime_abi.hpp dims".
- R1.9 (Typo in Code Span `d. Next`): RESOLVED — Corrected to `d.next`.
- R1.10 (Inaccurate `mesh_actor.cpp` Line Citations): PARTLY — `checkpointView` is still cited as
  `checkpoint` at line 251 (which is inside `resume`); constructor in `arena_manifold.cpp` cited at
  line 254 instead of line 23.
- R1.11 (Mailbox Queue Referred to as `inbound`): RESOLVED — Replaced with `state.mailbox`.

## BLOCKING

### 1. (§1.1, line 32; §2.2, lines 163, 190) Fictitious Dimensions Persist

Full title: Invention of Fictitious Dimensions `d.agent` and `d.timers` and Misrepresentation of
`runtime_abi.hpp:66–70`

**Problem:** While Round 1 finding W1 eliminated the fictitious `d.vagent.*` prefix, the revised
text introduces a new factual contradiction against the repository's runtime ABI:

1. §1.1 (lines 32–34) and §2.2 (lines 190–191) assert: "The 10 canonical `vagent` dimensions defined
   in `apps/common/xanadu/vagent/runtime_abi.hpp:67–87` (`d.agent`, `d.vars`, `d.values`, `d.skill`,
   `d.behavior`, `d.intention`, `d.subgoal`, `d.outbox`, `d.timers`, `d.stack`)..."
1. In reality (`apps/common/xanadu/vagent/runtime_abi.hpp:66–70`), `kAgentDimensionNames` defines 13
   dimensions: `d.behavior`, `d.skill`, `d.capacity`, `d.space`, `d.member`, `d.holon`,
   `d.intention`, `d.subgoal`, `d.outbox`, `d.belief`, `d.pending`, `d.source`, `d.extends`.
1. Furthermore, `runtime_abi.hpp:48–49` explicitly documents: "The reused genesis dimensions
   (`d.vars`, `d.values`, `d.clause`, `d.grab`, `d.step`, `d.spin`, `d.clone`, `d.stack`) are looked
   up from VortexCore." `d.vars`, `d.values`, and `d.stack` are core genesis dimensions
   (`apps/common/xanadu/vortex/vortex_core.hpp:37–40`), not dimensions defined in `runtime_abi.hpp`.
   The document contradicts itself by claiming they are defined in `runtime_abi.hpp` on line 32,
   while on lines 34–35 and 188–189 acknowledging them as `VortexCore` genesis dimensions.
1. The dimensions `d.agent` and `d.timers` do not exist anywhere in `runtime_abi.hpp` or the C++
   codebase. In `compiler.cpp:3186`, an agent's root cell is created as
   `arena_.makeCell(agentType_)` and is not connected along any `d.agent` dimension (as claimed on
   line 195). In `runtime_abi.hpp:178` and `agent_executor.cpp:969–978`, timers are outbox emissions
   (`kKindTimer`) managed by the mesh runtime and delivered to `d.pending`, not tracked on a ZigZag
   dimension `d.timers` (as claimed on line 296).
1. The text completely omits 8 real dimensions registered by `runtime_abi.hpp`: `d.capacity`,
   `d.space`, `d.member`, `d.holon`, `d.belief`, `d.pending`, `d.source`, `d.extends`.

**Evidence:**

From doc §1.1, lines 32–34:

> "The 10 canonical `vagent` dimensions defined in `apps/common/xanadu/vagent/runtime_abi.hpp:67–87`
> (`d.agent`, `d.vars`, `d.values`, `d.skill`, `d.behavior`, `d.intention`, `d.subgoal`, `d.outbox`,
> `d.timers`, `d.stack`)..."

From doc §2.2, line 163:

> `DIMS["Canonical Dimension Registry\n(44 VortexCore dims + 10 runtime_abi.hpp dims)"]`

From doc §2.2, lines 190–191:

> "All 10 canonical `vagent` dimensions (`d.agent`, `d.vars`, `d.values`, `d.skill`, `d.behavior`,
> `d.intention`, `d.subgoal`, `d.outbox`, `d.timers`, `d.stack`)."

From doc §2.2, line 195:

> "Agent Declarations: Linked along `d.agent`"

From doc §4.2, line 296:

> "Active Timers: Registered timers (`d.timers`)"

From repository `apps/common/xanadu/vagent/runtime_abi.hpp:66–70`:

```cpp
inline constexpr std::array<std::string_view, 13> kAgentDimensionNames{
    "d.behavior", "d.skill",     "d.capacity", "d.space",  "d.member",
    "d.holon",    "d.intention", "d.subgoal",  "d.outbox", "d.belief",
    "d.pending",  "d.source",    "d.extends"};
```

From repository `apps/common/xanadu/vagent/runtime_abi.hpp:48–49`:

```cpp
/// The reused genesis dimensions (`d.vars`, `d.values`, `d.clause`, `d.grab`,
/// `d.step`, `d.spin`, `d.clone`, `d.stack`) are looked up from VortexCore.
```

**Fix:** Reconcile all prose, tables, and diagrams with `runtime_abi.hpp:66–70`:

1. Correct the count to 13 dimensions and list the actual names from `kAgentDimensionNames`:
   `d.behavior`, `d.skill`, `d.capacity`, `d.space`, `d.member`, `d.holon`, `d.intention`,
   `d.subgoal`, `d.outbox`, `d.belief`, `d.pending`, `d.source`, `d.extends`.
1. Remove `d.vars`, `d.values`, and `d.stack` from the `runtime_abi.hpp` list (clarify that they are
   reused `VortexCore` genesis dimensions).
1. Remove references to fictitious `d.agent` and `d.timers`. In §4.2, refer to scheduled timers in
   the mesh runtime and event delivery on `d.pending`. In §2.2, if linking agent declarations
   requires a dimension in the program manifest, propose it explicitly as an authored dimension
   under Proposal P17.2 rather than attributing it to `runtime_abi.hpp`.

______________________________________________________________________

### 2. (§1.1, line 20; §1.2, line 66) Persistent Code Hallucinations

Full title: Uncorrected Fictitious `CompiledProgram::instantiate` Code Snippet and Out-of-Bounds
`vortex_core.cpp` Line Numbers

**Problem:** Despite being flagged in Round 1 (W2) and accepted in `r1-disposition.md`, the document
continues to publish hallucinated C++ code and impossible line citations:

- **Uncorrected `CompiledProgram::instantiate` Code Snippet (lines 20–25):** The document quotes:

```cpp
// apps/common/xanadu/vagent/program.cpp:52
std::expected<AgentInstance, Diagnostic>
CompiledProgram::instantiate(vortex::WorkerContext &context,
                             std::string_view agentType) const {
  return Compiler::compileAgent(unit_, agentType, context);
}
```

In the repository (`apps/common/xanadu/vagent/program.cpp:50–65`), the function signature and
implementation are:

```cpp
std::expected<CompiledAgent, std::string>
CompiledProgram::instantiate(zigzag::vortex::WorkerContext &context,
                             std::string_view agentType) {
  if (!declares(agentType)) {
    return std::unexpected(
        std::format("unknown agent type '{}' in program", agentType));
  }
  Compiler compiler(context.core);
  compiler.setBeliefEventDepth(beliefEventDepth_);
  auto compiled = compiler.compileAgent(unit_, agentType);
  if (!compiled) {
    return std::unexpected(std::format(
        "{}:{}: {}: {}", agentType, compiled.error().loc.line,
        compileErrorName(compiled.error().code), compiled.error().detail));
  }
  return *compiled;
}
```

The method is non-const, returns `std::expected<CompiledAgent, std::string>`, and constructs a local
`Compiler` instance. Types `AgentInstance` and `Diagnostic` do not exist.

- **Non-Existent Lines in `vortex_core.cpp` (lines 66–68):** Lines 66–68 state: "The Vortex VM
  (`apps/common/xanadu/vortex/vortex_core.cpp:1735, 1766, 1831, 1929`) reads the jump/call target
  via `readValue` as a raw integer and casts it to `CellRef`."
  `apps/common/xanadu/vortex/vortex_core.cpp` has only 585 total lines! Lines 1735, 1766, 1831, and
  1929 do not exist. Control-flow execution for `#Jump`, `#Branch`, `#Call`, and `#Return` actually
  resides in `apps/common/xanadu/vortex/vortex_vm.cpp:655–710`, where jump targets are read via
  `toInt(inputs[0])`, not `readValue`.

**Evidence:**

- Doc lines 20–25, 66–68.
- Repo `apps/common/xanadu/vagent/program.cpp:50–65`.
- Repo `apps/common/xanadu/vortex/vortex_core.cpp` (585 lines).
- Repo `apps/common/xanadu/vortex/vortex_vm.cpp:655–710`.

**Fix:**

1. Replace the snippet on lines 20–25 with the actual code from `program.cpp:50–65`.
1. Update lines 66–68 to cite `apps/common/xanadu/vortex/vortex_vm.cpp:655–694` and describe target
   reading via `toInt(inputs[0])`.

______________________________________________________________________

## MAJOR

### 3. (§1.2, line 51; §5, line 491) Non-Existent checkpoint and wallNow Type Mismatch

Full title: Non-Existent `MeshActorRuntime::checkpoint` Function Name and Incompatible
`std::uint64_t wallNow` Signature in Proposal P17.4

- **Non-Existent Function Name (lines 51, 554):** Lines 51 and 554 cite
  `MeshActorRuntime::checkpoint` at `apps/common/xanadu/vortex/mesh_actor.cpp:251`. No member
  function named `checkpoint` exists in `MeshActorRuntime`. The public inspection method is
  `MeshActorRuntime::checkpointView` (`apps/common/xanadu/vortex/mesh_actor.hpp:239`,
  `mesh_actor.cpp:924`). Line 251 in `mesh_actor.cpp` is located inside `MeshActorRuntime::resume`.

- **Incompatible wallNow Signature in P17.4 (lines 489–492):** Proposal P17.4 proposes:

```cpp
std::expected<void, ActorError>
upgrade(const identity::ActorPid &pid, const ActorGrant &newGrant,
        std::uint64_t wallNow, const StateReconciler &reconciler);
```

Throughout `MeshActorRuntime` (`mesh_actor.hpp:155, 164, 175`), wall-clock time is consistently
typed as `std::chrono::system_clock::time_point wallNow`, while integer logical time is
`ActorTime now`. Specifying `std::uint64_t wallNow` is a signature defect that fails to compile
against callers.

**Evidence:**

- Doc lines 51, 491, 554.
- Repo `apps/common/xanadu/vortex/mesh_actor.hpp:155, 164, 175, 239`.
- Repo `apps/common/xanadu/vortex/mesh_actor.cpp:251, 924`.

**Fix:**

1. Replace `MeshActorRuntime::checkpoint` with `MeshActorRuntime::checkpointView` and cite line 924.
1. In Proposal P17.4, change `std::uint64_t wallNow` to
   `std::chrono::system_clock::time_point wallNow`.

______________________________________________________________________

### 4. (§5, lines 414, 419) Inaccurate Line Citations for ArenaManifold Constructor

Full title: Stale Line Citations for `ArenaManifold::ArenaManifold` in Proposal P17.1

**Problem:** Proposal P17.1 asserts: "ArenaManifold::arenaDims\_ is a private C++ member populated
during construction (arena_manifold.cpp:254). Constructing ArenaManifold(const Manifold \*base)
leaves arenaDims\_ empty..." and "In ArenaManifold::ArenaManifold(const Manifold \*base)
(arena_manifold.cpp:254)..." The constructor
`ArenaManifold::ArenaManifold(const Manifold *base, const xanadu::Store *store)` is defined at line
23 of `apps/common/xanadu/zigzag/arena_manifold.cpp`. Line 254 is inside `ArenaManifold::denseOf`.

**Evidence:**

- Doc §5 Proposal P17.1 (lines 414, 419).
- Repo `apps/common/xanadu/zigzag/arena_manifold.cpp:23`.

**Fix:** Update the line citations in P17.1 to reference `arena_manifold.cpp:23`.

______________________________________________________________________

## MINOR

### 5. (§2.2, line 163) Mermaid Diagram Dimension Count Mismatch

Full title: Diagram Label Asserts 10 `runtime_abi.hpp` Dimensions

**Problem:** The Mermaid diagram label for node `DIMS` reads:
`DIMS["Canonical Dimension Registry\n(44 VortexCore dims + 10 runtime_abi.hpp dims)"]` As
established in Finding 1, `runtime_abi.hpp:66–70` defines 13 dimensions, not 10.

**Evidence:**

- Doc line 163.
- Repo `apps/common/xanadu/vagent/runtime_abi.hpp:66–70`.

**Fix:** Update the label to `(44 VortexCore dims + 13 runtime_abi.hpp dims)`.

______________________________________________________________________

### 6. (§1.2, line 90) restoreState Function Signature Inaccuracy

Full title: Inaccurate Argument Names in `AgentExecutor::restoreState` Reference

**Problem:** Line 90 references `restoreState(oldArena, oldRoot) (agent_executor.cpp:3158–3294)`.
The actual function declaration in `apps/common/xanadu/vagent/agent_executor.cpp:3158` is:
`void AgentExecutor::restoreState(State &state, CellRef saved);`.

**Evidence:**

- Doc line 90.
- Repo `apps/common/xanadu/vagent/agent_executor.cpp:3158`.

**Fix:** Change the text to reference `restoreState(state, saved)`.

______________________________________________________________________

VERDICT: NOT CONVERGED
