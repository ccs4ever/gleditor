# Round 1 critique: System Architect (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 3 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§5.1, §5.2, §7 P29) Layer Inversion: Lower-Layer `vortex_stdlib` Calling Upper-Layer VAgent

Full title: Placing `std:actor:spawn_image` in `vortex_stdlib` causes a backwards dependency on
`vagent::MetaRuntime` and `VagentAdapter`

**Problem:** Proposal P29 proposes exposing `std:actor:spawn_image(root, name, grant)` by touching
`apps/common/xanadu/vortex/vortex_stdlib.cpp`. In §5.2, the lifecycle of `spawn_image` is specified
to execute Step 1: `MetaRuntime::validate` (`apps/common/xanadu/vagent/meta.cpp:490–623`), Step 3:
`MeshActorRuntime::spawn`, and Step 5: `VagentAdapter::genesis`. This creates a critical
architectural layer inversion. The `apps/common/xanadu/vortex/` subsystem is the foundational
hyperstructural virtual machine and runtime substrate shared across all Vortex languages (`VQL`,
`VPL`, `VProlog`, `Vicon`, `vagent`). The `apps/common/xanadu/vagent/` subsystem is a high-level
surface language implementation layered on top of `vortex/`. A lower-layer engine library like
`vortex_stdlib` cannot `#include "common/xanadu/vagent/meta.hpp"` or call `MetaRuntime::validate` or
`VagentAdapter::genesis`. Doing so introduces circular dependencies and pollutes generic Vortex
runtime code with VAgent-specific AST and opcode validation. Furthermore, `vortex_stdlib` has zero
dependencies on `MeshActorRuntime` (`mesh_actor.hpp`).

**Evidence:** Citations demonstrating the layer violation across engine files:

- `design/vagent-metacircular-missing-features.md:402–406`: "1. Static Opcode Validation
  (`apps/common/xanadu/vagent/meta.cpp:490–623`): `MetaRuntime::validate` audits the emitted opcode
  graph"
- `design/vagent-metacircular-missing-features.md:502`: "P29 |
  `apps/common/xanadu/vortex/vortex_stdlib.cpp`, `apps/common/xanadu/vagent/adapter.cpp` | Expose
  `std:actor:spawn_image(...)` dispatching through `MeshActorRuntime::spawn`..."
- `apps/common/xanadu/vortex/vortex_stdlib.hpp:1–50`: `VortexStdLib` includes only core Vortex and
  ZigZag types; it has zero knowledge of VAgent or `MeshActorRuntime`.
- `apps/common/xanadu/vagent/meta.hpp:24–40` & `adapter.hpp:20–40`: `vagent/` explicitly includes
  and depends on `vortex/`, never the inverse.

**Fix:** Remove `std:actor:spawn_image` from `vortex_stdlib.cpp`. Relocate image validation,
promotion, and incarnation genesis to the VAgent runtime substrate in `apps/common/xanadu/vagent/`
(`adapter.cpp` and `executor.cpp`). In VAgent surface syntax, dynamic actor spawning should be
expressed as an actor performative/action (`spawn image(programRoot) as name with grant`) mediated
by `VagentAdapter::setSpawnSender` and `MeshActorRuntime::spawn`, preserving the strict
unidirectional dependency `vagent -> vortex`.

______________________________________________________________________

### 2. (§4.2, lines 319–324, 346–365) Cross-Arena Raw `CellRef` Leaks in Distributed Swarm

Full title: Passing un-promoted in-arena `CellRef` handles across distributed actor boundaries
violates Ruling V1 and Spool-Mesh isolation invariants

