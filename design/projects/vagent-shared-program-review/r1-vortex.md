# Round 1 critique: Vortex & Spool-Mesh Expert (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 4 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§4.3.1–§4.3.3, line 452) Cross-Arena Raw CellRef Leaking in State Reconciliation

Full title: State reconciliation assumes raw CellRef portability across distinct WorkerContext
arenas

**Problem:** The design specifies that unchanged fields (§4.3.1, line 452: "Value cell (or composite
subgraph) is preserved verbatim in the overlay"), belief tuples (§4.3.2, line 462: "Every belief
tuple `pred_name(...)` ... is preserved verbatim"), and skill/behavior instances (§4.3.3) are
preserved verbatim across the upgrade boundary. In Proposal P17.4 (line 674), `upgrade` constructs a
fresh `newContext` over `newProgramManifold` and passes both `oldContext` and `newContext` to
`reconciler`. However, `oldContext.arena` and `newContext.arena` are two completely distinct
`zigzag::ArenaManifold` instances with independent `slots_`, `links_`, and ephemeral cell allocation
spaces. An ephemeral `CellRef` (`0x80000000 | idx`) from `oldContext` cannot be preserved verbatim
or linked into `newContext.arena`: doing so injects a raw, unallocated, or colliding foreign cell
reference into `newContext`, causing memory corruption, link breakage, or segmentation faults during
subsequent turns.

**Evidence:**

- `apps/common/xanadu/zigzag/arena_manifold.hpp:468`: `slots_` is an internal private vector
  allocating dense slot indexes for ephemeral cells.
- `apps/common/xanadu/vortex/worker_context.hpp:10-18`: "each context owns its own ArenaManifold
  (dense vectors, single-owner, not thread-safe)... independent contexts coexist without a lock".
- `design/vagent-shared-program-and-live-upgrade.md:452, 462`: Claims cells are "preserved verbatim
  in the overlay".

**Fix:** Explicitly specify that state reconciliation performs a deep transcription/cloning of all
preserved state cells (fields, beliefs, spaces, skill/behavior states) from `oldContext.arena` into
`newContext.arena` using a typed graph copier, re-minting ephemeral cells in `newContext.arena` and
binding their ranks to `newProgramManifold`'s canonical base dimensions. Clarify that "preserved"
means semantically migrated, never raw numeric `CellRef` integer reuse across distinct arena
instances.

### 2. (§3.2, line 333; §5, line 585) Checkpoint Delta Breaks Store requireCellOp Invariants

Full title: Overlay-only checkpoint promotion violates Store::requireCellOp on base dimensions

**Problem:** The design claims in §3.2 #4 (lines 333-337) and Proposal P17.1 (lines 585-589) that
`checkpoint(pid)` can serialize only the overlay cells reachable from `$agent`, recording references
to base dimensions (`d.vagent.*`, `VortexCore` dimensions) by their base index
($1..N\_{\\text{prog}}$) without copying them into the checkpoint store. In the Spool-Mesh runtime,
checkpoints are published via `SwarmTransport::publishValue`
(`apps/common/xanadu/vortex/mesh_value.cpp:168`), which promotes into a newly instantiated,
standalone `xanadu::Store`. In `Store::setLink` (`apps/common/xanadu/store.cpp:898-902`), the engine
strictly enforces `requireCellOp(from)`, `requireCellOp(dim)`, and `requireCellOp(to)`. If
`targetStore` is a new store, it has zero operations for base dimensions in its `opsSpool`. Calling
`store.setLink` with base `DimRef`s will throw
`std::invalid_argument("the dimension a link is along: missing node")`. Furthermore,
`zigzag::promote` (`apps/common/xanadu/zigzag/arena_manifold.cpp:1869-1871`) maps non-ephemeral
cells directly via `real.emplace(arena, arena)` under the assumption that the target store already
contains those base operations.

**Evidence:**

- `apps/common/xanadu/store.cpp:609-623`: `Store::requireCellOp` verifies `opsSpool.get(ref)` exists
  and is `OpKind::Structure`.
