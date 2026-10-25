# Round 2 critique: Codebase Expert (`codebase`) (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root
(`/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7`). Tally: 3
BLOCKING, 5 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (Overlay promotion violates `Store::setLink` invariant): OPEN — Hand-waved "delta Store
  whose header names baseRef as parent" without proposing required changes to `Store` and
  `SegmentedOpsSpool` in the register.
- R1.B2 (Turn scratch `mark()` destroys live state): RESOLVED — COW scratch shadowing removed and
  replaced with direct overlay register allocation on `d.vars`/`d.values`.
- R1.B3 (Hallucinated VM execution architecture and `core.cpp`): OPEN — Replaced `core.cpp` with
  `vortex_core.cpp` while retaining impossible line numbers (1735, 1766, 1831, 1929) for a 586-line
  file; VM dispatch actually lives in `vortex_vm.cpp:655`.
- R1.B4 (Dimension namespace contradicts `runtime_abi.hpp`): OPEN — Substituted another set of
  fictitious dimensions (`d.agent`, `d.timers`) contradicted by the 13 canonical dimensions in
  `runtime_abi.hpp:66–69`.
- R1.B5 (Layer boundary conflation in Proposal P17.4): PARTLY — Flight draining moved to
  `AgentExecutor`, but `StateReconciler` signature is completely omitted and `MeshActorRuntime` is
  falsely credited with a "supervision tree".
- R1.M1 (Misstated `arenaDims_` and non-existent `dimensionNames()`): OPEN — Proposal P17.1 is
  entirely untouched; still cites non-existent `dimensionNames()` and wrong line 254.
- R1.M2 (Impossible post-construction `attachBase`): PARTLY — `attachBase` removed, but P17.2 still
  incorrectly specifies that `instantiate` constructs an `ArenaManifold`.
- R1.M3 (Hallucinated filenames in register): PARTLY — `core.cpp` and `sema.cpp` removed, but P17.5
  omits `ast.hpp` and `compiler.cpp`.
- R1.M4 (Handle kind `OpHandle` vs `UInt64`): OPEN — §1.2 line 70 still cites
  "`d.mesh.relocation`-marked `UInt64` handles".
- R1.M5 (Misstated signatures and error types): PARTLY — `ActorError` adopted in P17.4, but
  `CompiledProgram::instantiate` in §1.1 still cites non-existent `AgentInstance` and `Diagnostic`.

______________________________________________________________________

## BLOCKING

### 1. (§1.1, line 32) Dimension Namespace Falsification Persists

Full title: Dimension Namespace Contradicts Live `runtime_abi.hpp` and Re-introduces Fictitious
Dimensions

**Problem:** While Round 1 findings removed `d.vagent.*`, the revision replaced them with another
fictitious dimension inventory. The document claims `runtime_abi.hpp:67–87` defines "10 canonical
vagent dimensions (`d.agent`, `d.vars`, `d.values`, `d.skill`, `d.behavior`, `d.intention`,
`d.subgoal`, `d.outbox`, `d.timers`, `d.stack`)", links agent declarations along `d.agent` (§2.2
line 195), and registers timers on `d.timers` (§4.2 line 296). In the live repository,
`runtime_abi.hpp` defines 13 dimensions that do not include `d.agent` or `d.timers`, and `d.vars`,
`d.values`, `d.stack` are core genesis dimensions.

**Evidence:**

From doc §1.1, lines 32–35:

> "The 10 canonical `vagent` dimensions defined in `apps/common/xanadu/vagent/runtime_abi.hpp:67–87`
> (`d.agent`, `d.vars`, `d.values`, `d.skill`, `d.behavior`, `d.intention`, `d.subgoal`, `d.outbox`,
> `d.timers`, `d.stack`)..."

From doc §2.2, line 195:

> "Agent Declarations: Linked along `d.agent`..."

From doc §4.2, line 296:

> "Active Timers: Registered timers (`d.timers`)..."

From repository `apps/common/xanadu/vagent/runtime_abi.hpp:50–69`:

