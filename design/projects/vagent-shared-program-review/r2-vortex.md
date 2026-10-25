# Round 2 critique: Vortex & Spool-Mesh Expert (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 2 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Cross-Arena Raw CellRef Leaking): PARTLY — §4.3 specifies overlay translation, but §4.3.4
  claims frame locals remain valid verbatim, and P17.4 contradicts §4.3 via in-place base pointer
  swapping.
- R1.2 (Checkpoint Delta Breaks Store requireCellOp): OPEN — Asserting segment parentage without
  modifying `OpsSegmentHeader`, `SegmentedOpsSpool`, or `Store::requireCellOp` leaves delta store
  linking broken and unproposed.
- R1.3 (Hallucinated ActorGrant Fields): PARTLY — Fields removed, but P17.4 `upgrade` uses
  `uint64_t wallNow` instead of `system_clock::time_point`, bypasses `authorizer_` with a raw
  `ActorGrant`, and leaves `StateReconciler` undefined.
- R1.4 (WorkerContext Non-Movability & actor->cursor): OPEN — Context move replaced with impossible
  "swap base manifold pointer" on non-swappable `ArenaManifold`; omits `actor->cursor` re-spawning
  in the new context.
- R1.5 (Hallucinated Vortex VM Opcode Names): RESOLVED — Canonical opcodes adopted in §1.1/§2.2;
  phantom `#Unlink`/`#Step` in §1.2 line 75 is a minor remnant.
- R1.6 (Hallucinated Actor Fields): PARTLY — Conflates `MeshActorRuntime::Actor::mailbox` with
  `AgentExecutor::State`, and replanning leaves in-flight correlation susceptible to stale reply
  injection.
- R1.7 (Unsound Turn-Start Mark & -d.grab Reset): RESOLVED — COW shadowing and mark rollback
  abandoned in favor of direct overlay register routing on `d.vars`/`d.values`.
- R1.8 (Missing SpanReader in P17.3): PARTLY — Prose in line 470 mentions `spanReader`, but
  `ProgramManifoldResolver` signature in line 465 still returns only `const zigzag::Manifold *`.
- R1.9 (Non-Existent readScalarValue & core.cpp Citations): PARTLY — `core.cpp` renamed to
  `vortex_core.cpp`, but line 67 still cites non-existent lines in a 586-line file instead of
  `vortex_vm.cpp:655–694`.
- R1.10 (OpHandle in render): RESOLVED — Erroneous claims regarding `VortexCore::render`
  dereferencing `OpHandle` removed.

## BLOCKING

### 1. (§3.1, line 249; §5, line 421) Delta Store Parentage Contradicts Store

Full title: Delta store segment parentage assertion violates OpsSegmentHeader format and
Store::requireCellOp

**Problem:** Section 3.1 #2 (lines 249–252) states: "The promoted checkpoint is written as a delta
`Store` whose header explicitly names `baseRef` as its parent spool. When `Store::setLink` executes,
dimension cells referencing base dimensions resolve directly against the parent spool without
violating the `requireCellOp` invariant (`store.cpp:609, 898`)." And Proposal P17.1 (line 421) adds
`zigzag::promoteOverlay` without proposing any changes to `Store` or `SegmentedOpsSpool`. This is
impossible in the repository today and breaks foundational store invariants:

1. `OpsSegmentHeader` (`apps/common/xanadu/segmented_ops_spool.hpp:98–135`) is an exactly 80-byte
   header with strict offset assertions. It has no `parentSpool` or `baseRef` field.
1. `SegmentedOpsSpool` has no concept of a parent spool or cross-spool node lookup.
1. `Store::requireCellOp` (`apps/common/xanadu/store.cpp:609–624`) strictly validates
   `const auto *const node = opsSpool.get(ref);` against its own spool. In `Store::setLink`
   (`store.cpp:898–902`), linking along a base dimension (e.g. `d.agent` = 5) in a newly created
   checkpoint delta store fails immediately because `targetStore->opsSpool.get(5)` returns
   `nullptr`, throwing `std::invalid_argument`.
1. Checkpoints are published across nodes via `publishValue` / `receiveValue` as standalone sealed
   stores (`apps/common/xanadu/vortex/mesh_value.cpp:168`). A node receiving a checkpoint cannot
   validate operations against an undeclared parent spool without a major engine format overhaul.

**Evidence:**