- `apps/common/xanadu/store.cpp:898-902`: `Store::setLink` checks `requireCellOp(dim)`.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1869-1871`:
  `if (!isEphemeral(arena)) { real.emplace(arena, arena); continue; }`.
- `design/vagent-shared-program-and-live-upgrade.md:333-337, 585-589`.

**Fix:** Specify how the checkpoint store resolves base dimensions without violating
`requireCellOp`: either (1) the checkpoint preamble in `publishValue` copies the 15 canonical
`d.vagent.*` dimension definitions (`MakeCell` + `SetValue`) into the checkpoint store's initial
operations (~15 ops, 960 bytes, maintaining a self-contained verifiable store), or (2) the
checkpoint store registers `programStore` as an imported scroll in its bencode v4 tables and
references base dimensions via `ExternOpRef` / proxy cells. Option (1) is strongly recommended as it
eliminates AST/bytecode duplication while keeping checkpoint validation completely self-contained.

### 3. (§4.1, line 397; §5, line 648) Hallucinated ActorGrant Fields (epoch, seq, actorPid)

Full title: Proposal P17.4 invents non-existent ActorGrant fields and invalid upgrade signature

**Problem:** Proposal P17.4 specifies:

```cpp
struct UpgradeRequest {
  std::string signedUpgradeGrant;
  UpgradePolicy policy{};
};
std::expected<ActorGrant, RuntimeError>
upgrade(std::string_view actorPid, const PeerIdentity &callerIdentity,
        const UpgradeRequest &request, std::uint64_t wallNow,
        const StateReconciler &reconciler);
```

and asserts in line 668: "`signedUpgradeGrant` is a valid `ActorGrant` for `actorPid` with
`grant.epoch > oldGrant.epoch` (or `grant.epoch == oldGrant.epoch` and `grant.seq > oldGrant.seq`)."
In `MeshActorRuntime` (`apps/common/xanadu/vortex/mesh_actor.hpp:47-57`), `ActorGrant` contains NO
`epoch`, NO `seq`, and NO `actorPid` fields. `epoch` and `seq` are immutable properties of
`ActorPid` assigned by the runtime during initial spawn (`mesh_actor.cpp:191`). Furthermore,
`MeshActorRuntime` uses `const ActorPid &pid`, `const xanadu::identity::Fingerprint &identity`,
`const ActorMessage &request`, `std::chrono::system_clock::time_point wallNow`, and returns
`std::expected<..., ActorError>`, not `std::string_view`, `PeerIdentity`, `uint64_t`, or
`RuntimeError`.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.hpp:47-57`: Complete declaration of `struct ActorGrant`.
- `apps/common/xanadu/vortex/mesh_actor.hpp:188-193`: Existing `renew` signature.
- `design/vagent-shared-program-and-live-upgrade.md:648-665`.

**Fix:** Redesign Proposal P17.4 to conform to `MeshActorRuntime`'s actual types:

```cpp
std::expected<void, ActorError>
upgrade(const ActorPid &pid, const xanadu::identity::Fingerprint &identity,
        const ActorMessage &request,
        std::chrono::system_clock::time_point wallNow,
        const StateReconciler &reconciler);
```

Define authorization via the existing `Authorizer` delegate returning an `ActorGrant` with updated
`programRef` and `baseRef`. Replay protection must check `request.requestId` against the actor's
causal `floors` or verify a monotonic upgrade counter carried in the request message body, not on
non-existent `ActorGrant` fields.

### 4. (§5, line 674) WorkerContext Non-Movability Violation and Dropped actor->cursor

Full title: Proposal P17.4 violates WorkerContext non-movability and invalidates actor->cursor

**Problem:** Proposal P17.4 states that `upgrade` "Constructs `newContext` over the resolved
`newProgramManifold`... and on success swaps `actor->context = std::move(newContext)`". This is
doubly broken:

1. `WorkerContext` (`apps/common/xanadu/vortex/worker_context.hpp:57-60`) explicitly deletes its
   move constructor and move assignment operator (`WorkerContext(WorkerContext &&) = delete;`).
   `Actor::context` in `mesh_actor.cpp:66` is `std::unique_ptr<WorkerContext>`. A context cannot be
   constructed on the stack and moved into `actor->context`.
1. `Actor` holds `CellRef cursor;` (`mesh_actor.cpp:67`), spawned via
   `context.vm.spawnCursor(entry, actor->pid.cursor)` (`mesh_actor.cpp:204`). Replacing
   `actor->context` leaves `actor->cursor` holding a stale `CellRef` from the discarded arena, which
   does not exist in `newContext->arena` and is unknown to `newContext->vm.cursorPC_`. Any
   subsequent message dispatch or turn execution passes this dead cursor to `executor_`, causing
   immediate VM failure.

**Evidence:**

- `apps/common/xanadu/vortex/worker_context.hpp:57-60`: `WorkerContext` move operations deleted.
- `apps/common/xanadu/vortex/mesh_actor.cpp:66-67, 203-204`: `Actor::context` is `unique_ptr`;
  `cursor` is spawned in `context.vm`.
- `design/vagent-shared-program-and-live-upgrade.md:674-678`.

**Fix:** In Proposal P17.4:

1. Allocate `newContext` as:

   ```cpp
   auto newContext =
       std::make_unique<WorkerContext>(newProgramManifold, newProgramStore);
   ```

