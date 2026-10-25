# Round 1 critique: Codebase Expert (`codebase`) (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root
(`/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7`). Tally: 5
BLOCKING, 5 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§5 P17.1, line 585) Overlay Promotion Violates `Store::setLink` Invariant

Full title: Proposal P17.1 Overlay-Only Promotion Violates `Store::setLink` Invariant
(`requireCellOp`)

**Problem:** Proposal P17.1 and §3.2 state that `promoteOverlay` serializes only ephemeral overlay
cells into a checkpoint `targetStore` while recording non-ephemeral base `CellRef`s
(`!isEphemeral(ref)`) directly by their canonical base index in `programManifold`. In OSMIC, a
`Store` cannot record a `setLink` operation pointing to an operation index that does not exist in
its own `SegmentedOpsSpool`. If `targetStore` is a fresh store created for the checkpoint, invoking
`store.setLink` with base dimension `CellRef`s throws `std::invalid_argument` and crashes the
checkpoint.

**Evidence:**

From doc §5, lines 585–589:

> "Add `zigzag::promoteOverlay(const ArenaManifold &arena, CellRef root, Store *targetStore)` ...
> that serializes only the overlay cells reachable from `root` while recording non-ephemeral base
> `CellRef`s (`!isEphemeral(ref)`) by their canonical base index in `programManifold`."

From doc §3.2, lines 333–337:

> "Any reference to an unshadowed base cell ... (such as a dimension `DimRef` ... or a suspended
> plan step's base `CellRef`) is already non-ephemeral and is recorded as a base reference (`b`),
> **not** copied into the state checkpoint!"

From repository `apps/common/xanadu/store.cpp:898–902`:

```cpp
requireCellOp(from, "the cell a link is from");
requireCellOp(dim, "the dimension a link is along");
if (zigzag::noCell != to) {
  requireCellOp(to, "the cell a link is to");
}
```

From repository `apps/common/xanadu/store.cpp:609–625`:

```cpp
void Store::requireCellOp(const zigzag::CellRef ref, const char *what) const {
  const auto *const node = opsSpool.get(ref);
  if (nullptr == node || OpKind::Structure != node->kind) {
    throw std::invalid_argument(
        std::format("{} has no operation behind it", what));
  }
}
```

In a fresh checkpoint `Store`, `opsSpool` contains only operations appended to that store. Calling
`setLink` with a base `DimRef` (e.g. `dim = 14` from `programStore`) causes `opsSpool.get(14)` to
return `nullptr` and throw.

**Fix:** Specify that the agent's checkpoint `Store` is not a disconnected empty store, but an OSMIC
branch whose ancestral spool history begins at `programRef`'s sealed frontier (or explicitly specify
how `mesh_value.cpp` encodes cross-store base references via `baseRef` and `GlobalOpRef`).

### 2. (§3.2, line 326) Turn Scratch Cleanup via `mark()` Destroys Live State

Full title: Turn Scratch Cleanup via `ArenaManifold::mark()` Rolls Back All Turn Mutations

**Problem:** §3.2 claims that at the start of a turn, the executor takes an
`ArenaManifold::Mark turnMark = arena.mark()` and releases it after the turn to clean up transient
scratch computations and shadowed opcode output slots. In `ArenaManifold`, `release(turnMark)` is a
stack truncation that rolls back all slot allocations, link additions, and trail entries created
after `turnMark`. Releasing `turnMark` at turn end destroys every field mutation, belief assertion,
and intention posted during the turn. Furthermore, `ArenaManifold` provides no mechanism to
selectively unshadow individual cells.

**Evidence:**

From doc §3.2, lines 326–329:

> "At the start of a turn, the executor records an `ArenaManifold::Mark turnMark = arena.mark()`
> before running transient scratch computations, or resets shadowed `-d.grab` output slots after the
> turn completes, so only `$agent` state mutations and handle updates remain in `overlay_`."

From repository `apps/common/xanadu/zigzag/arena_manifold.cpp:1659–1679`:

