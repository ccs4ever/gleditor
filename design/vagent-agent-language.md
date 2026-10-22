# vagent: An Agent-Oriented Language over Vortex and the Spool-Mesh

| field         | value                                                                                                                                                                                                                       |
| ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| version       | 3.0 (see [Appendix](#appendix-versioning-and-change-history))                                                                                                                                                               |
| status        | Specification only. Nothing here is built or wired into the gleditor build.                                                                                                                                                 |
| compiles to   | Vortex opcode streams over an `ArenaManifold`, run one turn at a time by a Spool-Mesh actor executor                                                                                                                        |
| rests on      | Vortex ([runtime](vortex-hyperstructural-runtime.md)), Vlog ([logic](vlog-logic-extension.md)), vicon ([goals](vicon-goal-directed-language.md)), the Spool-Mesh actor runtime (`apps/common/xanadu/vortex/mesh_actor.hpp`) |
| prerequisites | Vlog §6 clause database (specified, unbuilt); vicon's front end and `std:goal` (partly built, `apps/common/xanadu/vicon/`, with the deviation recorded at `vicon/compiler.cpp:12-22`)                                       |
| owes C++      | one opcode, a front end, a mesh executor adapter, and five changes in `mesh_actor.cpp` ([§11](#11-what-the-c-core-has-to-gain))                                                                                             |

`vagent` is a front end for agent-oriented programs. It takes SARL's metamodel (agents, events,
capacities, skills, behaviors, spaces, contexts, holons) and adds the parts of the BDI model SARL
leaves to libraries (beliefs, declarative goals, a plan library, intentions). An agent is one actor
of `MeshActorRuntime` with a private `ArenaManifold`. It runs one turn at a time, and evaluates with
vicon's goal-directed `#Choice`/`#Fail`/`#Cut` regime inside a turn. Everything an agent does
outside its own arena (messages, spawns, device actions) is staged during the turn and performed
after it.

The siblings are [Vortex](vortex-hyperstructural-runtime.md) (the runtime),
[VQL](vql-query-language.md), [VPL](vpl-array-language.md), [Vlog](vlog-logic-extension.md),
[vlace](vlace-functional-language.md) and [vicon](vicon-goal-directed-language.md).
[`vql-agent-experiment.md`](vql-agent-experiment.md) tried the same metamodel as a VQL extension;
[§13](#13-refused-alternatives) records why this document does not take that route.

The document separates what is built, what is a prerequisite specified elsewhere, and what this
document owes. Claims an earlier version made and the tree does not support are withdrawn, and the
change history lists them.

______________________________________________________________________

## Contents

- [1. Status of each mechanism](#1-status-of-each-mechanism)
- [2. Glossary](#2-glossary)
- [3. Rulings](#3-rulings)
- [4. The agent as one actor](#4-the-agent-as-one-actor)
- [5. Structure in the arena](#5-structure-in-the-arena)
- [6. Deliberation](#6-deliberation)
- [7. Communication and perception](#7-communication-and-perception)
- [8. The surface language](#8-the-surface-language)
- [9. Lowering](#9-lowering)
- [10. The `std:agent` package](#10-the-stdagent-package)
- [11. What the C++ core has to gain](#11-what-the-c-core-has-to-gain)
- [12. Worked examples](#12-worked-examples)
- [13. Refused alternatives](#13-refused-alternatives)
- [14. Properties, with their conditions](#14-properties-with-their-conditions)
- [15. Breakages and costs](#15-breakages-and-costs)
- [16. Reconciliation with the convergence](#16-reconciliation-with-the-convergence)
- [17. Comparison](#17-comparison)
- [18. Open questions](#18-open-questions)
- [Appendix: Versioning and Change History](#appendix-versioning-and-change-history)

______________________________________________________________________

## 1. Status of each mechanism

"Built" means present in the tree at the cited place. "Prerequisite" means specified by a sibling
document and not yet built. "Owed" means this document asks for it and
[§11](#11-what-the-c-core-has-to-gain) prices it.

| Concept                    | Mechanism                                                                                                        | State                                                                     |
| -------------------------- | ---------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| Agent identity             | One `MeshActorRuntime` actor; `ActorPid` (`identity_layout.hpp:304`)                                             | built                                                                     |
| Agent state                | Cells under one agent cell in the actor's `ArenaManifold` (`mesh_actor.cpp:156`)                                 | built (arena); layout owed by the compiler                                |
| A turn per message         | `MeshActorRuntime::pumpOne` runs the executor on one mailbox message (`mesh_actor.cpp:458`)                      | built                                                                     |
| A turn with no message     | A local wake that runs the executor without a mailbox entry                                                      | **owed** (§11.3 d)                                                        |
| Turn executor              | A `MeshActorRuntime::Executor` that decodes, dispatches, drains the outbox                                       | **owed** (adapter, §11.2)                                                 |
| Speculation in a turn      | `#Choice`/`#Fail`/`#Cut` over `mark`/`release`/`discard` (`vortex_vm.cpp:944-985`)                               | built                                                                     |
| Bounded statements         | Cut to the depth recorded at the statement's start                                                               | **owed**: one opcode, `#ChoiceDepth` (§11.1)                              |
| Reversible assignment `<-` | A write the arena trails under the live mark; vicon's `Bind` reverts on backtrack (`vicon/compiler.cpp:358-361`) | built                                                                     |
| Beliefs                    | Clauses on `d.clause` off predicate cells (Vlog §6.1); unification by `#Unify`                                   | `#Unify` built; clause database a prerequisite; `std:agent` routines owed |
| Goals, plans, intentions   | Intention cells holding goal, plan ordinal, step path, waits and frame                                           | owed in Vortex (`std:agent`)                                              |
| Capacity dispatch          | A walk of the agent's `d.skill` rank comparing capacity names                                                    | owed in Vortex                                                            |
| Messages                   | Outbox entries; the adapter calls `MeshActorRuntime::send` after the turn                                        | **owed** (adapter)                                                        |
| Effects (device actions)   | Outbox entries; the adapter runs host bindings and reports completion as a local delivery                        | **owed** (adapter, §11.3 d)                                               |
| Perception                 | Host drivers post percept events as local deliveries                                                             | **owed** (adapter, §11.3 d)                                               |
| Spaces                     | A host agent holds a member rank and relays emissions                                                            | owed in Vortex; single-owner only until §11.3 c                           |
| Spawning a child           | `MeshActorRuntime::spawn` (`mesh_actor.hpp:132-134`) under a capability the adapter issues                       | runtime built; issuer and bridge **owed**                                 |
| Child loss                 | `monitor` and `ActorDown` after lease expiry and discovery evidence (`mesh_actor.cpp:640-660`)                   | built; minutes of latency (§5.4)                                          |
| Timers and deadlines       | Deadlines stored in cells; the adapter derives its timer heap from them                                          | **owed** (adapter)                                                        |
| Checkpoint and migration   | `checkpointView`, `publishValue`, `resume` (`mesh_actor.cpp:669-696`, `mesh_value.hpp:88-93`)                    | built                                                                     |
| Long-lived agents          | Reaping deliveries, windowing request ids, configurable value limits                                             | **owed** (§11.3 a, b, e)                                                  |
| Swarm-wide spaces          | `mesh_swarm` topics bound to a space                                                                             | not proposed ([§18](#18-open-questions), U-A4)                            |

vagent asks for **one opcode and no other change to the VM, no change to `ArenaManifold`, no change
to `CompactOpNode`, and no new `SystemDimensions` field**. The opcode is needed by vicon as well
(§11.1). Everything else it owes is at the boundary between the VM and the mesh runtime, which
Vortex code cannot cross: `OpcodeKind` has no send, spawn or host-call opcode
(`vortex_vm.hpp:24-96`), and the `WorkerContext` an executor receives has no handle to the runtime
(`mesh_actor.hpp:114-116`). AGENTS.md admits C++ for driver and OS-event interfacing, which is what
that boundary is.

______________________________________________________________________

## 2. Glossary

| Term                   | Meaning                                                                                                                                                    |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| agent                  | One actor of `MeshActorRuntime` running one compiled `agent` declaration                                                                                   |
| agent cell             | The cell that heads every rank of agent state; the actor's `stateRoot`                                                                                     |
| peer cell              | A cell whose content is an encoded `ActorPid`; how an agent names another. Peer cells are values: two naming one agent are two cells. (Not an `OpHandle`.) |
| turn                   | One run of the executor: a message turn for one mailbox message, or a wake turn for local work                                                             |
| event                  | A typed record: delivered in an `ActorSend`, posted locally by a host driver, or raised internally; decoded into an event cell for the turn                |
| performative           | A message kind with fixed receiver semantics: `tell`, `untell`, `achieve`, `cancel`, `refuse`, `ask`, `outcome`                                            |
| outbox entry           | A cell on `d.outbox` describing a message, spawn or effect the adapter performs after the turn                                                             |
| envelope               | The `ActorMessage` the adapter builds from an outbox entry; never visible to vagent code                                                                   |
| space                  | A named set of member peer cells held by one host agent, which relays every emission into it                                                               |
| context                | SARL's grouping of spaces. Each agent can name two: `inner` (the space it hosts) and `outer` (the space it was spawned into)                               |
| holon                  | An agent with children; its children are the members of its `inner` space                                                                                  |
| launcher               | The agent `vagent run` starts first; it hosts the node's default space                                                                                     |
| capacity               | A named set of method signatures                                                                                                                           |
| skill                  | An implementation of one or more capacities: written in vagent, or native (bound by the adapter to host code)                                              |
| behavior               | A reusable bundle of handlers, fields, goals and plans mounted on an agent                                                                                 |
| belief                 | A ground fact in the agent's belief base, with the set of sources that assert it                                                                           |
| goal                   | A declared desire with a success test (achieve), a holding condition (maintain) or neither (perform)                                                       |
| plan                   | A procedure declared `for` a goal, with an applicability condition                                                                                         |
| intention              | A goal instance being pursued, stored as cells: id, goal, plan, step path, waits and frame                                                                 |
| suspension point       | A statement at which an intention ends its turn and waits                                                                                                  |
| clone view             | A cell on another cell's `d.clone` rank that reads its master's content (Vortex §5.2)                                                                      |
| mark, release, discard | `mark` records the arena's extent; `release` undoes every write since it; `discard` forgets it and keeps the writes (`arena_manifold.hpp:638-646`)         |

Every structure below is an open **rank** with a head, as `store-slice-convergence.md` and the
Vortex document use the word. The mailbox is the runtime's C++ deque, not a structure in the arena.

______________________________________________________________________

## 3. Rulings

Each ruling states what it decides, why, and what it costs. Alternatives are in
[§13](#13-refused-alternatives).

**V1. An agent is one actor; there are no links between arenas.** `MeshActorRuntime::spawn` gives
each actor its own `WorkerContext` (`mesh_actor.cpp:156`). An agent names another only through a
peer cell and affects another only by message. The node's runtime keeps the actor table; no arena
holds a population. *Price:* a holon cannot read its children's state, and every holon interaction
is a message. The runtime's `ActorOptions` (128 actors, 128 mailbox messages,
`mesh_actor.hpp:57-65`) bound a node; they are constructor options the launcher reads from
`system://settings`.

**V2. Speculation is bounded by the turn.** Every choice point and mark opened in a turn is gone
before the turn ends. `checkpointView` refuses a non-zero `choiceDepth()` or `outstandingMarks()`
(`mesh_actor.cpp:673-675`), so this is the only regime under which an agent can be checkpointed.
`<-` is the arena's trailed write and nothing more. *Price:* a reversible assignment is reversible
only within its statement. Alternatives that remain when a turn ends are re-derived, not resumed.

**V3. One cursor per agent.** `VortexVM::backtrack` pops choice points belonging to other cursors
without releasing them (`vortex_vm.cpp:198-215`), so two interleaved speculative cursors in one VM
would corrupt each other. vagent never runs two. *Price:* intentions take turns (§6.5); parallelism
is between agents.

**V4. Intentions persist as data, never as VM state.** An intention is a set of cells: a numeric id,
its goal, the ordinal of its plan, the ordinals already tried, its step path, what it waits for
(with a deadline), its delegator if any, and a frame of plan-local variables. On resume the executor
re-enters the plan at the step path. This matches `ActorContinuation`'s comment that VM program
counters stay local and application state lives in the published root (`mesh_actor.hpp:92-93`).
*Price:* suspension points are allowed only where a step path and a frame describe the whole state
(§6.6).

**V5. Everything outside the arena is staged.** Messages, spawns and effects are outbox entries on
`d.outbox`. They are minted under whatever mark is live, so a backtrack removes them for free. The
adapter performs what remains after a successful turn. A turn that fails performs nothing. *Price:*
nothing leaves mid-turn; a plan that needs an answer or a device result suspends (§6.6).

**V6. Effects are asynchronous and suspend.** A capacity method declared `effect` is implemented
only by a native skill, which the adapter binds to host code. Calling one in a plan writes an outbox
entry and suspends the intention; the host runs the action after the turn and reports its result or
failure as a local delivery that resumes the intention. Calling one in a handler stages it without
waiting. *Price:* an effect can never be one arm of a search, and a plan pays a turn per effect. In
exchange, no device call runs inside a turn, so a failed or aborted turn never leaves the world
changed and the arena rolled back, and an agent never blocks on a device.

**V7. Iteration that acts is deterministic.** The arena trails every write to a cell older than the
live mark, and vicon's compiler records that even `:=` cannot skip the trail
(`vicon/compiler.cpp:358-361`). Icon's `every e1 do e2` drives `e1` by failing back into it, so
every write `e2` makes is undone at the next resumption. vagent has no `every`. It iterates with
`for x in e do body` over a list, a range or a belief query, walked without a choice point (§8.2).
*Price:* a generator cannot drive a loop that acts. A search that collects results walks a rank, as
`findall` does (§6.1).

**V8. Beliefs are a set of facts with provenance.** The belief base is a rank of predicate cells on
`d.belief`, each heading its clauses on `d.clause` as Vlog §6.1 lays them out. Believing a fact
already present adds a source and raises no event. A fact is removed when its last source withdraws.
*Price:* lookup is a linear clause walk until convergence U1 (rank indexing) lands, as Vlog §6.3
records. The database itself is a prerequisite (Vlog §6 is unbuilt).

**V9. Agent working state is arena state; the checkpoint is the one promotion.** R8 says an arena
mints no operations. vagent never promotes into a store the user visits. A checkpoint, however, is
`publishValue`, which promotes the reachable state into the published value's own transient `Store`
(`mesh_value.hpp:88-93`). That is the only promotion, and §4.4 prices it. An agent that must write
to a user's store uses an explicit `StoreWriter` capacity. *Price:* checkpoint cost grows with live
state, and lifetime replay from a spool is not on offer (§14).

**V10. Eleven dimensions, minted as data.** `std:agent` mints `d.behavior`, `d.skill`, `d.capacity`,
`d.space`, `d.member`, `d.holon`, `d.intention`, `d.subgoal`, `d.outbox`, `d.belief` and `d.pending`
on the `d.dims` rank when it loads, as R12 requires and as `VortexCore::mintNamedDimension` does
(`vortex_core.cpp:16-27`). C++ learns none of their names. *Price:* eleven cells per arena, and a
name lookup per dimension per load.

**V11. Values are copied; nothing is pointed at twice.** A link is reciprocal, so a cell has one
negward neighbour per dimension. Storing a value into a field, frame variable, event field, belief
argument or outbox entry copies its cells. The one deliberate sharing is a skill's field frame,
which its per-capacity instances reach through clone views (§5.3). *Price:* O(size) per store of a
list or map, paid by the writer.

**V12. Agent state never links into the program.** A checkpoint promotes everything reachable from
the agent cell along any dimension (`arena_manifold.hpp`, the comment above `promote`). A link from
state into a program cell would drag the compiled program and `home` into every checkpoint. So
capacities are matched by name, plans and handlers by ordinal, and the program region is never
written at run time. *Price:* a name comparison per dispatch step. Ordinals are valid only for the
same `programRef`, which `resume` already requires (`mesh_actor.cpp:216`).

**V13. Bounded statements cut to a recorded depth.** A statement takes its first result and drops
its choice points (vicon §4.4). vicon's flag-and-drain scheme cannot do this, because the flags are
trailed and undone (`vicon/compiler.cpp:12-22`). vagent records the choice depth when a statement
starts and executes `#Cut` to that depth when it produces. Reading the depth needs one opcode
(§11.1). *Price:* one opcode and one cell per statement.

**V14. Goal-directed structure, statically typed spellings.** Expressions succeed with a value or
fail; there is no boolean test in control flow. Precedence follows Icon for `&`, `|`, `to` and
assignment. Spellings follow the statically typed family (`==`, `!=`, `++`), because one typed `==`
replaces Icon's separate numeric and lexical comparisons. §8.11 lists every divergence from vicon.
*Price:* a vicon reader must map the spellings.

**V15. SARL's surface where SARL has one.** Handlers are `on Event [guard] { }`, the event is
`occurrence`, capacities are imported with `uses`, emission is scoped. §8.10 lists the differences.

______________________________________________________________________

## 4. The agent as one actor

### 4.1 One agent, one actor, one arena

Spawning an agent spawns one actor under a grant. The runtime's `Authorizer` turns a signed
capability into an `ActorGrant` (`mesh_actor.hpp:111-113`), checks that its `programRef` matches the
request, and caps `maxOutputBytes` at 32 KiB (`mesh_actor.cpp:131-139`). A running agent cannot sign
anything, so the adapter holds a **capability issuer** supplied by the host (the owner's delegated
device key). It signs a child's capability with figures no larger than the parent's own grant.

Four facts about the runtime shape what follows:

- **Only the actor's owner may send to it.** `receive` refuses an `ActorSend` from any identity but
  the owner (`mesh_actor.cpp:402-405`). vagent v3.0 specifies single-owner societies.
- **A failed executor stops the actor.** `pumpOne` sets `stopped` when the executor returns an error
  (`mesh_actor.cpp:501`). The adapter reports success for every language-level outcome and returns
  an error only for its own faults.
- **A grant expires.** At `expiresAt`, `receive` refuses and `pumpOne` stops the actor with no
  notice to anyone (`mesh_actor.cpp:438-440`, `462-466`). §4.5 gives the renewal path.
- **Spawn carries no arguments.** `ActorSpawn` must have an empty `value` (`actor_wire.cpp:87-90`).
  Agent parameters travel in the first message, `Initialize`.

### 4.2 The turn

A **message turn** handles one mailbox message. A **wake turn** handles local work: a ready
intention, a due timer, or a host delivery (an effect result or a percept). The adapter asks for
wake turns through the local wake of §11.3 (d). Both kinds run the same steps:

1. Re-arm fuel: `setExecutionLimits(cellCount() + maxCells, maxInstructions, maxOutputBytes)`
   (`arena_manifold.hpp:400`). The runtime sets these once at spawn (`mesh_actor.cpp:165`), which
   makes them lifetime totals; the adapter makes them per turn.
1. Read the clock once and write the turn time into the turn's event cell, so the turn's input is in
   the arena.
1. Push a base choice point (`VortexVM::pushChoicePoint`, which takes the turn's mark) whose
   alternative is the adapter's failure routine.
1. Decode the message or host delivery into an event cell, or pick the ready intention.
1. Run the dispatcher (§6.5) or continue the intention.
1. Run end-of-turn deliberation: reconsider live intentions, check maintain goals, wake matching
   `await`s (§6.5).
1. On success, `clearChoicePoints()` (`vortex_vm.cpp:242`), which discards every mark and keeps the
   writes. On a hard failure, backtrack to the base choice point, which releases the turn's mark and
   restores the arena, then run the recovery step below.
1. Drain `d.outbox` in order: spawns first (filling their peer cells), then messages, then effects,
   then timer changes. Unlink each entry.
1. Rebuild the adapter's ready list and timer heap from the intention and timer cells.

**Recovery after a hard failure.** The arena is back where it was, but the message or wake that
caused the turn is consumed. If the turn was running an intention, the adapter runs a second, short
turn of `std:agent`'s `fail_step(id, cause)`, which marks that plan failed with cause `fuel`,
`depth` or `contract`. No user code runs in it. Selection then proceeds as for any plan failure. If
the turn was handling an event, the event is dropped and counted (`dropped_events`).

At the end of step 9 no mark or choice point is live. The agent is quiescent, and `checkpointView`
accepts it, when in addition its mailbox is empty, no message it sent awaits completion, and the
adapter holds no undelivered host delivery for it. Ready intentions and pending timers do not
prevent a checkpoint: they are cells and travel with it.

### 4.3 What persists

| Structure                          | Where                                                | Survives the turn | In the checkpoint root |
| ---------------------------------- | ---------------------------------------------------- | ----------------- | ---------------------- |
| Fields (`var`)                     | `d.vars`/`d.values` off the agent cell               | yes               | yes                    |
| Beliefs and their sources          | `d.belief`, `d.clause`, `d.source`                   | yes               | yes                    |
| Installed skills and their fields  | `d.skill`; skill frames through `d.clone`            | yes               | yes                    |
| Mounted behaviors and their fields | `d.behavior`                                         | yes               | yes                    |
| Intentions, frames, waits          | `d.intention`, `d.subgoal`                           | yes               | yes                    |
| Timers                             | the `timers` field owned by `std:agent`              | yes               | yes                    |
| Hosted spaces and members          | `d.space`, `d.member`                                | yes               | yes                    |
| Children                           | `d.holon`                                            | yes               | yes                    |
| The turn's event cell              | unlinked at turn end                                 | no                | no                     |
| Internal events of the turn        | `d.pending`, emptied during the turn                 | no                | no (always empty)      |
| Outbox entries                     | `d.outbox`, drained after the turn                   | no                | no (always empty)      |
| Choice points, marks               | VM and arena                                         | no                | no (V2)                |
| Compiled program                   | named by `programRef`; never linked from state (V12) | n/a               | no                     |

### 4.4 Cells, checkpoints and their costs

An arena frees cells only by `release` truncation (`arena_manifold.hpp:638-646`). `compact()` drops
dead link entries and frees no cell (`arena_manifold.hpp:656-672`), and no reachability sweep is
built. A long-lived agent therefore accumulates unreachable cells: consumed event cells, finished
intentions, retracted clauses, uninstalled skills.

vagent reclaims them by **checkpoint and resume**. `checkpointView` hands the arena and root to
`publishValue`, which promotes what is reachable into a transient store, seals and signs it.
`resume` rebuilds an arena from that history (`mesh_actor.cpp:255-262`), and only reachable cells
come back. The costs, all paid per checkpoint:

- **Operations.** Promotion mints about one operation per live cell plus one per link. It is bounded
  by `PromotionBudget::maxOps` (65,536, `arena_manifold.hpp:232-234`) and by
  `ValueLimits::operations` (65,536, `mesh_value.hpp:28`), which `resume` further clamps to the
  grant's `maxCells` (`mesh_actor.cpp:259`). `resume` builds its `ValueLimits` locally, so the bound
  cannot be raised today; §11.3 (e) makes it an option. Until then an agent whose live state needs
  more than 65,536 operations cannot be checkpointed, moved or reclaimed.
- **Permascroll growth.** Promotion spools constructed bytes (string fields, belief constants) into
  the author's permascroll (`arena_manifold.hpp`, the comment above `promote`, step 1). The
  permascroll is append-only, so every checkpoint re-appends the live strings.
- **Quiescence.** A continuously busy agent may never reach the conditions of §4.2 (U-A1).

### 4.5 Leases and renewal

A grant's `expiresAt` ends the agent. The renewal path the runtime offers is checkpoint, publish,
and `resume` under a fresh capability that names the checkpoint's content hash as its base
(`mesh_actor.hpp`, the comment on `resume`). The adapter starts renewal when `vagent.renew_fraction`
of the lease has elapsed: it stops asking for wake turns, waits for quiescence, and resumes with a
capability from the issuer. If the lease ends first, the agent stops. Before a planned stop (lease
end without renewal, or `Destroy`), the adapter stages a `ChildStopping` message to the parent, so a
parent learns of an orderly stop at once and of a crash only through `ChildDown` (§5.4).

______________________________________________________________________

## 5. Structure in the arena

### 5.1 Dimensions

Minted by `std:agent` at load (V10):

| Dimension     | From → to                                     | Shape    | Why not an existing dimension                                                    |
| ------------- | --------------------------------------------- | -------- | -------------------------------------------------------------------------------- |
| `d.behavior`  | agent cell → behavior instances               | rank     | `d.vars` is the name-keyed field table; behaviors are not named fields           |
| `d.skill`     | agent cell → skill instances                  | rank     | as above; dispatch walks this rank and nothing else                              |
| `d.capacity`  | skill instance → its own capacity-name cell   | one link | a reference by name to something outside the arena (V12)                         |
| `d.space`     | agent cell → spaces it hosts                  | rank     | no existing rank lists hosted spaces                                             |
| `d.member`    | space cell → member peer cells                | rank     | `d.holon` is children only; a space can hold non-children                        |
| `d.holon`     | agent cell → peer cells of its children       | rank     | kept apart from `d.member` so monitoring and delegation never scan a space       |
| `d.intention` | agent cell → top-level intention cells        | rank     | `d.cursors` holds VM processes with program counters; an intention has none (V4) |
| `d.subgoal`   | intention → the subgoal intention it awaits   | one link | an intention awaits at most one subgoal                                          |
| `d.outbox`    | agent cell → outbox entries                   | rank     | must be distinct so the drain touches nothing else                               |
| `d.belief`    | agent cell → predicate cells                  | rank     | Vlog has `d.clause` for clauses but no directory of an agent's predicates        |
| `d.pending`   | agent cell → internal events raised this turn | rank     | the dispatcher needs a queue it can walk in Vortex; emptied before the turn ends |

Reused with their existing meanings: `d.vars`/`d.values` (fields, frames, event fields, as
`VortexCore::exportSymbol` uses them, `vortex_core.cpp:117-125`); `d.clause` (clauses, and on the
program side the alternatives of a goal or an event type, §5.7); `d.clone` (skill frames);
`d.source` (a belief's provenance, the meaning `result_slice.cpp:34` gives it); `d.scan` (vicon's
scanning cursor).

Attachment uses `ArenaManifold::insertIntoRank` (`arena_manifold.hpp:617`) or `zigzag::rankTail`
followed by `link` (as at `vortex_core.cpp:113`), never a bare `link`, which evicts whatever either
end held (`arena_manifold.hpp:582-592`). Removal links a cell's predecessor to its successor, which
leaves the removed cell with no link on that dimension.

### 5.2 The agent cell

```text
[agent cell "PatrolDrone"]
  ├─(+d.vars)──► [targets_seen] ─(+d.values)─► [3]
  │                └─(+d.vars)─► [timers] ─(+d.values)─► [list …]
  ├─(+d.skill)─► [instance] ─(+d.skill)─► [instance]
  │                ├─(+d.capacity)─► ["mars:Navigation"]
  │                └─(-d.clone … master)─ [skill frame ThrusterNav] ─(+d.vars)─► fields
  ├─(+d.behavior)─► [ObstacleAvoidance] ─(+d.vars)─► fields
  ├─(+d.belief)─► [pred at/2] ─(+d.clause)─► [at(3,4)] ─(+d.source)─► [peer] ─(+d.source)─► [peer]
  ├─(+d.intention)─► [intention 7: Survey] ─(+d.intention)─► [intention 9: KeepCharged]
  │                      └─(+d.subgoal)─► [intention 8: Reach]
  ├─(+d.space)─► [space inner] ─(+d.member)─► [peer] ─(+d.member)─► [peer]
  ├─(+d.holon)─► [peer child1] ─(+d.holon)─► [peer child2]
  ├─(+d.pending)─► (empty between turns)
  └─(+d.outbox)─► (empty between turns)
```

Each arrow is the only link its source has in that direction on that dimension, and each target has
only that one negward link. No arrow leaves the agent's state for the program or `home` (V12).

### 5.3 Capacities and skills

Installing a skill mints one **skill frame** cell, which holds the skill's fields on `d.vars`, and
one **instance** cell per capacity the skill implements, including capacities those extend. Each
instance is appended to the agent's `d.skill` rank, appended posward to the frame's `d.clone` rank,
and linked along `+d.capacity` to its own cell holding the capacity's qualified name. The frame is
the clone master and is never removed while an instance exists. A method body reaches its fields as
`cloneMaster(instance, d.clone)` then `d.vars` (`arena_manifold.hpp:522-523`).

A capacity call walks `d.skill` and takes the first instance whose capacity name matches. The cost
is O(installed instances). `install S` first uninstalls every installed skill that shares any
capacity with `S`, whole, then installs `S`, so a skill is never split across capacities.
`uninstall` runs the skill's `on uninstall` block, removes its instances, then its frame.

A **native skill** has no body. The adapter binds it by qualified name to host code registered at
start-up. Its methods are effects (V6), and it is the only kind of skill that may implement an
`effect` method.

### 5.4 Spaces, contexts and holons

A space is a cell on its host's `d.space` rank, with members as peer cells on `d.member`. Every
agent hosts its `inner` space from creation; children join it when spawned. `outer` is a reference
to the parent's `inner` (the parent's peer cell and the space name). A top-level agent's `outer` is
the launcher's space.

An emission into a space the agent does not host is one `ActorSend` to the host with a relay marker;
the host's `std:agent` relay handler sends one `ActorSend` per member, excluding the emitter. An
emission into a hosted space fans out directly.

A holon's children are separate actors, named by peer cells on `d.holon`. A child knows its parent
only through the spawn request's source, exposed as `parent`. The holon monitors each child through
`MeshActorRuntime::monitor`. `ActorDown` reaches a watcher only after phi-accrual suspicion, the
lease (60 s by default, `mesh_actor.hpp:63`), and discovery evidence supplied through `observe()`
(`mesh_actor.cpp:640-660`). So `ChildDown` means "the child's node is unreachable", minutes later.
An orderly stop is announced at once by `ChildStopping` (§4.5).

### 5.5 Events

An event cell's content is the event type's name. Its fields hang on `d.vars`/`d.values`, with
scalars as R6 scalar cells. Four implicit fields are always present and their names may not be
declared: `source` (the emitter's peer cell, or none for a host delivery), `space` (the space it
arrived through, or none), `time` (the turn time, §4.2) and `ordinal` (the emitter's emission
counter).

On the wire the event is the `value` of an `ActorSend` (`identity_layout.hpp:315-327`): a bencoded
dictionary of kind, type name, ordinal, fields and correlation id. `value` is limited to 32 KiB
(`actor_wire.cpp:80`); a larger payload needs U-A3. `sequence` must be zero for `ActorSend`
(`actor_wire.cpp:94-96`), so ordering information travels in the value. The `requestId` is
`sha256(emitter pid ‖ turn number ‖ position in the outbox)`, so a turn the adapter re-drains
produces the same ids, and the runtime's de-duplication treats a resend as a resend
(`mesh_actor.cpp:333-337`).

An event type that `extends` another matches handlers for both. The wire carries only the type name;
the receiver resolves the chain from its own program, so sender and receiver must agree on the
declaration.

### 5.6 Intentions

An intention cell's `d.vars` holds system attributes only: `id` (from a counter field on the agent
cell), `goal` (name and copied arguments), `plan` (ordinal in the goal's plan list), `tried`, `step`
(the step path), `waiting` (kind, key, deadline), `delegator` (a peer cell and correlation id, when
adopted for another agent), `blocked_until` (maintain back-off, §6.3) and `frame`. User plan-local
variables hang on the `frame` value's own `d.vars`, so a plan variable named `step` cannot collide
with the system's.

Top-level intentions are on `d.intention`. A subgoal intention is reached only along its parent's
`+d.subgoal`. Messages and timers name an intention by its `id`, never by `CellRef`, because refs
change when `resume` rebuilds the arena. Every deadline is an absolute time in a scalar cell, so the
adapter's timer heap is rebuilt from cells after every turn and after `resume`.

### 5.7 Program-side structure

The compiled program is loaded from `programRef` and never written at run time (V12). Its module
symbol table (`d.vars` off the module cell, as `exportSymbol` builds it) holds one name cell per
goal and per event type. Each heads a rank on `d.clause` of its alternatives: the plans for a goal
in plan-list order (§6.2), and the handlers for an event type in descending priority then
declaration order. This is Vlog's predicate-and-clauses shape reused: a goal is a predicate whose
clauses are plans. A behavior's program contributes its own ranks, and the dispatcher walks the
agent's then each mounted behavior's. An ordinal is a position on such a rank, valid for one
`programRef`.

______________________________________________________________________

## 6. Deliberation

### 6.1 Beliefs

The belief base is a set of facts, each with a set of sources (V8).

- `believe at(rover, 3, 4)` adds the fact with source `self`. If the fact is new, it raises a
  `+at/3` internal event; if it was present, it only adds the source.
- `forget at(rover, ?x, ?y)` removes source `self` from every matching fact. A fact with no source
  left is retracted and raises `-at/3`.
- `believes at(rover, ?x, ?y)` is a generator producing once per matching fact, binding `x` and `y`.
  `believes t from s` matches only facts with `s` among their sources.
- `for believes at(rover, ?x, ?y) do body` runs `body` once per matching fact.
- `findall(?x, at(rover, ?x, _))` is the list of bindings.

`findall` and `for believes` do not visit clauses by backtracking, because a result gathered inside
a branch is truncated when the branch resumes (V7). They first copy the predicate's clause list,
which gives Vlog's logical update view (Vlog §6.2): facts added or removed by the loop body are not
visited. Then for each clause they copy the query term with fresh variables, take a choice point,
and unify. On failure the choice point's mark is released. On success it is cut, keeping that copy's
bindings. The cost is one term copy per clause.

A fact received by `tell` gets the sender's peer cell as a source; `untell` removes it. Sources are
a rank on `d.source` from the clause, each its own peer cell (V11). Revision is by explicit `forget`
then `believe`, or `untell` then `tell`. The base is closed-world. Because beliefs are arena state
(V9), there is no history of them.

Internal events are cells on `d.pending`, handled after the handler that raised them, in raise
order. A belief event carries the fact as a term, which handlers match (§8.5). Rounds stop at
`vagent.belief_event_depth`; exceeding it is a hard failure.

### 6.2 Goals and plans

A goal is declared, not programmed:

```vagent
goal Reach(location: Point) achieve {
  succeed when believes position(location)
}
goal KeepCharged() maintain {
  holds believes battery(?level) & level > reserve_floor
}
goal Survey(target_id: Str, location: Point) perform
```

| Kind       | Succeeds when                                       | Fails when                                        | Re-adopted                                                 |
| ---------- | --------------------------------------------------- | ------------------------------------------------- | ---------------------------------------------------------- |
| `achieve`  | `succeed when` holds at a step boundary or turn end | no applicable untried plan remains                | no                                                         |
| `perform`  | a plan body completes                               | no applicable untried plan remains                | no                                                         |
| `maintain` | never ends while the agent lives                    | a restoring intention fails (reported, not fatal) | when `holds` fails at turn end and no back-off is in force |

`drop when` applies to every kind; when it holds, the intention ends as dropped and `GoalDropped` is
raised. A plan for an `achieve` or `maintain` goal whose body completes while the goal's condition
does not hold counts as a failed plan, as in Jadex and GOAL. Goal clauses are resolved in the
adopting agent's scope, so they may call that agent's capacities and read its fields and parameters.

A plan is bound to a goal by name, repeats the goal's parameter names, and has an applicability
condition:

```vagent
plan direct for Reach(location) where believes battery(?level) & level > reserve_floor {
  follow([location])
}
```

Bindings made by `where` are visible in the body. Plans for one goal may be declared in an agent, a
behavior or at top level; their order (agent, then behaviors in mount order, then imported modules
in import order) is the plan list.

### 6.3 Adoption, selection and back-off

`adopt G(args)` creates an intention, appends it to `d.intention`, marks it ready, and returns its
handle. Inside a plan, `achieve G(args)` creates a subgoal intention, links it along `+d.subgoal`,
and suspends the caller until the subgoal ends.

Plan selection takes the first plan in the plan list not in `tried` whose `where` condition
succeeds, evaluated bounded. It runs when the intention is first continued and after each plan
failure, always in a later wake turn than the failure, so a goal cannot spend one turn's fuel
cycling through its plans. If none applies, the goal fails.

At the end of every turn, each maintain goal whose `holds` fails, that has no live restorer and no
`blocked_until` in the future, gets a restorer adopted. A restorer that fails sets `blocked_until`
to now plus `vagent.maintain_backoff`; a belief event touching a predicate in `holds` clears it.
Without the back-off, a maintain goal with no applicable plan would adopt and fail a restorer every
turn.

### 6.4 Commitment

vagent v3.0 implements **single-minded commitment**. An intention keeps its goal until the goal
succeeds, its `drop when` holds, or no applicable plan remains. These conditions are checked when
the intention resumes and also at the end of every turn for every live intention, including those
waiting on a subgoal, a message, an effect or an `await`. Dropping a parent drops its subgoals and
sends `cancel` for any delegated goal. Blind and open-minded commitment are not offered (U-A5).

### 6.5 The deliberation cycle

The three selection functions are fixed in v3.0:

- **Event selection** is mailbox order (FIFO, the deque in `MeshActorRuntime::Actor`,
  `mesh_actor.cpp:350`), then host deliveries in arrival order. There is no priority mailbox (§13).
- **Option selection** is §6.3.
- **Intention selection** is ready order: an intention becomes ready when what it waits for arrives,
  and the adapter runs ready intentions oldest first, one per wake turn.

```text
message turn(m) or wake turn(w):
  match source:
    Event(e) | Performative(e) | Percept(e):
      for handler in handlers(e) by (priority desc, declaration order):   -- §5.7
        if guard(handler, e) succeeds and body(handler, e) succeeds: keep its writes
        else: undo its writes                       -- its own choice point is released
        if e was consumed: break
      wake intentions awaiting a matching event
    Ready(i) | Reply(i, value) | EffectDone(i, result) | Timer(i):
      continue(i)
    Down(child):
      raise ChildDown(child); fail intentions waiting on child
  while d.pending is not empty and rounds < belief_event_depth:
    take the first internal event; dispatch it as above; wake matching awaits
  reconsider every live intention (§6.4); check maintain goals (§6.3)

continue(i):
  if i's drop-when holds: end i as dropped
  if i's goal is achieve and succeed-when holds: end i as achieved
  if i has no plan: select one (§6.3); mark i ready; return
  run i's plan from i.step to the next suspension point or the end of the body
    suspension point: record step, frame, waiting; return
    end of body:      achieve and maintain re-test their condition; perform succeeds
    failure:          add the plan to tried; clear plan; mark i ready
  ending i: unlink it; mark its parent ready with SubgoalEnded, or stage the outcome
            performative to its delegator; raise GoalAchieved, GoalFailed or GoalDropped
```

`priority` orders handlers within one turn and nothing else. A handler cannot interrupt an
intention, because they run in different turns; it can **drop** one. A reaction therefore waits
behind every message and ready intention queued ahead of it, so worst-case reaction latency is the
queue length times the turn time. §15 prices that, and U-A11 records the adapter-side fast lane that
would shorten it.

`consume` inside a handler stops lower-priority handlers seeing the event, as Jason's one plan per
event does. Without it, all matching handlers run, as in SARL. An event no handler matches is
dropped and counted (`unhandled_events`).

### 6.6 Suspension points

| Statement                                    | Waits for                            | Resumed by                                    |
| -------------------------------------------- | ------------------------------------ | --------------------------------------------- |
| `achieve G(args)`                            | the subgoal intention to end         | `SubgoalEnded`                                |
| `achieve G(args) by a within d`              | the delegate's outcome or refusal    | `outcome`, `refuse`, or the deadline          |
| `x := ask a.q(args) within d`                | the answer                           | the reply, or the deadline                    |
| `await t within d`                           | a fact matching term `t`             | a belief event, or the deadline               |
| `await E [guard] within d`                   | an event of type `E` passing `guard` | the dispatcher, or the deadline               |
| `x := m(args)` or `m(args)`, `m` an `effect` | the host's result                    | `EffectDone`, `EffectFailed`, or the deadline |
| `yield`                                      | nothing; lets other work run         | its own ready mark                            |

A deadline is required on every wait for something outside the agent. For effects it is declared on
the capacity method (`effect def follow(route: [Point]) within 10min`). An elapsed deadline fails
the statement; `else e` supplies a value or recovery instead, with `cause` bound. A reply,
completion or event that arrives for an intention that has ended is dropped and counted
(`late_deliveries`).

The compiler accepts a suspension point only where V4 can describe the whole state: a statement of a
plan body, or a statement in the `then`, `else` or `do` body of an `if`, `while` or `for` that is
itself at such a place. The step path is the list of statement ordinals down to it. A `while`
resumes by re-testing its condition from frame variables. A `for` over a list or range keeps the
list and index in the frame and resumes at the next element. A `for believes` loop may not contain a
suspension point. A suspension point inside an `&` or `|` expression or a generator operand is a
compile-time error, because choice points do not survive a turn (V2).

### 6.7 Failure

| Outcome            | Cause                                                                            | Handled by                                                                    |
| ------------------ | -------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| expression failure | a generator ran dry                                                              | the enclosing `&`, `\|` or `if`; at statement level, the statement fails      |
| statement failure  | a failed bounded expression, `fail`, an elapsed deadline                         | fails the enclosing block, hence the handler or the plan                      |
| handler failure    | its block failed                                                                 | its writes are undone and its outbox entries removed; the event is unhandled  |
| plan failure       | the body failed, or it ended with an achieve or maintain condition unmet         | `PlanFailed` raised; another plan is selected in a later turn                 |
| goal failure       | no applicable untried plan remains                                               | `GoalFailed` raised; a parent's `achieve` fails; a delegator is told          |
| effect failure     | the host reports failure, or its deadline elapses                                | the suspended statement fails, with `cause` from the host                     |
| delivery failure   | a message is refused or never completed after `vagent.send_retries` (§7.3)       | `DeliveryFailed` raised; an `ask` or delegated `achieve` fails                |
| refusal            | the receiver's `accepts` refused a delegated goal                                | the delegating `achieve` fails at once with cause `refused`                   |
| contract violation | `requires`/`ensures` on a method (`ContractViolationKind`, `vortex_core.hpp:62`) | as plan failure, with cause `contract`; never stops the agent                 |
| hard failure       | fuel or belief-event depth exhausted in the turn                                 | the turn is undone; recovery (§4.2) fails the running plan or drops the event |
| child loss         | `ActorDown` for a monitored child                                                | `ChildDown` raised; intentions waiting on that child fail                     |

A plan's statements up to its last suspension point have committed: each suspension ended a turn,
and a turn's writes stand once it succeeds. So when a plan fails after an effect, the effect and the
arena writes made before it both stand, and the next plan starts from the world and the beliefs as
they now are. Writes made after the last suspension, in the failing turn, are undone with the failed
statement. A `PlanFailed` handler or the next plan does any compensation.

______________________________________________________________________

## 7. Communication and perception

### 7.1 Performatives

| vagent form                           | Kind      | Receiver's default handling (`std:agent`)                                                     | Reply awaited         |
| ------------------------------------- | --------- | --------------------------------------------------------------------------------------------- | --------------------- |
| `emit E{...} [in s] [for r]`          | `event`   | user handlers for `E`                                                                         | no                    |
| `tell a fact`                         | `tell`    | believe `fact` with the sender as a source; `+fact` handlers if new                           | no                    |
| `untell a fact`                       | `untell`  | remove the sender as a source of `fact`                                                       | no                    |
| `achieve G(args) by a within d`       | `achieve` | if `accepts(sender, G)` succeeds, adopt `G` with the sender as delegator; else stage `refuse` | `outcome` or `refuse` |
| (on drop of the delegating intention) | `cancel`  | drop the intention adopted for that correlation id                                            | no                    |
| `x := ask a.q(args) within d`         | `ask`     | run `answer q`, read-only; its value is the turn result                                       | the answer            |

Every `ActorSend` is answered by the runtime with an `ActorDeliverValue` carrying the receiver's
turn result (`mesh_actor.cpp:486-499`). For `ask` that result is the answer. For other kinds the
sender's adapter recognises the reply, completes it, and runs no user code; it still costs one short
message turn (§15).

`answer` declarations are checked read-only: no assignment to a field, no `believe`, `forget`,
`emit`, `tell`, `spawn`, `adopt`, `install` or effect. The answer must fit 32 KiB.

`accepts` is a capacity every agent has. Its default skill accepts delegation from the parent and
from children, and refuses everything else.

### 7.2 Scopes

`emit E{...}` goes to `outer`. `in s` names a space: `inner`, `outer`, or a space value from
`std:agent`. `for r` restricts delivery to the listed peers, which must be members of the space,
`outer` when `in` is absent. The emitter does not receive its own emission. A host receives every
emission into the spaces it hosts.

### 7.3 Delivery

A sent message is complete when its reply, a write-set delivery or an `ActorDown` for it arrives
(`mesh_actor.cpp:443-448`). Until then it blocks the sender's checkpoint (`mesh_actor.cpp:678-683`).
Several things leave a delivery incomplete for ever: a receiver that refuses with `MailboxFull`
(`mesh_actor.cpp:344-349`), an unknown or expired receiver, and an `ask` whose deadline elapsed
first. The adapter therefore drives `retry` for unacknowledged sends every
`vagent.send_retry_interval`, up to `vagent.send_retries`, then **abandons** the delivery through
§11.3 (a), raises `DeliveryFailed`, and frees its slot. `MailboxFull` is back-pressure: the sender's
retries are the policy.

There is no ordering guarantee. The runtime does not number messages, and it requires
`sequence == 0` on `ActorSend` (`actor_wire.cpp:94-96`). Each event carries its emitter's ordinal,
so a receiver can detect gaps and reorder in its own code; the adapter does not.

### 7.4 Liveness

No vagent construct waits inside a turn. Every wait on something outside the agent ends the turn and
has a deadline (§6.6), so a cycle of agents waiting on each other ends when the first deadline
elapses.

### 7.5 Perception

Percepts come from native skills. A host driver posts an event (a declared event type, such as
`Battery{level: Float}`) for its agent through the adapter, which queues it as a host delivery and
asks for a wake turn. Handlers turn percepts into beliefs:

```vagent
on Battery as b { forget battery(_); believe battery(b.level) }
```

Host deliveries are not messages. They take no mailbox slot and need no reply. They are not in the
checkpoint, so the adapter must hand them all to the agent before it checkpoints, and a crash loses
the undelivered ones; a driver re-reports its state periodically for that reason.

______________________________________________________________________

## 8. The surface language

### 8.1 Lexical structure

- Comments: `//` to end of line, and `/* ... */`, not nesting.

- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Types, events, goals, agents, behaviors, skills and
  capacities are `PascalCase`; everything else is `snake_case`. `_` alone is a binder that binds
  nothing.

- Literals: integers (`42`, `1_000`, `0x2A`), floats (`3.5`, `1e-3`), strings (`"..."` with `\n`,
  `\t`, `\"`, `\\`, `\u{...}`), `true`, `false`, `()`, durations (`250ms`, `2s`, `5min`).

- Statement termination: a newline ends a statement when the last token can end one (an identifier,
  a literal, `)`, `]`, `}`, or a keyword such as `yield`). A line that begins with a binary
  operator, `.`, `then`, `else` or `do` continues the previous statement. A line that begins with
  `(`, `[` or `{` never continues one. Newlines inside `( )`, `[ ]`, a record literal's `{ }` or a
  handler guard's `[ ]` are whitespace; inside a block nested in any of those they are significant
  again. `;` separates statements on one line.

- Keywords. Reserved everywhere:

  ```text
  achieve adopt agent answer ask await behavior believe believes by capacity consume def do drop
  effect else emit event extends fail findall for forget goal if implements import in install let
  native not on plan record return self skill spawn tell then to uninstall untell uses var where
  while within yield
  ```

  Contextual, reserved only in the position that uses them: `as`, `ensures`, `from`, `holds`,
  `inner`, `maintain`, `module`, `occurrence`, `outer`, `parent`, `perform`, `priority`, `requires`,
  `succeed`, `when`. A module path may use any word, so `import std:agent` parses.

### 8.2 Truth and failure

An expression succeeds with a value or fails (vicon §4.1).

- `a < b` produces `b` or fails; `a == b` produces `b` or fails. Comparison does not chain: write
  `a < b & b < c`. `==` compares values of one static type; `1 == 1.0` is a type error.
- `true` and `false` are values of type `Bool`. A `Bool`-typed expression is rejected as a
  condition, a guard, or an operand of `&`, `|` or `not`, with the message "compare with `== true`",
  because a `false` value is a success.
- `not e` succeeds with `()` if `e` fails and fails if `e` succeeds. It binds looser than
  comparison: `not a == b` is `not (a == b)`.
- `&` is conjunction: `e1 & e2` evaluates `e2` for each result of `e1` until `e2` succeeds. `|` is
  alternation. There is no `&&` or `||`.
- `!e` generates the elements of a list or the characters of a string. `*e` is the size of `e`.
- A statement is bounded: its first result is taken and its choice points are cut (V13). A failing
  statement fails its block. A block's value is its last statement's value, or `()`.
- `if c then e` without `else` is total: when `c` fails, the `if` succeeds with `()`. The failing
  form is `c & e`. This differs from vicon (§8.11), because in a handler or plan a failing `if`
  would undo the writes before it.
- Guards (`[...]`), `where`, `succeed when`, `drop when` and `holds` are bounded.
- `for x in e do body` evaluates `e` once, bounded, to a list or a range, and runs `body` for each
  element with no choice point between iterations. `body` is bounded each time. The loop fails if
  `body` fails. There is no `break`; an early exit is a `while`.

### 8.3 Assignment and positions

`x := e` assigns irreversibly. `x <- e` assigns reversibly: if control backtracks past it within the
statement, the old value returns. `let x := e` declares a local. `var x: T := e` declares a field.
Targets are places: a local, a field, or a field or index of one.

A position is **bounded** if it is a statement, or the `then`, `else` or `do` body of an `if`,
`while` or `for`, or the `else` of an `achieve`, `ask` or `await`, whose own position is bounded.
Every other position is **speculative**: an operand of `&`, `|`, `\`, `to`, a comparison or a call,
or anything inside one. A write or staged action in a speculative position is undone if control
backtracks over it; that is what makes `<-` and a speculative `emit` useful.

| Construct                                                                                             | Allowed in                                                                                                                    |
| ----------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `<-`, `emit`, `tell`, `untell`, `believe`, `forget`, `spawn`, `adopt`, `install`, `uninstall`, `drop` | any position in handlers, plans and methods; undone with a backtracked branch                                                 |
| `:=`, `consume`                                                                                       | bounded positions only                                                                                                        |
| effect call                                                                                           | plans: suspension positions; handlers: bounded positions, staged without waiting; methods, guards, conditions, answers: never |
| `achieve`, `ask`, `await`, `yield`                                                                    | plans, suspension positions only (§6.6)                                                                                       |

`:=` is restricted so that its name holds: in a speculative position it would be undone like `<-`.

### 8.4 Precedence

From tightest to loosest. Binary operators associate left unless marked.

| Level | Operators                              | Notes                                                          |
| ----- | -------------------------------------- | -------------------------------------------------------------- |
| 1     | call `f(x)`, index `a[i]`, field `a.b` |                                                                |
| 2     | unary `-`, `!`, `*`                    | `!xs` generates; `*xs` is the size                             |
| 3     | `\` (limitation)                       | `(1 to 10) \ 3`                                                |
| 4     | `*` `/` `%`                            |                                                                |
| 5     | `+` `-`                                |                                                                |
| 6     | `++` (concatenation)                   | strings and lists                                              |
| 7     | `<` `<=` `>` `>=` `==` `!=`            | non-associative                                                |
| 8     | `not`                                  | prefix; `not a == b` is `not (a == b)`                         |
| 9     | `\|` (alternation)                     | below comparison, as in Icon: `x == 1 \| 2` is `(x == 1) \| 2` |
| 10    | `to` … `by`                            |                                                                |
| 11    | `?` (string scanning)                  | above assignment, so `x := s ? e` is `x := (s ? e)`            |
| 12    | `:=` `<-`                              | right-associative                                              |
| 13    | `&`                                    | loosest, as in Icon                                            |

So `budget <- b - cost & budget > floor & route` is
`(budget <- (b - cost)) & (budget > floor) & route`.

### 8.5 Declarations

A fragment; the events it uses are declared elsewhere, and the literals stand for values a real
program takes from parameters or settings.

```vagent
event TargetDetected { target_id: Str, distance: Float }
event Alarm extends TargetDetected { level: Int }

capacity Navigation {
  effect def move(speed: Float, heading: Float) within 30s requires speed >= 0.0
  effect def stop() within 2s
}

native skill ThrusterNav(max_speed: Float) implements Navigation

behavior ObstacleAvoidance(threshold: Int) {
  uses Navigation
  on CollisionWarning as w [w.threat > threshold] priority 1 {
    stop()
    emit ObstacleEvaded{obstacle: w.obstacle_id}
  }
}

agent PatrolDrone(threshold: Int, max_speed: Float) {
  uses Navigation
  install ThrusterNav(max_speed)
  behavior ObstacleAvoidance(threshold)
  var targets_seen: Int := 0

  on TargetDetected [occurrence.distance < 50.0] {
    targets_seen := targets_seen + 1
    emit TargetLogged{target: occurrence.target_id}
  }
  on +at(?who, ?x, ?y) [who == self] {
    emit Moved{x: x, y: y}
  }
}
```

A handler binds the event as `occurrence`, or as the name after `as`, and reads fields by name. A
belief handler's trigger is a term: `on +at(?who, ?x, ?y)` binds the fact's arguments. A method of a
used capacity is called bare when one used capacity defines the name, and qualified
(`Navigation.stop()`) otherwise. Agent parameters are fields, set from the `Initialize` message
before `on Initialize` runs. `believe` members are asserted before `on Initialize`.

### 8.6 Actions

| Form                                                         | Value     | Notes                                                                  |
| ------------------------------------------------------------ | --------- | ---------------------------------------------------------------------- |
| `emit e [in s] [for r]`                                      | `()`      | staged (V5)                                                            |
| `tell a fact`, `untell a fact`                               | `()`      | staged                                                                 |
| `believe fact`, `forget fact`                                | `()`      | immediate in the arena; raise belief events (§6.1)                     |
| `spawn A(args) [in s]`                                       | a peer    | staged; the peer cell is filled when the adapter spawns after the turn |
| `adopt G(args)`                                              | intention | does not wait                                                          |
| `achieve G(args) [by a within d] [else e]`                   | `()`      | suspension point                                                       |
| `x := ask a.q(args) within d [else e]`                       | answer    | suspension point; also `let x := ask …`                                |
| `await t within d [else e]`, `await E [g] within d [else e]` | `()`      | suspension point                                                       |
| `drop i`                                                     | `()`      | ends an intention as dropped; sends `cancel` for delegated subgoals    |
| `install S(args)`, `uninstall S`                             | `()`      | §5.3                                                                   |
| `yield`, `consume`, `fail`, `return [e]`                     |           | as §8.3                                                                |

A peer cell returned by `spawn` may be stored and used in the same turn: messages staged to it
reference the cell, and the adapter performs the spawn first and fills the cell before it sends
them. If the spawn is refused, the adapter raises `SpawnFailed{child}` in a wake turn and drops the
messages staged to that peer.

### 8.7 Types

`Int`, `Float`, `Str`, `Bool`, `()`, `Duration`, `Time`, `Agent` (a peer cell), `Space`,
`Intention`, `Timer`, `Event` (the supertype of all events), `[T]` (a list), `Map<K, V>`, and
declared events and records. `record Name { fields }` declares a record type with no wire kind.
Lists and maps are `std:collections` structures of cells, and values are copied when stored (V11).
Types are checked statically; locals are inferred.

### 8.8 Names in scope

| Name         | Meaning                                                                       |
| ------------ | ----------------------------------------------------------------------------- |
| `self`       | this agent's peer cell                                                        |
| `parent`     | the peer cell of the agent that spawned this one; fails for a top-level agent |
| `inner`      | the space this agent hosts                                                    |
| `outer`      | the space this agent was spawned into; for a top-level agent, the launcher's  |
| `occurrence` | the event being handled                                                       |
| `cause`      | in an `else` after `achieve`, `ask`, `await` or an effect, the failure cause  |

A program starts with `vagent run module:Agent(args)`, which starts the launcher and then the named
agent in the launcher's space.

### 8.9 Grammar

```ebnf
Unit          ::= [ ModuleDecl ] { ImportDecl } { TopDecl } ;
ModuleDecl    ::= 'module' ModulePath ;
ImportDecl    ::= 'import' ModulePath [ 'as' Ident ] [ '(' IdentList ')' ] ;
ModulePath    ::= Word { ':' Word } ;          (* Word: an identifier or any keyword *)

TopDecl       ::= EventDecl | RecordDecl | CapacityDecl | SkillDecl | BehaviorDecl
                | AgentDecl | GoalDecl | PlanDecl ;

EventDecl     ::= 'event' Ident [ 'extends' Ident ] [ '{' [ FieldList ] '}' ] ;
RecordDecl    ::= 'record' Ident '{' [ FieldList ] '}' ;
FieldList     ::= Field { ',' Field } [ ',' ] ;
Field         ::= Ident ':' Type ;

CapacityDecl  ::= 'capacity' Ident [ 'extends' IdentList ] '{' { MethodSig } '}' ;
MethodSig     ::= [ 'effect' ] 'def' Ident '(' [ ParamList ] ')' [ ':' Type ]
                  [ 'within' Duration ] { Contract } ;
Contract      ::= ( 'requires' | 'ensures' ) Expr ;
MethodDef     ::= MethodSig Block ;
AnswerDef     ::= 'answer' Ident '(' [ ParamList ] ')' ':' Type Block ;

SkillDecl     ::= 'native' 'skill' Ident [ '(' ParamList ')' ] 'implements' IdentList
                | 'skill' Ident [ '(' ParamList ')' ] 'implements' IdentList
                  '{' { VarDecl | MethodDef | LifecycleDef } '}' ;
LifecycleDef  ::= 'on' ( 'install' | 'uninstall' ) Block ;

BehaviorDecl  ::= 'behavior' Ident [ '(' ParamList ')' ] '{' { Member } '}' ;
AgentDecl     ::= 'agent' Ident [ '(' ParamList ')' ] '{' { Member | AgentOnly } '}' ;
Member        ::= UsesDecl | VarDecl | HandlerDef | MethodDef | AnswerDef | GoalDecl | PlanDecl ;
AgentOnly     ::= InstallDecl | BehaviorUse | BeliefDecl ;
UsesDecl      ::= 'uses' IdentList ;
InstallDecl   ::= 'install' Ident [ '(' [ ArgList ] ')' ] ;
BehaviorUse   ::= 'behavior' Ident [ '(' [ ArgList ] ')' ] ;
BeliefDecl    ::= 'believe' Term ;
VarDecl       ::= 'var' Ident ':' Type [ ':=' Expr ] ;

HandlerDef    ::= 'on' Trigger [ 'as' Ident ] [ '[' Expr ']' ] [ 'priority' IntLit ] Block ;
Trigger       ::= Ident | ( '+' | '-' ) Term ;

GoalDecl      ::= 'goal' Ident '(' [ ParamList ] ')' GoalKind [ '{' { GoalClause } '}' ] ;
GoalKind      ::= 'achieve' | 'perform' | 'maintain' ;
GoalClause    ::= 'succeed' 'when' Expr | 'drop' 'when' Expr | 'holds' Expr ;
PlanDecl      ::= 'plan' Ident 'for' Ident '(' [ IdentList ] ')' [ 'where' Expr ] Block ;

Block         ::= '{' [ Stmt { Sep Stmt } ] [ Sep ] '}' ;
Sep           ::= ';' | NEWLINE ;
Stmt          ::= 'let' Ident [ ':' Type ] ':=' Expr | 'return' [ Expr ] | Expr ;

Expr          ::= Assign { '&' Assign } ;
Assign        ::= Scan [ ( ':=' | '<-' ) Assign ] ;
Scan          ::= ToExpr [ '?' ToExpr ] ;
ToExpr        ::= Alt [ 'to' Alt [ 'by' Alt ] ] ;
Alt           ::= NotExpr { '|' NotExpr } ;
NotExpr       ::= 'not' NotExpr | Cmp ;
Cmp           ::= Concat [ CmpOp Concat ] ;
CmpOp         ::= '<' | '<=' | '>' | '>=' | '==' | '!=' ;
Concat        ::= Add { '++' Add } ;
Add           ::= Mul { ( '+' | '-' ) Mul } ;
Mul           ::= Limit { ( '*' | '/' | '%' ) Limit } ;
Limit         ::= Unary { '\' Unary } ;
Unary         ::= ( '-' | '!' | '*' ) Unary | Postfix ;
Postfix       ::= Primary { '(' [ ArgList ] ')' | '[' Expr ']' | '.' Ident } ;
Primary       ::= Ident | Literal | 'self' | 'parent' | 'inner' | 'outer' | 'occurrence'
                | '(' Expr ')' | Block | ListLit | RecordLit
                | 'if' Expr 'then' Expr [ 'else' Expr ]
                | 'while' Expr 'do' Expr
                | 'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr
                | 'believes' Term [ 'from' Postfix ]
                | 'findall' '(' TermArg ',' Term ')'
                | Action ;
Action        ::= 'emit' Postfix [ 'in' Postfix ] [ 'for' Postfix ]
                | ( 'tell' | 'untell' ) Postfix Term
                | ( 'believe' | 'forget' ) Term
                | 'spawn' Ident '(' [ ArgList ] ')' [ 'in' Postfix ]
                | 'adopt' GoalCall
                | 'achieve' GoalCall [ 'by' Postfix 'within' Postfix ] [ 'else' Expr ]
                | 'ask' Postfix 'within' Postfix [ 'else' Expr ]
                | 'await' ( Term | Ident [ '[' Expr ']' ] ) 'within' Postfix [ 'else' Expr ]
                | 'drop' Postfix
                | 'install' Ident [ '(' [ ArgList ] ')' ] | 'uninstall' Ident
                | 'yield' | 'consume' | 'fail' ;
GoalCall      ::= Ident '(' [ ArgList ] ')' ;
RecordLit     ::= Ident '{' [ Ident ':' Expr { ',' Ident ':' Expr } [ ',' ] ] '}' ;
ListLit       ::= '[' [ ArgList ] ']' ;
Term          ::= Ident '(' [ TermArg { ',' TermArg } ] ')' ;
TermArg       ::= Expr | '?' Ident | '_' ;

ParamList     ::= Ident ':' Type { ',' Ident ':' Type } [ ',' ] ;
ArgList       ::= Expr { ',' Expr } [ ',' ] ;
IdentList     ::= Ident { ',' Ident } ;
Type          ::= Ident [ '<' Type { ',' Type } '>' ] | '[' Type ']' | '(' ')' ;
```

Notes the grammar cannot carry:

- **Record literals and blocks.** `Ident '{'` in an expression is a record literal, except in the
  expression of a `where`, a `Contract` or a goal clause, where a record literal must be
  parenthesised, as Rust does in `if` headers. Elsewhere no expression is directly followed by a
  block: bodies follow `then`, `do`, `else`, `]`, `)` or a declaration header.
- **Action operands are postfix expressions**, so `emit E{...} & next()` is
  `(emit E{...}) & next()`, and an `in`, `for`, `by`, `within`, `from` or `else` after an action
  belongs to it.
- **`else` binds to the nearest `if`, `achieve`, `ask` or `await`.**
- **`?` inside a term.** `?` immediately followed by an identifier inside a `Term` is a logic
  variable; elsewhere `?` is the scan operator. Variables bound in an `if` condition, a `where`, a
  guard or a `for believes` are visible in the corresponding body.
- **Terms versus calls.** After `believe`, `forget`, `believes`, `findall`, `tell`, `untell` and
  `await`, and in a `Trigger`, `name(args)` is a term.
- **Static restrictions**: positions (§8.3), suspension points (§6.6), read-only answers (§7.1),
  `Bool` conditions (§8.2), places as assignment targets, reserved implicit event fields (§5.5),
  plan parameters named as the goal's.

### 8.10 Differences from SARL

| SARL                                         | vagent                                                      | Why                                                                                                                               |
| -------------------------------------------- | ----------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `on E [guard] { }`, `occurrence`             | same, plus `as name`                                        | kept; `as` names the event when handlers nest                                                                                     |
| `uses Cap` imports capacity functions        | same                                                        | kept                                                                                                                              |
| `setSkill(new Impl)`                         | `install Impl(args)`                                        | a keyword form like every other agent action                                                                                      |
| `requires Cap` in a skill                    | not offered; `requires` is a method precondition            | a skill's needed capacities are found by `uses` in its methods                                                                    |
| `emit(e)` with a `[scope]` lambda            | `emit e in s for r`                                         | a list of peers is checkable; a scope lambda would run once per member in the emitter's turn                                      |
| `in(delay) [ ... ]`, `every(period) [ ... ]` | `Schedules.after`, `Schedules.periodic`, `Schedules.cancel` | vagent has no closures; a timer delivers an event instead                                                                         |
| `wake(e)`                                    | not offered                                                 | SARL's `wake` posts to the agent itself; vagent's internal events are belief, goal and plan events, and a timer or `yield` defers |
| event fields `var x : T`                     | `event E { x: T }`                                          | one field syntax for events and records                                                                                           |
| `Initialize.parameters`                      | typed agent parameters                                      | checkable at the spawn site                                                                                                       |
| agent `extends`                              | not offered; behaviors compose                              | inheritance of handlers and plan order would interact with plan lists                                                             |
| no priority                                  | `priority n`, within a turn only                            | deterministic order of matching handlers                                                                                          |
| no BDI                                       | beliefs, goals, plans, intentions                           | the point of the language                                                                                                         |
| Janus thread pool, shared JVM heap           | one actor and one arena per agent                           | V1                                                                                                                                |

### 8.11 Relationship to the siblings

| Concern              | vicon                                    | vagent                          | Divergence and reason                                                                                 |
| -------------------- | ---------------------------------------- | ------------------------------- | ----------------------------------------------------------------------------------------------------- |
| assignment           | `:=`, `<-`                               | `:=`, `<-`                      | `:=` restricted to bounded positions (§8.3)                                                           |
| comparison           | `=`, `~=`, numeric; `==`, `~==`, lexical | `==`, `!=`, typed               | vagent is statically typed, so one `==` replaces the numeric and lexical pairs (V14)                  |
| concatenation        | `\|\|`                                   | `++`                            | `\|\|` would read as boolean or to programmers from typed languages                                   |
| conditional          | `if then else`; fails without `else`     | total without `else`            | a failing `if` would undo the writes before it in a handler (§8.2)                                    |
| loops                | `while do`, `every do`                   | `while do`, `for in do`         | `every` drives a generator, and the trail undoes its body's writes (V7)                               |
| generator procedures | `suspend` in procedures                  | none; `def` returns once        | methods are called across skills and from handlers, where a live generator could not survive the turn |
| string scanning      | `s ? e`, `tab`, `move`                   | same, from `std:goal`           | `?` binds tighter than assignment                                                                     |
| module paths         | `std:goal`                               | `std:agent`, `mars:exploration` | none; vlace's `"std:xanadu"` and VQL's `std::` should converge (U-A6)                                 |
| locals               | assignment to a fresh name               | `let x := e`                    | plan frames must be known statically (§5.6)                                                           |
| logic variables      | Vlog terms                               | `?x` inside terms only          | only belief terms need them                                                                           |

______________________________________________________________________

## 9. Lowering

### 9.1 Cross-walk

The last column names what each construct needs beyond `link`, `value` and the existing opcodes.

| Construct                              | Lowering                                                                                                                                              | Beyond link/value                   |
| -------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| a statement                            | `d := #ChoiceDepth` at entry; the expression; `#Cut d` on its first result                                                                            | `#ChoiceDepth` (owed)               |
| `e1 & e2`, `e1 \| e2`, `\`, `to`, `<-` | vicon's lowering (vicon §4–§5)                                                                                                                        | none                                |
| `if c then a [else b]`                 | `#Choice alt=ELSE`; `c`; `#Cut d`; `a`; ELSE: `b`, or `()` when absent                                                                                | `#ChoiceDepth`                      |
| `for x in e do b`                      | bounded `e`; a counter or a rank walk; `b` as a statement per element                                                                                 | none                                |
| `for believes t do b`, `findall`       | copy the clause list; per clause: copy `t`, `#Choice`, `#Unify`, `#Cut d` or `#Fail`                                                                  | none                                |
| agent creation                         | `makeCell(type name)`; `setStateRoot`; ranks empty; fields from `Initialize`                                                                          | `setStateRoot` (built)              |
| `var` field                            | name cell on `d.vars` off the owner, value on `d.values`                                                                                              | none                                |
| `install S(args)`                      | skill frame; one instance per capacity on `d.skill` and on the frame's `d.clone` rank; capacity-name cell on `+d.capacity`                            | none                                |
| capacity call                          | dispatch walk (§5.3), then vicon's call convention: `Bind` per argument, `Bind` of the return continuation, `Jump` (`vicon/compiler.cpp`, `emitCall`) | none                                |
| effect call                            | outbox entry (capacity, method, copied arguments, deadline); intention `waiting` set; the step returns                                                | adapter: host binding               |
| `on E [g] priority p { b }`            | handler cell on the event type's `d.clause` rank (§5.7); dispatcher routine in `std:agent`                                                            | none                                |
| `believe t`, `forget t`                | clause add or source add; source removal and retraction with rank repair; belief event cell on `d.pending`                                            | none                                |
| `believes t`                           | clause walk with `#Unify`, one `#Choice` per clause                                                                                                   | none                                |
| `emit`, `tell`, `untell`               | outbox entry with copied fields                                                                                                                       | adapter: `send`                     |
| `spawn A(args)`                        | outbox entry; an unfilled peer cell returned                                                                                                          | adapter: issuer, `spawn`, `monitor` |
| `adopt G(args)`                        | intention on `d.intention`, marked ready                                                                                                              | adapter: wake                       |
| `achieve G(args)` in a plan            | subgoal intention on `+d.subgoal`; `waiting` set; the step returns                                                                                    | none                                |
| `achieve … by a within d`, `ask`       | outbox entry; `waiting` set with deadline; the step returns                                                                                           | adapter: `send`, timers             |
| `await`                                | `waiting` set with the term or event pattern and deadline; the step returns                                                                           | adapter: timers                     |
| `drop i`                               | unlink `i`; drop subgoals; outbox `cancel` entries                                                                                                    | none                                |
| `yield`                                | intention marked ready; the step returns                                                                                                              | adapter: wake                       |
| `requires`, `ensures`                  | the call site's `-d.contract`/`+d.contract` chains (Vortex §3)                                                                                        | none                                |
| turn end                               | `clearChoicePoints()`; drain; rebuild ready list and timers                                                                                           | adapter                             |

### 9.2 Calls

vagent uses vicon's procedure-call lowering, not `#Call`. `#Call` pushes a return frame holding one
opcode reference and jumps (`vortex_vm.cpp:665-687`); nothing binds arguments for the callee. vicon
binds each argument with `Bind` on the cursor's scope, binds the return continuation, and jumps
(`vicon/compiler.cpp`, `emitCall`). Capacity dispatch chooses the jump target at run time by binding
the chosen method's entry and jumping through it, so no program cell is rewritten (V12). Contracts
are evaluated as Vortex §3 describes; a violation returns `ExecutionResult::contractViolation`
(`vortex_core.hpp:62-70`).

### 9.3 A handler, lowered

`on TargetDetected [occurrence.distance < 50.0] { targets_seen := targets_seen + 1 }`:

```text
program: name cell "TargetDetected" ─(+d.clause)─► handler cell (priority 0, ordinal 3)
  guard stream (+d.spin): Resolve occurrence.distance → Lt 50.0 → produce or Fail
  body stream (+d.spin):  ChoiceDepth → d0; Resolve targets_seen → Add 1 → Bind; Cut d0

dispatcher (std:agent, Vortex), per handler on the rank:
  ChoiceDepth → h
  Choice alt=NEXT                -- the handler's own choice point and mark
  run guard; run body
  Cut h                          -- success: drop the choice point, keep the writes
  Jump NEXT_AFTER
NEXT: (reached by Fail)          -- failure: the mark was released, writes undone
```

______________________________________________________________________

## 10. The `std:agent` package

`std:agent` is written in Vortex, anchored on `d.stdlib` like every package
([vortex-hyperstructural-runtime.md](vortex-hyperstructural-runtime.md) §7.1). It reuses `std:logic`
(unification, and the clause database once Vlog §6 is built), `std:goal` (generators, scanning),
`std:collections` and `std:string`.

| Part         | Contents                                                                                                                                                                                                                                                                                                |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| genesis      | mints the eleven dimensions of §5.1                                                                                                                                                                                                                                                                     |
| dispatcher   | handler order (§5.7), `d.pending` rounds, `consume`, await matching                                                                                                                                                                                                                                     |
| deliberation | `adopt`, selection, `continue`, reconsideration, maintain back-off, `fail_step`                                                                                                                                                                                                                         |
| beliefs      | set-with-sources assert and retract, clause walk, `findall`                                                                                                                                                                                                                                             |
| relay        | the host's fan-out into hosted spaces                                                                                                                                                                                                                                                                   |
| capacities   | `Behaviors` (`mount(b: Str, args: [Event])`, `unmount(b: Str)`); `Schedules` (`after(d: Duration, e: Event): Timer`, `periodic(d: Duration, e: Event): Timer`, `cancel(t: Timer)`); `Clock` (`now(): Time`); `Accepts` (`accepts(from: Agent, goal: Str)`); `Logging` (`info(m: Str)`, `debug(m: Str)`) |
| functions    | `intentions(): [Intention]`, `members(s: Space): [Agent]`, `children(): [Agent]`, `text(x): Str`                                                                                                                                                                                                        |
| events       | `Initialize`, `Destroy`, `GoalAchieved{intention, goal}`, `GoalFailed{intention, goal, cause}`, `GoalDropped{intention, goal}`, `PlanFailed{intention, plan, cause}`, `ChildDown{child}`, `ChildStopping{child}`, `SpawnFailed{child}`, `DeliveryFailed{to, kind, cause}`                               |

A timer is an entry in the `timers` field: id, due time, period, and a copied event. When it falls
due, the adapter asks for a wake turn and the event is dispatched to the agent itself. `Clock.now()`
returns the turn time written in step 2 of §4.2, so a turn is a function of its input and its state.
`Logging` writes through the `vortex.agent` category, which the logging migration
(`design/logging-migration.md`) must register; it never logs event payloads.

Settings, read from `system://settings` by the launcher and the adapter (the runtime's own options
are constructor arguments, so the launcher is what reads them):

| Setting                      | Default                | Bounds what                              |
| ---------------------------- | ---------------------- | ---------------------------------------- |
| `vagent.max_actors`          | 128 (`ActorOptions`)   | agents per runtime                       |
| `vagent.max_mailbox`         | 128 (`ActorOptions`)   | queued messages per agent                |
| `vagent.max_deliveries`      | 4096 (`ActorOptions`)  | outstanding sends per runtime            |
| `vagent.value_operations`    | 65,536 (`ValueLimits`) | checkpoint size, once §11.3 (e) lands    |
| `vagent.belief_event_depth`  | 16                     | internal event rounds per turn           |
| `vagent.maintain_backoff`    | 30s                    | re-adoption after a failed restorer      |
| `vagent.send_retries`        | 5                      | retries before a delivery is abandoned   |
| `vagent.send_retry_interval` | 1s                     | spacing of retries                       |
| `vagent.renew_fraction`      | 0.8                    | when lease renewal starts                |
| `vagent.ready_per_message`   | 4                      | wake turns run between two message turns |

______________________________________________________________________

## 11. What the C++ core has to gain

### 11.1 One opcode

`#ChoiceDepth` writes the VM's current `choiceStack_.size()` into its output cell. With it, a
bounded statement records the depth at its start and cuts to it with the existing `#Cut`, which
already takes a barrier from its input (`vortex_vm.cpp:974-981`). Without it, depth is only known
statically, and a method called from inside a speculative expression starts at a depth the compiler
cannot know. The opcode is about ten lines in `vortex_vm.{hpp,cpp}` and a test. It also removes
vicon's recorded deviation, which names "a new opcode or a new `ChoicePoint` field" as the fix for
bounded evaluation and `\` (`vicon/compiler.cpp:12-22`).

No other VM change, no `ChoicePoint` field, no `ArenaManifold` method, no `CompactOpNode` change, no
`SystemDimensions` field.

### 11.2 A front end and a mesh executor adapter

```text
apps/common/xanadu/vagent/
  token.hpp, lexer.{hpp,cpp}      // §8.1, including the newline rule and contextual keywords
  ast.hpp, parser.{hpp,cpp}       // §8.9; precedence climbing over §8.4
  checker.{hpp,cpp}               // types and the static restrictions of §8.9
  compiler.{hpp,cpp}              // §9, to Vortex opcode streams
  agent_executor.{hpp,cpp}        // the MeshActorRuntime::Executor
  host_bindings.{hpp,cpp}         // native skills and percept drivers registered by name
apps/vagent/main.cpp              // `vagent run`, `vagent check`, a REPL driving one agent
```

The compiler returns `std::expected` with a named error, as `vprolog::Compiler` and vicon §8.2 do.
For scale: `apps/common/xanadu/vprolog/` is 1,632 lines and `apps/common/xanadu/vicon/` is 3,176;
the front end is expected to be nearer vicon's, and the adapter is new territory with no measured
comparison.

The adapter is C++ because it holds a `MeshActorRuntime&` and host bindings, which Vortex code
cannot reach (§1). Its duties, all per turn unless stated:

1. the steps of §4.2, including fuel re-arm, turn time, base choice point, `clearChoicePoints`, and
   recovery after a hard failure;
1. draining `d.outbox`: spawn under an issued capability then fill the peer cell; `send`; start
   effects through host bindings; update timers;
1. delivery upkeep: `retry` and abandonment (§7.3);
1. host deliveries: effect results and percepts, queued and fed to wake turns;
1. a ready list and a timer heap derived from cells after each turn and after `resume`;
1. lease renewal (§4.5);
1. scheduling: at most `vagent.ready_per_message` wake turns between two message turns.

### 11.3 Changes in `mesh_actor.cpp`

| Change | What                                                | Why                                                                                                                                                                                                                                                               | Price                                                                                                          |
| ------ | --------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| (a)    | Reap completed deliveries; add `abandon(requestId)` | `deliveries_` is never erased (the file has no `erase`) and `send` refuses at `maxOutstandingDeliveries` (`mesh_actor.cpp:521`), so a runtime can send 4096 messages in its lifetime; an abandoned delivery blocks its sender's checkpoint for ever               | erase on completion or abandonment; de-duplication of resends keys on a window of recent ids                   |
| (b)    | Window remembered request ids                       | `remembered` is never erased and `enqueue` refuses at `maxRememberedMessages` (`mesh_actor.cpp:344`), so an actor can receive 4096 messages in its lifetime                                                                                                       | keep ids above a per-sender watermark; checkpoints carry the watermark                                         |
| (c)    | Capability-bearing senders                          | `receive` admits `ActorSend` only from the owner (`mesh_actor.cpp:402-405`)                                                                                                                                                                                       | not needed for single-owner societies. Deferred (U-A2)                                                         |
| (d)    | Local wake                                          | the executor runs only for a mailbox message; an agent's own work (ready intentions, timers, host deliveries) is not a message, and a self-send collides with the actor's own `remembered` entry and is refused as `DuplicateMismatch` (`mesh_actor.cpp:333-337`) | `wake(pid)` runs the executor with no message: no `remembered` entry, no reply, no delivery. About forty lines |
| (e)    | Value limits as options                             | `resume` builds `ValueLimits` locally (`mesh_actor.cpp:258-259`), so the 65,536-operation checkpoint bound cannot be raised                                                                                                                                       | carry `ValueLimits` in `ActorOptions`                                                                          |

(a), (b) and (e) remove ceilings that would stop any long-lived actor, not only vagent's. The mesh
was built for bounded jobs, for which these limits suit. vagent depends on all of (a), (b), (d) and
(e).

______________________________________________________________________

## 12. Worked examples

### 12.1 A holon that fans out a search

```vagent
module demo:search
import std:agent

event SearchQuery { query_id: Str, pattern: Str }
event SubTask { query_id: Str, partition: Int, pattern: Str }
event PartitionResult { query_id: Str, partition: Int, hits: [Str] }
event SearchCompleted { query_id: Str, hits: [Str] }

agent Worker {
  on SubTask as t {
    emit PartitionResult{query_id: t.query_id, partition: t.partition,
                         hits: scan_partition(t.partition, t.pattern)}
  }
  def scan_partition(index: Int, pattern: Str): [Str] {
    ["alpha-" ++ text(index), "beta-" ++ text(index)]
  }
}

agent SearchCoordinator(partitions: Int) {
  on Initialize {
    for _ in 1 to partitions do spawn Worker() in inner
  }

  on SearchQuery as q [q.space == outer] {
    believe pending(q.query_id)
    for i in 0 to partitions - 1 do
      emit SubTask{query_id: q.query_id, partition: i, pattern: q.pattern} in inner
  }

  // A duplicate delivery of one partition fails the guard and changes nothing.
  on PartitionResult as r [r.space == inner & not believes reported(r.query_id, r.partition)] {
    believe reported(r.query_id, r.partition)
    for h in r.hits do believe hit(r.query_id, h)
    if *findall(?p, reported(r.query_id, ?p)) == partitions then {
      emit SearchCompleted{query_id: r.query_id, hits: findall(?h, hit(r.query_id, ?h))} in outer
      forget pending(r.query_id)
    }
  }
}
```

The worker's default scope is `outer`, which for a child is its parent's `inner`, so results reach
only the coordinator and the other workers. The `if` has no `else`, so it succeeds for every
partition but the last, and the beliefs written before it stand (§8.2). Each `SubTask` send brings
the coordinator a reply turn, so `partitions` must stay below half of `vagent.max_mailbox`, or the
fan-out must be windowed.

### 12.2 Swapping a skill at run time

```vagent
event Payload { data: Str }
event SealedPayload { key_id: Str, data: Str }
event SecurityAlert { level: Int }
event OutgoingReport { recipient: Agent, content: Str }

capacity Transport {
  def send_payload(dest: Agent, data: Str)
}
skill Loopback implements Transport {
  def send_payload(dest: Agent, data: Str) { emit Payload{data: data} for [dest] }
}
skill Sealed(key_id: Str) implements Transport {
  def send_payload(dest: Agent, data: Str) {
    emit SealedPayload{key_id: key_id, data: data} for [dest]
  }
}

behavior Reporter {
  uses Transport
  on OutgoingReport as o { send_payload(o.recipient, o.content) }
}

agent Adaptive(alert_level: Int, key_id: Str) {
  install Loopback()
  behavior Reporter
  var sealed: Bool := false

  on SecurityAlert as a [a.level > alert_level & sealed == false] {
    install Sealed(key_id)
    sealed := true
  }
}
```

An `OutgoingReport` before the alert dispatches `send_payload` to the `Loopback` instance. The
alert's turn uninstalls `Loopback` and installs `Sealed`, whose instance carries the capacity name
`Transport`. The next `OutgoingReport` reaches `Sealed`, and `Reporter` is unchanged. Both skills
emit into `outer`, of which `dest` must be a member. `send_payload` is not an `effect`: emitting is
staged messaging, not a device action.

### 12.3 A rover with goals, plans, effects and a reversible choice

```vagent
module mars:exploration
import std:agent
import std:goal (tab, match, upto)
import std:math (Point)
import mars:hardware (Chassis, Spectrometer, TerrainPlanner)

event Position { at: Point }
event Battery { level: Float }
event HazardDetected { kind: Str, severity: Int }
event MissionCommand { target_id: Str, location: Point }
event EmergencyHalted { kind: Str }
event ScienceLog { target_id: Str, mineral: Str }
event SampleSecured { target_id: Str, total: Int }
event SurveyFailed { target_id: Str, cause: Str }

capacity Mobility {
  effect def follow(route: [Point]) within 10min   // fails if a leg loses traction
  effect def halt() within 5s
}
capacity Science {
  effect def spectra(target_id: Str): Str within 1min
  effect def drill_core(target_id: Str) within 15min
}
capacity Routes {
  def direct(from_point: Point, to_point: Point): [Point]
  def ridge(from_point: Point, to_point: Point): [Point]
  def cost(route: [Point]): Float
}

goal Survey(target_id: Str, location: Point) perform
goal Reach(location: Point) achieve {
  succeed when believes position(location)
}

agent MarsSurveyor(hazard_limit: Int, reserve_floor: Float) {
  uses Mobility, Science, Routes
  install Chassis()
  install Spectrometer()
  install TerrainPlanner()
  var budget: Float := 0.0
  var samples: Int := 0

  on Position as p { forget position(_); believe position(p.at) }
  on Battery as b { forget battery(_); believe battery(b.level) }

  on HazardDetected as h [h.severity > hazard_limit] {
    halt()
    for i in intentions() do drop i
    emit EmergencyHalted{kind: h.kind}
  }

  on MissionCommand as m { adopt Survey(m.target_id, m.location) }

  plan survey for Survey(target_id, location) {
    achieve Reach(location) else {
      emit SurveyFailed{target_id: target_id, cause: cause}
      fail
    }
    let reading := spectra(target_id)
    let mineral := reading ? (tab(match("SPEC:")) & tab(upto(";")))
    emit ScienceLog{target_id: target_id, mineral: mineral}
    drill_core(target_id)
    samples := samples + 1
    emit SampleSecured{target_id: target_id, total: samples}
  }

  // Choose a route inside one statement: a failed arm restores budget.
  plan travel for Reach(location) where believes position(?here) & believes battery(?level) {
    let route := (budget <- level - cost(direct(here, location))
                  & budget > reserve_floor & direct(here, location))
               | (budget <- level - cost(ridge(here, location))
                  & budget > reserve_floor & ridge(here, location))
    follow(route)
  }
}
```

`mars:hardware` provides `Chassis` and `Spectrometer` as native skills, whose drivers also post
`Position` and `Battery` percepts, and `TerrainPlanner` as a vagent skill implementing `Routes`.

What happens:

1. `MissionCommand` adopts `Survey`, which becomes ready; its first wake turn selects `survey`.
1. `achieve Reach(location)` creates a subgoal and suspends `Survey`.
1. `Reach`'s wake turn selects `travel`, whose `where` binds `here` and `level` from beliefs. The
   route statement tries the direct arm; if `budget > reserve_floor` fails, the trail restores
   `budget` and the ridge arm runs. The statement's cut keeps the chosen arm's `budget`.
1. `follow(route)` stages the effect and suspends `Reach`. The turn ends; the chassis drives. While
   it drives, the agent handles percepts and other messages in their own turns.
1. When the drive ends, `EffectDone` resumes `Reach`. Its plan body is complete, so the goal's
   condition is tested from the latest `position` belief. If the rover is there, `Reach` succeeds
   and `Survey` resumes.
1. If a leg loses traction, `EffectFailed` fails `follow(route)`, hence `travel`. `Reach` has no
   other plan, so it fails, and `Survey`'s `else` emits `SurveyFailed` and fails `survey`. The rover
   is wherever it stopped, the `budget` written in the earlier turn stands, and nothing pretends
   otherwise.
1. A `HazardDetected` is handled in its own turn, between any two others, including during a drive:
   `halt()` is staged without waiting, every intention is dropped, and the drive's later
   `EffectFailed` or `EffectDone` finds no intention and is counted as late.

______________________________________________________________________

## 13. Refused alternatives

| Alternative                                                   | Refused because                                                                                                                                               |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Agents as cells in one shared arena                           | breaks share-nothing and the per-actor grant; one agent's fuel exhaustion would undo another's turn                                                           |
| Holon children inside the parent's arena                      | children would share the parent's failure domain and fuel, and a holon could not be distributed                                                               |
| Speculation across turns (shadow cells, per-intention arenas) | needs a mark live across turns, which `checkpointView` refuses; marks are LIFO truncation                                                                     |
| Persisting a generator's choice point at a suspension point   | a choice point is a C++ frame and an arena mark; neither is portable (`mesh_actor.hpp:92-93`)                                                                 |
| Continuation-passing so suspension can sit anywhere           | the compiler would reify generator state as cells; possible later (U-A7), not needed by the examples                                                          |
| Synchronous effects inside a turn                             | a long device action would block every other message, and a turn that failed after it would roll back the arena while the world kept the change               |
| Free-standing `commit` (Prolog cut over side effects)         | it protects nothing when written before the fallible call, which is the order programmers write; staged effects and bounded statements leave only safe orders |
| Self-sent messages for wake-ups                               | a self-send's reply collides with the actor's own `remembered` entry (`mesh_actor.cpp:333-337`) and never completes, blocking checkpoints for ever            |
| A priority mailbox                                            | needs a change to `enqueue` for an ordering handlers already have within a turn; reaction latency is addressed by U-A11 instead                               |
| Capacity identity by `CellRef` into the program               | every checkpoint would promote the program and `home` (V12), and refs change on `resume`                                                                      |
| Sharing values by reference                                   | a second binding of a cell evicts the first; copying is the default and the skill frame is the one shared structure (V11)                                     |
| Per-event `promote` to a user's store                         | R8: agent working state is not hypertime                                                                                                                      |
| vagent as a VQL extension (`vql-agent-experiment.md`)         | VQL's deferred evaluation has no intention that outlives a query; agent turns would need a second runtime under the query engine                              |
| `every e1 do e2` for loops that act                           | the arena trail undoes the body's writes on every resumption (V7)                                                                                             |
| A failing `if` without `else`                                 | in a handler it undoes every write before it, silently                                                                                                        |
| Icon's `=`/`==` and `\|\|` spellings                          | vagent is statically typed, so one typed `==` serves; `\|\|` reads as boolean or                                                                              |
| `&&` and `\|\|` beside `&` and `\|`                           | two spellings of conjunction that differ in backtracking would be a trap                                                                                      |
| Positional event patterns                                     | they silently rebind when an event's fields are reordered                                                                                                     |
| A dimension per entity kind                                   | fields, frames and payloads fit `d.vars`/`d.values`; plans and handlers fit `d.clause`; choice and trail are the VM's and the arena's                         |

______________________________________________________________________

## 14. Properties, with their conditions

- **No data race between agents.** Each agent's arena is touched only by its own turns on the
  runtime's owner thread (`mesh_actor.hpp:107-109`). Within an agent, turns are sequential.
- **An agent never blocks.** No construct waits inside a turn; effects run outside turns (V6).
- **Every wait on something outside the agent is bounded** by a deadline (§6.6), given the timers of
  §11.2 and their reconstruction from cells after `resume`.
- **A turn is atomic.** Its arena writes stand together or are undone together, and its messages,
  spawns and effects happen only if it succeeds (V5).
- **A turn is a function of its input and the agent's state.** Its input is the message or host
  delivery and the turn time, all written into the arena; nothing else reads a clock or randomness.
- **Reaction latency is not bounded by the language.** A handler runs when its message reaches the
  head of the mailbox, behind queued messages and up to `vagent.ready_per_message` wake turns
  between each.
- **Not claimed: lifetime replay.** The mesh does not log inbound messages or host deliveries, and
  agent state is not in a user's spool (V9). Recording inbound streams as a rank would allow it
  (U-A8).

______________________________________________________________________

## 15. Breakages and costs

- **Two turns per message sent.** Every `ActorSend` produces a reply turn for the sender.
- **Mailbox bound.** 128 messages per agent by default; fan-out beyond half of it overflows on the
  replies alone (§12.1). `MailboxFull` is handled by retries (§7.3).
- **Relay doubles hops,** and a space's host is its bottleneck and single point of failure.
- **Per-turn deliberation.** Every turn re-tests every live intention's `drop when` and
  `succeed when`, every maintain goal's `holds`, and, per event, every waiting `await`. The cost is
  linear in live intentions and their conditions.
- **A turn per effect and per suspension.** A plan of n effects takes at least n + 1 turns.
- **Checkpoint costs** (§4.4): an operation per live cell and link, the 65,536-operation bound until
  §11.3 (e), and permascroll growth by the live strings at every checkpoint.
- **Reclamation needs quiescence** (U-A1).
- **Copy on store** (V11): O(size) per stored list or map.
- **Belief queries are linear** until convergence U1. vagent is a fourth consumer of U1, after VPL,
  Vlog and vicon.
- **Restricted suspension** (§6.6): a plan that must wait inside a generator loop splits the work
  into a goal per element.
- **Child loss is slow** (§5.4): minutes, not milliseconds, unless the child announces its stop.
- **Single-owner societies** until §11.3 (c).
- **Fuzzing.** The parser, the event decoder, the outbox drain and host deliveries are new input
  surfaces and get `tests/fuzz/` targets.

______________________________________________________________________

## 16. Reconciliation with the convergence

- **R5 (`noCell == 0`).** The C++ adapter and compiler use `std::optional<CellRef>`, as
  `ArenaManifold::link` returns. A vagent failure is the absence of a result, not a zero.
- **R6 (scalars).** Fields, event fields, priorities, ordinals, ids and deadlines are scalar cells.
- **R7 (a `SetLink`'s subject is its chain).** vagent writes no Structure operations into a user's
  store. A checkpoint writes them into the published value's own transient store, through `promote`,
  which follows R7.
- **R8 (two regimes).** Agent state is arena state (V9). The checkpoint promotion targets a value's
  own store, which no user visits, so R8's "a visited store is not mutated" holds. Writing to a
  user's store is the explicit `StoreWriter` capacity, under `PromotionBudget`. Whether an agent
  acting for its owner counts as a user-generated update is U-A9.
- **R12 (a dimension is a cell).** The eleven dimensions are minted as data by `std:agent` (V10).
- **U1 (rank indexing).** A consumer, not a resolver (§15).

______________________________________________________________________

## 17. Comparison

| Property         | SARL / Janus                | Jason (AgentSpeak)            | GOAL                         | vagent                                            |
| ---------------- | --------------------------- | ----------------------------- | ---------------------------- | ------------------------------------------------- |
| beliefs          | none built in               | Prolog-like belief base       | Prolog knowledge and beliefs | a fact set with sources                           |
| goals            | none built in               | achievement and test goals    | declarative achieve goals    | achieve, perform, maintain                        |
| plan selection   | n/a                         | first applicable, overridable | rule order                   | first applicable untried; not overridable (U-A12) |
| commitment       | n/a                         | single-minded                 | blind until achieved         | single-minded, reconsidered every turn            |
| actions          | synchronous method calls    | synchronous, with suspension  | synchronous                  | asynchronous effects that suspend the plan        |
| failure handling | exceptions                  | `-!g` failure plans           | goal remains                 | `PlanFailed`, reselection, `else`                 |
| in-plan search   | none                        | none                          | none                         | goal-directed generators, reversible assignment   |
| organisation     | contexts, spaces, holons    | via JaCaMo                    | none                         | contexts, spaces, holons (single owner)           |
| protocols        | by hand                     | by hand                       | by hand                      | by hand; a `protocol` declaration is U-A13        |
| distribution     | Janus network               | via JADE or JaCaMo            | none                         | Spool-Mesh actors                                 |
| maturity         | production compiler and IDE | mature interpreter            | mature interpreter           | specification; no compiler exists                 |

Where vagent is weaker: no implementation, two messages per send, single-owner spaces, a turn per
effect, slow child-loss detection, checkpoint bounds, and reclamation that depends on quiescence.
Where it differs: in-plan search with automatic undo of reversible writes, staged turns that either
happen entirely or not at all, and agent state held as quotable cells.

______________________________________________________________________

## 18. Open questions

- **U-A1. Reclaiming a busy agent's arena.** A reachability sweep (new arena machinery, O(live
  cells)), or an adapter step that copies the reachable state to a fresh arena between turns, which
  needs a turn boundary but no empty mailbox.
- **U-A2. Cross-owner spaces.** What capability a non-owner presents, and who issues it.
- **U-A3. Large payloads** as torrents named by `infoHash`, received through `ActorDeliverWriteSet`.
- **U-A4. Swarm spaces** bound to a `mesh_swarm` topic over BEP 46, with no host relay.
- **U-A5. Commitment strategies.** Blind and open-minded commitment as goal modifiers.
- **U-A6. One module path spelling across the family.**
- **U-A7. Suspension anywhere,** by compiling generators into cells.
- **U-A8. Recorded inbound streams** for lifetime replay and testing.
- **U-A9. Agents and R8.** When an agent's beliefs or results may become hypertime in its owner's
  store.
- **U-A10. Tests.** The unit is one turn on one arena, so a form such as
  `test "name" { given A(args); deliver E{...}; expect emitted F{...} }` with a manual clock and
  mock native skills can be specified now; its harness waits on the adapter.
- **U-A11. A fast lane.** An adapter-side priority for events of declared types, ahead of ready
  intentions and replies, with no runtime change.
- **U-A12. Overridable selection functions,** as Vortex routines a program supplies.
- **U-A13. Protocol declarations** for conversations such as Contract-Net.

______________________________________________________________________

## Appendix: Versioning and Change History

**3.0 — Staged effects, local wakes, data-only identity.** Effects became asynchronous suspension
points run by native skills outside the turn, which gives forward failure its meaning and keeps
agents from blocking (V6). Wake-ups moved from self-sent messages, which the runtime refuses, to a
local wake (§11.3 d). Bounded statements now cut to a depth read by one owed opcode, because vicon's
flag scheme cannot survive the trail (V13). Capacities are matched by name and no state links into
the program (V12); values are copied (V11); skills have frames. The checkpoint is admitted to be a
promotion with stated bounds (V9, §4.4). Beliefs became a set with sources and belief handlers bind
arguments. Added: reconsideration of waiting intentions, maintain back-off, `await`, `cancel` and
`refuse`, recovery after a hard failure, perception, leases, delivery retries and abandonment, spawn
placeholders, the `d.pending` queue, program-side ranks on `d.clause`, a total `if`, contextual
keywords, and corrected wire facts (32 KiB values, `sequence == 0`, no ordering). The status table
now separates prerequisites from built mechanisms.

**2.0 — Turn-bounded speculation, one actor per agent, BDI made explicit.** Rewritten after a review
from five perspectives. Withdrew `ArenaSlice`, the `#Abort` and `#Trap` opcodes, the Ingestion Gate
and Grant Renewal protocols, shadow cells and their dimensions, topological GC, per-event `promote`,
lifetime replay, and free-standing `commit`. Added rulings, the turn and persistence model, clone
views for many-to-one references, beliefs, goal kinds, plans `for` goals, selection functions,
suspension points, performatives, a grammar with a precedence table, the owed C++, refused
alternatives and conditional properties.

**1.0 — The SARL metamodel on Vortex.** Initial draft.