**Problem:** In Pattern B (`agent CompilerCoordinator`), the compilation pipeline is divided across
four independent worker actors (`LexerWorker`, `ParserWorker`, `CheckerWorker`, `EmitterWorker`).
The sequence diagram (§4.2, lines 319–324) and coordinator implementation (§4.2, lines 346–365)
transmit raw in-arena cell handles (`ast_root: CellRef`, `image_root: CellRef`) between distinct
actors via asynchronous performatives (`tell AstConstructed(ast_root)`,
`achieve ValidateAst(ast_root)`). In `MeshActorRuntime`, each actor is allocated an independent,
isolated `WorkerContext` with its own private `ArenaManifold`
(`apps/common/xanadu/vortex/worker_context.hpp:51–74`). An ephemeral `CellRef` is a local integer
slot index in that worker's private arena; it is meaningless and unaddressable in any other actor's
arena. Passing raw `CellRef`s across actor boundaries directly violates Ruling V1 ("An agent is one
actor; there are no links between arenas. `MeshActorRuntime::spawn` gives each actor its own
`WorkerContext`") and causes dangling memory access or silent data corruption.

**Evidence:** From `design/vagent-metacircular-missing-features.md:319–324`:

```text
Parse-->>Coord: tell AstConstructed(ast_root)
Coord->>Check: achieve ValidateAst(ast_root) by CheckerAgent
Check-->>Coord: tell AstValidated(ast_root)
Coord->>Emit: achieve LowerImage(ast_root) by EmitterAgent
Emit-->>Coord: tell ImageLowered(image_root)
Coord->>Coord: let ver = std:zigzag:promote(image_root)
```

Additional repository citations:

- `design/vagent-metacircular-missing-features.md:361–365`: `CompilerCoordinator` calls
  `std:zigzag:promote(image_root)` in its own arena on an `image_root` cell that was minted inside
  `EmitterWorker`'s arena.
- `apps/common/xanadu/vortex/worker_context.hpp:70–74`: Each `WorkerContext` holds an independent
  `ArenaManifold arena`.
- `design/vagent-actor-executor.md:219`: "cross-arena links are V1-forbidden".

**Fix:** Clarify the architectural boundaries between Pattern A and Pattern B:

1. Intra-file compiler passes (lexing, parsing into an AST manifold, type checking, opcode emission)
   **must execute synchronously within a single actor turn in a single agent's arena** (Pattern A:
   `behavior StagedCompiler`), where the entire ZigZag AST manifold can be traversed and mutated
   with zero serialization overhead and zero cross-arena leaks.
1. Reposition Pattern B (`agent CompilerCoordinator`) to operate at **module/compilation-unit
   granularity**: independent worker actors compile whole `.vagent` units within their private
   arenas, promote the resulting image to the store via `std:zigzag:promote`, and transmit the
   resulting immutable `MicroversionId` string across the mesh. Receiving actors then fold that
   microversion into a read-only base manifold (`ActorLineage{base, store}`).

______________________________________________________________________

### 3. (§3.2, lines 215–218, §7 P27) Missing Mutable `Store&` and `parentVersion` in `promote`

Full title: `std:zigzag:promote` lacks mutable Store binding, parent microversion tracking, and
capability authorization

**Problem:** Proposal P27 proposes adding `promote(root: CellRef) -> string` to `std:zigzag` in
`apps/common/xanadu/vortex/vortex_stdlib.cpp`, specifying that it executes
`zigzag::promote(store, parentVersion, arena, root)` (`arena_manifold.hpp:930–934`). However,
`zigzag::promote` requires a non-const `xanadu::Store &store` and
`const xanadu::MicroversionId &parent`. In `vortex_stdlib.hpp:235–239, 505`, `VortexStdLib` holds
only an optional `const xanadu::Store *boundStore_{nullptr}`, which is strictly read-only and
carries no head microversion tracking. `WorkerContext` and `VortexCore` do not own or expose a
mutable `Store &`. Furthermore, the design fails to address capability authorization: if an actor
executes under an `ActorGrant` with `grant.immutable == true` (`mesh_actor.hpp:69`), store mutation
must be refused. The proposal specifies neither where `VortexStdLib` retrieves the mutable
`Store &`, nor where `parentVersion` is maintained, nor how concurrent promotions across worker
contexts are synchronized.

**Evidence:** Repository signatures and design claims:

- `design/vagent-metacircular-missing-features.md:215–218`: "5. `promote(root: CellRef) -> string`:
  Executes `zigzag::promote(store, parentVersion, ...)` (`arena_manifold.hpp:930–934`). Walks
  reachable subgraph from `root`, commits to `Store`..."
- `apps/common/xanadu/zigzag/arena_manifold.hpp:930–934`:
  `promote(xanadu::Store &store, const xanadu::MicroversionId &parent, ...)`
- `apps/common/xanadu/vortex/vortex_stdlib.hpp:235–239, 505`: Only
  `const xanadu::Store *boundStore_{nullptr}` exists.