```cpp
for (std::size_t i = trail_.size(); i > m.trailSize; i--) {
  const auto &entry   = trail_[i - 1];
  slots_[entry.dense] = entry.saved;
}
trail_.resize(m.trailSize);
for (std::size_t i = shadowOrder_.size(); i > m.shadowCount; i--) {
  overlay_.erase(shadowOrder_[i - 1]);
}
shadowOrder_.resize(m.shadowCount);
slots_.resize(m.cellCount);
links_.resize(m.linkSize);
```

`release(m)` reverts the entire arena back to its state at `m`. Any state cell minted or mutated
between `turnMark` and `release(turnMark)` is deleted.

**Fix:** Remove the claim that `mark()`/`release()` cleans up turn scratch across state mutations.
Specify that transient scratch computations either run in a child arena, use dedicated ephemeral
scratch registers, or that registers are cleared explicitly by writing zero/unit without rolling
back the arena stack.

### 3. (§1.2, line 63) Hallucinated VM Execution Architecture and `core.cpp`

Full title: Complete Hallucination of Core VM Execution Architecture and Non-Existent `core.cpp`

**Problem:** The design bases its memory model on the assertion that the Vortex VM executes from
`core.cpp` using `readScalarValue` and `writeScalarValue`, and that compiled opcodes mutate their
`-d.grab` output slot cells during execution, requiring copy-on-write shadowing in `ArenaManifold`.
There is no file named `core.cpp` in the repository, and no functions named `readScalarValue`,
`writeScalarValue`, or `resolveOperandCell` exist anywhere in `apps/`. In the actual Vortex VM
(`vortex_vm.cpp:308–322`), opcodes do NOT write to their `-d.grab` slot cell; the `-d.grab` slot
cell contains an immutable `CellRef` pointing to the target register cell, and the VM writes the
evaluated value into that target cell.

**Evidence:**

From doc §1.2, lines 65–72:

> "The Vortex VM (`core.cpp:1735`, `1766`, `1831`, `1929`) reads the jump/call target via
> `readScalarValue` as a raw `uint64_t` and casts it to `CellRef`:
>
> ````cpp
> const auto target = static_cast< CellRef >(
>     readScalarValue(context.arena, *targetCell, context.StringInterner));
> ```"
> ````

From doc §2.3, lines 260–266:

> "reading `stateH_` via `resolveOperandCell(context.arena, slotCell)` (`core.cpp:178`)" "via
> `writeScalarValue(context.arena, *outputCell, ...)` (`core.cpp:104`)"

Repository reality: No `apps/common/xanadu/vortex/core.cpp` exists. The files are `vortex_core.cpp`
and `vortex_vm.cpp`.

From `apps/common/xanadu/vortex/vortex_vm.cpp:655–661`:

```cpp
case OpcodeKind::Jump: {
  if (!inputs.empty() && cursor != noCell) {
    CellRef targetOp = toInt(inputs[0]);
    setCursorOpcode(cursor, targetOp);
    jumped = true;
  }
  break;
}
```

From `apps/common/xanadu/vortex/vortex_vm.cpp:316–320`:

```cpp
for (std::size_t i = 0; i < std::min(outCells.size(), outVals.size()); ++i) {
  core_.value(outCells[i], 0, -1, outVals[i]);
}
```

The slot cell on `-d.grab` is read by `core_.outputsOf` (`vortex_core.cpp:337–370`), and
`core_.value` writes to the register `outCells[i]`, NOT to the `-d.grab` slot cell.

**Fix:** Replace all citations to `core.cpp` and hallucinated helpers with `vortex_core.cpp:337–370`
(`VortexCore::wingOf`) and `vortex_vm.cpp:316–320` (`VortexVM::runPipeline` / `executeOpcodeBody`).
Correct the explanation: opcode slots are immutable static pointers; only target registers/handles
mutate.

### 4. (§1.1, line 31) Dimension Namespace Contradicts Live `runtime_abi.hpp`

Full title: Vagent Dimension Namespace Contradicts Live Codebase (`runtime_abi.hpp`)

**Problem:** The document invents a 13-dimension set (`d.vagent.field`, `d.vagent.belief`,
`d.vagent.pred`, `d.vagent.arg`, `d.vagent.intent`, `d.vagent.plan`, `d.vagent.step`,
`d.vagent.goal`, `d.vagent.event`, `d.vagent.space`, `d.vagent.entry`, `d.vagent.child`,
`d.vagent.type`) and claims `d.vagent.skill` and `d.vagent.behavior` are "two new canonical
dimensions bringing the set to 15". In the live repository (`runtime_abi.hpp:66–69`), the 13
`vagent` dimensions are already defined as: `d.behavior`, `d.skill`, `d.capacity`, `d.space`,
`d.member`, `d.holon`, `d.intention`, `d.subgoal`, `d.outbox`, `d.belief`, `d.pending`, `d.source`,
`d.extends`. Fields use Genesis dimensions `d.vars` and `d.values`. Children use `d.holon`.

**Evidence:**

From doc §1.1, lines 31–34 & §2.2, lines 195–197:

> "All 13 `vagent` dimensions (`d.vagent.field`, `d.vagent.belief`, `d.vagent.pred`, `d.vagent.arg`,
> `d.vagent.intent`, `d.vagent.plan`, `d.vagent.step`, `d.vagent.goal`, `d.vagent.event`,
> `d.vagent.space`, `d.vagent.entry`, `d.vagent.child`, `d.vagent.type`)..."

From doc §2.4, lines 277–279:

> "using two new canonical dimensions in the program manifold (`d.vagent.skill` and
> `d.vagent.behavior`, bringing the `vagent` dimension set to 15 dimensions)..."

From repository `apps/common/xanadu/vagent/runtime_abi.hpp:66–69`:

```cpp
inline constexpr std::array<std::string_view, 13> kAgentDimensionNames{
    "d.behavior", "d.skill",     "d.capacity", "d.space",  "d.member",
    "d.holon",    "d.intention", "d.subgoal",  "d.outbox", "d.belief",
    "d.pending",  "d.source",    "d.extends"};