```cpp
struct AgentDims {
  zigzag::DimRef behavior{};
  zigzag::DimRef skill{};
  zigzag::DimRef capacity{};
  zigzag::DimRef space{};
  zigzag::DimRef member{};
  zigzag::DimRef holon{};
  zigzag::DimRef intention{};
  zigzag::DimRef subgoal{};
  zigzag::DimRef outbox{};
  zigzag::DimRef belief{};
  zigzag::DimRef pending{};
  zigzag::DimRef source{};
  zigzag::DimRef extends{};
};

inline constexpr std::array<std::string_view, 13> kAgentDimensionNames{
    "d.behavior", "d.skill",     "d.capacity", "d.space",  "d.member",
    "d.holon",    "d.intention", "d.subgoal",  "d.outbox", "d.belief",
    "d.pending",  "d.source",    "d.extends"};
```

From repository `apps/common/xanadu/vagent/runtime_abi.hpp:103`:

```cpp
inline constexpr std::string_view kFieldTimers = "$timers";
```

`d.agent` and `d.timers` do not exist in the codebase. Timers are held on the `$agent` owner's
`+d.vars` rank under field name `kFieldTimers = "$timers"`. Genesis dimensions (`d.vars`,
`d.values`, `d.stack`, `d.clause`, `d.grab`, `d.step`, `d.spin`, `d.clone`) come from `VortexCore`
(`vortex_core.cpp:49–62`), not `runtime_abi.hpp`.

**Fix:** Correct the inventory to match `runtime_abi.hpp:66–69`: exactly 13 agent dimensions
(`d.behavior`, `d.skill`, `d.capacity`, `d.space`, `d.member`, `d.holon`, `d.intention`,
`d.subgoal`, `d.outbox`, `d.belief`, `d.pending`, `d.source`, `d.extends`). Remove `d.agent` and
`d.timers`; model program agent declarations along `d.holon` or `d.vars`, and model timers on
`$agent`'s `$timers` field rank.

______________________________________________________________________

### 2. (§3.1, line 249) Unproposed Engine Delta Store Parent Spools

Full title: Unproposed Engine Modification and Invariant Violation for Delta Store Parent Spools

**Problem:** In response to R1.B1, the document asserts: "The promoted checkpoint is written as a
delta Store whose header explicitly names baseRef as its parent spool. When Store::setLink executes,
dimension cells referencing base dimensions resolve directly against the parent spool without
violating the requireCellOp invariant (store.cpp:609, 898)." Neither `Store` nor `SegmentedOpsSpool`
supports parent spools or delta stores. In `store.cpp:609–623`, `requireCellOp` queries only its own
local `opsSpool.get(ref)`. Invoking `setLink` on a fresh checkpoint store with base `DimRef`s will
unconditionally throw `std::invalid_argument`. Proposal P17.1 modifies only `arena_manifold.hpp` and
`arena_manifold.cpp`, omitting any proposal to modify `Store` or `SegmentedOpsSpool`.

**Evidence:**

From doc §3.1, lines 249–252:

> "2. **Segment Parentage:** The promoted checkpoint is written as a delta `Store` whose header
> explicitly names `baseRef` as its parent spool. When `Store::setLink` executes, dimension cells
> referencing base dimensions resolve directly against the parent spool without violating the
> `requireCellOp` invariant (`store.cpp:609, 898`)."

From doc §5 P17.1, lines 412–413:

> "- **Files:** `apps/common/xanadu/zigzag/arena_manifold.hpp`,
> `apps/common/xanadu/zigzag/arena_manifold.cpp`"

From repository `apps/common/xanadu/store.cpp:609–623`:

```cpp
void Store::requireCellOp(const zigzag::CellRef ref, const char *what) const {
  const auto *const node = opsSpool.get(ref);
  if (nullptr == node || OpKind::Structure != node->kind) {
    throw std::invalid_argument(...);
  }
}
```

`SegmentedOpsSpool` has no link to an external parent spool. If `targetStore` is a new store for the
checkpoint, its `opsSpool` contains only checkpoint operations. Calling
`targetStore->setLink(..., baseDim, ...)` will fail in `requireCellOp(dim)` and throw
`std::invalid_argument`.

**Fix:** Either:

1. Formally add a numbered Proposal (e.g. `P17.6`) touching `apps/common/xanadu/store.hpp`,
   `apps/common/xanadu/store.cpp`, `apps/common/xanadu/segmented_ops_spool.hpp`, and
   `apps/common/xanadu/segmented_ops_spool.cpp` specifying how `SegmentedOpsSpool` chains parent
   spools and how `requireCellOp` resolves base references; or
1. Specify that the checkpoint store is an in-tree OSMIC hypertime branch of `programStore` sharing
   the same `SegmentedOpsSpool`; or
