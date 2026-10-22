# vagent: An Agent-Oriented Language over Vortex and the Spool-Mesh

| field         | value                                                                                                                                                                                                                       |
| ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| version       | 3.2 (see [Appendix](#appendix-versioning-and-change-history))                                                                                                                                                               |
| status        | Specification only. Nothing here is built or wired into the gleditor build.                                                                                                                                                 |
| compiles to   | Vortex opcode streams over an `ArenaManifold`, run one turn at a time by a Spool-Mesh actor executor                                                                                                                        |
| rests on      | Vortex ([runtime](vortex-hyperstructural-runtime.md)), Vlog ([logic](vlog-logic-extension.md)), vicon ([goals](vicon-goal-directed-language.md)), the Spool-Mesh actor runtime (`apps/common/xanadu/vortex/mesh_actor.hpp`) |
| prerequisites | Vlog §6 clause database (specified, unbuilt); vicon §6's scanning functions `tab`, `move`, `find`, `many`, `upto`, `match`, `any`, `bal` (specified; `std:goal` registers only `scan`, `subject` and `pos` today)           |
| proposes      | new code (front end, loader, adapter) and nine changes to existing code, each pending separate approval ([§11](#11-what-the-c-core-has-to-gain))                                                                            |

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
[§11](#11-what-the-c-core-has-to-gain) prices it; an owed change to existing code is a proposal
(P1–P9) pending separate approval. vicon's lexer, parser, compiler and driver exist under
`apps/common/xanadu/vicon/`; vagent reuses vicon's design, not its compiler, because that compiler
records what it cannot yet do (`vicon/compiler.cpp:12-22`). Code paths below are under
`apps/common/xanadu/` unless stated.

| Concept                    | Mechanism                                                                                                 | State                                                         |
| -------------------------- | --------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| Agent identity             | A stable agent name; `ActorPid` (`identity/identity_layout.hpp:304`) is only its current address          | name owed (V16); pid built                                    |
| Agent state                | Cells under one agent cell in the actor's `ArenaManifold` (`MeshActorRuntime::spawn`)                     | built (arena); layout owed by the compiler                    |
| A turn per message         | `MeshActorRuntime::pumpOne` runs the executor on one mailbox message                                      | built                                                         |
| A turn with no message     | A local wake that runs the executor without a mailbox entry                                               | **proposed**, P4                                              |
| Turn executor              | A `MeshActorRuntime::Executor` that decodes, dispatches, drains the outbox                                | **owed** (adapter, §11.1)                                     |
| Speculation in a turn      | `#Choice`/`#Fail`/`#Cut` over `mark`/`release`/`discard` (`VortexVM::step`)                               | built                                                         |
| Calls                      | `#Call`/`#Return` on the cursor's `+d.stack` (`VortexVM::step`)                                           | built                                                         |
| Bounded statements         | A choice point per statement, cut to the depth recorded at its start                                      | **proposed**: one opcode, P1                                  |
| Reversible assignment `<-` | A `Value` write the arena trails under the live mark (`ArenaManifold::release`)                           | built in the arena; vagent's lowering owed                    |
| Beliefs                    | Clauses on `d.clause` off predicate cells (Vlog §6.1); unification by `#Unify`                            | `#Unify` built; clause database a prerequisite; routines owed |
| Goals, plans, intentions   | Intention cells holding goal, plan name, step path, waits and frame                                       | owed in Vortex (`std:agent`)                                  |
| Capacity dispatch          | A walk of the agent's `d.skill` rank comparing capacity names                                             | owed in Vortex                                                |
| Messages                   | Outbox entries; the adapter calls `MeshActorRuntime::send` after the turn                                 | **owed** (adapter)                                            |
| Effects (device actions)   | Outbox entries; the adapter runs host bindings and reports completion as a host delivery                  | **owed** (adapter)                                            |
| Perception                 | Host drivers post percept events as host deliveries                                                       | **owed** (adapter)                                            |
| Spaces                     | A host agent holds a member rank and relays emissions                                                     | owed in Vortex; single-owner only (U-A2)                      |
| Spawning a child           | `MeshActorRuntime::spawn` under a capability the adapter issues, carrying the agent's parameters          | runtime built; issuer and bridge **owed**                     |
| Stopping an agent          | Mark stopped, notify monitors, free the actor's slots                                                     | **proposed**, P7                                              |
| Child loss                 | `monitor` and `ActorDown` after suspicion, lease expiry and discovery evidence (`MeshActorRuntime::tick`) | built; remote children only, minutes of latency (§5.4)        |
| Timers and deadlines       | Deadlines stored in cells; the adapter derives its timer heap from them                                   | **owed** (adapter)                                            |
| Lease renewal              | Replace a live actor's grant in place                                                                     | **proposed**, P6                                              |
| Checkpoint and migration   | `checkpointView`, `publishValue`, `resume` (`vortex/mesh_value.hpp`)                                      | built; migration changes the pid (§4.5)                       |
| Long-lived agents          | Reaping deliveries and slots, sender floors, configurable value limits, fair pumping                      | **proposed**, P2, P3, P5, P7, P8                              |
| Swarm-wide spaces          | `mesh_swarm` topics bound to a space                                                                      | not proposed ([§18](#18-open-questions), U-A4)                |

vagent asks for **one opcode and no other change to the VM, no change to `ArenaManifold`, no change
to `CompactOpNode`, and no new `SystemDimensions` field**. Everything else it owes is at the
boundary between the VM and the mesh runtime, which Vortex code cannot cross: `OpcodeKind` has no
send, spawn or host-call opcode (`vortex/vortex_vm.hpp`), and the `WorkerContext` an executor
receives has no handle to the runtime (`MeshActorRuntime::Executor`). AGENTS.md admits C++ for
driver and OS-event interfacing, which is what that boundary is.

______________________________________________________________________

## 2. Glossary

| Term                   | Meaning                                                                                                                               |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| agent                  | One actor of `MeshActorRuntime` running one compiled `agent` declaration                                                              |
| agent name             | A stable identity: the parent's name, `/`, and the parent's spawn ordinal. It survives migration; a pid does not (V16)                |
| agent cell             | The cell that heads every rank of agent state; the actor's `stateRoot`                                                                |
| peer cell              | A cell whose content is an agent name. Peer cells are values: two naming one agent are two cells                                      |
| turn                   | One run of the executor: a message turn for one mailbox message, or a wake turn for local work                                        |
| host delivery          | An effect result or a percept that the adapter hands to an agent in a wake turn; not a mailbox message                                |
| event                  | A typed record: delivered in an `ActorSend`, posted as a host delivery, or raised internally; decoded into an event cell for the turn |
| performative           | A message kind with fixed receiver semantics: `tell`, `untell`, `achieve`, `cancel`, `stop`, `refuse`, `outcome`, `ask`               |
| outbox entry           | A cell on `d.outbox` describing a message, spawn or effect the adapter performs after the turn                                        |
| envelope               | The `ActorMessage` the adapter builds from an outbox entry; never visible to vagent code                                              |
| space                  | A named set of member peer cells held by one host agent, which relays every emission into it                                          |
| context                | SARL's grouping of spaces. Each agent can name two: `inner` (the space it hosts) and `outer` (the space it was spawned into)          |
| holon                  | An agent with children; its children are the members of its `inner` space                                                             |
| launcher               | The agent `vagent run` starts first; it hosts the node's default space                                                                |
| capacity               | A named set of method signatures                                                                                                      |
| question               | A named, typed query an agent may be asked; `answer` declarations implement it                                                        |
| skill                  | An implementation of one or more capacities: written in vagent, or native (bound by the adapter to host code)                         |
| behavior               | A reusable bundle of handlers, fields, goals and plans mounted on an agent                                                            |
| belief                 | A ground fact of a declared predicate, with the set of sources that assert it                                                         |
| goal                   | A declared desire with a success test (achieve), a holding condition (maintain) or neither (perform)                                  |
| plan                   | A named procedure declared `for` a goal, with an applicability condition                                                              |
| intention              | A goal instance being pursued, stored as cells: id, kind, goal, plan, step path, waits, readiness and frame                           |
| goal intention         | The permanent intention a maintain goal creates when adopted; it runs no plan itself                                                  |
| restorer               | The `achieve`-like subgoal a goal intention adopts when its `holds` fails; it succeeds when `holds` holds again                       |
| activation frame       | The cell `#Call` pushes on the cursor's `+d.stack` for one call, holding its locals and spilled temporaries                           |
| suspension point       | A statement at which an intention ends its turn and waits                                                                             |
| clone view             | A cell on another cell's `d.clone` rank that reads its master's content (Vortex §5.2)                                                 |
| mark, release, discard | `mark` records the arena's extent; `release` undoes every write since it; `discard` forgets it and keeps the writes (`ArenaManifold`) |

Every structure below is an open **rank** with a head, as `store-slice-convergence.md` and the
Vortex document use the word. The mailbox is the runtime's C++ deque, not a structure in the arena.

______________________________________________________________________

## 3. Rulings

Each ruling states what it decides, why, and what it costs. Alternatives are in
[§13](#13-refused-alternatives).

**V1. An agent is one actor; there are no links between arenas.** `MeshActorRuntime::spawn` gives
each actor its own `WorkerContext`. An agent names another only through a peer cell and affects
another only by message. *Price:* a holon cannot read its children's state, and every holon
interaction is a message. The runtime's `ActorOptions` bound a node; they are constructor options
the launcher reads from `system://settings`.

**V2. Speculation is bounded by the turn.** Every choice point and mark opened in a turn is gone
before the turn ends. `checkpointView` refuses a non-zero `choiceDepth()` or `outstandingMarks()`,
so this is the only regime under which an agent can be checkpointed. *Price:* a reversible
assignment is reversible only within its statement. Alternatives that remain when a turn ends are
re-derived, not resumed.

**V3. One cursor per agent.** `VortexVM::backtrack` pops choice points belonging to other cursors
without releasing them, so two interleaved speculative cursors in one VM would corrupt each other.
*Price:* intentions take turns (§6.5); parallelism is between agents.

**V4. Intentions persist as data, never as VM state.** An intention is a set of cells: a numeric id,
its kind, its goal, the name of its plan, the names already tried, its step path, what it waits for
(with a deadline and, once matched, the matched payload), its readiness, its delegator if any, and a
frame of plan-local variables. On resume the executor re-enters the plan at the step path. This
matches `ActorContinuation`'s comment that VM program counters stay local and application state
lives in the published root. *Price:* suspension points are allowed only where a step path and a
frame describe the whole state (§6.6).

**V5. Everything outside the arena is staged.** Messages, spawns and effects are outbox entries on
`d.outbox`. They are minted under whatever mark is live, so a backtrack removes them for free. The
adapter performs what remains after a successful turn. A turn that fails hard performs nothing.
*Price:* nothing leaves mid-turn; a plan that needs an answer or a device result suspends (§6.6).

**V6. Effects are asynchronous and suspend.** A capacity method declared `effect` is implemented
only by a native skill, which the adapter binds to host code. Calling one in a plan writes an outbox
entry and suspends the intention; the host runs the action after the turn and reports its result or
failure as a host delivery that resumes the intention. Calling one in a handler stages it without
waiting; its failure is raised as an `EffectFailed` event. *Price:* an effect can never be one arm
of a search, and a plan pays a turn per effect. In exchange, no device call runs inside a turn, so a
failed turn never leaves the world changed and the arena rolled back, and an agent never blocks on a
device.

**V7. Iteration that acts is deterministic.** The arena trails every write to a cell older than the
live mark. Icon's `every e1 do e2` drives `e1` by failing back into it, so every write `e2` makes is
undone at the next resumption; vicon's compiler records the same for its loop flags
(`vicon/compiler.cpp:12-22`). vagent has no `every`. It iterates with `for x in e do body` over a
list, a range or a belief query, and with `while`, each iteration under its own choice point that is
cut when the iteration succeeds (§9.1). *Price:* a generator cannot drive a loop that acts.

**V8. Beliefs are a set of typed facts with provenance.** Every predicate is declared with typed
arguments, optionally with key arguments. The belief base is a rank of predicate cells on
`d.belief`, each heading its clauses on `d.clause` as Vlog §6.1 lays them out. Believing a fact
already present adds a source and raises no event; a fact is removed when its last source withdraws.
*Price:* lookup is a linear clause walk until convergence U1 lands (Vlog §6.3). The database itself
is a prerequisite.

**V9. Agent working state is arena state; the checkpoint is the one promotion.** R8 says an arena
mints no operations. vagent never promotes into a store the user visits. A checkpoint, however, is
`publishValue`, which promotes the reachable state into the published value's own transient `Store`
(`vortex/mesh_value.hpp`). That is the only promotion, and §4.4 prices it. An agent that must write
to a user's store uses an explicit `StoreWriter` capacity. *Price:* checkpoint cost grows with live
state, and lifetime replay from a spool is not on offer (§14).

**V10. Thirteen dimensions, minted as data.** `std:agent` finds or mints `d.behavior`, `d.skill`,
`d.capacity`, `d.space`, `d.member`, `d.holon`, `d.intention`, `d.subgoal`, `d.outbox`, `d.belief`,
`d.pending`, `d.source` and `d.extends` on the `d.dims` rank when it loads, as R12 requires.
`VortexCore::mintNamedDimension` looks a name up before minting it, so loading `std:agent` into a
resumed arena reuses the restored dimensions. `VortexCore::initGenesis` mints neither `d.source` nor
`d.extends`, which is why they are on the list. *Price:* thirteen cells per arena and a lookup per
dimension per load.

**V11. Values are copied; identities are names.** A link is reciprocal, so a cell has one negward
neighbour per dimension. Storing a value copies its cells (§5.9). The types that denote something
living elsewhere are names, not cells: an `Agent` is an agent name, an `Intention` and a `Timer` are
scalar ids, a `Space` is a host name and a space name. Copying a name copies the reference, so
nothing needs to be pointed at twice. The one deliberately shared structure is a skill's field frame
(§5.3). *Price:* O(size) per stored list or map; a name lookup per use of an intention or timer.

**V12. No state cell links into the program.** A checkpoint promotes everything reachable from the
agent cell along any dimension (the comment above `zigzag::promote`), so a link from state into a
program cell would drag the compiled program and `home` into every checkpoint. Capacities, plans,
skills and behaviors are therefore referred to by qualified name. The program's own output cells are
written at run time as scratch, which is how every Vortex opcode works (`VortexVM::step`); their
values never outlive a statement unless spilled to an activation frame (§5.8). *Price:* a name
comparison per dispatch and per plan lookup.

**V13. Each statement is a choice point cut to a recorded depth.** A statement takes its first
result and drops its choice points (vicon §4.4), and a statement that fails undoes only its own
writes. vagent records the choice depth when a statement starts, pushes the statement's own choice
point, and executes `#Cut` to the recorded depth when it produces. Reading the depth needs one
opcode (P1). *Price:* one opcode, one choice point and one mark per statement.

**V14. Goal-directed structure, statically typed spellings.** Expressions succeed with a value or
fail; there is no boolean test in control flow. Precedence follows Icon for `&`, `|`, `to` and
assignment. Spellings follow the statically typed family (`==`, `!=`, `++`), because one typed `==`
replaces Icon's numeric and lexical comparisons. §8.11 lists every divergence from vicon. *Price:* a
vicon reader must map the spellings.

**V15. SARL's surface where SARL has one.** Handlers are `on Event [guard] { }`, the event is
`occurrence`, capacities are imported with `uses`, emission is scoped. §8.10 lists the differences.

**V16. Agents are named; pids are addresses.** `resume` mints a new `ActorPid`, and a pid cannot be
known before a spawn completes. So vagent identifies an agent by its **name**: the parent's name,
`/`, and the parent's spawn counter, which the parent knows the moment it stages the spawn. Peer
cells hold names. Each agent keeps a `$peers` table from name to current pid, filled when a spawn
completes and updated from the sender name and pid that every envelope carries. *Price:* a table
lookup per send, and after a migration, messages to the old pid fail until the sender learns the new
one (§4.5).

______________________________________________________________________

## 4. The agent as one actor

### 4.1 One agent, one actor, one arena

Spawning an agent spawns one actor under a grant. The runtime's `Authorizer` turns a signed
capability into an `ActorGrant`, checks its `programRef` against the request, and caps
`maxOutputBytes` at 32 KiB (`MeshActorRuntime::spawn`). A running agent cannot sign anything, so the
adapter holds a **capability issuer** supplied by the host (the owner's delegated device key). The
issuer keeps a budget per agent, debited by every child it grants, so a holon cannot multiply its
own grant across many children.

**Parameters ride in the capability.** `ActorSpawn` must carry an empty `value`
(`identity/actor_wire.cpp`, `ActorMessage::isValid`), but its `capability` may hold 16 KiB. The
issuer signs the child's name and parameter values into the capability. The child's adapter reads
them from the spawn request, which `ActorContinuation` keeps, so they survive `resume`, and raises
`Initialize` as a local event in the agent's first wake turn. No message can overtake it, because
nothing reaches a handler before that turn.

Facts about the runtime that shape what follows:

- **Only the actor's owner may send to it.** `receive` refuses an `ActorSend` from any identity but
  the owner. vagent v3.2 specifies single-owner societies.
- **A failed executor stops the actor.** `pumpOne` sets `stopped` when the executor returns an
  error. The adapter reports success for every language-level outcome and returns an error only for
  its own faults.
- **A grant expires.** At `expiresAt`, `receive` refuses and `pumpOne` stops the actor with no
  notice to anyone. §4.5 gives the renewal path.
- **Slots are never freed.** `actors_`, `spawns_` and `monitors_` have no `erase` in
  `mesh_actor.cpp`, and nothing but an executor error or lease expiry stops an actor. `maxActors`
  and `maxMonitors` are therefore lifetime counts today; P7 makes them population bounds.
- **`pumpOne` is not fair.** It serves the first non-empty actor in `std::map` key order. P8 makes
  it round-robin.
- **Transport is pluggable.** `send` reaches a peer through the `Sender` the host installs; a
  same-node peer needs a loopback `Sender` that calls `receive` with the owner's identity, after
  `trustNode` for the local node (§11.1).

### 4.2 The turn

A **message turn** handles one mailbox message. A **wake turn** handles local work: a ready
intention, a due timer, `Initialize`, or a host delivery. The adapter asks for wake turns through
the local wake of P4. Both kinds run the same steps:

1. **Mark.** Take `m0 = arena.mark()`. `Mark` records the arena's cell count and scratch size.
1. **Fuel.** Re-arm
   `setExecutionLimits(m0.cells + maxCells, maxInstructions, m0.scratchSize + maxOutputBytes)`, so
   each turn gets the grant's budget of cells, work and string bytes relative to where the turn
   starts. The scratch limit is absolute in the arena, so it must be offset by `m0.scratchSize`, or
   an agent would stop for good after 32 KiB of accumulated strings. The VM's steps do not draw on
   the work counter (only allocation does), so the adapter also passes `maxInstructions` as `run`'s
   `maxCycles`.
1. **Turn cell.** Write the reserved field `$turn`: the turn time, read once, and for a message turn
   or host delivery the decoded event cell. `Clock.now()` reads it.
1. **Base.** Push a base choice point (`VortexVM::pushChoicePoint`) whose alternative is
   `std:agent`'s step-failure routine.
1. **Dispatch.** Run the dispatcher (§6.5) for an event, or continue the ready intention.
1. **Settle.** Repeat until nothing changes, at most `vagent.belief_event_depth` rounds in total:
   drain `d.pending`, then reconsider live intentions and maintain goals (§6.4). Each condition runs
   isolated, so none can fail the turn.
1. **Commit or undo.** On success: `clearChoicePoints()`, then `discard(m0)`; the writes stand. On a
   hard failure: `clearChoicePoints()`, `release(m0)`, reset the cursor's opcode, and run the
   recovery below.
1. **Scope.** Unlink the cursor's `+d.vars` and `+d.stack` ranks. They hold only this turn's
   activation frames (§5.8); without this the cursor's scope would grow for the agent's life.
1. **Drain** `d.outbox`: local spawns through `spawn()`, then messages (remote spawns travel as
   messages), then effects, then timer changes. Unlink `$turn`'s event cell and each entry.
1. **Derive** the adapter's ready list and timer heap from cells only (§5.6).

**Hard failures and how the adapter tells them apart.** `run` reports, and does not backtrack, three
kinds of trouble. An exception from the arena's limits (cells, work, scratch bytes) is caught. A
result with `contractViolation` set is a contract violation: the adapter calls
`VortexVM::backtrack(cursor)`, which lands in the statement's failure path, and runs on, so the
violation becomes an ordinary plan or handler failure with cause `contract`. A result whose cursor
has not reached its halt opcode after `maxCycles` steps is cycle exhaustion. The first and last, and
exceeding `vagent.belief_event_depth`, are hard failures.

**Recovery after a hard failure.** The arena is back where it was, but the message or wake that
caused the turn is consumed. If the turn was running an intention, the adapter runs a short turn of
`std:agent`'s `fail_step(id, cause)`, which records a plan failure with cause `fuel`, `cycles` or
`depth`; no user code runs in it. If the turn was handling an `ask` or a delegated `achieve`, the
turn result is a typed fault, so the sender's statement fails at once with cause `remote_fault`. Any
other event is dropped and counted in `$stats`.

At the end of step 10 no mark or choice point is live. The agent is quiescent, and `checkpointView`
accepts it, when in addition its mailbox is empty, no message it sent is incomplete, the adapter
holds no undelivered host delivery for it, and no effect it started is still running. Ready
intentions and pending timers do not prevent a checkpoint: they are cells and travel with it.

### 4.3 What persists

| Structure                          | Where                                              | Survives the turn | In the checkpoint root |
| ---------------------------------- | -------------------------------------------------- | ----------------- | ---------------------- |
| Fields (`var`)                     | `d.vars`/`d.values` off the agent cell             | yes               | yes                    |
| Reserved fields (`$...`)           | the same rank, names users cannot write            | yes               | yes                    |
| Beliefs and their sources          | `d.belief`, `d.clause`, `d.grab`, `d.source`       | yes               | yes                    |
| Installed skills and their fields  | `d.skill`; skill frames through `d.clone`          | yes               | yes                    |
| Mounted behaviors and their fields | `d.behavior`                                       | yes               | yes                    |
| Intentions, frames, waits          | `d.intention`, `d.subgoal`                         | yes               | yes                    |
| Hosted spaces and members          | `d.space`, `d.member`                              | yes               | yes                    |
| Children                           | `d.holon`                                          | yes               | yes                    |
| The turn's event cell              | `$turn`, unlinked in step 9                        | no                | no                     |
| Internal events, outbox entries    | `d.pending`, `d.outbox`, emptied each turn         | no                | no (always empty)      |
| Activation frames                  | cursor's `+d.stack` and `+d.vars`, unlinked step 8 | no                | no                     |
| Choice points, marks               | VM and arena                                       | no                | no (V2)                |
| Compiled program                   | loaded from `programRef`; never linked from state  | n/a               | no                     |

### 4.4 Cells, checkpoints and their costs

An arena frees cells and scratch bytes only by `release` truncation. `compact()` drops dead link
entries and frees no cell, and no reachability sweep is built. A long-lived agent therefore
accumulates unreachable cells and strings: consumed event cells, activation frames, finished
intentions, retracted clauses, uninstalled skills, and the bytes of every string they held.

vagent reclaims them by **checkpoint and resume**. `checkpointView` hands the arena and root to
`publishValue`, which promotes what is reachable into a transient store, seals and signs it, and
`resume` rebuilds an arena from that history, with only reachable cells. The costs per checkpoint:

- **Operations.** Promotion mints about one operation per live cell plus one per link. It is bounded
  by `PromotionBudget::maxOps` (65,536) and by `ValueLimits::operations` (65,536), which `resume`
  clamps to the grant's `maxCells` with a locally built `ValueLimits`. Until P5, an agent whose live
  state needs more than 65,536 operations cannot be checkpointed, moved or reclaimed. A belief costs
  its term's cells, its clause cell and one cell per source. An intention at rest costs about thirty
  cells (ten attribute pairs, its frame, its `tried` and `step` lists), so a thousand live
  intentions is roughly a third of the bound.
- **Permascroll growth.** Promotion spools constructed bytes (strings) into the author's append-only
  permascroll (the comment above `zigzag::promote`), so every checkpoint re-appends the live
  strings.
- **Program per agent.** Each agent's arena loads its own copy of the compiled program, because
  opcodes write their output cells (V12). A shared base `Manifold` with copy-on-write shadowing
  would avoid that; it is U-A14.
- **Quiescence.** A continuously busy agent may never reach the conditions of §4.2 (U-A1).

### 4.5 Leases, renewal and migration

`resume` cannot renew a lease in place: it refuses a checkpoint whose pid has the same node and an
epoch not older than the runtime's (`WrongEpoch` in `MeshActorRuntime::resume`), and it mints a new
pid while the old actor keeps its slot. Renewal is therefore P6: `renew` replaces a live actor's
grant with one the `Authorizer` issues for the same `programRef`, keeping the pid, arena and
mailbox. The adapter calls it when `vagent.renew_fraction` of the lease has elapsed. If the lease
ends first, the agent stops.

**Migration** to another node is checkpoint, publish and `resume` there, and it changes the pid.
Because peers hold names (V16), the migrated agent sends `Moved{name}` to its parent, its children
and the hosts of spaces it belongs to; every later envelope carries the new pid, and receivers
update `$peers`. A message sent to the old pid in between fails delivery and is resent after the
update. Intentions waiting on an effect cannot follow the device: `resume` fails them with cause
`migrated`.

An orderly stop (`stop`, `terminate`, or lease end without renewal) runs `on Destroy`, stages
`ChildStopping` to the parent, and asks the runtime to stop and reap the actor (P7). The parent
reports one `ChildStopping` even when the runtime's `ActorDown` follows it.

______________________________________________________________________

## 5. Structure in the arena

### 5.1 Dimensions

Found or minted by `std:agent` at load (V10):

| Dimension     | From → to                                          | Shape | Why not an existing dimension                                                       |
| ------------- | -------------------------------------------------- | ----- | ----------------------------------------------------------------------------------- |
| `d.behavior`  | agent cell → behavior instances                    | rank  | `d.vars` is the name-keyed field table; behaviors are not named fields              |
| `d.skill`     | agent cell → skill instances                       | rank  | as above; dispatch walks this rank and nothing else                                 |
| `d.capacity`  | skill instance → its own capacity-name cell        | link  | a reference by name to something outside the arena (V12)                            |
| `d.space`     | agent cell → spaces it hosts                       | rank  | no existing rank lists hosted spaces                                                |
| `d.member`    | space cell → member peer cells                     | rank  | `d.holon` is children only; a space can hold non-children                           |
| `d.holon`     | agent cell → peer cells of its children            | rank  | kept apart from `d.member` so monitoring and delegation never scan a space          |
| `d.intention` | agent cell → top-level intention cells             | rank  | `d.cursors` holds VM processes with program counters; an intention has none (V4)    |
| `d.subgoal`   | intention → its subgoal → that one's subgoal …     | chain | each intention awaits at most one subgoal, so the whole descent is one rank         |
| `d.outbox`    | agent cell → outbox entries                        | rank  | must be distinct so the drain touches nothing else                                  |
| `d.belief`    | agent cell → predicate cells                       | rank  | Vlog has `d.clause` for clauses but no directory of an agent's predicates           |
| `d.pending`   | agent cell → internal events raised this turn      | rank  | the dispatcher needs a queue it can walk in Vortex; emptied before the turn ends    |
| `d.source`    | clause → the peer cells that assert it             | rank  | the provenance meaning `result_slice.cpp:34` gives it; not minted by `initGenesis`  |
| `d.extends`   | event type or capacity name cell → its supertype's | link  | program side; `d.clone` would make sibling subtypes look like each other's subtypes |

Reused from `VortexCore::initGenesis` with their existing meanings: `d.vars`/`d.values` (fields,
frames, event fields, as `VortexCore::exportSymbol` uses them); `d.clause` (clauses, and on the
program side the alternatives of a goal or an event type, §5.7); `d.grab` and `d.step` (a term's
arguments as Vlog lays them out, and so lists, §5.9); `d.spin` (opcode streams); `d.clone` (skill
frames); `d.stack` (activation frames); `d.scan` (vicon's scanning cursor).
`ArenaManifold::projectProvenance` also mints `d.source`; agent arenas never project provenance, so
the two uses cannot meet.

Attachment uses `ArenaManifold::insertIntoRank` or `zigzag::rankTail` followed by `link`, never a
bare `link`, which evicts whatever either end held. Removal links a cell's predecessor to its
successor. A rank keeps no tail pointer, so appending walks it: O(length) per `adopt`, outbox entry
or pending event.

### 5.2 The agent cell

In the diagram, `(+d.x)` is the posward neighbour along `d.x` and `…` elides the rest of a rank.

```text
[agent cell "PatrolDrone"]
  ├─(+d.vars)──► [targets_seen] ─(+d.values)─► [3]
  │                └─(+d.vars)─► [$peers] ─(+d.vars)─► [$timers] ─(+d.vars)─► [$turn] …
  ├─(+d.skill)─► [instance "Navigation"] ─(+d.skill)─► [instance …]
  │                ├─(+d.capacity)─► ["mars:Navigation"]
  │                └─ on the d.clone rank headed by (rank head = master) [skill frame ThrusterNav]
  ├─(+d.behavior)─► [ObstacleAvoidance] ─(+d.vars)─► fields
  ├─(+d.belief)─► [pred at/3] ─(+d.clause)─► [clause] ─(+d.grab)─► [term at(…)]
  │                                              └─(+d.source)─► [peer] ─(+d.source)─► [peer]
  ├─(+d.intention)─► [intention 7: Survey] ─(+d.intention)─► [intention 9: KeepCharged]
  │                      └─(+d.subgoal)─► [intention 8: Reach]
  ├─(+d.space)─► [space inner] ─(+d.member)─► [peer] ─(+d.member)─► [peer]
  ├─(+d.holon)─► [peer child1] ─(+d.holon)─► [peer child2]
  ├─(+d.pending)─► (empty between turns)
  └─(+d.outbox)─► (empty between turns)
```

Each arrow is the only link its source has in that direction on that dimension. No arrow leaves the
agent's state for the program or `home` (V12). A child appears twice, as two peer cells: once on
`d.holon` and once on its space's `d.member`, so leaving the space does not disturb the holon.

### 5.3 Capacities and skills

Installing a skill mints one **skill frame** cell, whose content is the skill's qualified name and
which holds the skill's fields on `d.vars`, and one **instance** cell per capacity the skill
implements, including capacities those extend. Each instance is appended to the agent's `d.skill`
rank, appended posward to the frame's `d.clone` rank, and linked along `+d.capacity` to its own cell
holding the capacity's qualified name. The frame is the clone master (`ArenaManifold::cloneMaster`
returns the rank's negward end) and is never removed while an instance exists. A behavior instance's
content is likewise the behavior's qualified name.

A capacity call walks `d.skill`, takes the first instance whose capacity name matches, reads the
skill's name from its frame, and looks up `skill.method` in the program's symbol table (§5.7).
`install S` first uninstalls every installed skill that shares any capacity with `S`, whole, then
installs `S`, so a skill is never split across capacities. `uninstall` runs the skill's
`on uninstall` block, removes its instances, then its frame.

A **native skill** has no body. The adapter binds it by qualified name to host code registered at
start-up. Its methods are effects (V6), and it is the only kind of skill that may implement an
`effect` method. The host contract: starting an effect returns at once (the runtime is single
threaded per node, so a binding that blocks stalls every agent on the node); a binding may offer
`cancel`, which is best effort and idempotent; and a device shared by several intentions is
serialised or refused (`EffectFailed{cause: "busy"}`) by its binding.

### 5.4 Spaces, contexts and holons

A space is a cell on its host's `d.space` rank, with members as peer cells on `d.member`. Every
agent hosts its `inner` space from creation; children join it when spawned. `outer` is the parent's
`inner`, kept in `$outer` as the parent's name and the space name. A top-level agent's `outer` is
the launcher's space.

An emission into a space the agent does not host is one `ActorSend` to the host with a relay marker;
the host's `std:agent` relay handler sends one `ActorSend` per member, excluding the emitter. An
emission into a hosted space fans out directly. The host counts as a member for delivery: it
receives every emission into the spaces it hosts.

A holon's children are separate actors, named by peer cells on `d.holon`. A child knows its parent
as `parent`, kept in `$parent`. An orderly stop of a child is announced at once by `ChildStopping`
(§4.5). A crash of a **remote** child is reported by `ActorDown` to the holon, which monitors it,
but only after phi-accrual suspicion, the lease (60 s by default) and discovery evidence supplied
through `observe()` (`MeshActorRuntime::tick`); that is minutes. A crash of a **local** child
produces no evidence the runtime accepts ("missing evidence is never interpreted as absence",
`ActorDiscoveryEvidence`), so for local children only `ChildStopping` is reported.

### 5.5 Events and request ids

An event cell's content is the event type's name. Its fields hang on `d.vars`/`d.values` (§5.9).
Four implicit fields are always present and may not be declared: `source` (the emitter's name, or
absent for a host delivery), `space` (the space it arrived through, or absent), `time` (the turn
time) and `ordinal` (the emitter's emission counter).

On the wire the event is the `value` of an `ActorSend`: a bencoded dictionary of kind, type name,
the sender's name, ordinal, the sender's **floor** (§7.3), fields and correlation id. `value` is
limited to 32 KiB, and `sequence` must be zero for `ActorSend` (`ActorMessage::isValid`).

The runtime keys `deliveries_` and every actor's `remembered` by `requestId` alone, so an id must be
unique across senders. vagent's id is three fixed-width hex fields: 16 digits of a hash of the
sender's name, 16 of its turn counter, and 4 of the entry's position in the outbox, 38 characters in
all, within the 128 the wire allows. A hash, not the name, because names grow by one component per
level of a holon tree. A re-drained turn produces the same ids, so the runtime's de-duplication
treats a resend as a resend, and ids from one sender increase, which §7.3 and P3 use. Spawn requests
use the same scheme.

An event type that `extends` another matches handlers for both. The wire carries only the type name;
the receiver resolves the chain along `d.extends` in its own program.

### 5.6 Intentions and reserved fields

An intention cell's `d.vars` holds system attributes only: `id` (from `$next_id`), `kind`
(`achieve`, `perform` or `maintain`), `goal` (name and copied arguments), `plan` (the chosen plan's
qualified name, or absent), `tried` (names), `step` (the step path), `waiting` (kind, key, deadline,
and once matched the matched event or bindings), `ready` (absent when not ready, else the value of
`$turn_count` when it became ready), `delegator` (a peer cell and correlation id), `blocked_until`
(maintain back-off), `faulted` (conditions whose last evaluation faulted) and `frame`. Plan-local
variables hang on the `frame` value's own `d.vars`, so a plan variable named `step` cannot collide
with the system's.

Top-level intentions are on `d.intention`. A subgoal intention is reached only along the `d.subgoal`
chain from its top-level ancestor, so every walk of all intentions (reconsideration, readiness)
follows each chain. An `Intention` value is the scalar id; messages and timers name an intention by
it, because refs change when `resume` rebuilds the arena. Every deadline is an absolute time in a
scalar cell.

The adapter's ready list is the intentions with `ready`, ordered by it, ties by walk order; its
timer heap is the deadlines in `waiting` and `$timers`. Both are functions of cells only, rebuilt
after every turn and after `resume`, so a resumed agent runs the same schedule.

Reserved agent fields begin with `$`, which user identifiers cannot: `$name`, `$parent`, `$outer`,
`$peers` (name to pid), `$next_id`, `$spawn_count`, `$turn_count`, `$timers`, `$turn`,
`$terminating`, `$cancelled` (correlation ids cancelled before their `achieve` arrived, with an
expiry), and `$stats` (counts per event type of dropped, unhandled and late deliveries, and of
condition faults).

### 5.7 Program-side structure

The program is loaded from `programRef`. Its module symbol table (`d.vars` off the module cell, as
`exportSymbol` builds it) holds one name cell per goal, event type, question, and per method, answer
and `def` (as `skill.method`, `Agent.def`). Each goal and event-type cell heads a rank on `d.clause`
of its alternatives in declaration order: the plans for a goal, and the handlers for an event type.
A goal is a predicate whose clauses are plans, Vlog's shape reused. A handler or plan cell carries
its test on `+d.grab` (a handler's guard, a plan's `where`), as Vlog puts a clause's head, and its
body on `+d.spin`, as Vlog puts a clause's body; a handler's priority is a scalar on its `d.vars`.
Supertypes are reached along `d.extends`.

A behavior's program contributes its own ranks. To dispatch an event the dispatcher merges, by
descending priority then source order (agent, then behaviors in mount order) then position, the
ranks for the event's type and each supertype in every source. The cost is O(k) per step for k
ranks. The plan list of a goal is the agent's plans, then each mounted behavior's, then imported
modules', and an intention records plans by qualified name (`module:Agent.travel`), so mounting or
unmounting a behavior never changes what `plan` and `tried` mean.

### 5.8 Activations, locals and fields

Every method, handler, answer and condition call is a `#Call`, which pushes a frame cell on the
cursor's `+d.stack` holding the return opcode, and returns with `#Return`, which pops it. The call's
arguments and locals hang on that frame cell's `d.vars`/`d.values`. The compiler always follows a
`#Call` with an opcode, because `#Call` pushes no frame when its successor is empty and the next
`#Return` would then pop the caller's. `pushChoicePoint` records the stack top and `backtrack`
relinks it, so a failed call unwinds its frames with no extra code. Opcode output cells are static
per opcode, so a temporary that must live across a call, including a statement's recorded depth, is
spilled to the frame before the call and reloaded after it. That makes recursion safe. vicon's
`Bind`, which prepends to the cursor's scope and is never unbound, is not used.

A field read walks the owner's `d.vars` (the agent cell, a skill frame through `cloneMaster`, or a
behavior instance) and reads the value cell. A field write is a `Value` write on that cell, which
the arena trails when the cell is older than the live mark: that is what makes `<-` reversible, and
the position rules of §8.3 are what make `:=` stand.

### 5.9 Values

| Type                       | Cells                                                                                                  | Cost                             |
| -------------------------- | ------------------------------------------------------------------------------------------------------ | -------------------------------- |
| scalar (`Int`, `Float`, …) | one R6 scalar cell                                                                                     | O(1)                             |
| `Str`                      | one cell whose content is the text, interned in the arena's scratch                                    | O(length) bytes                  |
| record, event fields       | a value cell with a `d.vars` rank of name cells, each with its value on `+d.values`                    | O(fields)                        |
| list `[T]`                 | a head cell with the first element on `+d.grab` and the rest on `+d.step`: Vlog's compound-term layout | index and size O(n); `++` copies |
| `Map<K, V>`                | a list of `{key, value}` records                                                                       | lookup O(n)                      |
| `Opt<T>`                   | a value cell with or without its one value on `+d.grab`                                                | O(1) plus the value              |
| names and ids              | one scalar or text cell (V11)                                                                          | O(1)                             |

Because a list is a term, `#Unify` works on list-valued belief arguments. A `for x in xs` over a
list keeps the list and the walk position as an index in the frame, and resumes by walking to it.

______________________________________________________________________

## 6. Deliberation

### 6.1 Beliefs

Every predicate is declared, with its key arguments first and marked `key` when it has any:
`predicate at(key who: Agent, x: Float, y: Float)`. The checker types every `believe`, `forget`,
`revise`, `believes`, `tell`, `findall`, `await` and trigger against the declaration, and a `?v`
takes the type of its argument position. Strings serve as atoms.

- `believe at(self, 3.0, 4.0)` adds the fact with source `self`. If new, it raises `+at`; if
  present, it only adds the source. For a keyed predicate, a source holds at most one fact per key:
  a later `believe` or `tell` from the same source with the same key replaces that source's earlier
  fact.
- `forget at(self, ?x, ?y)` removes source `self` from every matching fact; a fact with no source
  left is retracted and raises `-at`.
- `revise at(w, 5.0, 6.0)` removes source `self` from every fact with key `w` (for an unkeyed
  predicate, from every fact of the predicate) and believes the new fact. Events are raised only if
  the set of facts changed. It is what a percept handler wants.
- `believes at(who, ?x, ?y)` produces once per matching fact, in clause order, binding `x` and `y`.
  When sources disagree, every value is a fact and `believes` yields each; `believes t from self`
  restricts to the agent's own.
- `for believes at(who, ?x, ?y) do body` runs `body` once per matching fact.
- `findall(?x, at(who, ?x, _))` is the list of bindings.

`findall` and `for believes` do not visit clauses by backtracking, because a result gathered inside
a branch is truncated when the branch resumes (V7). They copy the predicate's clause list first,
which gives Vlog's logical update view (Vlog §6.2). For each clause they copy the query term with
fresh variables, take a choice point and unify; on failure the mark is released, on success it is
cut, keeping that copy's bindings.

A fact is a clause cell with its head term on `+d.grab`, no body, and sources as a rank on
`+d.source`, each its own peer cell. `tell` adds the sender as a source; `untell` removes it. The
base is closed-world. A retracted clause is unlinked and dead until reclamation (§4.4).

Internal events are cells on `d.pending`, handled in raise order during step 6 of §4.2.

### 6.2 Goals and plans

An excerpt of §12.3's declarations:

```vagent
goal Reach(location: Point) achieve {
  succeed when believes position(location)
}
goal KeepCharged(floor: Float) maintain {
  holds believes battery(?level) & level > floor
}
goal Survey(target_id: Str, location: Point) perform
```

| Kind       | Succeeds when                                       | Fails when                             | Re-adopted                                                                |
| ---------- | --------------------------------------------------- | -------------------------------------- | ------------------------------------------------------------------------- |
| `achieve`  | `succeed when` holds at a step boundary or turn end | no applicable untried plan remains     | no                                                                        |
| `perform`  | a plan body completes                               | no applicable untried plan remains     | no                                                                        |
| `maintain` | never ends unless dropped                           | a restorer fails (reported, not fatal) | a restorer is adopted when `holds` fails at turn end, subject to back-off |

`drop when` applies to every kind; when it holds, the intention ends as dropped and `GoalDropped` is
raised. A plan for an `achieve` goal whose body completes while `succeed when` does not hold counts
as a failed plan, as in Jadex and GOAL. Goal clauses are resolved in the adopting agent's scope, so
they may read its fields, parameters and beliefs.

A plan is named and bound to a goal. Its header may list the goal's parameter names it uses, or omit
the list to bind them all:

```vagent
plan direct for Reach where believes battery(?level) & level > reserve_floor {
  follow([location])
}
```

Bindings made by `where` are visible in the body.

### 6.3 Adoption, selection and maintain goals

`adopt G(args)` creates an intention, appends it to `d.intention`, marks it ready, and returns its
id. Inside a plan, `achieve G(args)` creates a subgoal intention on the `d.subgoal` chain and
suspends the caller until the subgoal ends.

Plan selection takes the first plan in the plan list whose name is not in `tried` and whose `where`
succeeds, evaluated bounded and isolated as a condition (§6.4). It runs when the intention is first
continued and after each plan failure, always in a later wake turn than the failure, so a goal
cannot spend one turn's fuel cycling through its plans. A goal with no applicable plan fails at
once: waiting for applicability is written with `await` inside a plan, with a maintain goal, or by
re-adopting from a `GoalFailed` handler.

Adopting a maintain goal creates a **goal intention** of kind `maintain` that stays on `d.intention`
until dropped and never runs a plan itself. Its **restorer** is its subgoal, drawing on the same
plans; a restorer succeeds when the goal's `holds` holds again (tested like `succeed when`, §6.4)
and fails when its plans run out or a plan completes with `holds` still failing. At the end of every
turn, each maintain intention whose `holds` fails, that has no restorer and no `blocked_until` in
the future, gets one. A restorer that fails or is dropped sets `blocked_until` to now plus
`vagent.maintain_backoff`; a belief event on a predicate in `holds` clears it. Because the base is
closed-world, `holds` fails before the beliefs it reads exist, so a program adopts a maintain goal
once the beliefs it watches are present (§12.3).

### 6.4 Commitment and reconsideration

vagent v3.2 implements **single-minded commitment**. An intention keeps its goal until the goal
succeeds, its `drop when` holds, or no applicable plan remains. These conditions are checked when
the intention resumes and in the settle loop of every turn (§4.2 step 6) for every live intention,
including those waiting on a subgoal, a message or an `await`.

Three rules keep reconsideration safe:

- **Isolation.** Each condition (and each `where`, and each await guard) runs under its own choice
  point with a fuel slice (`vagent.condition_fuel`). A condition that fails is "does not hold". One
  that runs out of fuel or violates a contract is "unchanged" (no drop, no success, no restorer) and
  raises `ConditionFault{intention, cause}` once, recording the condition in `faulted` until it next
  evaluates cleanly. So one faulty condition cannot fail the turn, and does not raise an event every
  turn.
- **Effects in flight.** `succeed when` is not tested while the intention waits on an effect,
  because the plan owns the action; it is tested when the effect ends.
- **Drop takes effect at a boundary.** A plan that drops itself or an ancestor finishes its current
  statement; the intention then ends as dropped in the settle loop, and the statement's staged
  entries stand, as at any statement boundary.

Dropping an intention drops its subgoals, sends `cancel` for a delegated goal, and stages the host
binding's `cancel` for an effect in flight. Dropping only a subgoal makes its parent's `achieve`
fail with cause `dropped`, so the parent's `else` runs. Blind and open-minded commitment are not
offered (U-A5).

### 6.5 The deliberation cycle

The selection functions are fixed in v3.2:

- **Event selection** is mailbox order, then host deliveries in arrival order. There is no priority
  mailbox (§13).
- **Option selection** is §6.3.
- **Intention selection** is ready order (§5.6): oldest ready first, one per wake turn.

```text
turn:
  match source:
    Event(e) | Performative(e) | Percept(e):
      for each intention whose await matches e (guard isolated, with the intention's frame):
        copy e or the bindings into its waiting; mark it ready
      for handler in merged handlers(e) (§5.7):
        under the handler's own choice point:
          if guard succeeds and body succeeds: cut, keeping writes
          else: release, undoing writes and staged entries
        if e was consumed: break
    Initialize | Ready(i) | Reply(i, v) | EffectDone(i, r) | EffectFailed(i, c) | Timer(i):
      run Initialize's handlers, or continue(i)
    Down(child):
      raise ChildDown(child); fail intentions waiting on child
  settle: repeat until nothing changes, within belief_event_depth rounds
    dispatch every event on d.pending as above
    reconsider every live intention; check maintain goals     -- §6.4, isolated

continue(i):
  if drop-when holds: end i as dropped
  if i is achieve, not waiting on an effect, and succeed-when holds: end i as achieved
  if i has no plan: select one (§6.3); mark i ready; return
  run i's plan from i.step to the next suspension point or the end of the body
    suspension point: record step, frame, waiting; clear ready; return
    end of body:      achieve re-tests succeed-when; perform succeeds
    failure:          add the plan to tried; clear plan; raise PlanFailed; mark i ready
  ending i: unlink it; if it has a parent intention, mark the parent ready with SubgoalEnded;
            if it has a delegator, stage outcome; raise GoalAchieved, GoalFailed or GoalDropped
```

Awaits are matched before handlers, so `consume` does not hide an event from an await. Between
suspension points a plan segment runs alone, so beliefs and fields cannot change under it.

`priority` orders handlers within a turn. A handler cannot interrupt an intention, because they run
in different turns; it can **drop** one. Worst-case reaction latency is therefore the queue length
times the turn time; U-A11 records an adapter-side fast lane.

`consume` stops lower handlers seeing the event, as Jason's one plan per event does; without it, all
matching handlers run, as in SARL. An event no handler and no await matches is counted in `$stats`.

### 6.6 Suspension points

| Statement                                                    | Waits for                              | Resumed by                                    |
| ------------------------------------------------------------ | -------------------------------------- | --------------------------------------------- |
| `achieve G(args)`                                            | the subgoal intention to end           | `SubgoalEnded`                                |
| `achieve G(args) by a within d`                              | the delegate's outcome or refusal      | `outcome`, `refuse`, or the deadline          |
| `x := ask a.q(args) within d`, `let x := ask …`              | the answer                             | the reply, or the deadline                    |
| `await t [g] within d`                                       | a fact matching term `t` and guard `g` | a belief event, or the deadline               |
| `await E as e [g] within d`                                  | an event of type `E` passing `g`       | the dispatcher, or the deadline               |
| `m(args)`, `x := m(args)`, `let x := m(args)`, `m` an effect | the host's result                      | `EffectDone`, `EffectFailed`, or the deadline |
| `yield`                                                      | nothing; lets other work run           | its own ready mark                            |

A deadline is required on every wait for something outside the agent; for an effect it is declared
on the capacity method, and the checker rejects an `effect` method without `within`. An elapsed
deadline fails the statement; `else e` supplies a value or recovery, with `cause` bound. When a
delegated `achieve` or an effect times out, `cancel` is staged to the delegate or the host binding.
A reply, completion or event that arrives for an intention that has ended is dropped and counted in
`$stats`.

`await t [g]` tests the belief base first and succeeds at once if a fact matches `t` and passes `g`;
otherwise it waits for a belief event that does. `await E as e [g]` sees only events that arrive
after the suspension. In both cases the matched event or bindings are copied into the intention's
`waiting` at match time, so the resumed statement reads that copy, not the event cell that the
matching turn unlinked or a fact retracted since.

The compiler accepts a suspension point only where V4 can describe the whole state: a statement of a
plan body, or a statement in the `then`, `else` or `do` body of an `if`, `while` or `for` that is
itself at such a place. The step path is the list of statement ordinals down to it. A `while`
resumes by re-testing its condition from frame variables; a `for` over a list or range keeps the
list and index in the frame. A `for believes` loop may not contain a suspension point. A suspension
point inside an expression (an operand of `&`, `|`, a comparison or a call, such as `f(spectra(t))`)
is a compile-time error, because choice points do not survive a turn (V2).

### 6.7 Failure

| Outcome            | Cause                                                              | Handled by                                                                           |
| ------------------ | ------------------------------------------------------------------ | ------------------------------------------------------------------------------------ |
| expression failure | a generator ran dry                                                | the enclosing `&`, `\|` or `if`; at statement level, the statement fails             |
| statement failure  | a failed bounded expression, `fail`, an elapsed deadline           | the statement's own writes and staged entries are undone; its block fails            |
| handler failure    | its block failed                                                   | the handler's choice point is released: all its writes and staged entries are undone |
| plan failure       | the body failed, or it ended with `succeed when` unmet             | `PlanFailed` raised; another plan is selected in a later turn                        |
| goal failure       | no applicable untried plan remains                                 | `GoalFailed` raised; a parent's `achieve` fails; a delegator is told                 |
| effect failure     | the host reports failure, or the deadline elapses                  | in a plan, the suspended statement fails; from a handler, `EffectFailed` is raised   |
| delivery failure   | a message stays incomplete past its horizon (§7.3)                 | `DeliveryFailed` raised; an `ask` or delegated `achieve` fails                       |
| refusal            | the receiver's `accepts` refused a delegated goal                  | the delegating `achieve` fails at once with cause `refused`                          |
| remote fault       | the receiver of an `ask` or delegation failed hard                 | the waiting statement fails at once with cause `remote_fault`                        |
| contract violation | `requires`/`ensures` on a method                                   | as statement failure, with cause `contract`; never stops the agent                   |
| condition fault    | a condition ran out of fuel or broke a contract                    | `ConditionFault` raised once; the intention's state is unchanged (§6.4)              |
| hard failure       | fuel, cycles, scratch bytes or settle rounds exhausted in the turn | the turn is undone; recovery (§4.2)                                                  |
| child loss         | `ActorDown` for a monitored remote child, or `ChildStopping`       | `ChildDown` or `ChildStopping` raised; intentions waiting on that child fail         |

Handlers and plans fail differently. A handler runs under one choice point, so a failed handler
leaves no trace. In a plan each statement has its own choice point (V13): when a statement fails,
only its own writes and staged entries are undone, and the failure leaves the plan body for the
plan-failure path without touching earlier statements, in this turn or earlier ones. That is what
makes `emit SurveyFailed{...}` followed by `fail` work in §12.3, and it is why a plan that fails
after an effect starts the next plan from the world and the beliefs as they now are. In a handler or
method body, a failing statement's alternative is `#Fail`, so the failure propagates to the
handler's or the caller's choice point.

______________________________________________________________________

## 7. Communication and perception

### 7.1 Performatives and questions

| vagent form                               | Kind      | Receiver's default handling (`std:agent`)                                                                                                   | Reply awaited         |
| ----------------------------------------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- |
| `emit E{...} [in s] [for r]`              | `event`   | user handlers and awaits for `E`                                                                                                            | no                    |
| `tell a fact`                             | `tell`    | add the sender as a source of `fact`; `+` handlers if new                                                                                   | no                    |
| `untell a fact`                           | `untell`  | remove the sender as a source of `fact`                                                                                                     | no                    |
| `achieve G(args) by a within d`           | `achieve` | if `accepts(sender, goal_name)` succeeds, adopt `G` with the sender as delegator; else stage `refuse`; ignored if `$cancelled` holds its id | `outcome` or `refuse` |
| (staged when a delegated wait ends early) | `cancel`  | drop the intention adopted for that correlation id; if none yet, remember the id in `$cancelled` for `send_retries × send_retry_interval`   | no                    |
| (staged when a delegated intention ends)  | `outcome` | `{correlation, status, cause}`; resumes the delegating `achieve`; dropped if cancelled                                                      | no                    |
| (staged by `accepts` failing)             | `refuse`  | `{correlation}`; fails the delegating `achieve` with cause `refused`                                                                        | no                    |
| `stop a`                                  | `stop`    | accepted from the parent only: run `on Destroy`, then stop (§4.5)                                                                           | no                    |
| `x := ask a.q(args) within d`             | `ask`     | run the receiver's `answer q`, read-only; its value is the turn result                                                                      | the answer            |

Every `ActorSend` is answered by the runtime with an `ActorDeliverValue` carrying the receiver's
turn result. For `ask` that result is the answer, or a typed fault. For other kinds the sender's
adapter completes the reply without running user code; it still costs one short message turn (§15).

A **question** is declared once, top-level, like a predicate: `question stock(item: Str): Int`. An
`answer` in an agent or behavior implements a declared question with the same signature, and
`ask a.stock(item)` is checked against the declaration, so argument and result types are static
though an `Agent` value carries no interface. An agent with no `answer` for an asked question
replies with the fault `unanswered`.

`answer` bodies are read-only, transitively through every method they call: no field write, no
belief change, no message, spawn, adoption, installation or effect. The answer must fit 32 KiB.

`accepts(sender: Agent, goal_name: Str)` is a capacity every agent has (`std:agent`'s `Accepts`).
Its default skill accepts delegation from the parent and from children, and refuses everything else.
It sees the goal's name only; a delegate that must refuse on arguments does so by failing its plans,
which reports `outcome` with status `failed`.

### 7.2 Scopes

`emit E{...}` goes to `outer`. `in s` names a space: `inner`, `outer`, or a space value from
`std:agent`. `for r` restricts delivery to one name or a list of names, which must be members or the
host of that space (`outer` when `in` is absent). The emitter does not receive its own emission.

### 7.3 Delivery

A sent message is complete when its reply, a write-set delivery or an `ActorDown` for it arrives.
Until then it blocks the sender's checkpoint. A message can stay incomplete for ever: refused with
`MailboxFull`, sent to an unknown or expired receiver, accepted by a receiver whose lease then
ended, or overtaken by an `ask` deadline. Neither `send` nor `retry` can resend an acknowledged
delivery (both return early once `acknowledged` is set), so P2 adds `redeliver(requestId)`. The
adapter:

- redelivers a message whose reply has not arrived every `vagent.send_retry_interval`, whether or
  not it was acknowledged; a receiver that already handled it re-emits its stored reply;
- after `vagent.send_retries` redeliveries, or when the waiting statement's deadline elapses,
  abandons the delivery through P2 and raises `DeliveryFailed`.

**Floors.** Every envelope carries the sender's floor: the smallest id it is still redelivering, so
every id of that sender below the floor is complete or abandoned. A receiver forgets remembered ids
from that sender below the floor (P3), and drops an id below the floor as a stale duplicate. An id
above the floor is never windowed out, however late it arrives, so the lack of ordering loses
nothing.

A local spawn uses `spawn()`, which creates no delivery. A remote spawn is an `ActorSpawn` sent
through `send`; P2 makes its `ActorSpawnAck` complete it, which the runtime does not do today.

There is no ordering guarantee: the runtime does not number messages. Each event carries its
emitter's ordinal, so a receiver can detect gaps; the adapter does not reorder.

### 7.4 Liveness

No vagent construct waits inside a turn, and every wait on something outside the agent has a
deadline (§6.6), so a cycle of agents waiting on each other ends when the first deadline elapses.

### 7.5 Perception

Percepts come from native skills. A host driver posts an event of a declared type for its agent
through the adapter, which queues it as a host delivery and asks for a wake turn. Handlers turn
percepts into beliefs:

```vagent
on Battery as b { revise battery(b.level) }
```

Host deliveries take no mailbox slot and need no reply. They are not in the checkpoint, so the
adapter hands them all to the agent before it checkpoints, and a crash loses undelivered ones; a
driver re-reports its state periodically for that reason.

______________________________________________________________________

## 8. The surface language

### 8.1 Lexical structure

- Comments: `//` to end of line, and `/* ... */`, not nesting.

- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Types, events, goals, agents, behaviors, skills and
  capacities are `PascalCase`; predicates, questions, fields, locals and methods are `snake_case`.
  The checker rejects a `PascalCase` name that is not a type and a `snake_case` name that is one, so
  `Name {` is always a record literal and `name {` never is. `_` alone binds nothing. `$` begins
  reserved field names, which user code cannot declare or assign.

- Literals: integers (`42`, `1_000`, `0x2A`), floats (`3.5`, `1e-3`), strings (`"..."` with `\n`,
  `\t`, `\"`, `\\`, `\u{...}`), `true`, `false`, `()`, `none`, durations (`250ms`, `2s`, `5min`).

- `<-` is one token, so `a<-1` is an assignment; the checker warns when `<-` has no space before it.

- Statement termination: a newline ends a statement when the last token can end one (an identifier,
  a literal, `)`, `]`, `}`, or a keyword such as `yield`). A line that begins with `&`, `|`, `++`,
  `+`, `/`, `%`, a comparison, `.`, `?`, `to`, `by`, `then`, `else` or `do` continues the previous
  statement. A line that begins with `-`, `*`, `!`, `(`, `[` or `{` begins a new one, because those
  can start an expression. Newlines inside `( )`, `[ ]`, a record literal's `{ }` or a guard's `[ ]`
  are whitespace; inside a block nested in those, they are significant again. `;` separates
  statements on one line. Top-level and member declarations need no separator; a declaration ends
  where the next begins.

- Keywords. Reserved everywhere:

  ```text
  achieve adopt agent answer ask await behavior believe believes by capacity consume def do drop
  effect else emit event extends fail findall for forget goal if implements import in install key
  let mount native none not on plan predicate question record return revise self skill spawn stop
  tell terminate then to uninstall unmount untell uses var where while within yield
  ```

  Contextual, reserved only in the position that uses them: `as`, `ensures`, `from`, `holds`,
  `inner`, `maintain`, `module`, `occurrence`, `outer`, `parent`, `perform`, `priority`, `requires`,
  `succeed`, `when`. After `.`, in a field declaration and as a record-literal key, any word is
  accepted, so `f.goal` and an event field named `to` work, and `import std:agent` parses.
  Parameters and locals must be identifiers, because a parameter named with a keyword could never be
  read.

### 8.2 Truth and failure

An expression succeeds with a value or fails (vicon §4.1).

- `a < b` produces `b` or fails; `a == b` produces `b` or fails. Comparison does not chain: write
  `a < b & b < c`. `==` compares values of one static type; `1 == 1.0` is a type error.
- `true` and `false` are values of type `Bool`. In a condition, a guard, or an operand of `&`, `|`
  or `not`, an expression of type `Bool` whose outermost operator is not a comparison is rejected,
  with the message "compare with `== true`", because a `false` value is a success. `sealed == false`
  is accepted: its outermost operator is a comparison, which fails when `sealed` is `true`.
- `not e` succeeds with `()` if `e` fails and fails if `e` succeeds. It binds looser than
  comparison.
- `&` is conjunction: `e1 & e2` evaluates `e2` for each result of `e1` until `e2` succeeds. `|` is
  alternation. There is no `&&` or `||`. A comparison whose right operand is an alternation of
  literals (`x == 1 | 2`, which parses as `(x == 1) | 2` and always succeeds) draws a warning
  suggesting `x == (1 | 2)`.
- `!e` generates the elements of a list or the characters of a string. `*e` is the size of `e`.
- A statement is bounded: its first result is taken and its choice points are cut (V13). A failing
  statement fails its block. A block's value is its last statement's value, or `()`.
- `if c then e` without `else` is total: when `c` fails, the `if` succeeds with `()`, and `e` must
  have type `()`. The failing form is `if c then e else fail`.
- Conditions are pure. A guard, `where`, goal clause, contract, and the condition of an `if` or
  `while` may not contain an action of §8.6, an assignment or an effect call, and may call only
  `def`s the checker has found pure (no action, assignment or effect in them or anything they call).
  So mistyping `believe` for `believes` in a condition is a compile-time error.
- `a to b` includes `b`. `for x in e do body` evaluates `e` once, bounded, to a list or a range, and
  runs `body` for each element with no choice point between iterations. `body` is bounded each time.
  The loop fails if `body` fails. There is no `break`; an early exit is a `while`.
- A value of type `Opt<T>` read in an expression succeeds with its value or fails when it is `none`.
  So `[timer]` is a presence test and `not charging` succeeds when there is none. Assigning a `T` to
  an `Opt<T>` place stores the value.
- `fail` and `return` have no value; they may stand wherever any type is expected.

### 8.3 Assignment and positions

`x := e` assigns irreversibly. `x <- e` assigns reversibly: if control backtracks past it within the
statement, the old value returns. `let x := e` declares a local. `var x: T := e` declares a field
and requires an initial value, evaluated pure after the agent's parameters are set and before member
forms run; a field that may be empty is `Opt<T>` initialised to `none`. Targets are places: a local,
a field, or a field or index of one.

A position is **bounded** if it is a statement, or the `then`, `else` or `do` body of an `if`,
`while` or `for`, or the `else` of an `achieve`, `ask` or `await`, whose own position is bounded.
Every other position is **speculative**: an operand of `&`, `|`, `to`, a comparison or a call, or
anything inside one. A write or staged action in a speculative position is undone if control
backtracks over it; that is what makes `<-` and a speculative `emit` useful.

| Construct                                                                                                                                   | Allowed in                                                                                                                |
| ------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `<-`, `emit`, `tell`, `untell`, `believe`, `forget`, `revise`, `spawn`, `adopt`, `install`, `uninstall`, `mount`, `unmount`, `drop`, `stop` | any position in handler, plan and method bodies, except conditions (§8.2); undone with a backtracked branch               |
| `:=`, `consume`, `terminate`, `return`                                                                                                      | bounded positions only                                                                                                    |
| effect call                                                                                                                                 | plans: suspension positions; handlers: bounded positions, staged without waiting; never in methods, conditions or answers |
| `achieve`, `ask`, `await`, `yield`                                                                                                          | plans, suspension positions only (§6.6)                                                                                   |

### 8.4 Precedence

From tightest to loosest. Binary operators associate left unless marked.

| Level | Operators                              | Notes                                                          |
| ----- | -------------------------------------- | -------------------------------------------------------------- |
| 1     | call `f(x)`, index `a[i]`, field `a.b` |                                                                |
| 2     | unary `-`, `!`, `*`                    | `!xs` generates; `*xs` is the size                             |
| 3     | `*` `/` `%`                            |                                                                |
| 4     | `+` `-`                                |                                                                |
| 5     | `++` (concatenation)                   | strings and lists                                              |
| 6     | `<` `<=` `>` `>=` `==` `!=`            | non-associative                                                |
| 7     | `not`                                  | prefix; `not a == b` is `not (a == b)`                         |
| 8     | `\|` (alternation)                     | below comparison, as in Icon: `x == 1 \| 2` is `(x == 1) \| 2` |
| 9     | `to` … `by`                            | inclusive                                                      |
| 10    | `?` (string scanning)                  | above assignment, so `x := s ? e` is `x := (s ? e)`            |
| 11    | `:=` `<-`                              | right-associative                                              |
| 12    | `&`                                    | loosest, as in Icon                                            |

So `budget <- b - cost & budget > floor & route` is
`(budget <- (b - cost)) & (budget > floor) & route`. Icon's limitation `e \ n` is not offered (§13).

### 8.5 Declarations

A fragment; the events it uses are declared elsewhere, and the literals stand for values a real
program takes from parameters or settings.

```vagent
predicate at(key who: Agent, x: Float, y: Float)
question heading(): Float

event TargetDetected { target_id: Str, distance: Float }
event Alarm extends TargetDetected { level: Int }

capacity Navigation {
  effect def move(speed: Float, heading: Float) within 30s requires speed >= 0.0
  effect def stop_engines() within 2s
}

native skill ThrusterNav(max_speed: Float) implements Navigation

behavior ObstacleAvoidance(threshold: Int) {
  uses Navigation
  on CollisionWarning as w [w.threat > threshold] priority 1 {
    stop_engines()
    emit ObstacleEvaded{obstacle: w.obstacle_id}
  }
}

agent PatrolDrone(threshold: Int, max_speed: Float) {
  uses Navigation
  install ThrusterNav(max_speed)
  behavior ObstacleAvoidance(threshold)
  var targets_seen: Int := 0
  var bearing: Float := 0.0

  answer heading(): Float { bearing }

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
belief handler's trigger is a term whose `?` variables bind the fact's arguments; it takes no `as`.
Name resolution for a bare call: the agent's or behavior's own `def`s, then methods of used
capacities (qualified as `Navigation.move(...)` when two define the name), then `std:agent`
functions and capacities, which every agent uses without declaring them.

Creation order: parameters from the capability, then `var` initialisers, then `believe`, `install`
and `behavior` members in declaration order, then `on Initialize`. The same words used as actions
run when reached.

### 8.6 Actions

| Form                                         | Value       | Notes                                                                                     |
| -------------------------------------------- | ----------- | ----------------------------------------------------------------------------------------- |
| `emit e [in s] [for r]`                      | `()`        | staged (V5); an event with no fields is written `Tick{}`; `r` is a name or a list         |
| `tell a fact`, `untell a fact`               | `()`        | staged                                                                                    |
| `believe fact`, `forget fact`, `revise fact` | `()`        | immediate in the arena; raise belief events (§6.1)                                        |
| `spawn A(args) [in s]`                       | `Agent`     | staged; the name is known at once, the pid when the adapter spawns                        |
| `adopt G(args)`                              | `Intention` | does not wait                                                                             |
| `achieve G(args) [by a within d] [else e]`   | `()`        | suspension point                                                                          |
| `x := ask a.q(args) within d [else e]`       | the answer  | suspension point; `q` is a declared question                                              |
| `await t [g] within d [else e]`              | `()`        | suspension point; binds `t`'s variables                                                   |
| `await E as e [g] within d [else e2]`        | `E`         | suspension point                                                                          |
| `drop i`                                     | `()`        | ends an intention as dropped (§6.4); on an id that is not live, does nothing and succeeds |
| `install S(args)`, `uninstall S`             | `()`        | §5.3                                                                                      |
| `mount B(args)`, `unmount B`                 | `()`        | adds or removes a behavior instance                                                       |
| `stop a`                                     | `()`        | staged `stop` to a child                                                                  |
| `terminate`                                  | `()`        | after this turn, run `on Destroy` and stop                                                |
| `return [e]`                                 | none        | leaves the method, answer or `def`                                                        |
| `yield`, `consume`, `fail`                   |             | as §8.3                                                                                   |

If a spawn is refused, the adapter raises `SpawnFailed{child}` in a wake turn and fails deliveries
staged to that name.

### 8.7 Types

`Int`, `Float`, `Str`, `Bool`, `()`, `Duration`, `Time`, `Agent` (an agent name), `Space` (a host
name and a space name), `Intention` and `Timer` (scalar ids), `Event` (the supertype of all events),
`Opt<T>`, `[T]` (a list), `Map<K, V>` (from `std:agent`, §5.9), and declared events and records.
`record Name { fields }` declares a record with no wire kind; geometry such as a `Point` is a record
the program declares. `predicate p(args)` declares a belief predicate and `question q(args): T` an
askable query. Values are copied when stored (V11). Types are checked statically, including belief
terms against their predicates; locals are inferred.

### 8.8 Names in scope

| Name         | Type           | Meaning                                                                      |
| ------------ | -------------- | ---------------------------------------------------------------------------- |
| `self`       | `Agent`        | this agent's name (`$name`)                                                  |
| `parent`     | `Opt<Agent>`   | the name of the agent that spawned this one; `none` for a top-level agent    |
| `inner`      | `Space`        | the space this agent hosts                                                   |
| `outer`      | `Space`        | the space this agent was spawned into; for a top-level agent, the launcher's |
| `occurrence` | the event type | in a handler, the event being handled                                        |
| `cause`      | `Str`          | in an `else` after `achieve`, `ask`, `await` or an effect, the failure cause |

A program starts with `vagent run module:Agent(args)`, which starts the launcher and then the named
agent, with those arguments, in the launcher's space. Agents the program depends on, such as §12.4's
`Warehouse`, are started the same way and their names passed as arguments.

### 8.9 Grammar

`Ident`, `Word` (an identifier or any keyword), `IntLit`, `Duration`, `Literal` and `NEWLINE` are
the lexical forms of §8.1.

```ebnf
Unit          ::= [ ModuleDecl ] { ImportDecl } { TopDecl } ;
ModuleDecl    ::= 'module' ModulePath ;
ImportDecl    ::= 'import' ModulePath [ 'as' Ident ] [ '(' IdentList ')' ] ;
ModulePath    ::= Word { ':' Word } ;

TopDecl       ::= EventDecl | RecordDecl | PredicateDecl | QuestionDecl | CapacityDecl
                | SkillDecl | BehaviorDecl | AgentDecl | GoalDecl | PlanDecl ;

EventDecl     ::= 'event' Ident [ 'extends' Ident ] [ '{' [ FieldList ] '}' ] ;
RecordDecl    ::= 'record' Ident '{' [ FieldList ] '}' ;
PredicateDecl ::= 'predicate' Ident '(' [ PredParam { ',' PredParam } ] ')' ;
PredParam     ::= [ 'key' ] Ident ':' Type ;
QuestionDecl  ::= 'question' Ident '(' [ ParamList ] ')' ':' Type ;
FieldList     ::= Field { ',' Field } [ ',' ] ;
Field         ::= Word ':' Type ;

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
VarDecl       ::= 'var' Ident ':' Type ':=' Expr ;

HandlerDef    ::= 'on' ( Ident [ 'as' Ident ] | ( '+' | '-' ) Term )
                  [ '[' Expr ']' ] [ 'priority' IntLit ] Block ;

GoalDecl      ::= 'goal' Ident '(' [ ParamList ] ')' GoalKind [ '{' { GoalClause } '}' ] ;
GoalKind      ::= 'achieve' | 'perform' | 'maintain' ;
GoalClause    ::= 'succeed' 'when' Expr | 'drop' 'when' Expr | 'holds' Expr ;
PlanDecl      ::= 'plan' Ident 'for' Ident [ '(' [ IdentList ] ')' ] [ 'where' Expr ] Block ;

Block         ::= '{' [ Stmt { Sep Stmt } ] [ Sep ] '}' ;
Sep           ::= ';' | NEWLINE ;
Stmt          ::= 'let' Ident [ ':' Type ] ':=' Expr | Expr ;

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
Mul           ::= Unary { ( '*' | '/' | '%' ) Unary } ;
Unary         ::= ( '-' | '!' | '*' ) Unary | Postfix ;
Postfix       ::= Primary { '(' [ ArgList ] ')' | '[' Expr ']' | '.' Word } ;
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
                | ( 'believe' | 'forget' | 'revise' ) Term
                | 'spawn' Ident '(' [ ArgList ] ')' [ 'in' Postfix ]
                | 'adopt' GoalCall
                | 'achieve' GoalCall [ 'by' Postfix 'within' Postfix ] [ 'else' Expr ]
                | 'ask' Postfix 'within' Postfix [ 'else' Expr ]
                | 'await' ( Term | Ident 'as' Ident ) [ '[' Expr ']' ] 'within' Postfix
                  [ 'else' Expr ]
                | 'drop' Postfix | 'stop' Postfix | 'terminate'
                | 'install' Ident [ '(' [ ArgList ] ')' ] | 'uninstall' Ident
                | 'mount' Ident [ '(' [ ArgList ] ')' ] | 'unmount' Ident
                | 'return' [ Expr ] | 'yield' | 'consume' | 'fail' ;
GoalCall      ::= Ident '(' [ ArgList ] ')' ;
RecordLit     ::= Ident '{' [ Word ':' Expr { ',' Word ':' Expr } [ ',' ] ] '}' ;
ListLit       ::= '[' [ ArgList ] ']' ;
Term          ::= Ident '(' [ TermArg { ',' TermArg } ] ')' ;
TermArg       ::= '?' Ident | Expr ;

ParamList     ::= Ident ':' Type { ',' Ident ':' Type } [ ',' ] ;
ArgList       ::= Expr { ',' Expr } [ ',' ] ;
IdentList     ::= Ident { ',' Ident } ;
Type          ::= Ident [ '<' Type { ',' Type } '>' ] | '[' Type ']' | '(' ')' ;
```

Notes the grammar cannot carry:

- **Record literals and blocks.** `Ident '{'` in an expression is a record literal only when the
  identifier is `PascalCase` (§8.1); a `snake_case` identifier followed by `{` is never one. In a
  `where`, a contract or a goal clause a record literal must also be parenthesised, as Rust does in
  `if` headers.
- **Action operands are postfix expressions**, so `emit E{...} & next()` is
  `(emit E{...}) & next()`, and an `in`, `for`, `by`, `within`, `from` or `else` after an action
  belongs to it.
- **`else` binds to the nearest `if`, `achieve`, `ask` or `await`.** A `then` body that contains an
  `achieve`, `ask` or `await` without its own `else` must be braced when the `if` has an `else`; the
  checker reports the ambiguity otherwise.
- **Terms.** A term argument that begins with `?` is a logic variable; a scan inside a term needs
  parentheses. After `believe`, `forget`, `revise`, `believes`, `findall`, `tell`, `untell` and
  `await`, and in a trigger, `name(args)` is a term. Variables bound in an `if` condition, a
  `where`, a guard, an `await` or a `for believes` are visible in the corresponding body.
- **Static restrictions**: positions and pure conditions (§8.2, §8.3), suspension points (§6.6),
  read-only answers (§7.1), the `Bool` rule (§8.2), places as assignment targets, reserved implicit
  event fields (§5.5) and `$` names, plan parameters named as the goal's, terms typed by their
  predicates, `ask` typed by its question, `within` on every effect method, case convention.

### 8.10 Differences from SARL

| SARL                                         | vagent                                                      | Why                                                                                              |
| -------------------------------------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `on E [guard] { }`, `occurrence`             | same, plus `as name`                                        | kept; `as` names the event when handlers nest                                                    |
| `uses Cap` imports capacity functions        | same                                                        | kept                                                                                             |
| `setSkill(new Impl)`                         | `install Impl(args)`                                        | a keyword form like every other agent action                                                     |
| `requires Cap` in a skill                    | not offered; `requires` is a method precondition            | a skill's needed capacities are found by `uses` in its methods                                   |
| `emit(e)` with a `[scope]` lambda            | `emit e in s for r`                                         | a list of names is checkable; a scope lambda would run once per member in the emitter's turn     |
| `emit` also delivers to the emitter          | the emitter does not receive its own emission               | an agent's own reactions come from internal events; self-delivery would cost a turn per emission |
| `in(delay) [ ... ]`, `every(period) [ ... ]` | `Schedules.after`, `Schedules.periodic`, `Schedules.cancel` | vagent has no closures; a timer delivers an event instead                                        |
| `wake(e)`                                    | not offered                                                 | vagent's internal events are belief, goal and plan events, and a timer or `yield` defers         |
| `killMe`                                     | `terminate`                                                 | a keyword like the other lifecycle actions                                                       |
| event fields `var x : T`                     | `event E { x: T }`                                          | one field syntax for events and records                                                          |
| `Initialize.parameters`                      | typed agent parameters carried in the capability            | checkable at the spawn site; no race with other messages                                         |
| agent `extends`                              | not offered; behaviors compose                              | inheritance of handlers and plans would interact with plan lists                                 |
| no priority                                  | `priority n`, within a turn only                            | deterministic order of matching handlers                                                         |
| no BDI                                       | predicates, questions, beliefs, goals, plans, intentions    | the point of the language                                                                        |
| Janus thread pool, shared JVM heap           | one actor and one arena per agent                           | V1                                                                                               |

### 8.11 Relationship to the siblings

| Concern              | vicon                                     | vagent                                      | Divergence and reason                                                                              |
| -------------------- | ----------------------------------------- | ------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| assignment           | `:=`, `<-` (both by `Bind`)               | `:=`, `<-` (by `Value` on a place)          | `:=` restricted to bounded positions (§8.3); places, not cursor-scope bindings (§5.8)              |
| comparison           | `=`, `~=` numeric; `==`, `~==` lexical    | `==`, `!=`, typed                           | vagent is statically typed, so one `==` serves (V14)                                               |
| concatenation        | `\|\|`                                    | `++`                                        | `\|\|` would read as boolean or to programmers from typed languages                                |
| conditional          | `if then else`; fails without `else`      | total without `else`                        | a failing `if` would undo the writes before it in a handler (§8.2)                                 |
| loops                | `while do` (one choice point), `every do` | `while do` (one per iteration), `for in do` | a single loop choice point releases every iteration's writes at exit; `every` undoes its body (V7) |
| limitation           | `e \ n`                                   | not offered                                 | the counter would be restored by the trail on each resumption (§13)                                |
| generator procedures | `suspend` in procedures                   | none; `def` returns once                    | methods are called from handlers, where a live generator could not survive the turn                |
| string scanning      | `s ? e`, `tab`, `move`                    | same, once `std:goal` registers them        | `?` binds tighter than assignment                                                                  |
| module paths         | `std:goal`                                | `std:agent`, `mars:exploration`             | none; vlace's `"std:xanadu"` and VQL's `std::` should converge (U-A6)                              |
| locals               | assignment to a fresh name                | `let x := e`                                | plan frames must be known statically (§5.6)                                                        |
| logic variables      | Vlog terms                                | `?x` inside typed terms only                | only belief terms need them                                                                        |

______________________________________________________________________

## 9. Lowering

### 9.1 Cross-walk

The last column names what each construct needs beyond `link`, `value` and the existing opcodes.

| Construct                               | Lowering                                                                                                                                                      | Beyond link/value                   |
| --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| a statement                             | `d := #ChoiceDepth`; `#Choice alt=SFAIL`; the expression; `#Cut d` on its first result. SFAIL: in a plan, `#Jump` to the plan-failure path; elsewhere `#Fail` | `#ChoiceDepth` (P1)                 |
| `e1 & e2`, `e1 \| e2`, `to`             | vicon's lowering (vicon §4–§5)                                                                                                                                | none                                |
| `if c then a [else b]`                  | fresh `d := #ChoiceDepth`; `#Choice alt=ELSE`; `c`; `#Cut d`; `a`; `#Jump END`; ELSE: `b`, or `()`                                                            | `#ChoiceDepth`                      |
| `while c do b`                          | LOOP: fresh `d`; `#Choice alt=DONE`; `c`; `#Cut d`; `b` as a statement; `#Jump LOOP`; DONE: `()`                                                              | `#ChoiceDepth`                      |
| `for x in e do b`                       | bounded `e`; a counter or a walk along `+d.grab`/`+d.step` (§5.9); `b` as a statement per element                                                             | none                                |
| `for believes t do b`, `findall`        | copy the clause list; per clause: copy `t`, `#Choice`, `#Unify`, `#Cut` or `#Fail`                                                                            | none                                |
| local read, `let x := e`                | the activation frame's (or intention frame's) `d.vars` entry                                                                                                  | none                                |
| field read; `:=`, `<-` on a field       | walk the owner's `d.vars`; `#Value` on the value cell (trailed when older than the mark)                                                                      | none                                |
| method, handler, answer, condition call | `#Call` (pushes the frame), arguments on the frame's `d.vars`, spill live temporaries, `#Nop` after; `#Return` pops; reload                                   | none                                |
| capacity call                           | dispatch walk (§5.3), then a method call                                                                                                                      | none                                |
| effect call                             | outbox entry (capacity, method, copied arguments, deadline); `waiting` set; the step returns                                                                  | adapter: host binding               |
| agent creation                          | `makeCell(type name)`; `setStateRoot`; reserved fields; parameters from the capability                                                                        | `setStateRoot` (built)              |
| `install S(args)`                       | skill frame; one instance per capacity on `d.skill` and on the frame's `d.clone` rank; capacity-name cell on `+d.capacity`                                    | none                                |
| `on E [g] priority p { b }`             | handler cell on the event type's `d.clause` rank: `g` on `+d.grab`, `b` on `+d.spin`, priority on `d.vars` (§5.7)                                             | none                                |
| `believe`, `forget`, `revise`           | clause or source add; key replacement; source removal and retraction with rank repair; belief event on `d.pending`                                            | none                                |
| `believes t`                            | clause walk with `#Unify`, one `#Choice` per clause                                                                                                           | none                                |
| `emit`, `tell`, `untell`, `stop`        | outbox entry with copied fields                                                                                                                               | adapter: `send`                     |
| `spawn A(args)`                         | name from `$spawn_count`; outbox entry with parameters                                                                                                        | adapter: issuer, `spawn`, `monitor` |
| `adopt G(args)`                         | intention on `d.intention`, `ready` stamped                                                                                                                   | adapter: wake                       |
| `achieve G(args)` in a plan             | subgoal intention on the `d.subgoal` chain; `waiting` set; the step returns                                                                                   | none                                |
| `achieve … by a within d`, `ask`        | outbox entry; `waiting` set with deadline; the step returns                                                                                                   | adapter: `send`, timers             |
| `await`                                 | test the base (for a term); else `waiting` set with the pattern and deadline; the step returns                                                                | adapter: timers                     |
| `drop i`                                | look `i` up; if live: unlink, drop subgoals, outbox `cancel` entries for delegations and effects                                                              | adapter: host `cancel`              |
| `yield`                                 | `ready` stamped; the step returns                                                                                                                             | adapter: wake                       |
| `terminate`                             | `$terminating` set; the adapter runs `on Destroy` in a wake turn, then stops                                                                                  | runtime: P7                         |
| `requires`, `ensures`                   | the call site's `-d.contract`/`+d.contract` chains (Vortex §3)                                                                                                | none                                |
| turn end                                | `clearChoicePoints()`; `discard(m0)`; unlink the cursor's scope; drain; derive                                                                                | adapter                             |

### 9.2 A handler, lowered

`on TargetDetected [occurrence.distance < 50.0] { targets_seen := targets_seen + 1 }`:

```text
program: name cell "TargetDetected" ─(+d.clause)─► handler cell (d.vars: priority 0)
  guard (+d.grab): read occurrence.distance → Lt 50.0 → produce or Fail
  body  (+d.spin): ChoiceDepth → spill d0; Choice alt=SFAIL
                   walk agent d.vars to "targets_seen" → Add 1 → Value on its value cell
                   reload d0 → Cut d0 → Return
                   SFAIL: Fail

dispatcher (std:agent, Vortex), per handler in merged order:
  ChoiceDepth → h; Choice alt=NEXT  -- the handler's own choice point and mark
  Call guard; Nop; Call body; Nop
  Cut h                            -- success: drop the choice point, keep the writes
  Jump AFTER
NEXT: (reached by Fail)            -- failure: the mark was released, writes undone,
                                   -- and the frames unwound with the stack top
```

______________________________________________________________________

## 10. The `std:agent` package

`std:agent` is written in Vortex, anchored on `d.stdlib` like every package
([vortex-hyperstructural-runtime.md](vortex-hyperstructural-runtime.md) §7.1). It reuses `std:logic`
(unification, and the clause database once Vlog §6 is built), `std:goal` (scanning, once it
registers vicon §6's functions), `std:collections` (lists) and `std:string`.

| Part         | Contents                                                                                                                                                                                                                                                                                                                                                              |
| ------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| genesis      | finds or mints the thirteen dimensions of §5.1; creates the reserved fields                                                                                                                                                                                                                                                                                           |
| dispatcher   | handler merge (§5.7), the settle loop, `consume`, await matching and capture                                                                                                                                                                                                                                                                                          |
| deliberation | `adopt`, selection, `continue`, isolated reconsideration, maintain intentions, restorers and back-off, `fail_step`                                                                                                                                                                                                                                                    |
| beliefs      | set-with-sources assert, retract and keyed revise; clause walk; `findall`                                                                                                                                                                                                                                                                                             |
| relay        | the host's fan-out into hosted spaces                                                                                                                                                                                                                                                                                                                                 |
| capacities   | `Schedules` (`after(d: Duration, e: Event): Timer`, `periodic(d: Duration, e: Event): Timer`, `cancel(t: Timer)`); `Clock` (`now(): Time`); `Accepts` (`accepts(sender: Agent, goal_name: Str)`); `Logging` (`info(m: Str)`, `debug(m: Str)`)                                                                                                                         |
| functions    | `intentions(): [Intention]` (top-level only; a drop covers their subgoals), `is_maintain(i: Intention)` (succeeds or fails), `members(s: Space): [Agent]`, `children(): [Agent]`, `text(x): Str` (built into the checker for every type); `Map<K, V>` operations                                                                                                      |
| events       | `Initialize`, `Destroy`, `GoalAchieved{intention, goal}`, `GoalFailed{intention, goal, cause}`, `GoalDropped{intention, goal}`, `PlanFailed{intention, plan, cause}`, `EffectFailed{capacity, method, cause}`, `ConditionFault{intention, cause}`, `ChildDown{child}`, `ChildStopping{child}`, `SpawnFailed{child}`, `DeliveryFailed{to, kind, cause}`, `Moved{name}` |

A timer is an entry in `$timers`: id, due time, period, and a copied event. When it falls due, the
adapter asks for a wake turn and the event is dispatched to the agent itself. `Logging` writes
through the `vortex.agent` category, which the logging migration (`design/logging-migration.md`)
must register; it never logs event payloads.

Settings, read from `system://settings` by the launcher and the adapter:

| Setting                      | Default                | Bounds what                                    |
| ---------------------------- | ---------------------- | ---------------------------------------------- |
| `vagent.max_actors`          | 128 (`ActorOptions`)   | agents per runtime, once P7 reaps              |
| `vagent.max_mailbox`         | 128 (`ActorOptions`)   | queued messages per agent                      |
| `vagent.max_deliveries`      | 4096 (`ActorOptions`)  | outstanding sends per runtime                  |
| `vagent.value_operations`    | 65,536 (`ValueLimits`) | checkpoint size, once P5 lands                 |
| `vagent.belief_event_depth`  | 16                     | settle rounds per turn                         |
| `vagent.condition_fuel`      | 1,000 steps            | each condition, `where` and await guard        |
| `vagent.maintain_backoff`    | 30s                    | re-adoption after a failed or dropped restorer |
| `vagent.send_retries`        | 5                      | redeliveries before a delivery is abandoned    |
| `vagent.send_retry_interval` | 1s                     | spacing of redeliveries                        |
| `vagent.renew_fraction`      | 0.8                    | when lease renewal starts                      |
| `vagent.ready_per_message`   | 4                      | wake turns run between two message turns       |

______________________________________________________________________

## 11. What the C++ core has to gain

Nothing is in production, so changes to the Vortex core, the Vortex standard library and the mesh
runtime are allowed where vagent strictly needs them. Each is a **proposal**: it is listed in §11.2
with the reason it cannot be done in vagent's own code, what vagent does without it, and a status. A
proposal is incorporated into the design only after its own approval; until then the design depends
on it only as stated in its "Without it" column, and every section that relies on one cites its
number. New files under `apps/common/xanadu/vagent/` and `apps/vagent/` (§11.1) change no existing
code and are not proposals.

The test for "strictly needed" is the same for every row: the capability must be impossible, or
possible only by a workaround that breaks a property of §14, from three places in order of
preference. First Vortex code in `std:agent`. Then the adapter, which is C++ but new and vagent's
own. Only then existing code.

### 11.1 New code: a front end, a loader and a mesh executor adapter

```text
apps/common/xanadu/vagent/
  token.hpp, lexer.{hpp,cpp}      // §8.1
  ast.hpp, parser.{hpp,cpp}       // §8.9; precedence climbing over §8.4
  checker.{hpp,cpp}               // types, predicates, questions, purity, the restrictions of §8.9
  compiler.{hpp,cpp}              // §9: opcode graphs, activation frames, spills
  program.{hpp,cpp}               // the sealed program artifact: what programRef names
  agent_executor.{hpp,cpp}        // the MeshActorRuntime::Executor
  host_bindings.{hpp,cpp}         // native skills and percept drivers registered by name
apps/vagent/main.cpp              // `vagent run`, `vagent check`, a REPL driving one agent
```

`programRef` names a sealed artifact: the compiled opcode graph of the program's modules, hashed,
with an opcode allowlist (every opcode the compiler emits, `#ChoiceDepth` included; there is no
host-reaching opcode to exclude). `program.cpp` verifies it before a spawn or resume, on every node
that hosts an agent. In a single-owner society the code comes from the owner, so verification
protects against corruption, not against a hostile author.

The compiler returns `std::expected` with a named error, as `vprolog::Compiler` and vicon §8.2 do.
For scale: `apps/common/xanadu/vprolog/` is 1,632 lines and `apps/common/xanadu/vicon/` 3,176; the
front end should be nearer vicon's, and the adapter has no measured comparison.

The adapter holds a `MeshActorRuntime&`, the capability issuer and the host bindings, which Vortex
code cannot reach (§1). Its duties: the steps of §4.2, hard-failure classification and recovery; the
drain; delivery upkeep and floors (§7.3); host deliveries; the derived ready list and timer heap;
renewal and stopping (§4.5); and scheduling at most `vagent.ready_per_message` wake turns between
two message turns. It also installs the transport: a loopback `Sender` for same-node peers that
calls `receive` with the owner's identity, `trustNode` for the local node, and a fixed
`hashcashStamp` on spawns (the validator requires one and the runtime does not check it). The host
spawns the launcher with a source pid it mints for itself.

### 11.2 Proposed changes to existing code

All proposals are **pending approval**.

| No. | Change                                                                                                                                                                                                   | Where                              | Why it cannot live in `std:agent` or the adapter                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | Without it                                                                                                                                                                                         | Used by              |
| --- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------- |
| P1  | `#ChoiceDepth`: write `choiceStack_.size()` into the output cell. About ten lines in `OpcodeKind` and `VortexVM::step`, the opcode name tables in `vql/compiler.cpp` and `vpl/compiler.cpp`, and a test  | Vortex VM                          | A statement must cut to the depth at its own start (V13). That depth is dynamic: a method called from inside a speculative expression starts above choice points the compiler cannot count. Vortex code can read no VM state but its cells, and the adapter runs between turns, not between statements. The existing `#Cut` already takes a barrier input, so reading the depth is the only missing piece. vicon's compiler names "a new opcode or a new ChoicePoint field" as the fix for the same defect (`vicon/compiler.cpp:12-22`) | Static depths only: methods may not be called from speculative positions, so `f(x) & g(y)` with methods `f` and `g` is a compile-time error, and recursion through speculative calls is impossible | V13, §9.1            |
| P2  | Reap completed deliveries; `abandon(requestId)`; `redeliver(requestId)`; an `ActorSpawnAck` completes its spawn                                                                                          | `mesh_actor.cpp`                   | `deliveries_` is private to the runtime and has no `erase`, so a runtime can send 4096 messages in its lifetime; `send` and `retry` return early once a delivery is acknowledged; a spawn ack only sets `acknowledged`. The adapter sees none of these tables                                                                                                                                                                                                                                                                           | An agent society stops sending after 4096 messages per node in total, and an incomplete delivery blocks its sender's checkpoint for ever                                                           | §7.3                 |
| P3  | Forget remembered ids below the sender's floor                                                                                                                                                           | `mesh_actor.cpp`                   | `remembered` is private and has no `erase`; `enqueue` refuses at `maxRememberedMessages`. Only the runtime can trim it                                                                                                                                                                                                                                                                                                                                                                                                                  | Each agent can receive 4096 messages in its lifetime                                                                                                                                               | §7.3                 |
| P4  | Local wake: `wake(pid)` runs the executor with `ActorExecution::message` absent (it becomes `std::optional`, touching the existing mesh executor and its tests), applies the lease check, sends no reply | `mesh_actor.{hpp,cpp}`             | The executor runs only inside `pumpOne` for a mailbox message, and the adapter has no other way to get the actor's `WorkerContext`. A self-sent message is refused as `DuplicateMismatch` when its reply collides with the actor's own `remembered` entry, so the adapter cannot simulate a wake with a message                                                                                                                                                                                                                         | No intention runs until a mailbox message arrives; ready intentions, timers, effect results and percepts wait for unrelated traffic                                                                | §4.2, V6, §6.6, §7.5 |
| P5  | `ValueLimits` in `ActorOptions`                                                                                                                                                                          | `mesh_actor.cpp`, `mesh_value.hpp` | `resume` builds `ValueLimits` locally; nothing outside can set it                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | Live agent state is capped at 65,536 promotion operations for checkpoint, migration and reclamation                                                                                                | §4.4                 |
| P6  | `renew(pid, capability)`: replace a live actor's grant in place                                                                                                                                          | `mesh_actor.cpp`                   | `resume` refuses the same node and epoch and mints a new pid; the grant is private                                                                                                                                                                                                                                                                                                                                                                                                                                                      | Agents end at lease expiry; the issuer must grant very long leases                                                                                                                                 | §4.5                 |
| P7  | `stop(pid)`: mark stopped, notify monitors, and once deliveries drain erase the actor, its `spawns_` and `monitors_` entries and watcher-list memberships                                                | `mesh_actor.cpp`                   | The three tables are private with no `erase`, and the only ways to stop an actor are an executor error or lease expiry                                                                                                                                                                                                                                                                                                                                                                                                                  | `maxActors` and `maxMonitors` are lifetime counts: a node can ever spawn 128 agents                                                                                                                | §4.5, §5.4           |
| P8  | Fair pumping: resume `pumpOne`'s scan after the last actor served                                                                                                                                        | `mesh_actor.cpp`                   | The scan order is inside `pumpOne`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | Agents with low keys can starve the rest                                                                                                                                                           | §14                  |
| P9  | Register `std:agent` as a standard-library module                                                                                                                                                        | `vortex_stdlib.cpp`                | The package is Vortex code, but modules are registered by name in `VortexStdLib`                                                                                                                                                                                                                                                                                                                                                                                                                                                        | None: this is how every package is found                                                                                                                                                           | §10                  |
| —   | Capability-bearing senders (`receive` admits `ActorSend` only from the owner)                                                                                                                            | `mesh_actor.cpp`                   | not proposed                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | single-owner societies (U-A2)                                                                                                                                                                      | —                    |

P2, P3, P5, P7 and P8 remove ceilings and unfairness that would affect any long-lived actor, not
only vagent's; the mesh was built for bounded jobs. The prerequisites in the header (Vlog §6's
clause database, vicon §6's scanning functions) are changes their own documents propose and are
approved there, not here.

______________________________________________________________________

## 12. Worked examples

### 12.1 Scatter and gather in a holon

```vagent
module demo:search
import std:agent

predicate pending(query_id: Str)
predicate reported(query_id: Str, partition: Int)
predicate hit(query_id: Str, text: Str)

event SearchQuery { query_id: Str, pattern: Str }
event SubTask { query_id: Str, partition: Int, pattern: Str }
event PartitionResult { query_id: Str, partition: Int, hits: [Str] }
event SearchCompleted { query_id: Str, hits: [Str] }

agent Worker {
  on SubTask as t {
    emit PartitionResult{query_id: t.query_id, partition: t.partition,
                         hits: scan_partition(t.partition, t.pattern)} for t.source
  }
  def scan_partition(index: Int, pattern: Str): [Str] {
    ["alpha-" ++ text(index), "beta-" ++ text(index)]
  }
}

agent SearchCoordinator(partitions: Int) {
  var workers: [Agent] := []

  on Initialize {
    for _ in 1 to partitions do workers := workers ++ [spawn Worker() in inner]
  }

  on SearchQuery as q [q.space == outer] {
    believe pending(q.query_id)
    for i in 0 to partitions - 1 do
      emit SubTask{query_id: q.query_id, partition: i, pattern: q.pattern} in inner for workers[i]
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

Each sub-task goes to one worker, and each worker answers only the coordinator, so a query costs
`partitions` messages each way. The coordinator also receives one reply turn per `SubTask` it sends,
so its mailbox must hold `2 × partitions` (default 128, so up to 64 partitions), or the fan-out must
be windowed. The `if` has no `else`: until every partition has reported, its condition fails, the
`if` succeeds with `()`, and the beliefs written before it stand (§8.2).

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
  def send_payload(dest: Agent, data: Str) { emit Payload{data: data} for dest }
}
skill Sealed(key_id: Str) implements Transport {
  def send_payload(dest: Agent, data: Str) {
    emit SealedPayload{key_id: key_id, data: data} for dest
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

An `OutgoingReport` before the alert dispatches `send_payload` to `Loopback`. The alert's turn
uninstalls `Loopback` and installs `Sealed`, whose instance carries the capacity name `Transport`.
The next report reaches `Sealed`; `Reporter` is unchanged. `sealed == false` is accepted by the
`Bool` rule because its outermost operator is a comparison. `send_payload` is not an `effect`:
emitting is staged messaging, not a device action.

### 12.3 A rover with goals, plans, effects and a reversible choice

`mars:hardware` declares the geometry, the percepts, the capacities and their skills; the mission
module only uses them. `tab`, `match` and `upto` are vicon §6's scanning functions, a prerequisite.

```vagent
module mars:hardware

record Point { x: Float, y: Float }

event Position { at: Point }
event Battery { level: Float }

capacity Mobility {
  effect def follow(route: [Point]) within 10min   // fails if a leg loses traction
  effect def halt() within 5s
  effect def dock() within 30min
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

native skill Chassis implements Mobility       // its driver posts Position and Battery
native skill Spectrometer implements Science
skill TerrainPlanner(leg_cost: Float) implements Routes {
  def direct(from_point: Point, to_point: Point): [Point] { [to_point] }
  def ridge(from_point: Point, to_point: Point): [Point] {
    [Point{x: from_point.x, y: to_point.y}, to_point]
  }
  def cost(route: [Point]): Float {
    let total := 0.0
    for _ in route do total := total + leg_cost
    total
  }
}
```

```vagent
module mars:exploration
import std:agent
import std:goal (tab, match, upto)
import mars:hardware (Point, Position, Battery, Mobility, Science, Routes, Chassis, Spectrometer,
                      TerrainPlanner)

predicate position(at: Point)
predicate battery(level: Float)

event HazardDetected { kind: Str, severity: Int }
event MissionCommand { target_id: Str, location: Point }
event EmergencyHalted { kind: Str }
event ScienceLog { target_id: Str, mineral: Str }
event SampleSecured { target_id: Str, total: Int }
event SurveyFailed { target_id: Str, cause: Str }

goal Survey(target_id: Str, location: Point) perform
goal Reach(location: Point) achieve {
  succeed when believes position(location)
}
goal KeepCharged(floor: Float) maintain {
  holds believes battery(?level) & level > floor
}

agent MarsSurveyor(hazard_limit: Int, reserve_floor: Float, leg_cost: Float, settle: Duration) {
  uses Mobility, Science, Routes
  install Chassis()
  install Spectrometer()
  install TerrainPlanner(leg_cost)
  var budget: Float := 0.0
  var samples: Int := 0
  var charging: Opt<Intention> := none

  on Position as p { revise position(p.at) }
  on Battery as b {
    revise battery(b.level)
    if not charging then charging := adopt KeepCharged(reserve_floor)
  }

  on HazardDetected as h [h.severity > hazard_limit] {
    halt()
    for i in intentions() do if not is_maintain(i) then drop i
    emit EmergencyHalted{kind: h.kind}
  }

  on MissionCommand as m { adopt Survey(m.target_id, m.location) }

  plan survey for Survey {
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
  plan travel for Reach where believes position(?here) & believes battery(?level) {
    let route := (budget <- level - cost(direct(here, location))
                  & budget > reserve_floor & direct(here, location))
               | (budget <- level - cost(ridge(here, location))
                  & budget > reserve_floor & ridge(here, location))
    follow(route)
    await position(location) within settle
  }

  plan recharge for KeepCharged {
    dock()
    await battery(?level) [level > floor] within settle
  }
}
```

What happens:

1. The first `Battery` percept believes the level and adopts `KeepCharged`, a goal intention that
   watches `battery`. Adopting it only once a level is believed keeps the closed-world base from
   sending the rover to dock at start-up (§6.3).
1. `MissionCommand` adopts `Survey`; its first wake turn selects `survey`, whose
   `achieve Reach(location)` creates a subgoal and suspends `Survey`.
1. `Reach`'s wake turn selects `travel`, whose `where` binds `here` and `level`. The route statement
   tries the direct arm; if `budget > reserve_floor` fails, the trail restores `budget` and the
   ridge arm runs. The statement's cut keeps the chosen arm's `budget`.
1. `follow(route)` stages the effect and suspends `Reach`. While the chassis drives, the agent
   handles percepts and other messages in their own turns. `succeed when` is not tested meanwhile.
1. When the drive ends, `travel` awaits the final `Position` percept, which may arrive before or
   after `EffectDone`; `await` tests the base first, so either order works. `Reach` succeeds and
   `Survey` resumes, then runs `spectra` and `drill_core`, each a suspension.
1. If a leg loses traction, `follow` fails, hence `travel`; `Reach` has no other plan and fails;
   `Survey`'s `else` emits `SurveyFailed`, which stands, and fails `survey`. The rover is wherever
   it stopped and `budget` keeps the value written in the earlier turn.
1. If the battery falls below `reserve_floor`, `KeepCharged`'s `holds` fails at a turn end and a
   restorer runs `recharge`, which docks and then waits for a level above the floor, so a late
   percept does not count against it. If docking fails, re-adoption waits for
   `vagent.maintain_backoff` or the next `battery` change.
1. A `HazardDetected` is handled in its own turn, including during a drive: `halt()` is staged, and
   every top-level intention but the maintain goal is dropped. Dropping `Survey` drops `Reach` with
   it, and stages the chassis's `cancel` for `follow`; `Survey`'s `else` does not run, because
   `Survey` itself ended. The drive's later result is counted as late.

### 12.4 Delegation, questions and waiting for a told fact

```vagent
module demo:depot
import std:agent

predicate delivered(item: Str, count: Int)
question stock(item: Str): Int

event Order { item: Str, count: Int }
event Shortage { item: Str, cause: Str }

goal Handle(item: Str, count: Int) perform
goal Fetch(item: Str, count: Int) perform

agent Warehouse(units: Int) {
  answer stock(item: Str): Int { units }
}

agent Courier {
  plan fetch for Fetch {
    // A real courier would act here; it tells its delegator what it carried.
    tell parent delivered(item, count)
  }
}

agent Dispatcher(warehouse: Agent, deadline: Duration) {
  var courier: Opt<Agent> := none

  on Initialize { courier := spawn Courier() in inner }
  on Order as o { adopt Handle(o.item, o.count) }

  plan handle for Handle {
    let available := ask warehouse.stock(item) within deadline else 0
    if available < count then {
      emit Shortage{item: item, cause: "stock"}
      fail
    }
    achieve Fetch(item, count) by courier within deadline else {
      emit Shortage{item: item, cause: cause}
      fail
    }
    await delivered(item, count) within deadline
  }
}
```

`vagent run demo:depot:Warehouse(units)` starts the warehouse, and its name is passed to the
dispatcher's `warehouse` parameter (§8.8). `ask` suspends `handle` until the warehouse's turn
returns its answer, checked against `question stock`, or substitutes `0` at the deadline. The
comparison `available < count` fails when stock suffices, so the total `if` does nothing.
`achieve … by courier` sends the `achieve` performative; the courier's default `accepts` takes
delegation from its parent and adopts `Fetch` with the dispatcher as delegator. When `fetch` ends,
the courier stages the `tell` and then the `outcome`. Delivery is unordered, so the dispatcher may
learn of the outcome before the fact: the final `await` finds the fact already believed, or waits
for it. `courier` is `Opt<Agent>`, so if the spawn failed the `achieve` statement fails at once.

______________________________________________________________________

## 13. Refused alternatives

| Alternative                                                   | Refused because                                                                                                                                               |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Agents as cells in one shared arena                           | breaks share-nothing and the per-actor grant; one agent's fuel exhaustion would undo another's turn                                                           |
| Holon children inside the parent's arena                      | children would share the parent's failure domain and fuel, and a holon could not be distributed                                                               |
| Speculation across turns (shadow cells, per-intention arenas) | needs a mark live across turns, which `checkpointView` refuses; marks are LIFO truncation                                                                     |
| Persisting a generator's choice point at a suspension point   | a choice point is a C++ frame and an arena mark; neither is portable                                                                                          |
| Continuation-passing so suspension can sit anywhere           | the compiler would reify generator state as cells; possible later (U-A7), not needed by the examples                                                          |
| Synchronous effects inside a turn                             | a long device action would block every other message, and a turn that failed after it would roll back the arena while the world kept the change               |
| Free-standing `commit` (Prolog cut over side effects)         | it protects nothing when written before the fallible call, which is the order programmers write; staged effects and bounded statements leave only safe orders |
| Self-sent messages for wake-ups                               | a self-send's reply collides with the actor's own `remembered` entry and never completes                                                                      |
| The pid as an agent's identity                                | a pid is unknown until a spawn completes and changes on `resume`; names are known at once and survive migration (V16)                                         |
| Request ids unique only per sender                            | the runtime keys deliveries and remembered ids by the id alone, so two agents' ids would collide (§5.5)                                                       |
| A per-sender high-water mark for remembered ids               | delivery is unordered, so a late id below the mark would be dropped as a duplicate; a sender-declared floor loses nothing (§7.3)                              |
| `Initialize` as a first message, with gating                  | a held `ask` cannot be answered later and held messages are outside the checkpoint; parameters in the capability have no race                                 |
| Renewal by checkpoint and `resume` on the same node           | refused as `WrongEpoch`; in-place renewal is P6                                                                                                               |
| A priority mailbox                                            | needs a change to `enqueue` for an ordering handlers already have within a turn; reaction latency is U-A11                                                    |
| Capacity or plan identity by `CellRef` or ordinal             | refs into the program drag it into checkpoints (V12) and change on `resume`; ordinals shift when behaviors are mounted                                        |
| Sharing values by reference                                   | a second binding of a cell evicts the first; identities are names, values are copied (V11)                                                                    |
| vicon's `Bind` for assignment and calls                       | it prepends to the cursor's scope and is never unbound, so scope grows for the agent's life and nested calls see the wrong continuation                       |
| A call convention of `#Jump` beside `#Call`                   | two stack protocols on `+d.stack` would corrupt each other; `#Call`/`#Return` already push and pop frames and unwind on backtrack                             |
| Forbidding recursion                                          | spilling live temporaries to activation frames makes recursion safe at the cost of a copy per call                                                            |
| Statements without their own choice point                     | a failing statement would backtrack to the turn's base choice point and undo everything since the turn began                                                  |
| Icon's limitation `e \ n`                                     | its counter must survive backtracking into `e`, which the trail undoes; vicon records the same defect (`vicon/compiler.cpp:12-22`)                            |
| One choice point for a whole `while`                          | its release at loop exit would undo every iteration's writes (vicon's `emitWhile`)                                                                            |
| Per-event `promote` to a user's store                         | R8: agent working state is not hypertime                                                                                                                      |
| vagent as a VQL extension (`vql-agent-experiment.md`)         | VQL's deferred evaluation has no intention that outlives a query; agent turns would need a second runtime under the query engine                              |
| `every e1 do e2` for loops that act                           | the arena trail undoes the body's writes on every resumption (V7)                                                                                             |
| A failing `if` without `else`                                 | in a handler it undoes every write before it, silently                                                                                                        |
| Untyped belief terms and untyped `ask`                        | a statically typed language would lose its checks at the most-used data structure and at every question                                                       |
| `revise` by predicate only                                    | it would erase every self-sourced fact of a many-valued predicate; keys say which facts a new value replaces                                                  |
| Actions in conditions                                         | a mistyped `believe` for `believes` would assert a fact and succeed                                                                                           |
| Icon's `=`/`==` and `\|\|` spellings                          | vagent is statically typed, so one typed `==` serves; `\|\|` reads as boolean or                                                                              |
| `&&` and `\|\|` beside `&` and `\|`                           | two spellings of conjunction that differ in backtracking would be a trap                                                                                      |
| Positional event patterns                                     | they silently rebind when an event's fields are reordered                                                                                                     |
| Renaming `in`, `for` and `by` per construct                   | the grammar is unambiguous with them, and each reads as English where it appears; the cost is a dense line such as `for p in ps do emit E{} in s for p`       |

______________________________________________________________________

## 14. Properties, with their conditions

- **No data race between agents.** Each agent's arena is touched only by its own turns on the
  runtime's owner thread. Within an agent, turns are sequential.
- **No interleaving inside a plan segment.** Intentions interleave only at suspension points, so
  between two of them a plan sees beliefs and fields change only by its own hand.
- **An agent never blocks,** given host bindings that return at once (§5.3). No construct waits
  inside a turn; effects run outside turns (V6).
- **Every wait on something outside the agent is bounded** by a deadline (§6.6), given the timers of
  §11.1 and their reconstruction from cells after `resume`.
- **A turn is atomic.** Its arena writes stand together or are undone together, and its messages,
  spawns and effects happen only if it succeeds (V5).
- **No condition fault can fail a turn** (§6.4).
- **A turn is a function of its input and the agent's state.** Its input is the message or host
  delivery and the turn time in `$turn`; nothing else reads a clock or randomness, and the schedule
  of ready intentions is held in cells (§5.6).
- **Cursor scope is bounded by one turn's activations,** and each turn's string budget is relative
  to where it starts (§4.2).
- **Agents do not leak slots, and the runtime serves agents fairly,** given P7 and (h).
- **Reaction latency is not bounded by the language.** A handler runs when its message reaches the
  head of the mailbox, behind queued messages and up to `vagent.ready_per_message` wake turns
  between each.
- **Not claimed: lifetime replay.** The mesh logs neither inbound messages nor host deliveries, and
  agent state is not in a user's spool (V9). Recording inbound streams as a rank would allow it
  (U-A8).

______________________________________________________________________

## 15. Breakages and costs

- **Two turns per message sent.** Every `ActorSend` produces a reply turn for the sender.
- **Mailbox bound.** 128 messages per agent by default; fan-out must stay below half of it or be
  windowed (§12.1). `MailboxFull` is handled by redelivery (§7.3).
- **Relay doubles hops,** and a space's host is its bottleneck and single point of failure.
- **A choice point and a mark per statement** (V13).
- **Per-turn deliberation.** The settle loop re-tests every live intention's conditions and every
  maintain goal's `holds` each round, and, per event, every waiting `await`, each within its fuel
  slice. The cost is linear in live intentions and their conditions, times the rounds, and walks
  follow subgoal chains.
- **Appends walk ranks.** No rank keeps a tail pointer, so adopting, staging and raising cost
  O(length) each; list indexing is O(n) (§5.9).
- **A turn per effect and per suspension.** A plan of n effects takes at least n + 1 turns.
- **Calls cost a frame** and a spill of live temporaries.
- **Dispatch merges ranks:** O(k) per step for k sources and supertypes.
- **Checkpoint costs** (§4.4): an operation per live cell and link, the 65,536-operation bound until
  P5, and permascroll growth by the live strings at every checkpoint.
- **Garbage between checkpoints:** cells and scratch bytes are reclaimed only by checkpoint and
  resume (§4.4), which needs quiescence (U-A1).
- **A program copy per agent** (§4.4), until U-A14.
- **Copy on store** (V11): O(size) per stored list or map.
- **Belief queries are linear** until convergence U1. vagent is a fourth consumer of U1, after VPL,
  Vlog and vicon.
- **Restricted suspension** (§6.6): a plan that must wait inside a belief loop splits the work into
  a goal per element.
- **Child loss is slow and remote-only** (§5.4); local children are known only through orderly
  stops.
- **Migration** makes messages to the old pid fail until senders learn the new one (§4.5).
- **Single-owner societies** until U-A2.
- **Fuzzing.** The parser, the program loader, the event decoder, the outbox drain and host
  deliveries are new input surfaces and get `tests/fuzz/` targets.

______________________________________________________________________

## 16. Reconciliation with the convergence

- **R5 (`noCell == 0`).** The C++ adapter and compiler use `std::optional<CellRef>`, as
  `ArenaManifold::link` returns. A vagent failure is the absence of a result, and `Opt<T>` is the
  language's absence, not a reserved value.
- **R6 (scalars).** Fields, event fields, priorities, ids, readiness stamps and deadlines are scalar
  cells.
- **R7 (a `SetLink`'s subject is its chain).** vagent writes no Structure operations into a user's
  store. A checkpoint writes them into the published value's own transient store, through `promote`,
  which follows R7.
- **R8 (two regimes).** Agent state is arena state (V9). The checkpoint promotion targets a value's
  own store, which no user visits, so R8's "a visited store is not mutated" holds. Writing to a
  user's store is the explicit `StoreWriter` capacity, under `PromotionBudget`. Whether an agent
  acting for its owner counts as a user-generated update is U-A9.
- **R12 (a dimension is a cell).** The thirteen dimensions are found or minted as data (V10).
- **U1 (rank indexing).** A consumer, not a resolver (§15).

______________________________________________________________________

## 17. Comparison

| Property         | SARL / Janus                | Jason (AgentSpeak)            | GOAL                         | vagent                                                                                             |
| ---------------- | --------------------------- | ----------------------------- | ---------------------------- | -------------------------------------------------------------------------------------------------- |
| beliefs          | none built in               | Prolog-like belief base       | Prolog knowledge and beliefs | typed, optionally keyed predicates; a fact set with sources                                        |
| goals            | none built in               | achievement and test goals    | declarative achieve goals    | achieve, perform, maintain                                                                         |
| plan selection   | n/a                         | first applicable, overridable | rule order                   | first applicable untried; not overridable (U-A12)                                                  |
| commitment       | n/a                         | single-minded                 | blind until achieved         | single-minded, reconsidered every turn within the costs of §15                                     |
| actions          | synchronous method calls    | synchronous, with suspension  | synchronous                  | asynchronous effects that suspend the plan                                                         |
| failure handling | exceptions                  | `-!g` failure plans           | goal remains                 | no failure plans: a `PlanFailed` handler, then reselection in a later turn, and `else` at the call |
| in-plan search   | none                        | none                          | none                         | goal-directed generators, reversible assignment                                                    |
| organisation     | contexts, spaces, holons    | via JaCaMo                    | none                         | contexts, spaces, holons (single owner)                                                            |
| protocols        | by hand                     | by hand                       | by hand                      | by hand, with typed questions; a `protocol` declaration is U-A13                                   |
| distribution     | Janus network               | via JADE or JaCaMo            | none                         | Spool-Mesh actors                                                                                  |
| maturity         | production compiler and IDE | mature interpreter            | mature interpreter           | specification; no compiler exists                                                                  |

Where vagent is weaker: no implementation, two messages per send, single-owner spaces, a turn per
effect, slow and remote-only child-loss detection, checkpoint bounds, a program copy per agent, and
reclamation that depends on quiescence. Where it differs: in-plan search with automatic undo of
reversible writes, turns that happen entirely or not at all, typed beliefs and questions, and agent
state held as quotable cells.

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
- **U-A14. A shared program.** Loading the program once per node as a base `Manifold` under each
  agent's arena, with opcode output cells shadowed on write, instead of a copy per agent.

______________________________________________________________________

## Appendix: Versioning and Change History

**3.2 — Correct ids, statement choice points, captured awaits.** Request ids gained a sender hash,
because the runtime keys deliveries by id alone; remembered ids are forgotten below a floor the
sender declares, not a high-water mark, so unordered delivery loses nothing; acknowledged messages
are redelivered by a new `redeliver`. Each statement got its own choice point (V13), so a failing
statement undoes only itself. Parameters moved into the capability, removing `Initialize` gating.
Per-turn string budgets are relative to the turn's mark. Calls use `#Call`/`#Return`. The end of a
turn became a settle loop of pending events and reconsideration; awaits capture what they matched,
with isolated guards; `drop` of a dead id is a no-op and `intentions()` is top-level only;
`ConditionFault` is raised once. Added: keyed predicates and keyed `revise`, typed `question`s,
`await` guards on terms, a values layout (§5.9), the `ready` stamp, `d.source` and `d.extends` as
minted dimensions (thirteen), restorer success and back-off rules, transitive purity, `return` and
`fail` as expressions, case convention, reaping of `spawns_` and `monitors_`, and a `Point` record
in place of a nonexistent `std:math` type. vicon §6's scanning functions are listed as a
prerequisite.

**3.1 — Names, activations and isolated reconsideration.** Agents identified by stable names, with
pids as addresses. Renewal, stopping and fair pumping added to the owed runtime changes. Locals and
calls moved to activation frames with spilled temporaries; `while` given a choice point per
iteration; Icon's `\` dropped; plans recorded by name; handlers merged by priority. Reconsideration
isolated; effects in flight cancelled on drop; maintain goals given goal intentions. Added typed
predicates, `revise`, `Opt<T>`, pure conditions, a syntactic `Bool` rule, `await … as`,
`stop`/`terminate`, a program artifact, and a delegation example.

**3.0 — Staged effects, local wakes, data-only identity.** Effects became asynchronous suspension
points run by native skills outside the turn. Wake-ups moved from self-sent messages to a local
wake. Bounded statements cut to a depth read by one owed opcode. Capacities matched by name; values
copied; skills given frames; the checkpoint admitted to be a promotion with stated bounds. Beliefs
became a set with sources.

**2.0 — Turn-bounded speculation, one actor per agent, BDI made explicit.** Rewritten after a review
from five perspectives. Withdrew `ArenaSlice`, the `#Abort` and `#Trap` opcodes, the Ingestion Gate
and Grant Renewal protocols, shadow cells and their dimensions, topological GC, per-event `promote`,
lifetime replay, and free-standing `commit`.

**1.0 — The SARL metamodel on Vortex.** Initial draft.