```

From repository `apps/common/xanadu/vagent/runtime_abi.hpp:13–15`:

> "Field. A name cell on the owner's `+d.vars` rank, its value cell on `+d.values` (§5.8)."

**Fix:** Realign the document with the real `kAgentDimensionNames` in `runtime_abi.hpp`, or if an
explicit renaming of the dimension namespace is intended, add a numbered proposal in Section 5
detailing the migration of `runtime_abi.hpp`, `compiler.cpp`, and `agent_executor.cpp`.

### 5. (§4.2, line 411) Layer Boundary Conflation in Proposal P17.4

Full title: Severe Layer Boundary Conflation in P17.4 (`MeshActorRuntime` vs. `AgentExecutor`)

**Problem:** §4.2 and Proposal P17.4 assert that `MeshActorRuntime::ActorEntry`
(`mesh_actor.hpp:404`) has members `inbound`, `flights` (`MeshFlight`), and `timers`, and that
`MeshActorRuntime::upgrade` is responsible for checking and draining pending host effects. In
reality:

1. `mesh_actor.hpp` has only 271 lines. The struct is `MeshActorRuntime::Actor` in
   `mesh_actor.cpp:55`.
1. The actor message queue is named `mailbox`, not `inbound`.
1. `flights` (`std::map<std::int64_t, AgentExecutor::Flight>`) exists exclusively on
   `AgentExecutor::State` (`agent_executor.cpp:332`), and timers live on `$agent`'s `$timers` rank.
   `MeshActorRuntime` has no concept of flights or timers.
1. Calling `DrainFlights` inside `MeshActorRuntime::upgrade` is architecturally impossible because
   `MeshActorRuntime` does not have access to `AgentExecutor` state.

**Evidence:**

From doc §4.2, lines 411–425:

> "preserves the `MeshActorRuntime::ActorEntry` (`mesh_actor.hpp:404`)... In-Flight Host Effects
> (`actor->flights`, `MeshFlight`)... `policy.flightMode == DrainFlights`"

Repository `apps/common/xanadu/vortex/mesh_actor.cpp:55–82`: `MeshActorRuntime::Actor` contains
`mailbox`, `remembered`, `floors`, `cursor`, `stateRoot`, `context`, `grant`. It has no `flights`,
`monitors_`, or `timers`.

From repository `apps/common/xanadu/vagent/agent_executor.cpp:332`:

```cpp
struct AgentExecutor::State {
  ...
  std::map<std::int64_t, AgentExecutor::Flight> flights;
};
```

**Fix:** Move the quiescent flight draining and barrier logic to `AgentExecutor` (before requesting
an upgrade from the runtime), and update Proposal P17.4 so `MeshActorRuntime::upgrade` manages only
the `Actor` grant, `context`, and `stateRoot` transition. Fix the citation to
`MeshActorRuntime::Actor` in `mesh_actor.cpp:55` and replace `inbound` with `mailbox`.

## MAJOR

### 1. (§1.2, line 89) Misstated `arenaDims_` and Non-Existent `dimensionNames()`

Full title: Misstated `ArenaManifold::arenaDims_` Behavior and Non-Existent `dimensionNames()` in
P17.1

**Problem:** Proposal P17.1 claims that constructing `ArenaManifold(const Manifold *base)` leaves
`arenaDims_` empty, causing `ensureDimension` to mint duplicate ephemeral dimensions, and proposes
seeding `arenaDims_` via `base->dimensionNames()`. In reality:

1. Neither `Manifold` nor `ArenaManifold` has a member function named `dimensionNames()`.
1. `ArenaManifold::dimensionNamed(name)` (`arena_manifold.cpp:61–73`) already checks
   `base_->dimensionNamed(name)`.
1. `ArenaManifold::ensureDimension(name)` (`arena_manifold.cpp:81–85`) calls `dimensionNamed(name)`
   first and returns the existing base dimension without minting a duplicate.

**Evidence:**

From doc §5, lines 581–584:

> "In `ArenaManifold::ArenaManifold(const Manifold *base)` (`arena_manifold.cpp:254`), seed
> `arenaDims_` with `base->dimensionNames()`... so `ensureDimension`... resolve base dimensions in
> $O(1)$ without minting duplicate ephemeral dimension cells."

From repository `apps/common/xanadu/zigzag/arena_manifold.cpp:81–85`:

```cpp
DimRef ArenaManifold::ensureDimension(const std::string_view name) {
  const auto existing = dimensionNamed(name);
  if (noCell != existing) {
    return existing;
  }
```

From repository `apps/common/xanadu/zigzag/arena_manifold.cpp:61–73`:

```cpp
if (base_ != nullptr) {
  std::optional<DimRef> baseDim;
  if (reader != nullptr) {
    baseDim = base_->dimensionNamed(name, *reader);
  } else if (store_ != nullptr) {
    baseDim = base_->dimensionNamed(name, *store_);
  } else if (base_->store() != nullptr) {
    baseDim = base_->dimensionNamed(name);
  }
  if (baseDim.has_value()) {
    return *baseDim;
  }
}
```

**Fix:** Clarify P17.1: `ensureDimension` already reuses base dimensions if
`base_->store() != nullptr` or a `SpanReader` is attached. The issue is that `ArenaManifold`
constructed without a reader or store pointer cannot evaluate `textOf` on base dimension cells.
Specify adding `base->dimensions()` name caching or passing `defaultReader_` during `WorkerContext`
initialization.

### 2. (§5 P17.2, line 606) Impossible Post-Construction `context.attachBase`

Full title: Impossible Post-Construction `context.attachBase` in Proposal P17.2

**Problem:** Proposal P17.2 states that `instantiate` will attach `context.arena` as an overlay on
`programManifold()` via `context.attachBase(&programManifold())`. No such method exists on
`WorkerContext` or `ArenaManifold`. Furthermore, `WorkerContext`'s member `core` runs
`initGenesis()` in its constructor. If `WorkerContext` is constructed without `baseManifold`,
`initGenesis()` already mints all 44 genesis dimensions as ephemeral cells in `arena`. Attaching a
base after `WorkerContext` is constructed leaves `core.dims()` permanently bound to ephemeral cells
rather than base dimensions.

**Evidence:**

From doc §5, lines 606–609:

> "Change `CompiledProgram::instantiate(WorkerContext &context, ...)` so it does **not** re-run
> `Compiler::compileAgent`. Instead, it attaches `context.arena` as an overlay on
> `programManifold()` (`context.attachBase(&programManifold())`)..."

From repository `apps/common/xanadu/vortex/worker_context.hpp:51–55`:

```cpp
struct WorkerContext {
  explicit WorkerContext(const Manifold *baseManifold     = nullptr,
                         const xanadu::SpanReader *reader = nullptr)
      : arena(baseManifold), core(*arena.setDefaultSpanReader(reader)),
        vm(core), stdlib(core, vm) {}
```

From repository `apps/common/xanadu/vortex/vortex_core.cpp:14`:
`VortexCore::VortexCore(ArenaManifold &arena) : arena_(arena) { initGenesis(); }`

**Fix:** Specify that `WorkerContext` must be constructed with `baseManifold` and `reader` from the
start (as in `MeshActorRuntime::spawn` / `resume`), rather than attempting an impossible in-place
`attachBase` after genesis initialization.

### 3. (§5 P17.3, line 615; §5 P17.5, line 684) Hallucinated Filenames in Register

Full title: Hallucinated Filenames and Omitted Files in Proposals P17.3 and P17.5

**Problem:**

1. Proposal P17.3 lists `apps/common/xanadu/vortex/core.hpp` and `core.cpp`. Neither file exists;
   the actual files are `vortex_core.hpp`, `vortex_core.cpp`, and `worker_context.hpp`.
1. Proposal P17.3 modifies checkpointing but omits `apps/common/xanadu/vortex/mesh_value.cpp` (where
   `publishValue` and `receiveValue` are implemented).
1. Proposal P17.5 lists `apps/common/xanadu/vagent/sema.cpp`. There is no `sema.cpp` in `vagent`;
   the semantic analysis lives in `checker.cpp` and `checker.hpp`. It also omits `ast.hpp`,
   `compiler.hpp`, and `compiler.cpp` for compiling `on Upgrade`.

**Evidence:**

Doc §5, line 615: `Files: apps/common/xanadu/vortex/core.hpp, apps/common/xanadu/vortex/core.cpp...`
Doc §5, line 684:
`Files: apps/common/xanadu/vagent/parser.cpp, apps/common/xanadu/vagent/sema.cpp...`

Directory listing of `apps/common/xanadu/vortex/` confirms `vortex_core.hpp`, `vortex_core.cpp`,
`worker_context.hpp`, `mesh_value.cpp`. Directory listing of `apps/common/xanadu/vagent/` confirms
`checker.cpp`, `checker.hpp`, `ast.hpp`, `compiler.cpp`, `compiler.hpp`.

**Fix:** Correct file paths in P17.3 to `vortex_core.hpp`, `vortex_core.cpp`, `worker_context.hpp`,
and `mesh_value.cpp`. Correct file paths in P17.5 to `ast.hpp`, `parser.cpp`, `checker.cpp`,
`compiler.cpp`, `agent_executor.hpp`, `agent_executor.cpp`.

### 4. (§2.3, line 257; §3.1, line 301) Handle Kind Contradiction (`OpHandle` vs `UInt64`)

Full title: Handle Representation Contradiction: `OpHandle` vs `UInt64` with `setValueBits`

**Problem:** §2.3 states that pointing `stateH_` at `$agent` calls
`arena.setValueBits(stateH_, ValueKind::UInt64, agentRoot)`. §3.1 refers to handles as `UInt64`
payloads. In `ArenaManifold` and `foreign_op_handle.cpp`, a handle MUST have `ValueKind::OpHandle`.
Setting `ValueKind::UInt64` causes `handleTarget()` to return `std::nullopt` and causes
`isCellRelocationHandle()` to return `false`, breaking handle dereferencing across the VM and
promotion.

**Evidence:**

From doc §2.3, lines 256–258:

> "Writing the agent's ephemeral `$agent` root `CellRef` into `stateH_` calls
> `arena.setValueBits(stateH_, ValueKind::UInt64, agentRoot)`..."

From repository `apps/common/xanadu/zigzag/arena_manifold.cpp:1088–1093`:

```cpp
std::optional<CellRef> ArenaManifold::handleTarget(const CellRef ref) const noexcept {
  const auto cell = slot(ref);
  if (!cell || xanadu::ValueKind::OpHandle !=
                   static_cast<xanadu::ValueKind>(cell->valueKind)) {
    return std::nullopt;
  }
  return static_cast<CellRef>(cell->valueBits);
}
```

From repository `apps/common/xanadu/foreign_op_handle.cpp:111`:
`if (arena.valueKindOf(handle) != ValueKind::OpHandle) return false;`

**Fix:** Replace all occurrences of `ValueKind::UInt64` for handles with `ValueKind::OpHandle`.

### 5. (§1.1, line 19; §5 P17.4, line 654) Misstated Signatures and Error Types

Full title: Misstated Existing Method Signatures and Error Types

**Problem:**

1. §1.1 quotes `CompiledProgram::instantiate` as returning
   `std::expected<AgentInstance, Diagnostic>` and being `const`. The real signature is
   `std::expected<CompiledAgent, std::string>`
   `instantiate(WorkerContext &context, std::string_view agentType)` (non-const).
1. §1.1 quotes `instance.agentRoot`. In `runtime_abi.hpp:607–623`, the field is
   `CompiledAgent::agentCell`.
1. Proposal P17.4 introduces `StateReconciler` returning
   `std::expected<zigzag::CellRef, RuntimeError>`. No `RuntimeError` type exists in
   `zigzag::vortex::mesh`; the error enum is `ActorError`.

**Evidence:**

From doc §1.1, lines 20–25 vs repository `apps/common/xanadu/vagent/program.hpp:40–42`:

```cpp
[[nodiscard]] std::expected<CompiledAgent, std::string>
instantiate(zigzag::vortex::WorkerContext &context,
            std::string_view agentType) override;
```

From doc §5, lines 654–660 vs repository `apps/common/xanadu/vortex/mesh_actor.hpp:29–43`:
`enum class ActorError { ... };`

**Fix:** Update `CompiledProgram::instantiate` to return
`std::expected<CompiledAgent, std::string>`, replace `instance.agentRoot` with `compiled.agentCell`,
and replace `RuntimeError` with `ActorError`.

## MINOR

### 1. (§1.2, line 74; §1.2, line 89; §1.2, line 101) Erroneous Line Number Citations

Full title: Erroneous File and Line Number Citations

**Problem:** Multiple line number citations in existing files are off:

- `arena_manifold.cpp:254` cited for constructor (actual: line 23; line 254 is `expectWritten`).
- `arena_manifold.cpp:277` cited for `shadow` (actual: line 1464; line 277 is in `attach`).
- `mesh_actor.cpp:251` cited for `checkpoint` (actual: line 251 is inside `resume()`; checkpoint is
  `checkpointView` at line 924).
- `agent_executor.cpp:663` cited for `instantiate` (actual: line 693).
- `agent_executor.cpp:801–836` cited for `skills_`/`behaviors_` vectors (actual: lines 801–836
  handle spaces and spawn args; skills/behaviors vectors do not exist on `AgentExecutor`).

**Fix:** Update citations to the exact lines in the codebase.

### 2. (§2.1, line 149; §5 P17.2, line 598) Multi-Agent Compilation Scope Omission

Full title: Multi-Agent Program Compilation Scope Omission in P17.2

**Problem:** `Compiler::compileAgent` compiles a single agent type. A `vagent` translation unit can
declare multiple agent types, skills, and behaviors. P17.2 states that `compileProgram` compiles the
translation unit once into `programManifold`, but does not specify whether `Compiler` is modified to
lower all agent types, skills, and behaviors into a single shared graph or whether multiple
`Compiler` passes are invoked into the same staging context.

**Fix:** Explicitly specify in P17.2 how `Compiler` iterates over `unit.agents`, `unit.skills`, and
`unit.behaviors` to lower all declarations into the single sealed `programStore`.

VERDICT: NOT CONVERGED