1. Specify that state promotion re-declares dimensions locally in the checkpoint delta store header.

______________________________________________________________________

### 3. (§1.2 line 67) Hallucinated Line Numbers and File Mismatch in VM Citation

Full title: Impossible Line Numbers and Wrong Source File for Vortex VM Instruction Execution

**Problem:** In response to R1.B3, the revision changed `core.cpp` to
`apps/common/xanadu/vortex/vortex_core.cpp`, but kept the hallucinated line numbers:
`vortex_core.cpp:1735, 1766, 1831, 1929`. `vortex_core.cpp` has only 586 lines in total.
Furthermore, opcode execution and jump/branch resolution are implemented in `vortex_vm.cpp`, not
`vortex_core.cpp`.

**Evidence:**

From doc §1.2, lines 66–68:

> "The Vortex VM (`apps/common/xanadu/vortex/vortex_core.cpp:1735, 1766, 1831, 1929`) reads the
> jump/call target via `readValue` as a raw integer and casts it to `CellRef`."

From repository line count check:

```sh
wc -l apps/common/xanadu/vortex/vortex_core.cpp
586 apps/common/xanadu/vortex/vortex_core.cpp
```

From repository `apps/common/xanadu/vortex/vortex_vm.cpp:655–675`:

```cpp
case OpcodeKind::Jump: {
  if (!inputs.empty() && cursor != noCell) {
    CellRef targetOp = toInt(inputs[0]);
    setCursorOpcode(cursor, targetOp);
    jumped = true;
  }
  break;
}
case OpcodeKind::Branch: {
  if (inputs.size() >= 2 && cursor != noCell &&
      VortexCore::evaluateTruthiness(inputs[0])) {
    CellRef targetOp = toInt(inputs[1]);
    setCursorOpcode(cursor, targetOp);
    jumped = true;
  }
  break;
}
case OpcodeKind::Call: {
...
```

**Fix:** Replace citation `apps/common/xanadu/vortex/vortex_core.cpp:1735, 1766, 1831, 1929` with
`apps/common/xanadu/vortex/vortex_vm.cpp:655–680` (`OpcodeKind::Jump`, `Branch`, `Call`).

______________________________________________________________________

## MAJOR

### 1. (§5 P17.1, line 415) Non-Existent dimensionNames() in P17.1

Full title: Proposal P17.1 Retains Hallucinated `dimensionNames()` and Wrong Line Citation

**Problem:** In Proposal P17.1, lines 415 and 419–420 were not updated. The text still states: "In
`ArenaManifold::ArenaManifold(const Manifold *base)` (`arena_manifold.cpp:254`), seed `arenaDims_`
with `base->dimensionNames()`".

1. Neither `Manifold` nor `ArenaManifold` has a member named `dimensionNames()`.
1. Line 254 of `arena_manifold.cpp` is inside `expectWritten` logging; the constructor is at line
   23\.
1. `ArenaManifold::dimensionNamed` (`arena_manifold.cpp:61–73`) already queries
   `base_->dimensionNamed(name)` when `store_` or a reader is present, and `ensureDimension`
   (`arena_manifold.cpp:81–85`) returns existing dimensions without minting duplicates.

**Evidence:**

From doc §5 P17.1, lines 418–420:

> "In `ArenaManifold::ArenaManifold(const Manifold *base)` (`arena_manifold.cpp:254`), seed
> `arenaDims_` with `base->dimensionNames()` (preserving non-ephemeral base `DimRef`s)."

From repository `apps/common/xanadu/zigzag/arena_manifold.cpp:23–28`:

```cpp
ArenaManifold::ArenaManifold(const Manifold *base, const xanadu::Store *store)
    : base_(base), store_(store) {
  if (store_ != nullptr) {
    projectProvenance(*store_);
  }
}
```

From repository `apps/common/xanadu/zigzag/arena_manifold.cpp:250–256`:

```cpp
                   const std::source_location where) noexcept {
  if (!result) {
    GLEDITOR_LOG_WARN("zigzag.arena", "arena write refused ({}) at {}:{}",
                      toString(result.error()), where.file_name(),
                      where.line());
  }
}
```

`git grep "dimensionNames" apps/common/xanadu/zigzag/` confirms `dimensionNames()` does not exist on
`Manifold`.

