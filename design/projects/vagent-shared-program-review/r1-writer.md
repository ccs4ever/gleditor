# Round 1 critique: Technical Writer (`design/vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root.
Tally: 3 BLOCKING, 5 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§1.1, line 31) Fictitious d.vagent.\* Dimensions Contradict Runtime ABI

Full title: Invention of Fictitious `d.vagent.*` Namespace and Contradiction of Canonical Dimension
Schema

**Problem:**
The specification asserts in §1.1 (lines 31–34) and §2.2 (lines 195–197) that the existing `vagent`
runtime compiles into 13 `d.vagent.*` dimensions:
`d.vagent.field`, `d.vagent.belief`, `d.vagent.pred`, `d.vagent.arg`, `d.vagent.intent`,
`d.vagent.plan`, `d.vagent.step`, `d.vagent.goal`, `d.vagent.event`, `d.vagent.space`,
`d.vagent.entry`, `d.vagent.child`, `d.vagent.type`. It then proposes in §2.4 (lines 278–279) and
§6.1 (line 711) to add `d.vagent.skill` and `d.vagent.behavior` as "two new canonical dimensions,"
bringing the count to 15.

This is factually incorrect and contradicts the live repository code as well as the companion
specification [`design/vagent-agent-language.md`](../../vagent-agent-language.md):

1. In the actual codebase (`apps/common/xanadu/vagent/runtime_abi.hpp:67–87`) and
   `apps/common/xanadu/vagent/agent_executor.cpp:3165–3280`, the 13 canonical `vagent` dimensions
   are:
   `d.behavior`, `d.skill`, `d.capacity`, `d.space`, `d.member`, `d.holon`, `d.intention`,
   `d.subgoal`, `d.outbox`, `d.belief`, `d.pending`, `d.source`, `d.extends`.
1. Crucially, `d.skill` and `d.behavior` are **already** part of the 13 dimensions defined in
   `runtime_abi.hpp:67` and `vagent-agent-language.md` V10 (lines 206–216). They are not "new
   dimensions."
1. In `vagent-agent-language.md` (§5) and `runtime_abi.hpp`, fields live on `d.vars`/`d.values`
   (matching Vortex conventions), child agents live on `d.holon`, hosted space peers live on
   `d.space`/`d.member`, intentions and subgoals live on `d.intention`/`d.subgoal`, and beliefs and
   clauses live on `d.belief`/`d.clause`.
1. The design document invents a completely separate, un-namespaced 15-dimension topology
   (`d.vagent.*`), misattributes it to the current implementation, and provides no proposal
   (`P17.*`) or migration justification to overturn the established ABI.

**Evidence:**

- Doc §1.1 (lines 31–34): "All 13 `vagent` dimensions (`d.vagent.field`, `d.vagent.belief`,
  `d.vagent.pred`, `d.vagent.arg`, `d.vagent.intent`, `d.vagent.plan`, `d.vagent.step`,
  `d.vagent.goal`, `d.vagent.event`, `d.vagent.space`, `d.vagent.entry`, `d.vagent.child`,
  `d.vagent.type`) on top of the 44 `VortexCore` genesis dimensions."

- Doc §2.4 (lines 278–279): "using two new canonical dimensions in the program manifold
  (`d.vagent.skill` and `d.vagent.behavior`, bringing the `vagent` dimension set to 15
  dimensions)..."

- Doc §3.3 (lines 359–380): Entire symbolic root layout keyed by `d.vagent.*`.

- Repo `apps/common/xanadu/vagent/runtime_abi.hpp:67–70`:

  ```cpp
  inline constexpr std::array<std::string_view, 13> kAgentDimensionNames{
      "d.behavior", "d.skill",     "d.capacity", "d.space",  "d.member",
      "d.holon",    "d.intention", "d.subgoal",  "d.outbox", "d.belief",
      "d.pending",  "d.source",    "d.extends"};
  ```

- Repo `apps/common/xanadu/vagent/agent_executor.cpp:3193–3280`: Uses `d.vars`, `d.values`,
  `d.grab`, `d.step`, `d.belief`, `d.clause`, `d.space`, `d.member`, `d.holon`, `d.intention`,
  `d.subgoal`.