- `apps/common/xanadu/vortex/mesh_actor.hpp:69`:
  `struct ActorGrant { ... bool immutable{false}; };`.

**Fix:** Specify the capability and store-session architecture for `promote`:

1. Verify that `promote` checks the caller's capability grant, immediately refusing with
   `ActorError::CapabilityRefused` if `grant.immutable` is true.
1. Bind an explicit mutable store session (`LineageStoreSession` or active `StoreSink`) to the
   executing actor's context that provides access to the mutable `Store &` and tracks the current
   head `MicroversionId parent`.
1. Update Proposal P27 to isolate this store-session capability binding in `WorkerContext` /
   `MeshActorRuntime` rather than assuming ambient store access in `vortex_stdlib`.

______________________________________________________________________

## MAJOR

### 4. (§2.2, line 149; §3.2, line 209) Hallucinated `consumeWork` Signature & `ValueKind::String`

Full title: Factually inaccurate C++ claims regarding `ArenaManifold::consumeWork` and `ValueKind`

**Problem:** The design makes two factual claims about existing C++ interfaces that contradict the
codebase:

1. In §2.2, line 149, the document asserts: "Work consumption:
   `WorkerContext::arena.consumeWork(file_byte_size)` prevents memory exhaustion." In the
   repository, `ArenaManifold::consumeWork()` takes `void` and decrements `workRemaining_` by 1
   evaluation step (`arena_manifold.hpp:486–492`). It does not accept byte counts or meter memory
   allocations (governed by `allocationLimit_` and `scratchLimit_`). Calling
   `consumeWork(file_byte_size)` will fail to compile.
1. In §3.2, line 209, the document asserts: "`set_value(cell: CellRef, value: any) -> CellRef`:
   Updates the scalar value and value kind (`Int64`, `Double`, `String`, `None`) on an existing cell
   (`...setValueBits(...)`)." `ValueKind` (`apps/common/xanadu/ops.hpp:154–163`) contains `None`,
   `Double`, `Bool`, `Int64`, `OpHandle`, `ExternRef`, `Timestamp`, and `ForeignOpHandle`. There is
   no `String` kind. 64-byte `CompactOpNode` scalar bits store 64-bit primitive numbers and
   pointers. Textual strings are primedia text payloads addressed by spans (`setContent` /
   `arena.makeCell(string_view)`), not scalar value bits.

**Evidence:** Codebase declarations:

- `apps/common/xanadu/zigzag/arena_manifold.hpp:486–492`:
  `void consumeWork() const { if (workRemaining_) { ... --*workRemaining_; } }`
- `apps/common/xanadu/ops.hpp:154–163`:
  `enum class ValueKind : std::uint8_t { None = 0, Double = 1, Bool = 2, Int64 = 3, ... };` (no
  `String`).

**Fix:** Address the inaccuracies directly:

1. Correct §2.2: file ingestion bounds checking must enforce `file_size <= grant.maxOutputBytes` and
   charge memory against `arena.setExecutionLimits(...)` / memory allocation caps, while
   `consumeWork()` is invoked during turn execution.
1. Correct §3.2: document that `set_value` updates 64-bit scalar value bits (`Int64`, `Double`,
   `Bool`, `None`), while string text updates must use dedicated primedia content APIs (`set_text`
   or `make_cell(text)`).

______________________________________________________________________

### 5. (§4.2, lines 332–367) Unbounded Actor Lifecycle Leak in Swarm Coordinator

Full title: Dynamic worker spawning without termination leaks actors, member cells, and mailboxes

**Problem:** In `agent CompilerCoordinator`, every compilation job spawns four new worker actors
(`spawn LexerWorker()`, `spawn ParserWorker()`, `spawn CheckerWorker()`, `spawn EmitterWorker()`).
None of these spawned actors are ever terminated or reaped. In `MeshActorRuntime`, spawning an actor
allocates an `ActorPid`, an entry in `actors_` (`mesh_actor.hpp:334`), a member cell on `d.agents`,
and mailbox ranks (`+d.pending`, `+d.outbox`). Spawning four un-reaped actors per compilation job
without an explicit lifecycle termination model causes an unbounded retention leak of actor handles,
memory, and cells in long-running builds.

**Evidence:** Repository evidence of actor allocations:

- `design/vagent-metacircular-missing-features.md:342, 347, 352, 357`: Spawns `LexerWorker`,
  `ParserWorker`, `CheckerWorker`, `EmitterWorker` per phase.