1. Spawn a fresh cursor in `newContext->vm` before activating the context:
   `const auto entry = newContext->arena.makeCell("actor:await-message");`
   `actor->cursor = newContext->vm.spawnCursor(entry, actor->pid.cursor);`

1. Swap via `actor->context = std::move(newContext);`.

## MAJOR

### 5. (§1.1, line 35; §2.2, line 220) Hallucinated Vortex VM Opcode Names

Full title: Multiple non-existent opcodes listed as compiled Vortex instructions

**Problem:** Sections 1.1 and 2.2 list `#Add`, `#Move`, `#Step`, `#Select`, `#Link`, `#Unlink`,
`#Isolate`, `#Content`, `#SetContent`, `#Jump`, `#Branch`, `#Call`, `#Choice`, `#Return`, `#Point`,
`#Grab`, `#Release`, `#Save`, `#Yield`, `#Clock`, `#Digest` as the compiled opcode vocabulary. In
`zigzag::vortex::OpcodeKind` (`apps/common/xanadu/vortex/vortex_vm.hpp:27-91`), none of `#Move`,
`#Step`, `#Select`, `#Unlink`, `#Isolate`, `#Content`, `#SetContent`, `#Grab`, `#Release`, `#Save`,
`#Yield`, `#Clock`, or `#Digest` exist. Unlinking is `#Break` (`VortexCore::breakLink`), content
splicing/reading is `#Value` / `#Splice`, and parameter passing uses dual-wing binding (`inputsOf` /
`outputsOf` along `d.grab`) rather than dedicated opcodes.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_vm.hpp:27-91`: Canonical `OpcodeKind` definition.
- `design/vagent-shared-program-and-live-upgrade.md:35-37, 220-222`.

**Fix:** Replace fabricated opcodes with actual `OpcodeKind` identifiers (`#Link`, `#Break`,
`#Value`, `#Splice`, `#Point`, `#Jump`, `#Branch`, `#Call`, `#Return`, `#Choice`, `#Fail`). Clarify
that `yield`, `Wait`, `clock`, and `digest` are handled via runtime ABI helper functions or
asynchronous mesh effects rather than VM opcodes.

### 6. (§4.2, line 417; §5, line 678) Hallucinated Actor Fields (flights, MeshFlight, inbound)

Full title: Actor fields in live upgrade description contradict MeshActorRuntime implementation

**Problem:** Section 4.2 and Proposal P17.4 refer to `actor->flights`, `MeshFlight`,
`UpgradePolicy::flightMode`, `actor->inbound`, `actor->monitors_`, and `actor->timers`. In
`MeshActorRuntime` (`apps/common/xanadu/vortex/mesh_actor.hpp`, `mesh_actor.cpp:55-82`):

1. `MeshFlight` and `actor->flights` do not exist. Outbound in-flight deliveries are tracked in
   `MeshActorRuntime::deliveries_` (`std::map<std::string, Delivery>`), not on `Actor`.
1. The actor message queue is `actor->mailbox` (`std::deque<std::pair<ActorMessage, std::size_t>>`),
   not `inbound`.
1. Monitors live in `MeshActorRuntime::monitors_` (`std::map<std::string, Monitor>`), not per-actor.
1. Timers are held in `AgentExecutor::timers_` in the `vagent` runtime layer, not in
   `MeshActorRuntime`.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.cpp:55-82`: `struct MeshActorRuntime::Actor` members.
- `apps/common/xanadu/vortex/mesh_actor.hpp:263-266`: `monitors_` and `deliveries_` on runtime.
- `design/vagent-shared-program-and-live-upgrade.md:417-427, 678`.

**Fix:** Update §4.2 and P17.4 to refer to `actor->mailbox`, use `deliveries_` (filtered by
`delivery.message.source == pid && !delivery.completed`) for tracking pending outbound deliveries,
and clarify that monitors are runtime-global while timers are managed in `AgentExecutor`.

### 7. (§2.3, line 240; §3.2, line 326) Unsound Turn-Start Mark and -d.grab Output Slot Reset

Full title: Turn-start Mark rollback destroys state mutations; slot resetting lacks mechanism

**Problem:** Section 3.2 #3 suggests: "At the start of a turn, the executor records an
`ArenaManifold::Mark turnMark = arena.mark()` before running transient scratch computations, or
resets shadowed `-d.grab` output slots after the turn completes". This is contradictory and unsound:

1. Calling `arena.release(turnMark)` at turn end would replay the trail and truncate ALL cells
   minted during the turn, destroying the agent's new state mutations, beliefs, fields, and
   intentions along with the scratch computations.
1. If `turnMark` is left unreleased, `arena.outstandingMarks() > 0`, which blocks `compact()` and
   causes `checkpointView` to refuse with `ActorError::NotQuiescent` (`mesh_actor.cpp:928-929`).
1. `ArenaManifold` has no API to "reset" or unshadow specific base cells from `slots_`.

**Evidence:**

- `apps/common/xanadu/zigzag/arena_manifold.hpp:638-668`: `mark()`, `release()`, `discard()`.
- `apps/common/xanadu/vortex/mesh_actor.cpp:928-929`: `checkpointView` requires
  `outstandingMarks() == 0`.
- `design/vagent-shared-program-and-live-upgrade.md:240, 326-329`.

**Fix:** Drop the turn-start `Mark` and slot-reset mechanism. Explain that shadowed `-d.grab` output
cells reside in the ephemeral overlay (`slots_`) and are harmless because `promoteOverlay` walks
only the state subgraph reachable from `stateRoot` (`$agent`). Unreferenced opcode output slots are
naturally excluded from checkpoints without requiring rollback or unshadowing.

### 8. (§5, line 624) Missing SpanReader in Proposal P17.3 for WorkerContext Construction

Full title: ProgramManifoldResolver must supply SpanReader for base permascroll span reads

**Problem:** Proposal P17.3 defines:

```cpp
using ProgramManifoldResolver =
    std::function<const zigzag::Manifold *(std::string_view programRef)>;
