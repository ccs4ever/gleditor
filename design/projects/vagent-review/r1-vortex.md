# Round 1 critique: Vortex / Vlog / ArenaManifold / Spool-Mesh review of vagent-agent-language.md

Paths are relative to apps/common/xanadu/ unless stated. Line numbers verified against the worktree.

## Line-reference audit

- ActorPid identity_layout.hpp:304, ActorMessage :315, ActorGrant mesh_actor.hpp:44,
  ActorContinuation :94, MeshActorRuntime :111, ArenaManifold arena_manifold.hpp:247, promote
  :834-835: all correct.
- "mesh_actor.cpp:668-682" (doc 7.3, 10.1, 10.4) is stale. checkpointView is 668-696 and the
  delivery loop is 678-683. Cite the function, not a range.
- Invented (nothing in the tree): #Abort, #Trap, ArenaSlice, Ingestion Gate, Grant Renewal Protocol,
  "local re-admission handshake", d.trail ring semantics, `MeshActorRuntime::Authorizer` extension
  of expiresAt.
- Spelling: the real dimension is `d.pinning-cursors` (SystemDimensions::pinningCursors,
  vortex_core.hpp). The doc writes `d.pinning_cursors`.

## Findings

### 1. BLOCKING (sections 4.1, header, 1.4, 6.1, 7.4): the "asks the C++ core for …

Full title: 1. BLOCKING (sections 4.1, header, 1.4, 6.1, 7.4): the "asks the C++ core for nothing"
and Acceptance Test claims are not honest

- `OpcodeKind` (vortex_vm.hpp:24-96) has no Send, Emit, Spawn or host-call opcode. `Executor` is
  `std::function<expected<string>(const ActorExecution&, WorkerContext&)>` (mesh_actor.hpp:~120).
  The WorkerContext carries arena/core/vm/stdlib and no handle to MeshActorRuntime.
- So `emit` (6.1: "calls MeshActorRuntime::send"), `spawn` and `ask` cannot be reached from Vortex
  code today. Owed C++:
  - an executor adapter that drains the outbox ring after the turn and calls `send`;
  - a spawn path through `ActorSpawn`;
  - a clock source for `Schedules`.
- The Acceptance Test text "coupled with the Spool-Mesh actor message-boundary primitives" is an
  escape hatch that admits non-link/value primitives. vicon 8.1 and 8.2 state the owed front end
  honestly (lexer, parser, compiler, driver). vagent has no such section.