- `apps/common/xanadu/segmented_ops_spool.hpp:98–135`: `struct OpsSegmentHeader` layout (80 bytes;
  no parent spool field).
- `apps/common/xanadu/store.cpp:609–624`: `Store::requireCellOp` checks `this->opsSpool.get(ref)`.
- `apps/common/xanadu/store.cpp:899`: `requireCellOp(dim, "the dimension a link is along");`.
- `design/vagent-shared-program-and-live-upgrade.md:249–252, 421–422`.

**Fix:** Choose and specify one of two sound approaches:

- **Approach A (Recommended, zero engine format changes):** In `zigzag::promoteOverlay`, seed the
  checkpoint store with the 10 canonical dimension definitions (`MakeCell` + `SetValue` for
  `d.agent`, `d.vars`, etc., taking ~10 ops = 640 bytes). Base dimension references inside the
  checkpoint resolve directly within the store's own spool, preserving `requireCellOp` and
  standalone store verifiability without touching `OpsSegmentHeader` or `Store`.
- **Approach B:** Formally introduce Proposal `P17.6` to modify `OpsSegmentHeader` (bumping
  `opsSegmentFormatVersion` from 2 to 3), `SegmentedOpsSpool`, and `Store::requireCellOp` to
  delegate missing `CellRef` lookups to a parent spool `baseRef`, pricing the format migration and
  Merkle verification overhead across the swarm.

### 2. (§4.3.4, line 354; §5, line 494) Contradictory Upgrade Context Mechanics

Full title: Contradiction between in-place base pointer swap and new overlay translation; broken
activation frame and cursor lifecycles

**Problem:** The design contains a fundamental architectural contradiction regarding how live
upgrade modifies the actor context:

1. Proposal P17.4 (line 494) states that `upgrade` "waits for a quiescent turn boundary, executes
   `reconciler`, and swaps the base manifold pointer while preserving the actor's mailbox, pid, and
   supervision tree."
   - `WorkerContext` (`apps/common/xanadu/vortex/worker_context.hpp:51–68`) owns
     `ArenaManifold arena` by value and deletes all move operations.
   - `zigzag::ArenaManifold` (`apps/common/xanadu/zigzag/arena_manifold.hpp:251–272`) holds
     `const Manifold *base_` initialized at construction; it has NO member function or setter to
     swap `base_`.
   - Even if `base_` were mutable, swapping the base manifold underneath existing overlay cells
     breaks the ZigZag graph: overlay cells link to non-ephemeral base `CellRef`s (e.g. opcode entry
     points, dimension IDs, constants) that differ between $V_1$ and $V_2$.
1. Conversely, §4.3 (lines 312–314) and §8.2 (line 569) specify that reconciliation runs as a
   "translation from the $V_1$ overlay to a new $V_2$ overlay attached to `programManifoldV2`".
   - If a new overlay arena / `WorkerContext` is constructed, §4.3.4 (lines 354–356) is broken: it
     claims that for identical plans (`planGraphHash` matches), "The intention continues seamlessly:
     its activation frame locals on `d.vars`/`d.values` remain valid, its `step` ordinal is
     preserved, and execution will resume at the next wake turn."
   - Activation frames on `d.stack` and frame locals on `d.vars`/`d.values` are ephemeral cells
     allocated in $V_1$'s `ArenaManifold` (`slots_`). They DO NOT EXIST in the new $V_2$ overlay
     arena. Accessing them from $V_2$ without transcribing them injects dead foreign `CellRef`s into
     the new VM.
1. Proposal P17.4 does not account for `actor->cursor` (`mesh_actor.cpp:67`). Replacing
   `actor->context` leaves `actor->cursor` pointing to a cell from the discarded VM/arena, causing
   the next message dispatch to trap or crash.

**Evidence:**

- `apps/common/xanadu/vortex/worker_context.hpp:51–68`: Non-movable, owns `ArenaManifold arena` by
  value.
- `apps/common/xanadu/zigzag/arena_manifold.hpp:251–272`: `base_` is immutable.
- `apps/common/xanadu/vortex/mesh_actor.cpp:66–67, 203–204`: `Actor::context` is
  `std::unique_ptr<WorkerContext>`; `cursor` is registered in `context.vm`.
- `design/vagent-shared-program-and-live-upgrade.md:312–314, 354–356, 494–496, 569`.

**Fix:**