```

and constructs `WorkerContext(base)`. In `apps/common/xanadu/vortex/worker_context.hpp:52-55`:

```cpp
WorkerContext(const Manifold *baseManifold = nullptr,
              const xanadu::SpanReader *reader = nullptr)
```

sets `core(*arena.setDefaultSpanReader(reader))`. If `reader` is omitted,
`arena.setDefaultSpanReader` remains null. Any subsequent instruction or reconciliation walk that
reads string or text content from base cells via `core.render(cell)` or `arena.textOf(cell)` will
fail to resolve the underlying permascroll spans in `programStore`.

**Evidence:**

- `apps/common/xanadu/vortex/worker_context.hpp:52-55`.
- `apps/common/xanadu/vortex/mesh_actor.cpp:305`: Explicitly sets
  `setDefaultSpanReader(actor->checkpoint->store.get())`.
- `design/vagent-shared-program-and-live-upgrade.md:624-637`.

**Fix:** Extend `ProgramManifoldResolver` to return both the base `Manifold` and its `SpanReader`
(e.g., `struct ProgramBundle { const Manifold *manifold; const SpanReader *reader; };`), and pass
both to `std::make_unique<WorkerContext>(bundle.manifold, bundle.reader)`.

## MINOR

### 9. (§1.2, line 66) Non-Existent readScalarValue and Erroneous core.cpp Citations

Full title: Non-existent readScalarValue cited and core.cpp cited instead of vortex_vm.cpp

**Problem:** Section 1.2 lines 66-72 cites `core.cpp:1735, 1766, 1831, 1929` and quotes
`readScalarValue(context.arena, *targetCell, context.StringInterner)`. There is no file `core.cpp`
(the files are `vortex_core.cpp` and `vortex_vm.cpp`), and `readScalarValue` does not exist in the
codebase. Jump/call targets are read via `toInt(inputs[...])` in `vortex_vm.cpp:657, 674`.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_vm.cpp:657, 674`.
- `design/vagent-shared-program-and-live-upgrade.md:66-72`.

**Fix:** Update file paths to `apps/common/xanadu/vortex/vortex_vm.cpp` and replace
`readScalarValue` with `toInt(inputs[...])`.

### 10. (§2.3, line 260) OpHandle Not Evaluated by VortexCore::render

Full title: VortexCore::render does not dereference OpHandle operands

**Problem:** In `vortex_core.cpp:281-298`, `VortexCore::render` inspects `ValueKind` for `Double`,
`Int64`, and `Bool`, and falls through to `textOf(master)`. It does not handle
`ValueKind::OpHandle`. The design states in §2.3 (line 261) that operand resolution transparently
reads shadowed `OpHandle` entries. In reality, in `vagent`, handle dereferencing is performed via
`#Link` pipelines or explicit indirection in `compiler.cpp`.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_core.cpp:281-298`.
- `apps/common/xanadu/vortex/vortex_vm.cpp:1083`.

**Fix:** Clarify in §2.3 that `OpHandle` cells are dereferenced via pipeline linkage or explicit
indirection rather than native `render()` transparent dereference, or propose extending
`VortexCore::render` to handle `ValueKind::OpHandle`.

VERDICT: NOT CONVERGED