**Fix:** Update P17.1 to cite `arena_manifold.cpp:23`. Replace `base->dimensionNames()` with caching
name lookups via `base->dimensions()` and `base->dimensionNamed(name, reader)`.

______________________________________________________________________

### 2. (§1.1, line 20) Infeasible instantiate Construction in P17.2

Full title: Mechanically Infeasible `ArenaManifold` Construction in `instantiate` and Wrong Return
Type

**Problem:**

1. §1.1 lines 20–25 still quotes `CompiledProgram::instantiate` as returning
   `std::expected<AgentInstance, Diagnostic>` and being `const`. In the codebase
   (`program.hpp:40–42`, `program.cpp:50–52`), it returns
   `std::expected<CompiledAgent, std::string>` and is non-const.
1. Proposal P17.2 states: "Change `CompiledProgram::instantiate` to construct an `ArenaManifold`
   overlay directly over `programManifold()`". In `WorkerContext` (`worker_context.hpp:51–55`),
   `arena` is a direct value member, and `core`, `vm`, `stdlib` hold references to it
   (`non-copyable and non-movable by design`). `instantiate(WorkerContext &context, ...)` cannot
   construct or rebind `context.arena`. `WorkerContext` must be constructed with `baseManifold`
   beforehand, as P17.3 correctly specifies for `MeshActorRuntime`.

**Evidence:**

From doc §1.1, lines 20–25:

```cpp
// apps/common/xanadu/vagent/program.cpp:52
std::expected<AgentInstance, Diagnostic>
CompiledProgram::instantiate(vortex::WorkerContext &context,
                             std::string_view agentType) const {
  return Compiler::compileAgent(unit_, agentType, context);
}
```

From repository `apps/common/xanadu/vagent/program.hpp:40–42`:

```cpp
[[nodiscard]] std::expected<CompiledAgent, std::string>
instantiate(zigzag::vortex::WorkerContext &context,
            std::string_view agentType) override;
```

From repository `apps/common/xanadu/vortex/worker_context.hpp:51–55`:

```cpp
struct WorkerContext {
  explicit WorkerContext(const Manifold *baseManifold     = nullptr,
                         const xanadu::SpanReader *reader = nullptr)
      : arena(baseManifold), core(*arena.setDefaultSpanReader(reader)),
        vm(core), stdlib(core, vm) {}
```

**Fix:** Update §1.1 to match `program.hpp:40`. In P17.2, clarify that `instantiate` assumes
`context.arena` was already constructed over `programManifold()` by `MeshActorRuntime`, and only
executes pre-compiled initialization routines to populate the agent overlay.

______________________________________________________________________

### 3. (§5 P17.4 lines 489–496) Undefined `StateReconciler` and Hallucinated Supervision Tree

Full title: Undefined `StateReconciler` Signature and Hallucinated Supervision Tree in Proposal
P17.4

**Problem:**

1. Proposal P17.4 adds `MeshActorRuntime::upgrade(..., const StateReconciler &reconciler)`, but
   `StateReconciler` is not defined anywhere in the document. Its type signature, input contexts,
   return type, and failure reporting are omitted.