- Spec `design/vagent-agent-language.md:206–216` (V10) and §5 (lines 372–382, 463–467): Confirms
  the same 13 dimensions.

**Fix:**
Eliminate all references to the fictitious `d.vagent.*` namespace across prose, tables, and
diagrams. Reconcile the layout with the established 13 dimensions from `runtime_abi.hpp` and
`vagent-agent-language.md` V10:

- Use `d.vars` and `d.values` for fields on `$agent`, skills, and behaviors.
- Use `d.skill`, `d.behavior`, and `d.capacity` for mounted extensions.
- Use `d.intention` and `d.subgoal` for intention tracking.
- Use `d.belief` and `d.clause` for the belief base.
- Use `d.holon` for spawned children and `d.space`/`d.member` for tuple spaces.

______________________________________________________________________

### 2. (§1.2, line 66) Non-Existent C++ Source Files and API Signatures

Full title: Citations of Non-Existent Files (`core.cpp`), Fictitious Functions, and Inaccurate
Signatures

**Problem:**
The document repeatedly cites non-existent repository files, non-existent functions, and invalid API
signatures:

1. Lines 66, 261, 265, and 615 cite `apps/common/xanadu/vortex/core.hpp` and `core.cpp`. Neither
   file exists in the repository. The actual runtime files are
   `apps/common/xanadu/vortex/vortex_core.{hpp,cpp}`, `vortex_vm.{hpp,cpp}`, and
   `worker_context.hpp`.
1. Proposal P17.3 (line 615) explicitly names `core.hpp` and `core.cpp` as files to modify.
1. Lines 71, 261, and 265 cite non-existent functions:
   - `readScalarValue(context.arena, *targetCell, context.StringInterner)` (does not exist;
     `StringInterner` is not a member of `WorkerContext`, and `readScalarValue` is not in the repo).
   - `resolveOperandCell(context.arena, slotCell)` (does not exist; operand resolution in
     `VortexCore` is handled via `inputsOf` and `wingOf` in `vortex_core.cpp:357–373`).
   - `writeScalarValue(context.arena, *outputCell, ...)` (does not exist; opcode outputs are
     written via `core_.value(outCells[i], 0, -1, newOutputs[i])` in `vortex_vm.cpp:1118–1120`).
1. Lines 20–25 quote `CompiledProgram::instantiate` as returning
   `std::expected<AgentInstance, Diagnostic>` with `(WorkerContext &, std::string_view) const`.
   In reality (`apps/common/xanadu/vagent/program.cpp:50–65`), it returns
   `std::expected<CompiledAgent, std::string>`, is non-const, and instantiates
   `Compiler compiler(context.core);` before calling `compiler.compileAgent(unit_, agentType);`.

**Evidence:**

- Doc lines 20–25, 66–72, 260–266, 615.

- Repo `apps/common/xanadu/vortex/`: contains `vortex_core.hpp`, `vortex_core.cpp`, `vortex_vm.hpp`,
  `vortex_vm.cpp`, `worker_context.hpp`; no `core.hpp` or `core.cpp`.

- Repo `apps/common/xanadu/vortex/vortex_vm.cpp:655–680`: Opcode jumps and calls use
  `CellRef targetOp = toInt(inputs[0]);`, not `readScalarValue`.

- Repo `apps/common/xanadu/vagent/program.cpp:50–52`:

  ```cpp
  std::expected<CompiledAgent, std::string>
  CompiledProgram::instantiate(zigzag::vortex::WorkerContext &context,
                               std::string_view agentType)
  ```

**Fix:**
Replace all citations of `core.hpp`/`core.cpp` with `vortex_core.hpp`/`vortex_core.cpp` and
`vortex_vm.hpp`/`vortex_vm.cpp`. Update Proposal P17.3 to target `worker_context.hpp`,
`mesh_actor.hpp`, and `mesh_actor.cpp`. Replace descriptions of `readScalarValue`,
`resolveOperandCell`, and `writeScalarValue` with actual `VortexCore` (`inputsOf`/`wingOf`) and
`VortexVM` (`executeOpcodeBody`) mechanisms. Correct the `instantiate` signature.