1. Align Proposal P17.4 with §4.3: `upgrade` allocates a fresh `WorkerContext` over $V_2$'s base
   manifold:

   ```cpp
   auto newContext =
       std::make_unique<WorkerContext>(newProgram.manifold, newProgram.reader);
   ```

1. In §4.3.4, specify that continuing intentions have their active `d.stack` frames, `d.vars`, and
   `d.values` deeply transcribed into `newContext->arena` by the reconciler, binding return targets
   to $V_2$'s opcode `CellRef`s. Alternatively, if frames are not transcribed, require intentions to
   abort and replan at goal level upon upgrade.

1. In P17.4, explicitly re-spawn the actor cursor in the new context's VM before activating:

   ```cpp
   const auto entry = newContext->arena.makeCell("actor:await-message");
   actor->cursor    = newContext->vm.spawnCursor(entry, actor->pid.cursor);
   actor->context   = std::move(newContext);
   ```

### 3. (§5, lines 489–492) Proposal P17.4 upgrade Signature Contradictions

Full title: MeshActorRuntime::upgrade uses integer wallNow, bypasses authorizer, and leaves
StateReconciler undefined

**Problem:** Proposal P17.4 specifies:

```cpp
std::expected<void, ActorError>
upgrade(const identity::ActorPid &pid, const ActorGrant &newGrant,
        std::uint64_t wallNow, const StateReconciler &reconciler);
```

This signature violates `MeshActorRuntime` conventions in three ways:

1. **Clock Type:** `wallNow` is typed as `std::uint64_t`. In `MeshActorRuntime`
   (`apps/common/xanadu/vortex/mesh_actor.hpp:175, 181, 186, 193`), every runtime entry point uses
   `std::chrono::system_clock::time_point wallNow`.
1. **Bypassing Capability Verification:** `upgrade` takes a raw `const ActorGrant &newGrant`. In
   `MeshActorRuntime`, grants are never passed raw by callers; they are authenticated from an
   `ActorMessage` via `authorizer_(identity, request)` (see `renew` in `mesh_actor.hpp:190–193` and
   `mesh_actor.cpp:245–253`). A supervisor or orchestrator initiates an upgrade via a signed mesh
   request.
1. **Undefined `StateReconciler`:** `StateReconciler` is used in line 491 but is never defined
   anywhere in the document. Its signature, parameters, and error model are completely omitted.

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.hpp:172–194`: `receive`, `pumpOne`, `wake`, and `renew` all
  take `std::chrono::system_clock::time_point wallNow`.
- `apps/common/xanadu/vortex/mesh_actor.cpp:245–253`: `renew` delegates to
  `authorizer_(identity, request)` to issue `ActorGrant`.
- `design/vagent-shared-program-and-live-upgrade.md:489–492`.

**Fix:** Redesign the signature in P17.4 to match `MeshActorRuntime` conventions and define
`StateReconciler`:

```cpp
using StateReconciler =
    std::function<std::expected<void, ActorError>(
        const WorkerContext &oldContext, WorkerContext &newContext,
        std::optional<CellRef> oldRoot, std::optional<CellRef> &newRoot)>;

std::expected<void, ActorError>
upgrade(const ActorPid &pid, const xanadu::identity::Fingerprint &identity,
        const ActorMessage &request,
        std::chrono::system_clock::time_point wallNow,
        const StateReconciler &reconciler);
```

Explain that `upgrade` verifies `authorizer_(identity, request)` to obtain `newGrant`, ensuring
`newGrant.programRef` matches the target upgrade version.

## MAJOR

### 4. (§5, lines 464–471) ProgramManifoldResolver Omits SpanReader

Full title: ProgramManifoldResolver signature omits SpanReader needed by WorkerContext

**Problem:** Proposal P17.3 defines:

```cpp
using ProgramManifoldResolver =
    std::function<const zigzag::Manifold *(std::string_view programRef)>;