- `apps/common/xanadu/vortex/mesh_actor.hpp:334`:
  `std::map<std::string, std::unique_ptr<Actor>> actors_;`.
- `apps/common/xanadu/vagent/adapter.cpp:356–385`: `stop` percept handling and child orderly
  shutdown protocol.

**Fix:** Require an explicit lifecycle management pattern in §4.2:

- Either make the workers persistent singletons/pooled actors initialized once in the coordinator's
  `on Initialize` handler and reused across jobs; or
- Have worker actors complete their task by sending their result performative and then explicitly
  terminating via a `stop` percept / performative as supported by `VagentAdapter`.

______________________________________________________________________

### 6. (§6.3, §7 P30) Missing `.vagent` Metacircular Parity and Macro Desugaring Alternative

Full title: Proposal P30 omits necessary `.vagent` compiler passes and fails to evaluate macro
desugaring in `expand.vagent`

**Problem:** Proposal P30 specifies implementing `match` expressions and tuple destructuring in C++
(`parser.cpp`, `checker.cpp`, `compiler.cpp`), but lists only `stdlib/parser.vagent` on the VAgent
side, omitting `stdlib/expand.vagent`, `stdlib/checker.vagent`, and `stdlib/emitter.vagent`. In
§6.3, the text explicitly asserts: "Exhaustiveness is verified statically by `checker.vagent`;
non-exhaustive matches emit compile-time diagnostic `E310: Non-exhaustive match`." If
`checker.vagent` verifies exhaustiveness and `emitter.vagent` lowers pattern arms into decision
trees, they must be included in the Proposal Register. Furthermore, per `GEMINI.md` ("New C++ code
must justify why it isn't being written in Vortex"), the design fails to evaluate whether `match`
can be desugared into existing `if/then/else` cascades and `#ZZ_STEP` hops inside
`stdlib/expand.vagent` (the AST expansion and desugaring pass). Desugaring in `expand.vagent` would
avoid modifying C++ `compiler.cpp` and `emitter.vagent`, maintaining a lean metacircular compiler
core.

**Evidence:** Codebase citations:

- `design/vagent-metacircular-missing-features.md:488–490, 503`.
- `apps/common/xanadu/vagent/stdlib/expand.vagent:1–45`: `AstExpander` is the established pass for
  path desugaring and syntactic peeling.
- `GEMINI.md`: "New C++ code must justify why it isn't being written in Vortex."

**Fix:** Update Proposal P30:

1. Include `stdlib/expand.vagent`, `stdlib/checker.vagent`, and `emitter.vagent` in the proposal
   register for complete metacircular parity.
1. In §6.3 and §8, evaluate desugaring `match` within `expand.vagent` versus native opcode lowering,
   justifying why AST desugaring in Vortex is or is not sufficient.

______________________________________________________________________

## MINOR

### 7. (§2.2, lines 135–142, §7 P26) Global Host CLI `args()` Leaking Into Actor Abstraction

Full title: `std:env:args` exposes global process CLI arguments instead of actor invocation params

**Problem:** `std:env:args() -> list[string]` returns host invocation arguments. In a distributed or
multi-actor mesh runtime, exposing the host CLI process argument vector globally breaks actor
encapsulation; child actors spawned dynamically via `spawn_image` require their own invocation
parameters rather than inheriting the root process's CLI arguments.

**Fix:** Clarify that `std:env:args()` reflects the actor's scoped invocation arguments carried in
`ActorMessage` / `ActorGenesis`, defaulting to the process CLI arguments only for the root actor.

______________________________________________________________________

### 8. (§5.1, lines 378–388) Unspecified VAgent Representation of Actor Types

Full title: Surface signature uses C++ struct names without defining their VAgent record schemas

**Problem:** Section 5.1 defines `spawn_image(..., grant: Option[ActorGrant]) -> Result[...]`,
referencing types that currently exist only as C++ structs in `mesh_actor.hpp`. The document does
not specify their surface VAgent record or enum definitions in `apps/common/xanadu/vagent/stdlib/`.

**Fix:** Define the concrete VAgent record and enum schemas for `ActorPid`, `ActorGrant`, and
`ActorError` in §5.1 or standard library declarations.

______________________________________________________________________

VERDICT: NOT CONVERGED