______________________________________________________________________

### 3. (§4.2, line 411) Hallucinated Actor Records and Move-Assignment

Full title: Hallucinated `ActorEntry`, Phantom `MeshFlight` Types, and Moving Non-Movable
`WorkerContext`

**Problem:**

1. Line 411 cites `MeshActorRuntime::ActorEntry` at `mesh_actor.hpp:404`. `mesh_actor.hpp` has only
   271 lines. The actual struct is private `struct MeshActorRuntime::Actor` declared in
   `mesh_actor.hpp:242` and defined in `apps/common/xanadu/vortex/mesh_actor.cpp:55–82`.
1. Lines 417–427 introduce `actor->flights`, `MeshFlight`, and `EffectStatus::Pending` as existing
   runtime mechanisms. None of these types, enums, or struct members exist in `MeshActorRuntime` or
   anywhere in `apps/common/xanadu/`.
1. Proposal P17.4 (line 676) specifies: `actor->context = std::move(newContext)` where `newContext`
   is an instance of `WorkerContext`. However:
   - `WorkerContext` explicitly deletes move operations (`WorkerContext(WorkerContext &&) = delete;`
     in `apps/common/xanadu/vortex/worker_context.hpp:59–60`) because its member references into
     `arena` bind `core`, `vm`, and `stdlib` permanently to the context's address.
   - `MeshActorRuntime::Actor::context` is a `std::unique_ptr<WorkerContext>` (`mesh_actor.cpp:66`).
     Direct move-assignment of a `WorkerContext` object does not compile.

**Evidence:**

- Doc lines 411, 417–427, 676.

- Repo `apps/common/xanadu/vortex/mesh_actor.hpp:242` and `mesh_actor.cpp:55–82`: Defines
  `struct MeshActorRuntime::Actor`.

- Repo `apps/common/xanadu/vortex/worker_context.hpp:59–60`:

  ```cpp
  WorkerContext(WorkerContext &&)                 = delete;
  WorkerContext &operator=(WorkerContext &&)      = delete;
  ```

- Repo `apps/common/xanadu/vortex/mesh_actor.cpp:66`:

  ```cpp
  std::unique_ptr<WorkerContext> context;
  ```

**Fix:**

1. Refer to `MeshActorRuntime::Actor` (or the public view `ActorExecution` / `ActorCheckpointView`).
1. Clearly declare `flights` and asynchronous host effect tracking as a new feature under Proposal
   P17.4 with explicit struct definitions, rather than asserting they already exist.
1. Correct the context swap in P17.4 to use `std::unique_ptr<WorkerContext>`:
   `actor->context = std::make_unique<WorkerContext>(newProgramManifold, ...);`.

______________________________________________________________________

## MAJOR

### 4. (§1–§6) Missing Refused Alternatives Section (House Style)

Full title: Specification Lacks Required Refused Alternatives Section

**Problem:**
Under Standing Rule 1 and SKILL.md §1 & §2.1, design specifications in this repository must record
refused alternatives along with the technical reasons and prices for their rejection. The document
ends at §6.2 (line 737) without any Refused Alternatives section.

Critical design choices lack documented alternatives:

1. Re-compiling AST per agent on resume vs. sharing a sealed base `Manifold`.
1. Process migration with PID changes vs. in-place live upgrade under the same `ActorPid`.
1. Serializing agent state as JSON/bencode blobs vs. topological promotion of ephemeral overlay
   deltas.
1. Mid-turn preemption vs. strict quiescent turn-boundary upgrades.
1. Flat opcode relocation tables vs. P16 `OpHandle` handles and `d.mesh.relocation`.

**Evidence:**

- Repo Standing Rule 1 (`SKILL.md:54–56`): "Plans are argued, not asserted: cite exact code
  (`file:line`), price each ruling (bytes, cycles, links, messages, segments), and record refused
  alternatives."
- `design/vagent-shared-program-and-live-upgrade.md`: Contains no §7 or Refused Alternatives
  section.