- Further C++ the doc needs but never prices:
  - the front end;
  - the executor adapter that applies maxInstructions/maxSolutions (mesh_actor.hpp comment: "must be
    applied by the configured program adapter");
  - the Space registry and subscription (see finding 3).

Fix: add a section "What the C++ core actually has to gain", in vicon 8 style, listing the executor
adapter, outbox drain, spawn bridge and front end. Reword the header and 4.1 to "asks the VM and the
arena for nothing" and drop the "coupled with" clause, or admit the host boundary as a named, priced
exception.

### 2. BLOCKING (7.3, 7.4, 10.4): the checkpoint quiescence description contradicts mesh_actor.cpp

- 7.4 says an emission "completes upon ActorAck", so fire-and-forget emits never block quiescence in
  `deliveries_`. This is wrong.
  - ActorAck sets only `acknowledged` (mesh_actor.cpp:409-416).
  - `completed` is set only when ActorDeliverValue, ActorDeliverWriteSet or ActorDown arrives
    (:425-448).
  - `checkpointView` refuses while any delivery with `source == pid` is `!completed` (:678-683).
  - `deliveries_` is never erased (grep: no erase). Every `emit` with no reply therefore blocks
    every future checkpoint, up to the `maxOutstandingDeliveries` limit (4096), and then `send`
    returns ResourceLimit.
- "mailbox.empty()" is the C++ `actor->mailbox` deque (cap `maxMailboxMessages` 128), not the
  `+d.event` ring. The doc conflates them. Events sitting on `+d.event` in the arena do not count
  toward quiescence. The checkpoint is of the arena rooted at `stateRoot`, so pending +d.event cells
  would be silently captured or lost.
- The "Ingestion Gate" and "Grant Renewal Protocol" do not exist. `receive` enqueues
  unconditionally; `checkpointView` just returns NotQuiescent. Nothing freezes arrivals.
- `baseRef`: the doc says "hash of the committed base ops spool snapshot". `resume` requires
  `grant->baseRef == checkpoint.contentHash` (:216), the published value, and the code comment says
  application state lives in the signed PublishedValue root.

Fix: either add a "completion on ack for fire-and-forget" change to the owed-C++ list (a real
runtime change: ack completes ActorSend deliveries that expect no value, and acked deliveries are
reaped), or specify that vagent emits use request/reply. Delete the Ingestion Gate and Renewal as
built features and move them to "proposed". Rewrite the baseRef description.

### 3. BLOCKING (1.3, 5.4, 6.1, 7.4): Spaces do not exist in MeshActorRuntime, and its …

Full title: 3. BLOCKING (1.3, 5.4, 6.1, 7.4): Spaces do not exist in MeshActorRuntime, and its
ActorSend model is point-to-point and owner-only

- `send(destinationNode, message)` requires `message.target` and rejects a target whose node differs
  from the destination (mesh_actor.cpp:508-512). `receive` returns UnknownProcess without source and
  target (:394-395). ActorSend and ActorMonitor require `actor->owner == identity` (:402-405). Only
  the spawning identity may send to an actor.
- This is exactly the "PID ! Msg" addressing that 1.3 says vagent is not. There is no topic,
  subscription, multicast or filter in mesh_actor.hpp or mesh_swarm.hpp.
- `MeshLensChannel` (mesh_lens_channel.hpp:1-60) is a UI proxy-cell materialization channel
  (Unmaterialized/Resolving/Active/Disconnected). It is not a "local lens channel with zero-copy
  pointer promotion in microsecond latencies" (7.4).
- ActorSend carries "a small inline bencoded value" (vortex-distribution.md wire table, 64 KiB frame
  cap). `ActorGrant::maxOutputBytes` is capped at 32768 (mesh_actor.cpp:135). A "typed payload" with
  `coordinates: [Float;3]` needs an encoding the doc never specifies, and large payloads must go as
  torrents via ActorDeliverWriteSet.
- Requests are deduplicated by `requestId` (`remembered`, `deliveries_`). A duplicate with different
  content is DuplicateMismatch. Event identity is therefore a caller-minted requestId, not
  "content-addressed". The doc must say how it is minted deterministically.

Fix: add an explicit Space layer as owed work (subscriber set per Space, fan-out to N targets,
owner/capability rule that lets non-owner senders emit). Price it and say whether it is a Vortex
program over `d.space` plus per-recipient ActorSend, or C++. Correct the MeshLensChannel claim.
Specify the event wire encoding and the requestId scheme.

### 4. BLOCKING (10.3, 10.4, 6.1 #Abort row): speculation, marks and "ArenaSlice" do not …

Full title: 4. BLOCKING (10.3, 10.4, 6.1 #Abort row): speculation, marks and "ArenaSlice" do not
work as described

- There is no `ArenaSlice`. The only undo primitive is `Mark` (arena_manifold.hpp:170-182) with
  `release`/`discard`, which are LIFO truncation ("assumed well nested", :763). "Rollback resets
  only the slice watermark without destroying interleaved allocations of peer intentions" (10.4) is
  impossible with truncation-based undo.
- `VortexVM::backtrack(cursor)` (vortex_vm.cpp:198-215) pops every choice point above the first one
  belonging to the failing cursor and discards the non-matching ones without `release`. There is one
  `choiceStack_` per VM. Multiple intention cursors failing interleaved will drop each other's
  choice points. Concurrent intentions as separate cursors are unsupported as written.
- `choiceDepth()==0` and `outstandingMarks()==0` at checkpoint force every mark released or
  discarded at turn end. A speculative `<-` write therefore cannot survive a turn boundary. Yet 10.3
  and 10.4 keep shadow cells (`d.shadow`), a `d.trail` ring and "cross-turn macro-intentions"
  holding speculative state. Pick one:
  - (a) speculation is turn-bounded, so d.trail, d.shadow, d.choice and ArenaSlice are redundant.
    The arena trail already undoes `<-` (vicon 4.3: "reversible assignment is free"), and 6.1's
    "appends previous value to d.trail ring" duplicates it; or
  - (b) speculation crosses turns, so the shadows are plain committed cells with hand-written undo
    (a real trail ring, priced), and the "O(1) rollback" claim goes.
- `d.choice` rank and "unlinks choice frames on +d.choice": choice frames are C++ `ChoicePoint`
  structs in `choiceStack_` (vortex_vm.hpp:~100), not cells. `#Choice` takes `altOp` from inputs[0]
  (vortex_vm.cpp:944-960). A `d.choice` rank of alternative plans is at best documentation of altOp
  targets.
- `#Abort` and `#Trap` are not opcodes. Hard contract failure is `ExecutionResult.contractViolation`
  (vortex_core.hpp:~55; Precondition and Postcondition only). 10.1 also says "without introducing
  new VM opcodes", then names #Abort as a "protocol". Define abort as a compiled sequence
  (`cut(barrier)` via `#Cut` with an input, plus unlink) and say so.
- `commit;` as `#Cut` (vortex_vm.cpp:974-985) calls `arena.discard(mark)`. It does not "promote
  speculative allocations" and does not flush an outbox. Those are separate steps the compiler must
  emit. State it.

### 5. MAJOR (4.3): mark/release/promote flow is mis-described

- `mark()` returns a `Mark` value and `release(const Mark&)` takes it (arena_manifold.hpp:638-646).
  It is not a scoped watermark, and release undoes everything since the mark.
- Flagging scratch cells with ephemeralBit is not done by mark(). Every arena ref carries
  ephemeralBit (header comment :24, :459).
- `promote(Store&, MicroversionId parent, const ArenaManifold&, CellRef root, PromotionBudget)`
  returns `std::optional<Promoted>`. It mints Store operations and maps refs. It does not "seal
  nodes into SegmentedOpsSpool" itself, and an actor's WorkerContext has no Store. The mesh
  checkpoint path publishes a PublishedValue and ActorCheckpointView{arena, root}, not Store ops.
  "Operations are merged locally by minting new operations on its local spool" (1) needs a Store per
  agent, owed and unpriced.
- The diagram's `promote()` then `release()` order: after release, refs promoted from that arena are
  truncated away. State that the agent must continue from Store-side refs, or drop promote from the
  per-event path and promote only at checkpoint.
- A `Mark` is only legitimate across events if the next event is not processed inside it; see
  finding 6.

### 6. MAJOR (10.3, 11.1): `ask` suspension contradicts both quiescence and "deadlock freedom"

- A suspended intention awaiting ActorDeliverValue ends its turn with its choice points live
  (choiceDepth != 0). The next event's handler then runs inside that outstanding mark, so its writes
  are undone if the first intention later backtracks. Either `ask` may not occur under a live choice
  point, or events are queued until the intention resumes (which is blocking).
- 11.1 "synchronous RPC-style blocking cycles cannot be constructed" is false given 10.3. Two agents
  that `ask` each other wait forever, with no timeout in the design. Add a deadline (ActorGrant
  expiresAt is lease-level, not per-ask) or restrict the claim.
- 11.1 "entire lifetime can be replayed from genesis": arena state is not logged. Only promoted
  state exists as ops, and timers and wall-clock (`Schedules`, `expiresAt`) are nondeterministic
  inputs. Qualify.

### 7. MAJOR (7.1): ActorGrant semantics are misdescribed and produce a ceiling

- `maxInstructions` and `maxCells` are installed once at spawn via
  `arena.setExecutionLimits(initial + maxCells, maxInstructions, maxOutputBytes)`
  (mesh_actor.cpp:162-169). They are lifetime fuel, not "per event activation" or "per transaction".
  The arena comment says "failed work costs fuel" (arena_manifold.hpp:399).
- Cells are reclaimed only by `release` truncation. `collectGarbage()` only calls `compact()`, which
  discards dead link runs (vortex_core.cpp:513-517). It never frees cells. So "ArenaManifold
  reclaims isolated cells during compaction" and the O(1) topological GC (2.6) are false for cells.
- A long-lived agent therefore monotonically exhausts maxCells and maxInstructions, throws "work
  budget exhausted", gets ExecutionFailed, and `pumpOne` sets `actor->stopped = true` (:501). That
  is exactly the "ceiling users work around" the house style forbids.
- Failure model: any executor failure stops the actor. `#Fail` with an empty choice stack returns
  `success=false` (vortex_vm.cpp:962-972), i.e. kills the agent. So `achieve G` without `else`, or a
  plan with no outer `#Choice`, kills the agent on soft failure. Specify that every top-level
  handler is wrapped in a base `#Choice` whose altOp is the fallback.
- "IntentionFailed escalated to the enclosing holon supervisor" has no mechanism: `link()` is just a
  monitor, with no kill propagation and no supervision (mesh_actor.hpp:~150 and `redispatch`
  comment: "one-for-all supervision not supported").
- Grant Renewal does not exist. `actor->grant` is set only in spawn/resume; `pumpOne` and `receive`
  stop the actor at `expiresAt`. The honest renewal path is `checkpointView` then publish then
  `resume` with a fresh capability (comment on `resume`). Say that, including that it needs the
  quiescence that finding 2 shows is unreachable today.
- Fix: state per-turn fuel as owed (re-arm `setExecutionLimits` each pumpOne in the adapter, price
  it), and specify cell reclamation (periodic checkpoint-and-resume rebuild).

### 8. MAJOR (2.2, 6.1, 6.5, 7.1): agent identity is split between a cell and an actor

- 6.1 `spawn` mints a child headcell linked on `+d.holon` in the parent's arena. 7.1 says every
  agent is an actor with its own arena and ActorPid. A child that is a cell in the parent's arena
  shares its arena, fuel and failure domain, contradicting share-nothing. A child that is an actor
  needs `MeshActorRuntime::spawn` (ActorSpawn, Authorizer, trusted node, maxActors=128 default). The
  holon examples spawn 4 workers, and swarms will exceed 128.
- State which, and what `d.holon` then links (a proxy cell naming an ActorPid?).
- 2.3 and 6.5 give `d.holon` and `d.agent` in a way that satisfies deg\<=1 only for the head. Okay,
  but `Child -> -d.holon -> Agent` in the 2.3 diagram is drawn for Child1 only; fine. The
  "population node" is never defined.
- The 6.1 `HolonGoalFailed` "Merkle event" (6.5) is unexplained. Nothing in the tree is a Merkle
  event.

### 9. MAJOR (4.2): the dual-wing description overstates what Call does

- `Call` (vortex_vm.cpp:665-687) pushes a return frame onto `+d.stack` and sets the cursor's opcode
  to inputs[0]. It does not bind arguments into the callee. How a skill method reads its arguments,
  and where `let mut` skill state lives across calls and reentrant calls, is unspecified.
- Precondition failure (step 1) returns a ContractViolation result. Combined with finding 7 it stops
  the actor. Say so.
- Dynamic skill lookup is a rank walk in Vortex code with Eq/Branch, which is feasible, but then
  6.1's "constructs call opcode cell at call time" mints a cell per call (fuel). Note the cost, or
  precompile call sites and patch inputs[0].
- Citation "vortex_core.hpp:34-41" is the SystemDimensions struct, not the calling convention. Cite
  vortex_vm.cpp step() (:995-1100).

### 10. MINOR

- 1.2 and 2.6 use `noCell == 0` and `-1`/`0` link targets as sentinels. This is inherited from
  Vortex 1 (vortex-hyperstructural-runtime.md:96, :729), but the C++ API returns `std::optional`
  (`ArenaManifold::link`). Say these are Vortex-level encodings only.
- 2.2 does not say how the 16 new dims are minted. The vicon precedent (R12: a dimension is a cell
  on `d.dims`) should be cited, and each dimension should be priced.
- 3.2 class diagram: `ArenaManifold.value()` does not exist (it is `VortexCore::value`).
  `ActorMessage` omits target, programRef, hashcashStamp, capability, reason, sequence.
- Grammar: the `Schedules` capacity defines `every(...)`, which collides with the `EveryStmt`
  keyword. `in(...)` collides with `for ... in`.
- 10.5 `commit;` inside `scan` and the `scan packet` block: vicon 6 is the authority; cite it where
  the semantics (`&pos` trailed) are inherited.
- Missing house-style sections: refused alternatives, prices per ruling, breakages and costs,
  reconciliation with store-slice-convergence (vicon has 11 and 12).

## What checks out

- `#Choice`/`#Fail`/`#Cut` opcodes exist and work on the arena Mark as described
  (vortex_vm.cpp:944-985, 178-240).
- `mark`/`release`/`discard`, `outstandingMarks`, `choiceDepth`, `compact` refused while marked: all
  real.
- `checkpointView` really refuses on a non-empty mailbox, live choice stack, outstanding marks and
  incomplete deliveries.
- `d.scan` exists in SystemDimensions; the std:goal scan design is vicon's.
- `ActorGrant`, `ActorContinuation` field lists in 3.2 and 7.3 match the headers.

VERDICT: NOT CONVERGED