1. Line 495 claims `MeshActorRuntime::upgrade` preserves "the actor's mailbox, pid, and supervision
   tree". `MeshActorRuntime` does not maintain a supervision tree
   (`apps/common/xanadu/vortex/mesh_actor.hpp:229`: "one-for-all supervision are not supported by
   this mesh"). Supervision belongs to `AgentExecutor` or application-level holons.

**Evidence:**

Doc §5 P17.4, lines 489–496:

```cpp
std::expected<void, ActorError>
upgrade(const identity::ActorPid &pid, const ActorGrant &newGrant,
        std::uint64_t wallNow, const StateReconciler &reconciler);
```

> "...swaps the base manifold pointer while preserving the actor's mailbox, pid, and supervision
> tree."

From repository `apps/common/xanadu/vortex/mesh_actor.hpp:229`:

> `/// one-for-all supervision are not supported by this mesh.`

**Fix:** Explicitly define `StateReconciler` in P17.4:

```cpp
using StateReconciler =
    std::function<std::expected<void, ActorError>(
        WorkerContext &oldContext, WorkerContext &newContext,
        CellRef oldStateRoot, CellRef &newStateRoot)>;
```

Remove reference to "supervision tree" in `MeshActorRuntime::upgrade`.

______________________________________________________________________

### 4. (§1.2 line 70) Handle Kind Misstated as `UInt64`

Full title: Handle Kind Contradiction (`UInt64` vs `ValueKind::OpHandle`) in §1.2

**Problem:** Line 70 states: "it remaps `d.mesh.relocation`-marked `UInt64` handles
(`arena_manifold.cpp:2011`)". In `ArenaManifold` (`arena_manifold.cpp:1800, 1949`), relocation
handles must have `ValueKind::OpHandle`. If marked as `UInt64`, `from.valueKindOf(cell)` is not
`OpHandle`, and promotion will not treat them as handles. Furthermore, line 2011 is not the handle
remapping logic in `arena_manifold.cpp` (it is lines 1800 and 1939–1956).

**Evidence:**

From doc §1.2, lines 69–71:

> "...it remaps `d.mesh.relocation`-marked `UInt64` handles (`arena_manifold.cpp:2011`)..."

From repository `apps/common/xanadu/zigzag/arena_manifold.cpp:1800`:

```cpp
if (from.valueKindOf(cell) == xanadu::ValueKind::OpHandle &&
    xanadu::isCellRelocationHandle(from, cell)) { ... }
```

**Fix:** Change "`UInt64` handles" to "`ValueKind::OpHandle` handles
(`arena_manifold.cpp:1800, 1949–1956`)".

______________________________________________________________________

### 5. (§3.2 line 263; §8.1 line 558) Unexplained "Mount Checkpoint Delta into `context.arena`"

Full title: Mechanically Unspecified Checkpoint Delta Mounting in `ArenaManifold`

**Problem:** §3.2 line 263 states: "3. Mount checkpoint delta into `context.arena`", and §8.1 line
558 states: "loads the checkpoint's state cells into the overlay... in $O(1)$". In
`zigzag::ArenaManifold`, `base_` is a single pointer (`const Manifold *base_`). Persistent cells
from a separate checkpoint delta store cannot be resolved through `base_` (which points to
`programManifold`), nor are they ephemeral cells in `slots_` (which have bit 31 set).
`ArenaManifold` has no `mount` method.

**Evidence:**

From doc §3.2, lines 262–264:

```
2. Construct WorkerContext(baseManifold).
3. Mount checkpoint delta into context.arena.
```

From repository `apps/common/xanadu/zigzag/arena_manifold.hpp:240–241`:

```cpp
const Manifold *base_{nullptr};
const xanadu::Store *store_{nullptr};
```

**Fix:** Specify the mechanism for delta rehydration: either checkpoint delta state is loaded into
ephemeral overlay slots via `deserializeValue` / `materializeValue` over `programManifold`, or a
composite manifold (`CompositeManifold`) is defined in `zigzag` to chain the delta `Manifold` over
`programManifold`.

______________________________________________________________________

## MINOR

### 1. (§5 P17.5 line 508) Omission of `ast.hpp` and `compiler.cpp`

Full title: P17.5 File List Omits AST and Compiler Files for `on Upgrade`

**Problem:** P17.5 lists `parser.cpp`, `checker.cpp`, `agent_executor.hpp`, `agent_executor.cpp`,
but omits `ast.hpp` (AST definition for `UpgradeEventDef`) and `compiler.cpp` (handler compilation).

**Fix:** Add `apps/common/xanadu/vagent/ast.hpp` and `apps/common/xanadu/vagent/compiler.cpp` to
P17.5.

______________________________________________________________________

### 2. (§1.2 line 52) Citation of `mesh_actor.cpp:251` for Checkpoint

Full title: Erroneous Line Citation for Checkpointing in `mesh_actor.cpp`

**Problem:** `apps/common/xanadu/vortex/mesh_actor.cpp:251` is inside `resume()`. Checkpoint
creation is `checkpointView` at line 924.

**Fix:** Change citation to `apps/common/xanadu/vortex/mesh_actor.cpp:924`.

______________________________________________________________________

### 3. (§4.2 line 294) `state.mailbox` Citation

Full title: Mailbox Queue Sits on `Actor`, Not `AgentExecutor::State`

**Problem:** §4.2 line 294 states "Messages queued in `state.mailbox`". In the repository, `mailbox`
is a member of `MeshActorRuntime::Actor` (`mesh_actor.cpp:55`), while `AgentExecutor::State` has no
mailbox field.

**Fix:** Change `state.mailbox` to `actor->mailbox`.

______________________________________________________________________

VERDICT: NOT CONVERGED