**Fix:**
Add §7 "Refused Alternatives" documenting each rejected design option, its fatal flaw, and its price
relative to the chosen hyperstructural approach.

______________________________________________________________________

### 5. (§1–§6) Missing Open Questions Table (House Style)

Full title: Specification Lacks Required Open Questions Section and Table

**Problem:**
SKILL.md §2.1 checklist explicitly requires auditing whether the document contains an Open
Questions table. The specification omits this required section entirely.

Multiple unpriced edge cases and lifecycle questions remain unaddressed in the text:

1. Timeout and abort policies if external host completions never arrive during a `DrainFlights`
   barrier.
1. Resolution semantics when multiple concurrent upgrade requests arrive for the same `ActorPid`.
1. Eviction, lifecycle, and memory reclamation of unreferenced `programRef` manifolds on a host.
1. Maximum allowable size and link-budget thresholds for the overlay state delta during checkpoint
   promotion.

**Evidence:**

- `SKILL.md:89–90`: "Does the document follow house style (priced rulings, cited `file:line`
  evidence, refused alternatives section, open questions table)?"
- `design/vagent-shared-program-and-live-upgrade.md`: Contains no Open Questions table.

**Fix:**
Add §8 "Open Questions" with a Markdown table detailing Question ID, Title, Status, Default Policy,
and Impacted Proposals.

______________________________________________________________________

### 6. (§5, lines 570–703) Proposals P17.1–P17.5 Lack Pricing and Fallbacks

Full title: Proposals P17.1–P17.5 Omit Required Resource Pricing and Fallback Strategies

**Problem:**
Standing Rule 6 and SKILL.md §1 dictate that every proposal in the Proposal Register must state the
exact files touched, why the change cannot live in the caller/compiler, the cost (priced in bytes,
cycles, links, messages, segments), and the fallback if not approved.

In §5, proposals `P17.1` through `P17.5` provide "Files", "Why", and "Exact Change", but completely
omit:

1. **Price / Cost:** No byte, cycle, link, or segment accounting is provided for dimension seeding,
   program store sealing, runtime registry lookups, context swapping, or manifest verification.
1. **Fallback:** No fallback mechanism is specified if any proposal (e.g. P17.1 or P17.4) is
   refused by the codebase owner.

**Evidence:**

- Repo Standing Rule 6 (`SKILL.md:67–70`): "Any change to existing C++ engine, core, stdlib, or
  runtime files must be isolated in a numbered Proposal Register (`P1`, `P2`, ...) stating the exact
  files touched, why the change cannot live in the caller/compiler, the cost, and the fallback if
  not approved."
- Doc §5 (lines 570–703): Subsections P17.1–P17.5 lack "Price" and "Fallback" entries.

**Fix:**
Expand each proposal in §5 with explicit `Cost / Price` (measuring memory overhead, cycle latency,
and OSMIC operations) and `Fallback` subsections.

______________________________________________________________________

### 7. (§4.3.5, line 542) Surface Type Inconsistency: String vs Str

Full title: Inconsistent Primitive Type `String` Used Instead of `vagent` Canonical `Str`

**Problem:**
Lines 542, 549, and 691 define the upgrade lifecycle hook as `on Upgrade(from_version: String)`.
In the `vagent` agent language specification (`design/vagent-agent-language.md` lines 654, 1195,
1467), the built-in primitive string type is `Str`, not `String`. Using `String` causes the grammar
production and code snippet to fail static type checking under `vagent`'s own type rules.

The common review brief (`design/projects/vagent-shared-program-review/brief-common.md:24`)
explicitly mandated `on Upgrade(from_version: Str)`.

**Evidence:**

- Doc line 542: `#### 4.3.5 Optional Lifecycle Hook: on Upgrade(from_version: String)`
- Doc line 549: `on Upgrade(from_version: String) {`
- Doc line 691: `1. **on Upgrade(from_version: String) handler:**`
- Spec `design/vagent-agent-language.md:1195`: lists types as `Int`, `Float`, `Str`, `Bool`, `()`.
- Brief `brief-common.md:24`: `on Upgrade(from_version: Str)`.