```

and states in line 470: "construct `WorkerContext(programManifold, spanReader)`". `WorkerContext`
(`apps/common/xanadu/vortex/worker_context.hpp:52–55`) requires
`(const Manifold *baseManifold = nullptr, const xanadu::SpanReader *reader = nullptr)`. If
`ProgramManifoldResolver` only returns `const zigzag::Manifold *`, `spanReader` is unbound and
cannot be supplied. Omitting `spanReader` leaves `arena.setDefaultSpanReader(nullptr)`, which
prevents `core.render()` and `arena.textOf()` from reading text spans from the base program store.

**Evidence:**

- `apps/common/xanadu/vortex/worker_context.hpp:52–55`.
- `design/vagent-shared-program-and-live-upgrade.md:464–471`.

**Fix:** Update `ProgramManifoldResolver` in P17.3 to return a bundle containing both the manifold
and span reader:

```cpp
struct ProgramBundle {
  const zigzag::Manifold *manifold{nullptr};
  const xanadu::SpanReader *reader{nullptr};
};
using ProgramManifoldResolver =
    std::function<std::optional<ProgramBundle>(std::string_view programRef)>;
```

### 5. (§4.1, line 286; §4.2) Conflation of Actor Mailbox and In-Flight State

Full title: Conflation of actor mailbox with adapter state and missing request invalidation on
replan

**Problem:**

1. Section 4.2 line 294 refers to messages queued in `state.mailbox`. The actor message mailbox
   belongs to `MeshActorRuntime::Actor::mailbox`
   (`std::deque<std::pair<ActorMessage, std::size_t>>`), not `AgentExecutor::State`
   (`apps/common/xanadu/vagent/agent_executor.cpp:297–358`).
1. Section 4.2 line 299 states that pending requests in `state.requests` remain tracked by
   correlation ID, and if an intention replans, late replies are dropped. However,
   `AgentExecutor::PendingRequest` (`agent_executor.cpp:289–295`) maps
   `requestId -> {ask, intention, key, to}`, indexing by intention ID (`int64_t`). If an intention's
   current plan fails during upgrade and deliberation selects a new plan for the same goal under the
   same intention ID, a late reply arriving for the *old* plan will match
   `state.requests[requestId].intention` and resume the intention at an invalid step in the *new*
   plan!

**Evidence:**

- `apps/common/xanadu/vortex/mesh_actor.cpp:69`: `mailbox` is an `Actor` member.
- `apps/common/xanadu/vagent/agent_executor.cpp:289–295, 335`: `struct PendingRequest` and
  `requests`.
- `design/vagent-shared-program-and-live-upgrade.md:294, 299–303`.

**Fix:**

1. Clarify that message queuing is managed in `MeshActorRuntime::Actor::mailbox`.
1. In §4.2 #3, specify that when an intention's plan fails or replans during upgrade, the reconciler
   purges all entries in `state.requests` associated with that intention (or records an invalidation
   epoch on the request), ensuring stale replies cannot resume newly deliberated plans.

## MINOR

### 6. (§1.2, line 67) Non-Existent Lines in vortex_core.cpp Cited

Full title: Citation of non-existent lines in vortex_core.cpp instead of vortex_vm.cpp

**Problem:** Section 1.2 line 67 states: "The Vortex VM
(`apps/common/xanadu/vortex/vortex_core.cpp:1735, 1766, 1831, 1929`) reads the jump/call target via
`readValue` as a raw integer and casts it to `CellRef`." `vortex_core.cpp` has only 586 lines. Jump,
branch, and call instruction execution lives in `apps/common/xanadu/vortex/vortex_vm.cpp:655–694`,
and reads operands via `toInt(inputs[...])`.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_core.cpp`: Total line count is 586.
- `apps/common/xanadu/vortex/vortex_vm.cpp:655–694`: `OpcodeKind::Jump`, `Branch`, `Call` dispatch.
- `design/vagent-shared-program-and-live-upgrade.md:67`.

**Fix:** Update citation to `apps/common/xanadu/vortex/vortex_vm.cpp:655–694` using
`toInt(inputs[...])`.

### 7. (§1.2, line 75) Non-Existent #Unlink and #Step Opcodes Retained

Full title: Retained phantom opcodes #Unlink and #Step in §1.2

**Problem:** Section 1.2 lines 75–76 states: "Every `#Link`, `#Unlink`, and `#Step` instruction
takes its dimension operand as `cInt(...)`." `#Unlink` and `#Step` do not exist in
`zigzag::vortex::OpcodeKind` (`apps/common/xanadu/vortex/vortex_vm.hpp:27–91`). Unlinking is
performed via `OpcodeKind::Break`.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_vm.hpp:27–91`.
- `design/vagent-shared-program-and-live-upgrade.md:75–76`.

**Fix:** Replace `#Unlink` and `#Step` with `#Break` and cursor progression.

VERDICT: NOT CONVERGED