**Fix:**
Replace `String` with `Str` across §4.3.5 and §5 (Proposal P17.5).

______________________________________________________________________

### 8. (§2.2, line 167) Mermaid Diagram Dimension Count and Duplicate Stack

Full title: Architecture Diagram Contradicts Text Dimension Count and Redundantly Lists `d.stack.*`

**Problem:**
In §2.2 (line 167), the Mermaid diagram label for node `DIMS` reads:
`44 VortexCore dims + 13 d.vagent.* dims + d.stack.*`

This introduces two inconsistencies:

1. It contradicts §2.4 (line 278) and §6.1 (line 711), which specify 15 dimensions.
1. It lists `d.stack.*` as an extra category outside the 44 `VortexCore` genesis dimensions. In
   reality, `d.stack` is already one of the core genesis dimensions (`dims_.stack` in
   `apps/common/xanadu/vortex/vortex_core.hpp:39`).

**Evidence:**

- Doc line 167:
  `DIMS["Canonical Dimension Registry\n44 VortexCore dims + 13 d.vagent.* dims + d.stack.*"]`
- Doc lines 278–279: "bringing the `vagent` dimension set to 15 dimensions"
- Repo `apps/common/xanadu/vortex/vortex_core.hpp:39`: `DimRef stack{noCell};` is a member of
  `SystemDimensions`.

**Fix:**
Update the diagram label in §2.2 to reflect the actual canonical dimensions without redundant
`d.stack.*` additions.

______________________________________________________________________

## MINOR

### 9. (§2.2, line 191) Typo in Code Span (d. Next)

Full title: Extraneous Space Inside Code Span for Genesis Dimension `d.Next`

**Problem:**
Line 191 contains a typographical error: `` `d. Next` `` has an extraneous space between the period
and the dimension name.

**Evidence:**

- Doc line 191: `` (`d.First`, `d. Next`, `d.grab`, ... ``

**Fix:**
Change `` `d. Next` `` to `` `d.Next` ``.

______________________________________________________________________

### 10. (§1.2, lines 50–51) Inaccurate mesh_actor.cpp Line Citations

Full title: Outdated Line Numbers Cited for `MeshActorRuntime` Member Functions

**Problem:**
The specification cites stale line numbers for functions in
`apps/common/xanadu/vortex/mesh_actor.cpp`:

- Line 50: `MeshActorRuntime::checkpoint` (`mesh_actor.cpp:251`) -> `checkpointView` is at line
  924\.
- Line 51: `MeshActorRuntime::resume` (`mesh_actor.cpp:277`) -> `resume` is at line 214.
- Line 617: `MeshActorRuntime::spawn` (`mesh_actor.cpp:188`) -> `spawn` is at line 152.
- Line 641: `MeshActorRuntime::renew` (`mesh_actor.cpp:651`) -> `renew` is at line 635.

**Evidence:**

- Repo `apps/common/xanadu/vortex/mesh_actor.cpp`: lines 152 (`spawn`), 214 (`resume`), 635
  (`renew`), and 924 (`checkpointView`).

**Fix:**
Update the line citations in §1.2 and §5 to match the current repository line positions.

______________________________________________________________________

### 11. (§4.1, line 400) Mailbox Queue Referred to as inbound

Full title: Actor Mailbox Queue Inaccurately Named `inbound` Instead of `mailbox`

**Problem:**
The document repeatedly refers to the actor's inbound message queue as `actor->inbound` and invokes
`inbound.pop()`. In `MeshActorRuntime::Actor`, the queue is declared as
`std::deque<std::pair<ActorMessage, std::size_t>> mailbox;` (`mesh_actor.cpp:69`).

**Evidence:**

- Doc lines 400, 414, 422, 678: `inbound.pop()`, `actor->inbound`.

- Repo `apps/common/xanadu/vortex/mesh_actor.cpp:69`:

  ```cpp
  std::deque<std::pair<ActorMessage, std::size_t>> mailbox;
  ```

**Fix:**
Replace `inbound` with `mailbox` (or clarify that `inbound` refers to the logical message stream).

______________________________________________________________________

VERDICT: NOT CONVERGED
