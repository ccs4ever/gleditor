# vagent: An Agent-Oriented Language over Vortex and the Spool-Mesh

| field       | value                                                                                                                                                                                                                       |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| version     | 2.0 (see [Appendix](#appendix-versioning-and-change-history))                                                                                                                                                               |
| status      | Specification only. Nothing here is built or wired into the gleditor build.                                                                                                                                                 |
| compiles to | Vortex opcode streams over an `ArenaManifold`, run one turn at a time by a Spool-Mesh actor executor                                                                                                                        |
| rests on    | Vortex ([runtime](vortex-hyperstructural-runtime.md)), Vlog ([logic](vlog-logic-extension.md)), vicon ([goals](vicon-goal-directed-language.md)), the Spool-Mesh actor runtime (`apps/common/xanadu/vortex/mesh_actor.hpp`) |
| owes C++    | a front end, a mesh executor adapter, and two retention changes in `mesh_actor.cpp` ([§11](#11-what-the-c-core-has-to-gain))                                                                                                |

`vagent` is a front end for agent-oriented programs. It takes SARL's metamodel (agents, events,
capacities, skills, behaviors, spaces, contexts, holons) and adds the parts of the BDI model SARL
leaves to libraries (beliefs, declarative goals, a plan library, intentions). It compiles onto the
machinery the repository already has. An agent is one actor of `MeshActorRuntime` with a private
`ArenaManifold`. It handles one message per turn, and evaluates with vicon's goal-directed
`#Choice`/`#Fail`/`#Cut` regime inside that turn.

The siblings are [Vortex](vortex-hyperstructural-runtime.md) (the runtime),
[VQL](vql-query-language.md), [VPL](vpl-array-language.md), [Vlog](vlog-logic-extension.md),
[vlace](vlace-functional-language.md) and [vicon](vicon-goal-directed-language.md).
[`vql-agent-experiment.md`](vql-agent-experiment.md) tried the same metamodel as a VQL extension;
[§13](#13-refused-alternatives) records why this document does not take that route.

The document separates what is built, what is reused unchanged and what is owed. Where an earlier
draft claimed a mechanism the tree does not have, the claim is withdrawn here rather than softened.
The change history lists those withdrawals.

______________________________________________________________________

## Contents

- [1. What is being claimed](#1-what-is-being-claimed)
- [2. Glossary](#2-glossary)
- [3. Rulings](#3-rulings)
- [4. The agent as one actor](#4-the-agent-as-one-actor)
- [5. Structure in the arena](#5-structure-in-the-arena)
- [6. Deliberation](#6-deliberation)
- [7. Communication](#7-communication)
- [8. The surface language](#8-the-surface-language)
- [9. Lowering](#9-lowering)
- [10. The `std:agent` package](#10-the-stdagent-package)
- [11. What the C++ core has to gain](#11-what-the-c-core-has-to-gain)
- [12. Worked examples](#12-worked-examples)
- [13. Refused alternatives](#13-refused-alternatives)
- [14. Properties, with their conditions](#14-properties-with-their-conditions)
- [15. Breakages and costs](#15-breakages-and-costs)
- [16. Reconciliation with the convergence](#16-reconciliation-with-the-convergence)
- [17. Open questions](#17-open-questions)
- [18. Comparison](#18-comparison)
- [Appendix: Versioning and Change History](#appendix-versioning-and-change-history)

______________________________________________________________________

## 1. What is being claimed

The table states each concept, the mechanism it uses, and whether that mechanism exists. "Built"
means present in the tree at the cited place. "Owed" means this document asks for it, and
[§11](#11-what-the-c-core-has-to-gain) prices it.

| Concept                       | Mechanism                                                                                   | State                                               |
| ----------------------------- | ------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| Agent identity                | One `MeshActorRuntime` actor; `ActorPid` (`identity_layout.hpp:304`)                        | built                                               |
| Agent state                   | Cells under one agent cell in the actor's `ArenaManifold` (`mesh_actor.cpp:155-160`)        | built (arena); layout owed by the compiler          |
| One event per turn            | `MeshActorRuntime::pumpOne` runs the executor on one mailbox message (`mesh_actor.cpp:458`) | built                                               |
| Turn executor                 | A `MeshActorRuntime::Executor` that decodes, dispatches, drains the outbox                  | **owed** (adapter, §11.2)                           |
| Speculation inside a turn     | vicon: `#Choice`/`#Fail`/`#Cut` over `mark`/`release`/`discard` (`vortex_vm.cpp:944-985`)   | built                                               |
| Reversible assignment `<-`    | An ordinary write trailed by the enclosing `Mark` (vicon §4.3)                              | built                                               |
| Beliefs                       | Vlog clauses on `d.clause` off predicate cells; query by `#Unify` (Vlog §6)                 | built (Vlog); `std:agent` wrappers owed in Vortex   |
| Goals, plans, intentions      | Intention cells holding a goal, a plan ordinal and a step path; advanced turn by turn       | owed in Vortex (`std:agent`), no new opcode         |
| Capacity dispatch             | A walk of the agent's `d.skill` rank comparing clone masters                                | owed in Vortex                                      |
| Emission                      | Envelope cells staged on `d.outbox`; the adapter calls `MeshActorRuntime::send` at turn end | **owed** (adapter)                                  |
| Spaces                        | A host agent holds a member rank and relays emissions                                       | owed in Vortex; single-owner only until §11.3 (c)   |
| Spawning a child              | `MeshActorRuntime::spawn` (`mesh_actor.hpp:131`) called by the adapter                      | built (runtime); **owed** (bridge)                  |
| Child death                   | `MeshActorRuntime::monitor` and `ActorDown` (`mesh_actor.cpp:425-448`)                      | built                                               |
| Timers and deadlines          | A clock in the adapter that enqueues self-messages                                          | **owed** (adapter)                                  |
| Checkpoint and migration      | `checkpointView` at a quiescent boundary (`mesh_actor.cpp:669-696`), then `resume`          | built                                               |
| Long-lived agents             | Reaping completed deliveries and windowing remembered request ids                           | **owed** (`mesh_actor.cpp`; §11.3)                  |
| Swarm-wide spaces over BEP 46 | `mesh_swarm` topics bound to a space                                                        | not proposed here ([§17](#17-open-questions), U-A4) |

What the table does not contain is as important. vagent asks for **no new opcode, no change to
`ArenaManifold`, no change to `CompactOpNode`, and no new `SystemDimensions` field**. It does ask
for C++, and §11 lists all of it. The C++ is owed at the boundary between the VM and the mesh
runtime, which Vortex code cannot cross today. `OpcodeKind` has no send, spawn or host-call opcode
(`vortex_vm.hpp:24-96`), and the `WorkerContext` an executor receives has no handle to the runtime
(`mesh_actor.hpp:117-118`). AGENTS.md admits C++ for driver and OS-event interfacing, and this
boundary is that kind of interface.

______________________________________________________________________

## 2. Glossary

One name per concept, used everywhere below.

| Term             | Meaning                                                                                                                                                                         |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| agent            | One actor of `MeshActorRuntime` running one compiled `agent` declaration                                                                                                        |
| agent cell       | The single cell in the agent's arena that heads every rank of agent state; it is the actor's `stateRoot`                                                                        |
| handle           | A cell whose content is an encoded `ActorPid`; how an agent names another agent. Handles are values: two handles naming one agent are two cells                                 |
| turn             | One execution of the executor on one mailbox message                                                                                                                            |
| event            | A typed record delivered as the payload of one `ActorSend` and decoded into an event cell for the turn                                                                          |
| envelope         | The `ActorMessage` carrying an event; never visible to vagent code                                                                                                              |
| space            | A named set of member handles held by one host agent, which relays every emission into it                                                                                       |
| context          | SARL's grouping of spaces. In vagent each agent has exactly two contexts it can name: `inner` (the space it hosts for its children) and `outer` (the space it was spawned into) |
| holon            | An agent with children; its children are the members of its `inner` space                                                                                                       |
| capacity         | A named set of method signatures                                                                                                                                                |
| skill            | An implementation of one or more capacities, installed on an agent                                                                                                              |
| behavior         | A reusable bundle of handlers, fields and methods mounted on an agent                                                                                                           |
| belief           | A ground Vlog fact in the agent's belief base                                                                                                                                   |
| goal             | A declared, parameterised desire with a success test (achieve), a holding condition (maintain) or neither (perform)                                                             |
| plan             | A procedure declared `for` a goal, with an applicability condition                                                                                                              |
| intention        | A goal instance being pursued: a cell holding the goal, the plan chosen, the step path reached and the plan-local frame                                                         |
| suspension point | A statement at which an intention may end a turn and continue in a later one: `achieve` of a subgoal, `ask`, `yield`                                                            |

The words "ring", "mailbox ring" and "headcell" from version 1.0 are retired. Every structure below
is an open **rank** with a head, as `store-slice-convergence.md` and the Vortex document use the
word, and the mailbox is the C++ deque inside the runtime, not a structure in the arena.

______________________________________________________________________

## 3. Rulings

Each ruling states what it decides, why, and what it costs. Alternatives are in
[§13](#13-refused-alternatives).

**V1. An agent is one actor; there are no links between arenas.** Each agent has its own
`ArenaManifold` because `MeshActorRuntime::spawn` gives each actor its own `WorkerContext`
(`mesh_actor.cpp:156`). An agent names another only through a handle, and changes another only by
sending it a message. There is no population rank; the node's runtime keeps the actor table.
*Price:* a holon cannot walk its children's state, and every holon interaction is a message. The
default `ActorOptions::maxActors` of 128 (`mesh_actor.hpp:58`) bounds agents per runtime. That value
is a default, so vagent reads it from `system://settings` and it is not a ceiling of the language.

**V2. Speculation is bounded by the turn.** Every `Mark` opened in a turn is discarded or released
before the turn ends. `checkpointView` refuses a non-zero `choiceDepth()` or `outstandingMarks()`
(`mesh_actor.cpp:673-675`), so this is the only regime under which an agent can be checkpointed.
`<-` is therefore vicon's trailed write and nothing more: the 1.0 `d.trail`, `d.shadow`, `d.choice`
dimensions and the `ArenaSlice` type are withdrawn. *Price:* a reversible assignment is reversible
only within the statement that makes it (vicon §4.4: a statement is bounded). Alternatives that
remain after a turn ends are re-derived, not resumed (V4).

**V3. One cursor per agent, one intention step per turn.** The executor runs a turn on one VM
cursor. `VortexVM::backtrack` pops choice points belonging to other cursors without releasing them
(`vortex_vm.cpp:198-215`), so two interleaved speculative cursors in one VM would corrupt each
other. vagent never has two. *Price:* intentions do not run in parallel inside an agent; they take
turns (§6.5). Parallelism is between agents.

**V4. Intentions persist as data, never as VM state.** An intention that continues across turns is
stored as cells: its goal, the ordinal of its chosen plan, the ordinals of plans already tried, its
step path, and its plan-local variables on `d.vars`/`d.values`. On resume, the executor re-enters
the plan's code at the step path. This matches the comment on `ActorContinuation`: VM program
counters stay local and application state lives in the published root (`mesh_actor.hpp:92-93`).
*Price:* suspension points are restricted to places where the step path and the frame describe the
whole state: statements of a plan body and the bodies of `if`, `while` and `for` over a list or a
range, never inside an `&`/`|` expression or a generator operand ([§6.6](#66-suspension-points)).

**V5. All emissions leave at the end of the turn.** `emit`, `tell`, `untell` and delegation write
envelope cells onto `d.outbox`. Those cells are minted under whatever `Mark` is live, so a backtrack
removes them with no extra mechanism. The adapter sends what remains when the turn finishes. A turn
that ends in a hard failure sends nothing. *Price:* no message leaves mid-turn, so a turn cannot
consult another agent except at a suspension point (`ask`), which ends the turn.

**V6. Irreversible effects are statements.** A capacity method declared `effect` (hardware, external
I/O) may be called only in a bounded position ([§8.3](#83-assignment-and-positions)): as a
statement, or as a `then`, `else` or `do` body in a bounded position. The compiler rejects an
`effect` call inside `&`, `|`, `\` or a generator operand. *Price:* an effect cannot be one arm of a
search. That is the intent. After an effect has happened, plan failure is handled forward (§6.7),
never by backtracking past it. This replaces 1.0's `commit;`, which round 1 showed being written on
the wrong side of the failure point in every example.

**V6a. Iteration that acts is deterministic.** The arena trails every write to a cell older than the
live mark, and vicon's compiler records that even `:=` cannot skip the trail
(`vicon/compiler.cpp:358-361`). Icon's `every e1 do e2` drives `e1` by failing back into it, so
every arena write `e2` makes is undone at the next resumption, and all of them are gone when `e1`
runs dry. vagent therefore has no `every`. It iterates with `for x in e do body`, where `e` is a
list, a range `a to b [by c]`, or `believes t`, walked without a choice point (§8.2). *Price:* a
generator cannot drive a loop that acts. A search that collects its results walks a rank instead, as
`findall` does (§6.1).

**V7. Beliefs are Vlog facts.** The belief base is a rank of predicate cells on `d.belief` off the
agent cell, each heading its clauses on `d.clause` exactly as Vlog §6.1 lays them out. `believe` is
`assertz` and `forget` is `retract`. A query is `#Unify` against the clauses. *Price:* belief lookup
is a linear clause walk until convergence U1 (rank indexing) lands, as Vlog §6.3 already records.

**V8. Agent working state is arena state, not hypertime.** R8 says an arena dies with its owner and
mints no operations. An agent's beliefs, intentions and fields are arena cells published as the
checkpoint root. vagent never calls `promote` implicitly. An agent that must write to a `Store` uses
an explicit `StoreWriter` capacity whose skill calls `zigzag::promote` (`arena_manifold.hpp:835`)
and turns a refused promotion into a failed call. *Price:* "replay the agent's life from the spool"
is not on offer (§14).

**V9. Ten dimensions, minted as data.** `std:agent` mints `d.behavior`, `d.skill`, `d.capacity`,
`d.space`, `d.member`, `d.holon`, `d.intention`, `d.subgoal`, `d.outbox` and `d.belief` on the
`d.dims` rank when the package loads, as R12 requires and as `VortexCore::mintNamedDimension`
already does (`vortex_core.cpp:20-26`). C++ learns none of their names. Everything else reuses an
existing dimension with its existing meaning (§5.1). *Price:* ten cells at package load, and a name
lookup per dimension per load.

**V10. Many-to-one references go through clone views.** A link is reciprocal, so a cell can have
only one negward neighbour along a dimension. Wherever several cells must refer to one (skills to a
capacity), each referrer links to its own view cell appended posward to the target's `d.clone` rank,
and readers compare `cloneMaster` (`arena_manifold.hpp:523`). *Price:* one extra cell per reference.

**V11. The language is goal-directed, as vicon is.** Expressions succeed with a value or fail; there
is no boolean test in control flow. The expression grammar follows Icon's precedence for `&`, `|`,
`to`, assignment and comparison, so a vicon reader and a vagent reader parse the same text the same
way. *Price:* programmers from SARL or Kotlin must learn that `a < b` produces `b` or fails, and
that `&` is conjunction of generators, not bitwise or logical and.

**V12. SARL's surface where SARL has one.** Handlers are `on Event [guard] { }`, the event is
`occurrence`, capacities are imported with `uses`, emission is scoped. Where vagent differs (skills
installed by `install`, BDI constructs SARL lacks), [§8.10](#810-differences-from-sarl) says so.

______________________________________________________________________

## 4. The agent as one actor

### 4.1 One agent, one actor, one arena

Spawning an agent spawns one actor: `MeshActorRuntime::spawn` checks the grant, creates a
`WorkerContext` with its own `ArenaManifold` and `VortexVM`, and mints a cursor
(`mesh_actor.cpp:150-175`). The executor's first turn builds the agent cell and its ranks. From then
on every turn is one message.

Two facts about the runtime shape what follows:

- **Only the actor's owner may send to it.** `receive` refuses an `ActorSend` whose authenticated
  identity is not the actor's owner (`mesh_actor.cpp:402-405`). Agents spawned under one identity
  can therefore talk to each other, and agents of different identities cannot. vagent v2.0 specifies
  single-owner societies; cross-owner spaces need §11.3 (c).
- **A failed executor stops the actor.** `pumpOne` sets `stopped` when the executor returns an error
  (`mesh_actor.cpp:501`). The adapter therefore reports success to the runtime for every turn whose
  failure is a language-level outcome (a failed handler, a failed intention, a contract violation),
  and returns an error only for its own faults (an undecodable message). A vagent agent is not
  killed by a soft failure.

### 4.2 The turn

A turn processes one message. The executor:

1. Re-arms fuel: `setExecutionLimits(cellCount() + maxCells, maxInstructions, maxOutputBytes)`
   (`arena_manifold.hpp:400`). The runtime installs these limits once, at spawn
   (`mesh_actor.cpp:165`), which makes them lifetime totals; the adapter makes them per turn.
1. Takes `m0 = arena.mark()`.
1. Decodes the message into an event cell (§5.5), or recognises it as a reply, a resume or a down
   notice (§6.5).
1. Runs the deliberation step for that message (§6.5).
1. Checks maintain goals (§6.3).
1. On success, `discard(m0)`: the turn's writes stand. On a hard failure, `release(m0)`: the arena
   is exactly as it was before the turn, and the outbox written during the turn is gone with it.
1. Hands every envelope left on `d.outbox` to `MeshActorRuntime::send`, unlinks them, and returns.

At step 7 no mark is outstanding and no choice point is live. If the mailbox is also empty and no
sent message is still awaiting its delivery, the agent is quiescent and `checkpointView` accepts it.

### 4.3 What persists

| Structure                          | Where                                     | Survives the turn | In the checkpoint root |
| ---------------------------------- | ----------------------------------------- | ----------------- | ---------------------- |
| Fields (`var`)                     | `d.vars`/`d.values` off the agent cell    | yes               | yes                    |
| Beliefs                            | `d.belief` predicates, `d.clause` clauses | yes               | yes                    |
| Installed skills and their fields  | `d.skill` rank                            | yes               | yes                    |
| Mounted behaviors and their fields | `d.behavior` rank                         | yes               | yes                    |
| Intentions and their frames        | `d.intention` rank, `d.subgoal` descent   | yes               | yes                    |
| Hosted spaces and members          | `d.space`, `d.member`                     | yes               | yes                    |
| Children                           | `d.holon` rank of handles                 | yes               | yes                    |
| The turn's event cell              | unlinked at turn end                      | no                | no                     |
| Outbox envelopes                   | `d.outbox`, drained at turn end           | no                | no (always empty)      |
| Choice points, marks               | VM and arena                              | no                | no (V2)                |
| Compiled program                   | `programRef`                              | n/a               | named, not copied      |

### 4.4 Fuel, cells, and reclamation

An arena frees cells only by `release` truncation (`arena_manifold.hpp:638-646`). `compact()` drops
dead link entries and frees no cell (`arena_manifold.hpp:656-672`), and no reachability sweep is
built. So a long-lived agent accumulates unreachable cells: consumed event cells, finished
intentions, retracted clauses, uninstalled skills.

vagent reclaims them by **checkpoint and resume**: publish the checkpoint, then `resume` it on a
fresh arena, which rebuilds only what is reachable from the root. This needs the agent to be
quiescent, which a busy agent may rarely be. The cost and the gap are recorded in §15 and U-A1. The
1.0 claim of an O(1) "topological GC" is withdrawn: unlinking a subtree makes it unreachable, not
reclaimed.

______________________________________________________________________

## 5. Structure in the arena

### 5.1 Dimensions

Minted by `std:agent` at load (V9):

| Dimension     | From → to                                     | Shape                 | Why not an existing dimension                                                    |
| ------------- | --------------------------------------------- | --------------------- | -------------------------------------------------------------------------------- |
| `d.behavior`  | agent cell → behavior instances               | rank                  | `d.vars` is the field table; behaviors are not named fields                      |
| `d.skill`     | agent cell → skill instances                  | rank                  | as above; dispatch walks this rank and nothing else                              |
| `d.capacity`  | skill instance → its clone view of a capacity | one link per instance | a skill's capacity is a reference, not a member of any rank                      |
| `d.space`     | agent cell → spaces it hosts                  | rank                  | no existing rank lists hosted spaces                                             |
| `d.member`    | space cell → member handles                   | rank                  | `d.holon` is children only; a space can hold non-children                        |
| `d.holon`     | agent cell → handles of its children          | rank                  | kept apart from `d.member` so monitoring and delegation never scan a space       |
| `d.intention` | agent cell → intention cells                  | rank                  | `d.cursors` holds VM processes with program counters; an intention has none (V4) |
| `d.subgoal`   | intention → the subgoal intention it awaits   | one link              | an intention awaits at most one subgoal at a time (§6.6)                         |
| `d.outbox`    | agent cell → staged envelopes                 | rank                  | must be distinct so the drain cannot touch anything else                         |
| `d.belief`    | agent cell → predicate cells                  | rank                  | Vlog has `d.clause` for clauses but no directory of an agent's predicates        |

Reused, with their existing meaning: `d.vars`/`d.values` (fields, frames, event fields, symbol
tables, as `VortexCore::exportSymbol` uses them, `vortex_core.cpp:117-125`), `d.clause` (clauses),
`d.clone` (views), `d.source` (a belief's provenance, the meaning `result_slice.cpp:34` gives it),
`d.grab`/`d.step`/`d.spin`/`d.stack`/`d.contract` (calls, unchanged), and `d.scan` (vicon's scanning
cursor).

Every attachment uses `ArenaManifold::insertIntoRank` (`arena_manifold.hpp:617`) or
`zigzag::rankTail` followed by `link` (the pattern at `vortex_core.cpp:113`). Never a bare `link`,
which evicts whatever either end held (`arena_manifold.hpp:582-592`). Removing a cell from a rank
links its predecessor to its successor, which leaves the removed cell with no link on that
dimension.

### 5.2 The agent cell

```text
[agent cell: "PatrolDrone"]
   ├─(+d.vars)──► [targets_seen] ─(+d.values)─► [scalar 3]
   ├─(+d.skill)─► [ThrusterNav] ─(+d.skill)─► [Comms]
   │                  └─(+d.capacity)─► [view] ─(-d.clone … master)─ [capacity Navigation]
   ├─(+d.behavior)─► [ObstacleAvoidance] ─(+d.vars)─► …
   ├─(+d.belief)─► [pred at/2] ─(+d.clause)─► [at(3,4)] ─(+d.clause)─► …
   ├─(+d.intention)─► [intention Survey] ─(+d.intention)─► [intention Recharge]
   │                        └─(+d.subgoal)─► [intention Reach]
   ├─(+d.space)─► [space inner] ─(+d.member)─► [handle] ─(+d.member)─► [handle]
   ├─(+d.holon)─► [handle child1] ─(+d.holon)─► [handle child2]
   └─(+d.outbox)─► [envelope] ─(+d.outbox)─► [envelope]
```

Every arrow is the only link its source has in that direction on that dimension, and every target
has only that one negward link. The capacity master is the one cell referred to by many skills, and
it is reached only through views (V10).

### 5.3 Capacities and skills

A `capacity` declaration compiles to a capacity cell exported as a symbol of its module on the
module's `d.vars` rank. That is the symbol table `VortexCore::exportSymbol` already builds. It is
not a `d.capacity` chain, which 1.0 drew and which would have blocked the skill pointer.

Installing a skill appends a skill instance cell to the agent's `d.skill` rank. For each capacity
the skill implements, it appends a view cell to that capacity's `d.clone` rank and links the
instance to it along `+d.capacity`. A skill implementing several capacities gets one instance cell
per capacity, which keeps `d.capacity` at one link per cell. The instances share the skill's field
frame through `d.clone` in the same way.

A capacity call walks `d.skill` from the agent cell and takes the first instance whose
`cloneMaster(link(instance, +d.capacity))` is the wanted capacity. The cost is O(installed skills),
typically under ten. Re-installing a skill for the same capacity removes the old instance and
appends the new one, so there is at most one instance per capacity. The call itself is the dual-wing
convention unchanged (§9.2).

### 5.4 Spaces, contexts and holons

A space is a cell on its host's `d.space` rank. Its members are handle cells on `d.member`. Every
agent hosts exactly one space at creation, its `inner` space. Its children join it when spawned.

`outer` is a space reference: the parent's handle and the space name. A top-level agent's `outer` is
the node's default space, hosted by the launcher agent (§8.8). An emission into a space the agent
does not host is one `ActorSend` to the host with a relay marker. The host's `std:agent` relay
handler sends one `ActorSend` per member, excluding the emitter. An emission into a hosted space
fans out directly.

A holon's children are separate actors. The holon holds their handles on `d.holon` and is monitoring
each one through `MeshActorRuntime::monitor`, so `ActorDown` reaches it as a `ChildDown` event. A
child knows its parent only through the spawn request's source, exposed as `parent`. No dimension
links a child to its parent, because they are in different arenas (V1).

### 5.5 Events

An event is decoded into one event cell whose content is the event type's name. Its fields hang on
`d.vars`/`d.values` exactly as a scope does, with scalars as R6 scalar cells. Two implicit fields
are always present: `source` (the emitter's handle) and `space` (the space it arrived through, or
none for a direct `tell`). The cell is quotable and queryable while the turn runs and is unlinked at
turn end.

On the wire the event is the `value` of an `ActorSend` (`identity_layout.hpp:315-327`): a bencoded
dictionary of kind, type name, fields and correlation id. The frame limit is 64 KiB
([vortex-distribution.md](vortex-distribution.md), wire table). A larger payload must travel as a
torrent named by `infoHash`, which the adapter does not yet do (U-A3). The `requestId` is
`sha256(emitter pid ‖ turn sequence ‖ emission ordinal)`, so a re-executed turn produces the same
ids and the runtime's de-duplication (`DuplicateMismatch`, `mesh_actor.cpp:335-337`) treats a resend
as a resend. Event identity is this id, not the content hash.

### 5.6 Intentions

An intention cell holds, on `d.vars`/`d.values`: `goal` (the goal's name and argument cells), `plan`
(the chosen plan's ordinal in the goal's plan list), `tried` (the ordinals already failed, a
`std:collections` list), `step` (the step path, §6.6), `waiting` (what it awaits: a subgoal, a
correlation id, or a timer) and the plan-local variables. A subgoal it awaits is linked along
`+d.subgoal`. Nothing in an intention is a `CellRef` into the program; ordinals are re-resolved
against `programRef` on resume, so a checkpoint moved to another node resumes correctly.

______________________________________________________________________

## 6. Deliberation

### 6.1 Beliefs

The belief base is the agent's Vlog database (V7). Statements and expressions:

- `believe at(rover, 3, 4)` asserts the fact (Vlog `assertz`) and queues a `+at/3` internal event.
- `forget at(rover, ?x, ?y)` retracts every matching fact (Vlog `retract`) and queues one `-at/3`
  per retraction.
- `believes at(rover, ?x, ?y)` is a generator: it produces once per matching fact, binding `x` and
  `y` for the rest of the expression. `?x` names a fresh logic variable; a plain identifier is a
  value.
- `findall(?x, at(rover, ?x, _))` is the list of bindings, from `std:agent` (§10).
- `for believes at(rover, ?x, ?y) do body` runs `body` once per matching fact.

`findall` and `for believes` cannot use backtracking to visit the clauses, because a result gathered
inside a branch is truncated when the branch is resumed (V6a). They walk the predicate's `d.clause`
rank directly. For each clause they copy the query term with fresh variables, take a mark, and
unify. On failure they release the mark. On success they discard it, keeping that copy's bindings,
and run the body or append the binding. Each attempt has its own variables, so a kept binding cannot
disturb the next attempt. The cost is one term copy per clause.

A fact received by `tell` carries its sender's handle on `+d.source`, the provenance meaning that
dimension already has. `believes` ignores provenance; `believes_from(sender, at(...))` filters on
it.

Revision is last-writer-wins per fact, by explicit `forget` then `believe`. The belief base is
closed-world: a fact not present is not believed. Retracted clauses stay in the arena until
reclamation (§4.4). Because beliefs are arena state (V8), the spool keeps no history of them.

Internal belief events are handled in the same turn, after the handler that caused them, in the
order they were queued. A handler for `+at` that itself believes or forgets queues further events.
The executor stops after `belief_event_depth` rounds (a setting) and reports a hard failure. That
bound stops a handler loop from consuming the turn's fuel silently.

### 6.2 Goals and plans

A goal is declared, not programmed:

```vagent
goal Reach(location: Point) achieve {
  succeed when position() == location
  drop when not believes mission_active()
}
goal KeepCharged() maintain {
  holds battery() > reserve_floor()
}
goal Survey(target_id: Str, location: Point) perform
```

| Kind       | Succeeds when                                         | Fails when                                                 | Re-adopted                                  |
| ---------- | ----------------------------------------------------- | ---------------------------------------------------------- | ------------------------------------------- |
| `achieve`  | its `succeed when` condition holds at a step boundary | no applicable untried plan remains                         | no                                          |
| `perform`  | a plan body completes                                 | no applicable untried plan remains                         | no                                          |
| `maintain` | never ends while the agent lives                      | an adopted restoring intention fails (reported, not fatal) | whenever `holds` fails at the end of a turn |

`drop when` applies to every kind: when it holds at a step boundary, the intention ends without
success or failure, and a `GoalDropped` internal event is queued. A plan for an `achieve` goal whose
body completes while `succeed when` does not hold counts as a failed plan, as in Jadex and GOAL.

A plan is a procedure bound to a goal by name, with an applicability condition:

```vagent
plan direct for Reach(location) where battery() > reserve_floor() + leg_cost(location) {
  drive(location)
}
```

Many plans may be declared `for` one goal, in an agent, a behavior or at top level. Their order is
declaration order: agent body, then behaviors in mount order, then imported modules in import order.
That order is the plan list whose ordinals intentions store.

### 6.3 Adoption and the plan selection function

`adopt G(args)` creates an intention for `G`, appends it to `d.intention`, queues a resume for it,
and returns its handle. It does not wait. Inside a plan, `achieve G(args)` adopts `G` as a subgoal,
links it on `+d.subgoal`, and suspends the calling intention until the subgoal ends (§6.6).

Plan selection, on adoption and after each plan failure, takes the first plan in the plan list that
is not in `tried` and whose `where` condition succeeds. The condition is evaluated as a bounded
expression: its first result is taken and its bindings are kept for the body. If none applies, the
goal fails.

At the end of every turn the executor evaluates each maintain goal's `holds` condition. For each one
that fails and has no live restoring intention, it adopts one. The cost is the sum of the
conditions' costs, every turn (§15).

### 6.4 Commitment

vagent v2.0 implements **single-minded commitment**. An intention keeps its goal until the goal
succeeds, its `drop when` holds, or no applicable plan remains. Reconsideration happens only at step
boundaries, which are the points where the intention resumes: before the first step, after each
suspension, and when a plan fails. Blind and open-minded commitment are not offered (U-A5).

### 6.5 The deliberation cycle

The cycle is one message per turn. The three selection functions of the BDI literature are fixed in
v2.0 and stated here:

- **Event selection** is the runtime's mailbox order: FIFO, the `std::deque` in
  `MeshActorRuntime::Actor` (`mesh_actor.cpp:350`). There is no priority mailbox; see §13.
- **Option selection** is §6.3: first applicable untried plan in plan-list order.
- **Intention selection** is message order. An intention advances only when a message for it
  arrives: a `Resume` the agent sent itself, a reply it was waiting for, or a timer.

```text
turn(message):
  match classify(message):
    Event(e) | Performative(e):
      for handler in matching(e) ordered by (priority desc, declaration order):
        m = mark()
        if guard(handler, e) succeeds and body(handler, e) succeeds: discard(m)
        else: release(m)                      -- a failed handler leaves no trace
        if e was consumed: break
      run queued belief events the same way
    Resume(i) | Reply(i, value) | Timer(i) | SubgoalEnded(i, outcome):
      continue(i)
    Down(child):
      queue ChildDown(child); fail every intention waiting on child
  check maintain goals; adopt restorers
  drain d.outbox

continue(i):
  if i.goal's drop-when holds: end i as dropped
  if i.goal is achieve and succeed-when holds: end i as achieved
  run i's plan from i.step to the next suspension point or the end of the body
    at a suspension point: record step, frame, waiting; return
    at the end:            achieve → re-test succeed-when; perform → achieved
    on failure:            add plan to tried; reselect; if none, end i as failed
  ending i: unlink it from d.intention; send SubgoalEnded to its parent intention, if any,
            or the outcome performative to its delegator, if any; queue GoalAchieved/GoalFailed
```

`priority` orders handlers within one turn and nothing else. It is a scalar cell on the handler's
program cell, defaulting to zero. A high-priority handler cannot interrupt a running intention step,
because the step and the handler run in different turns. The handler can **drop** an intention, and
the intention's queued `Resume` then finds it gone and does nothing. Preemption is therefore at turn
granularity. A long plan step that must be preemptible inserts `yield`.

`consume` inside a handler stops lower-priority handlers from seeing the same event, as Jason's
single-plan-per-event rule does. Without it, all matching handlers run, as in SARL.

### 6.6 Suspension points

A suspension point is where an intention may end its turn. There are three:

| Statement                             | Waits for                            | Resumed by                        |
| ------------------------------------- | ------------------------------------ | --------------------------------- |
| `achieve G(args)` (in a plan)         | the subgoal intention to end         | `SubgoalEnded`                    |
| `achieve G(args) by agent within d`   | the delegate's outcome performative  | the reply, or the deadline timer  |
| `x := ask agent.query(args) within d` | the answer                           | `ActorDeliverValue`, or the timer |
| `yield`                               | nothing; gives other messages a turn | the `Resume` it queued            |

The compiler accepts a suspension point only where V4 can describe the whole state: a statement of a
plan body, or a statement inside the `then`, `else` or `do` body of an `if`, `while` or `for` that
is itself at such a place. The step path is the list of statement ordinals from the plan body down
to the suspension point. A `while` loop's progress is in its frame variables, so resuming re-tests
its condition. A `for` over a list or a range keeps its list and index in the frame, so it resumes
at the next element. A `for believes` loop walks a clause rank that other turns may change, so it
may not contain a suspension point. A suspension point inside a generator operand or inside an `&`
or `|` expression is a compile-time error, because a generator's position is a choice point and
choice points do not survive a turn (V2). This restriction is the price of V4, and §13 records the
continuation-passing alternative.

Deadlines are mandatory on `ask` and delegated `achieve`. A deadline elapsing fails the statement.
`else e` on the statement supplies a value or recovery instead. A reply that arrives after its
intention has ended is dropped and counted (`std:agent` statistic `late_replies`).

### 6.7 Failure

| Outcome            | Cause                                                                                     | Handled by                                                                      |
| ------------------ | ----------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| expression failure | a generator ran dry                                                                       | the enclosing `&`/\`                                                            |
| statement failure  | a failed bounded expression, `fail`, an expired deadline                                  | fails the enclosing block, hence the handler or the plan                        |
| handler failure    | its block failed                                                                          | the handler's mark is released; the event counts as unhandled                   |
| plan failure       | the body failed, or an achieve plan ended with the goal unmet                             | `PlanFailed` queued; the next applicable plan is selected                       |
| goal failure       | no applicable untried plan remains                                                        | `GoalFailed` queued; parent's `achieve` statement fails; delegator told         |
| contract violation | `requires`/`ensures` on a capacity method (`ContractViolationKind`, `vortex_core.hpp:62`) | as plan failure, with cause `contract`; never kills the agent                   |
| fuel exhaustion    | the turn's instruction or cell limit                                                      | `release(m0)`: the whole turn is undone; `TurnAborted` queued for the next turn |
| child down         | `ActorDown` for a monitored child                                                         | `ChildDown` queued; intentions waiting on that child fail                       |

There is no backtracking across an effect (V6), so a plan that fails after acting fails forward: its
effects stand, the next plan starts from the world as it now is, and a `PlanFailed` handler can
compensate. `achieve G(args) else e` at a subgoal runs `e` when the subgoal fails, with `cause`
bound to the failure's cause.

Every top-level handler body and intention step runs under a base choice point whose alternative is
the failure path above. A `#Fail` with an empty choice stack makes the VM report failure
(`vortex_vm.cpp:962-972`), and without the base choice point that would surface as an executor error
and stop the actor (§4.1).

______________________________________________________________________

## 7. Communication

### 7.1 Kinds of message

| vagent form                        | Kind on the wire | Receiver's default handling (`std:agent`)                     | Reply                |
| ---------------------------------- | ---------------- | ------------------------------------------------------------- | -------------------- |
| `emit E{...}` / `emit E{...} in s` | `event`          | user handlers for `E`                                         | none awaited         |
| `tell a fact`                      | `tell`           | `believe fact` with provenance; `+fact` handlers              | none awaited         |
| `untell a fact`                    | `untell`         | `forget fact` where the sender is the provenance              | none awaited         |
| `achieve G(args) by a within d`    | `achieve`        | `adopt G(args)` if `accepts(sender, G)` succeeds; else refuse | outcome performative |
| `ask a.q(args) within d`           | `ask`            | runs the `answer q` declaration, read-only                    | the answer value     |

Every `ActorSend` is answered by the runtime with an `ActorDeliverValue` carrying the receiver's
turn result (`mesh_actor.cpp:488-499`). For `ask`, that result is the answer. For the other kinds,
the adapter recognises the reply as an acknowledgement, marks it, and ends the turn without running
user code. That costs the sender one extra short turn per message sent (§15).

`answer` declarations are compiled under a read-only check: no field assignment, no `believe` or
`forget`, no `emit`/`tell`, no `effect` call, no `adopt`. The answer must fit `maxOutputBytes`,
which the runtime caps at 32 KiB (`mesh_actor.cpp:135`).

`accepts` is a capacity every agent has, with a default skill that accepts delegation from the
agent's parent and its own children and refuses everything else. Replacing the skill changes the
policy.

### 7.2 Scopes

`emit E{...}` goes to `outer`. `in s` names a space: `inner`, `outer`, or a space value obtained
from `std:agent`. `for r` restricts delivery to the listed handles, which must be members. The
emitter does not receive its own emission. The host receives every emission into the spaces it
hosts, because it relays them.

### 7.3 Ordering and delivery

Messages from one sender to one receiver arrive in send order, because the runtime numbers them
(`ActorMessage::sequence`) and the transport is ordered per peer. No order holds across senders, and
relay through a host adds one hop. Delivery is at least once with de-duplication by `requestId`
(§5.5). A sent message is complete when its reply arrives (`mesh_actor.cpp:441-447`). Until then it
blocks the sender's checkpoint (`mesh_actor.cpp:678-683`).

### 7.4 Liveness

An agent never blocks: an intention waiting on an `ask` or a delegated goal has ended its turn, and
the agent keeps handling messages. Two intentions can wait on each other through two agents, for
example A's plan asks B while B's plan asks A. The mandatory deadline breaks that wait, so every
wait is bounded by its deadline. §14 states the property with this condition.

______________________________________________________________________

## 8. The surface language

### 8.1 Lexical structure

- Comments: `//` to end of line, and `/* ... */`, which do not nest.

- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Types, events, goals, agents, behaviors, skills and
  capacities are `PascalCase`; everything else is `snake_case`. The checker warns on a mismatch.

- Literals: integers (`42`, `1_000`, `0x2A`), floats (`3.5`, `1e-3`), strings (`"..."` with `\n`,
  `\t`, `\"`, `\\`, `\u{...}`), `true`, `false`, `()`, durations (`250ms`, `2s`, `5min`).

- Statement termination: a newline ends a statement when the last token can end one (an identifier,
  a literal, `)`, `]`, `}`, or a keyword such as `yield`). A line that begins with a binary operator
  or `.` continues the previous statement. A line that begins with `(`, `[` or `{` never continues
  one. Newlines inside `( )`, `[ ]`, a record literal's `{ }` or a handler guard are whitespace. `;`
  separates statements on one line.

- Reserved words:

  ```text
  achieve adopt agent answer as ask behavior believe believes by capacity consume def do drop
  effect else emit ensures event extends fail findall for forget goal holds if implements import
  in inner install let maintain module not occurrence on outer parent perform plan priority
  requires self skill succeed tell then to uninstall untell uses var when where while within yield
  ```

### 8.2 Truth and failure

An expression succeeds with a value or fails (V11, vicon §4.1). Consequences a reader must know:

- `a < b` produces `b` or fails; `a == b` produces `b` or fails. A chain `a < b < c` works as in
  Icon.
- `true` and `false` are values of type `Bool`. They are not tests: `if flag then …` succeeds
  whenever `flag` produces a value, including `false`. Write `if flag == true then …`, or make the
  method fail instead of returning `false`. Capacity methods in this document fail instead of
  returning `Bool`.
- `not e` succeeds with `()` if `e` fails, and fails if `e` succeeds.
- `&` is conjunction: `e1 & e2` evaluates `e2` for each result of `e1` until `e2` succeeds. `|` is
  alternation: the results of `e1`, then those of `e2`. Neither is a boolean operator, and there is
  no `&&` or `||`.
- `!e` generates the elements of a list or the characters of a string, as in vicon. It is not
  negation. `*e` is the size of a list or string.
- A statement is bounded: its first result is taken and its choice points are dropped (vicon §4.4).
  A failing statement fails its block. A block's value is its last statement's value, or `()`.
- Handler guards (`[...]`), `where` conditions, `succeed when`, `drop when` and `holds` are bounded
  expressions: they succeed or fail once.
- `for x in e do body` evaluates `e` once, bounded, to a list or a range, and runs `body` for each
  element in order with no choice point between iterations. `body` is bounded each time, so its
  writes and actions stand. The loop fails if `body` fails, and `break` is not offered: an early
  exit is a `while`. `for believes t do body` walks the matching facts the same way (§6.1).

### 8.3 Assignment and positions

`x := e` assigns irreversibly. `x <- e` assigns reversibly: if control backtracks past it within the
same statement, the old value returns (V2). `let x := e` declares a local. `var x: T := e` declares
a field of an agent, behavior or skill. Assignment targets are places: a local, a field, or a field
or index of one. Anything else on the left is a compile-time error.

A position is **bounded** if it is a statement, or the `then`, `else` or `do` body of an `if`,
`while` or `for` whose own position is bounded. Every other position is **speculative**: an operand
of `&`, `|`, `\`, `to`, a comparison or a call, or anything inside one. A write or action in a
speculative position is undone if control backtracks over it, which is what makes `<-` and a
speculative `emit` useful. The compiler holds three rules about positions:

| Construct                                                                                             | Allowed in                                     |
| ----------------------------------------------------------------------------------------------------- | ---------------------------------------------- |
| `<-`, `emit`, `tell`, `untell`, `believe`, `forget`, `spawn`, `adopt`, `install`, `uninstall`, `drop` | any position; undone with a backtracked branch |
| `:=`, `effect` calls, `consume`                                                                       | bounded positions only                         |
| `achieve` (subgoal or delegated), `ask`, `yield`                                                      | suspension positions only (§6.6)               |

`:=` is restricted so that its name means what it says: in a speculative position it would be undone
like `<-`, so the compiler asks for `<-` there.

### 8.4 Precedence

From tightest to loosest. All binary operators associate left unless marked.

| Level | Operators                              | Notes                                                              |
| ----- | -------------------------------------- | ------------------------------------------------------------------ |
| 1     | call `f(x)`, index `a[i]`, field `a.b` |                                                                    |
| 2     | unary `-`, `not`, `!`, `*`             | `*xs` is the size of `xs`, as in Icon                              |
| 3     | `\` (limitation)                       | `(1 to 10) \ 3`; binds tighter than arithmetic, as vicon writes it |
| 4     | `*` `/` `%`                            |                                                                    |
| 5     | `+` `-`                                |                                                                    |
| 6     | `++` (concatenation)                   | strings and lists; `+` on strings is a type error                  |
| 7     | `<` `<=` `>` `>=` `==` `!=`            | non-associative as written; chained comparison is explicit         |
| 8     | `\|` (alternation)                     | below comparison, as in Icon: `x == 1 \| 2` is `(x == 1) \| 2`     |
| 9     | `to` … `by`                            |                                                                    |
| 10    | `?` (string scanning)                  | above assignment, unlike Icon, so `x := s ? e` is `x := (s ? e)`   |
| 11    | `:=` `<-`                              | right-associative                                                  |
| 12    | `&`                                    | loosest, as in Icon                                                |

So `budget <- b - cost & budget > floor & route` is
`(budget <- (b - cost)) & (budget > floor) & route`, which is the shape every plan writes.

### 8.5 Declarations

```vagent
event TargetDetected { target_id: Str, distance: Float }
event Alarm extends TargetDetected { level: Int }

capacity Navigation {
  effect def move(speed: Float, heading: Float) requires speed >= 0.0
  effect def stop()
  def position(): Point
}

skill ThrusterNav(max_speed: Float) implements Navigation {
  var speed: Float := 0.0
  effect def move(s: Float, heading: Float) {
    s <= max_speed & { speed := s; thrusters.set(s, heading) }
  }
  effect def stop() { speed := 0.0; thrusters.set(0.0, 0.0) }
  def position(): Point { thrusters.fix() }
  on uninstall { stop() }
}

behavior ObstacleAvoidance(threshold: Int) {
  uses Navigation
  on CollisionWarning as w [w.threat > threshold] priority 100 {
    stop()
    emit ObstacleEvaded{obstacle: w.obstacle_id}
  }
}

agent PatrolDrone(threshold: Int) {
  uses Navigation
  install ThrusterNav(12.0)
  behavior ObstacleAvoidance(threshold)
  var targets_seen: Int := 0
  on TargetDetected [occurrence.distance < 50.0] {
    targets_seen := targets_seen + 1
    emit TargetLogged{target: occurrence.target_id}
  }
}
```

A method of a used capacity is called bare (`stop()`) when one used capacity defines that name, and
qualified (`Navigation.stop()`) otherwise; an ambiguous bare call is a compile-time error.

A handler binds the event as `occurrence`, or as the name after `as`. Handlers read fields by name;
there is no positional destructuring, so reordering an event's fields changes nothing.

### 8.6 Statements that act on the agent

| Form                                       | Value      | Notes                                                                    |
| ------------------------------------------ | ---------- | ------------------------------------------------------------------------ |
| `emit e [in s] [for r]`                    | `()`       | staged on `d.outbox` (V5)                                                |
| `tell a fact`, `untell a fact`             | `()`       | staged                                                                   |
| `believe fact`, `forget fact`              | `()`       | immediate in the arena; queue belief events                              |
| `spawn A(args) [in s]`                     | handle     | staged; the handle is valid immediately, the child exists after the turn |
| `adopt G(args)`                            | intention  | does not wait                                                            |
| `achieve G(args) [by a within d] [else e]` | `()`       | plan bodies only; a suspension point                                     |
| `x := ask a.q(args) within d [else e]`     | the answer | plan bodies only; a suspension point                                     |
| `drop i`                                   | `()`       | ends an intention as dropped                                             |
| `install S(args)`, `uninstall S`           | `()`       | replaces the skill for each capacity `S` implements                      |
| `yield`                                    | `()`       | plan bodies only; a suspension point                                     |
| `consume`                                  | `()`       | handlers only                                                            |
| `fail`                                     | fails      |                                                                          |

`spawn` is staged like an emission: the adapter performs the `ActorSpawn` after the turn. The handle
it returns is computed in advance (`ActorPid` is derived from the spawn request, the node, the epoch
and a sequence, `mesh_actor.cpp:159`); if the spawn is refused, the handle names no actor and the
first message to it produces `ActorDown`, which arrives as `ChildDown`.

### 8.7 Types

`Int`, `Float`, `Str`, `Bool`, `()`, `Duration`, `Time`, `Agent` (a handle), `Space`, `Intention`,
`[T]` (a list), `Map<K, V>`, and declared event types used as record types. Lists and maps are
`std:collections` structures of cells, so their elements are cells and are quotable. Type checking
is static for declarations and event fields, and the compiler infers the types of locals.

### 8.8 Names in scope

| Name         | Meaning                                                                            |
| ------------ | ---------------------------------------------------------------------------------- |
| `self`       | this agent's handle                                                                |
| `parent`     | the handle of the agent that spawned this one; fails for a top-level agent         |
| `inner`      | the space this agent hosts                                                         |
| `outer`      | the space this agent was spawned into; for a top-level agent, the launcher's space |
| `occurrence` | the event being handled                                                            |
| `cause`      | in an `else` after `achieve` or `ask`, the failure cause as a `Str`                |

A program starts with `vagent run module:Agent(args)`, which spawns a launcher agent hosting the
node's default space and then the named agent inside it.

### 8.9 Grammar

```ebnf
Unit          ::= [ ModuleDecl ] { ImportDecl } { TopDecl } ;
ModuleDecl    ::= 'module' ModulePath ;
ImportDecl    ::= 'import' ModulePath [ 'as' Ident ] [ '(' IdentList ')' ] ;
ModulePath    ::= Ident { ':' Ident } ;

TopDecl       ::= EventDecl | CapacityDecl | SkillDecl | BehaviorDecl | AgentDecl
                | GoalDecl | PlanDecl ;

EventDecl     ::= 'event' Ident [ 'extends' Ident ] [ '{' [ FieldList ] '}' ] ;
FieldList     ::= Field { ',' Field } [ ',' ] ;
Field         ::= Ident ':' Type ;

CapacityDecl  ::= 'capacity' Ident [ 'extends' IdentList ] '{' { MethodSig } '}' ;
MethodSig     ::= [ 'effect' ] 'def' Ident '(' [ ParamList ] ')' [ ':' Type ] { Contract } ;
Contract      ::= ( 'requires' | 'ensures' ) Expr ;
MethodDef     ::= MethodSig Block ;
AnswerDef     ::= 'answer' Ident '(' [ ParamList ] ')' ':' Type Block ;

SkillDecl     ::= 'skill' Ident [ '(' ParamList ')' ] 'implements' IdentList
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
Trigger       ::= Ident | ( '+' | '-' ) Ident [ '/' IntLit ] ;

GoalDecl      ::= 'goal' Ident '(' [ ParamList ] ')' GoalKind [ '{' { GoalClause } '}' ] ;
GoalKind      ::= 'achieve' | 'perform' | 'maintain' ;
GoalClause    ::= 'succeed' 'when' Expr | 'drop' 'when' Expr | 'holds' Expr ;
PlanDecl      ::= 'plan' Ident 'for' Ident '(' [ IdentList ] ')' [ 'where' Expr ] Block ;

Block         ::= '{' [ Stmt { Sep Stmt } ] [ Sep ] '}' ;
Sep           ::= ';' | NEWLINE ;
Stmt          ::= 'let' Ident [ ':' Type ] ':=' Expr
                | 'return' [ Expr ]
                | Expr ;

Expr          ::= Assign { '&' Assign } ;
Assign        ::= Scan [ ( ':=' | '<-' ) Assign ] ;
Scan          ::= ToExpr [ '?' ToExpr ] ;
ToExpr        ::= Alt [ 'to' Alt [ 'by' Alt ] ] ;
Alt           ::= Cmp { '|' Cmp } ;
Cmp           ::= Concat [ CmpOp Concat ] ;
CmpOp         ::= '<' | '<=' | '>' | '>=' | '==' | '!=' ;
Concat        ::= Add { '++' Add } ;
Add           ::= Mul { ( '+' | '-' ) Mul } ;
Mul           ::= Limit { ( '*' | '/' | '%' ) Limit } ;
Limit         ::= Unary { '\' Unary } ;
Unary         ::= ( '-' | 'not' | '!' | '*' ) Unary | Postfix ;
Postfix       ::= Primary { '(' [ ArgList ] ')' | '[' Expr ']' | '.' Ident } ;
Primary       ::= Ident | Literal | 'self' | 'parent' | 'inner' | 'outer' | 'occurrence'
                | '(' Expr ')' | Block | ListLit | RecordLit
                | 'if' Expr 'then' Expr [ 'else' Expr ]
                | 'while' Expr 'do' Expr
                | 'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr
                | 'believes' Term
                | 'findall' '(' TermArg ',' Term ')'
                | Action ;
Action        ::= 'emit' Postfix [ 'in' Postfix ] [ 'for' Postfix ]
                | ( 'tell' | 'untell' ) Postfix Term
                | ( 'believe' | 'forget' ) Term
                | 'spawn' Ident '(' [ ArgList ] ')' [ 'in' Postfix ]
                | 'adopt' GoalCall
                | 'achieve' GoalCall [ 'by' Postfix 'within' Postfix ] [ 'else' Expr ]
                | 'ask' Postfix 'within' Postfix [ 'else' Expr ]
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

- **Record literals and blocks.** `Ident '{'` in an expression is always a record literal. No
  expression production puts a block directly after an identifier: bodies follow `then`, `do`,
  `else`, `]`, `)` or a declaration header, and a block on the next line is a new statement by §8.1.
  This is why `if`, `while` and `for` use `then` and `do`.
- **Action operands are postfix expressions**, so `emit E{...} & next()` is `(emit E{...}) & next()`
  and an `in`, `for`, `by`, `within` or `else` after an action belongs to it. A computed operand is
  parenthesised: `drop (pick() | fallback())`.
- **`else` binds to the nearest `if`, `achieve` or `ask`.**
- **Static restrictions** checked after parsing: the position rules (§8.3), suspension points
  (§6.6), `answer` bodies read-only (§7.1), assignment targets are places, `consume` only in
  handlers, `achieve`/`ask`/`yield` only in plan bodies.
- **Terms versus calls.** After `believe`, `forget`, `believes`, `findall`, `tell` and `untell`, and
  in a `Trigger`, `name(args)` is a term, not a call.

### 8.10 Differences from SARL

| SARL                                       | vagent                                                      | Why                                                                                                    |
| ------------------------------------------ | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `on E [guard] { }`, `occurrence`           | same                                                        | kept                                                                                                   |
| `uses Cap` imports capacity functions      | same                                                        | kept                                                                                                   |
| `setSkill(new Impl)` at run time           | `install Impl(args)`, declared or as a statement            | a keyword form like every other agent action; arguments construct the skill                            |
| `emit(e)` with `[scope]` lambda            | `emit e in s for r`                                         | a static list of handles is checkable; a scope lambda would run in the emitter's turn for every member |
| `in`/`every` from the `Schedules` capacity | `Schedules.after`, `Schedules.periodic`, `Schedules.cancel` | `in` is a reserved word in vagent; one naming scheme for the three                                     |
| `wake`                                     | `emit e in inner for [self]` is not offered                 | an agent's internal events are belief, goal and plan events                                            |
| behaviors registered at run time           | `behavior B(args)` member, `Behaviors.mount/unmount`        | parameters added                                                                                       |
| no priority                                | `priority n`, within-turn order only                        | extension; deterministic order of matching handlers                                                    |
| no BDI                                     | beliefs, goals, plans, intentions                           | the point of the language                                                                              |
| Janus thread pool, shared JVM heap         | one actor and one arena per agent                           | V1                                                                                                     |

### 8.11 Relationship to the siblings

| Concern            | vicon                                  | vagent                                  | Divergence and reason                                                                     |
| ------------------ | -------------------------------------- | --------------------------------------- | ----------------------------------------------------------------------------------------- |
| assignment         | `:=`, `<-`                             | `:=`, `<-`                              | none                                                                                      |
| conditional, loops | `if then else`, `while do`, `every do` | `if then else`, `while do`, `for in do` | `every` drives a generator, and the trail undoes its body's writes (V6a)                  |
| string scanning    | `s ? e`, `tab`, `move`                 | same, from `std:goal`                   | `?` binds tighter than assignment (§8.4)                                                  |
| module paths       | `std:goal`                             | `std:agent`, `mars:exploration`         | none; vlace's string form `"std:xanadu"` and VQL's `std::` should converge on this (U-A6) |
| local binding      | assignment to a fresh name             | `let x := e`                            | vagent declares locals so plan frames are known statically (§5.6)                         |
| logic variables    | Vlog terms                             | `?x` inside terms only                  | only belief terms need them                                                               |

______________________________________________________________________

## 9. Lowering

### 9.1 Cross-walk

The last column names what each construct needs beyond `link`, `value` and the existing opcodes.
"Adapter" means the owed executor (§11.2).

| Construct                              | Lowering                                                                                                                                  | Beyond link/value                             |
| -------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------- |
| agent creation (first turn)            | `makeCell(type name)` for the agent cell; `setStateRoot`; ranks created empty                                                             | `setStateRoot` (built)                        |
| `var` field                            | name cell on `d.vars` off the owner, value on `d.values`                                                                                  | none                                          |
| `install S(args)`                      | skill instance per capacity on `d.skill` via `rankTail`+`link`; view appended to the capacity's `d.clone` rank; `+d.capacity` to the view | none                                          |
| capacity call                          | Vortex routine walks `d.skill` comparing `cloneMaster`; `#Call` with the method entry on `+d.grab`, arguments on `+d.step`                | none                                          |
| `on E [g] priority p { b }`            | handler program cell with `p` as a scalar; the dispatcher routine matches the type name, runs `g` then `b` under a mark                   | none                                          |
| `believe t` / `forget t`               | Vlog `assertz` / `retract` on the predicate's `d.clause` rank; belief event cell queued                                                   | none                                          |
| `believes t`                           | Vlog clause walk with `#Unify`, one `#Choice` per clause                                                                                  | none                                          |
| `e1 & e2`, `e1 \| e2`, `\`, `to`, `<-` | exactly vicon's lowering (vicon §4–§5)                                                                                                    | none                                          |
| `for x in e do b`                      | bounded `e` to a list or range; a deterministic rank walk or counter; `b` bounded per element                                             | none                                          |
| `for believes t do b`, `findall`       | clause-rank walk; per clause: copy `t`, mark, `#Unify`, release or discard (§6.1)                                                         | none                                          |
| `emit e in s for r`                    | envelope cell (type, fields, space, recipients) appended to `d.outbox`                                                                    | adapter: `send` at turn end                   |
| `tell`, `untell`                       | envelope cell on `d.outbox`                                                                                                               | adapter                                       |
| `spawn A(args)`                        | envelope cell on `d.outbox`; handle cell computed and returned                                                                            | adapter: `MeshActorRuntime::spawn`, `monitor` |
| `adopt G(args)`                        | intention cell appended to `d.intention`; `Resume` envelope to `self` on `d.outbox`                                                       | adapter (self-send)                           |
| `achieve G(args)` in a plan            | as `adopt`, plus `+d.subgoal` link and `waiting`; the step returns                                                                        | none beyond `adopt`                           |
| `achieve … by a within d`              | `achieve` envelope on `d.outbox`; timer request; `waiting` set; the step returns                                                          | adapter: send, timer                          |
| `ask a.q() within d`                   | `ask` envelope; timer request; `waiting` set; the step returns                                                                            | adapter: send, timer                          |
| `drop i`                               | unlink `i` from `d.intention`; subgoals dropped recursively                                                                               | none                                          |
| `yield`                                | `Resume` envelope to `self`; the step returns                                                                                             | adapter (self-send)                           |
| `requires` / `ensures`                 | the existing `-d.contract` / `+d.contract` chains on the method's call site                                                               | none                                          |
| turn end                               | `discard` or `release` of the turn mark                                                                                                   | adapter: drain, fuel re-arm                   |

### 9.2 Capacity calls

The call uses the dual-wing convention as built, with no change. `#Call` pushes a return frame on
the cursor's `+d.stack` and jumps to `inputs[0]` (`vortex_vm.cpp:665-687`). Argument cells are the
call opcode's `+d.grab`/`+d.step` operands; the callee reads them through the frame. Method
contracts are the call site's `-d.contract` and `+d.contract` chains, evaluated as the Vortex
document §3 describes. A violation returns `ExecutionResult::contractViolation`
(`vortex_core.hpp:62-70`), which §6.7 maps to plan failure.

The compiler emits one call opcode per call site in the program, with `inputs[0]` patched by the
dispatch routine at run time. It does not mint a call cell per call, which would spend fuel on every
invocation.

### 9.3 A handler, lowered

`on TargetDetected [occurrence.distance < 50.0] { targets_seen := targets_seen + 1 }` becomes:

```text
handler cell "TargetDetected"  (+d.vars: priority = 0, ordinal = 3)
  guard stream (+d.spin):   #Load occurrence.distance → #Lt 50.0 → (fails or continues)
  body stream (+d.spin):    #Load targets_seen → #Add 1 → #Store targets_seen
dispatcher (std:agent, Vortex):
  for each handler cell matching the event type name, in (priority, ordinal) order:
    #Choice alt=next-handler ; m = mark ; run guard ; run body ; discard(m) ; #Cut
```

______________________________________________________________________

## 10. The `std:agent` package

`std:agent` is written in Vortex, anchored on `d.stdlib` like every package
([vortex-hyperstructural-runtime.md](vortex-hyperstructural-runtime.md) §7.1). It reuses `std:logic`
(Vlog clause storage and unification), `std:goal` (generators, scanning), `std:collections` (lists,
maps) and `std:string` unchanged.

| Part         | Contents                                                                                                                                                                                                                     |
| ------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| genesis      | mints the ten dimensions of §5.1 on `d.dims`                                                                                                                                                                                 |
| dispatcher   | handler matching and ordering (§6.5), belief event rounds, `consume`                                                                                                                                                         |
| deliberation | `adopt`, plan selection, `continue`, maintain checks, intention ending                                                                                                                                                       |
| relay        | the host's fan-out of emissions into hosted spaces                                                                                                                                                                           |
| capacities   | `Behaviors` (`mount`, `unmount`), `Schedules` (`after`, `periodic`, `cancel`), `Clock` (`now`), `Accepts` (`accepts`), `Logging`                                                                                             |
| functions    | `findall`, `intentions()` (a list of this agent's intentions), `members(s)`, `children()`, `text(x)`                                                                                                                         |
| events       | `Initialize{args}`, `Destroy`, `GoalAchieved{intention, goal}`, `GoalFailed{intention, goal, cause}`, `GoalDropped{intention, goal}`, `PlanFailed{intention, plan, cause}`, `ChildDown{child, reason}`, `TurnAborted{cause}` |

`Clock.now()` reads the wall time the runtime passed to `pumpOne` for this turn; it does not read a
clock during the turn, so a turn is a function of its message and that time. `Logging` writes
through the `vortex.agent` category, which the logging migration (`design/logging-migration.md`)
must register; it never logs event payloads.

Settings read from `system://settings` with their defaults: `vagent.max_actors` (128, the runtime
default), `vagent.belief_event_depth` (16), `vagent.ask_default_deadline` (none: the deadline is
written at the call), and the fuel figures passed to the grant. None is a constant in code.

______________________________________________________________________

## 11. What the C++ core has to gain

### 11.1 Nothing in the VM, the arena, or the store

No opcode, no `ChoicePoint` field, no `ArenaManifold` method, no `CompactOpNode` change, no
`SystemDimensions` field. The control model is vicon's and the database is Vlog's.

### 11.2 A front end and a mesh executor adapter

```text
apps/common/xanadu/vagent/
  token.hpp, lexer.{hpp,cpp}      // §8.1, including the newline rule
  ast.hpp, parser.{hpp,cpp}       // §8.9; precedence climbing over §8.4
  checker.{hpp,cpp}               // types, static restrictions (§8.9 notes)
  compiler.{hpp,cpp}              // lowering (§9) to Vortex opcode streams
  agent_executor.{hpp,cpp}        // the MeshActorRuntime::Executor (below)
apps/vagent/main.cpp              // `vagent run`, `vagent check`, a REPL that drives one agent
```

The compiler's surface follows `vprolog::Compiler` and returns `std::expected` with a named error,
as vicon §8.2 sets out.

The adapter is the only part with run-time responsibilities. It is C++ because it holds a
`MeshActorRuntime&`, which Vortex code cannot reach (§1). Per turn it:

1. re-arms fuel with `setExecutionLimits` (§4.2);
1. decodes the `ActorMessage` into an event cell or classifies it as reply, resume, timer or down;
1. runs the dispatcher or `continue` on the agent's cursor under the turn mark, under a base choice
   point (§6.7);
1. drains `d.outbox`: `send` for envelopes, `spawn` and `monitor` for spawns;
1. keeps a timer heap and enqueues timer messages to the agent through the runtime's `receive`;
1. returns the turn result, which is the answer for an `ask` and empty otherwise.

Estimated size: the adapter about 800 lines with tests; the front end about the size of vprolog's.

### 11.3 Changes in `mesh_actor.cpp`

| Change | What                          | Why                                                                                                                                                                                 | Price                                                                                               |
| ------ | ----------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| (a)    | Reap completed deliveries     | `deliveries_` is never erased (no `erase` in the file) and `send` refuses at `maxOutstandingDeliveries` (`mesh_actor.cpp:521`), so a runtime can send 4096 messages in its lifetime | erase on completion; the de-duplication of re-sends must then key on a window of recent ids         |
| (b)    | Window remembered request ids | `remembered` is never erased and `enqueue` refuses at `maxRememberedMessages` (`mesh_actor.cpp:344`), so an actor can receive 4096 messages in its lifetime                         | keep ids above a per-sender sequence watermark; checkpoints carry the watermark instead of every id |
| (c)    | Capability-bearing senders    | `receive` admits `ActorSend` only from the actor's owner (`mesh_actor.cpp:402-405`)                                                                                                 | not needed for single-owner societies; needed for any space spanning identities. Deferred (U-A2)    |

(a) and (b) are bugs for any long-lived actor, not only for vagent. The mesh was built for bounded
jobs, and these limits suit those. Without (a) and (b), a vagent agent stops working after 4096
messages, which is a ceiling users would have to work around, and so vagent depends on them.

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
    for n in 1 to partitions do spawn Worker() in inner
  }

  on SearchQuery as q [q.space == outer] {
    believe pending(q.query_id)
    for i in 0 to partitions - 1 do
      emit SubTask{query_id: q.query_id, partition: i, pattern: q.pattern} in inner
  }

  // A duplicate delivery of one partition is ignored by the guard.
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

The worker's default scope is `outer`, which for a child is its parent's `inner`, so its results
reach only the coordinator and its siblings. The coordinator tells partitions apart by belief, not
by counting hits. `*xs` is the list size (`std:collections`).

### 12.2 Swapping a skill at run time

```vagent
capacity Transport {
  effect def send_payload(dest: Agent, data: Str)
}
skill Loopback implements Transport {
  effect def send_payload(dest: Agent, data: Str) { emit Payload{data: data} for [dest] }
}
skill Sealed(key_id: Str) implements Transport {
  effect def send_payload(dest: Agent, data: Str) {
    emit SealedPayload{key_id: key_id, data: data} for [dest]
  }
}

event Payload { data: Str }
event SealedPayload { key_id: Str, data: Str }
event SecurityAlert { level: Int }
event OutgoingReport { recipient: Agent, content: Str }

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

Trace: an `OutgoingReport` before the alert dispatches `send_payload` to the `Loopback` instance.
The alert's turn removes that instance from `d.skill` and appends a `Sealed` instance with its own
view of `Transport`. The next `OutgoingReport` reaches `Sealed`, and `Reporter` is unchanged. The
guard's `sealed == false` keeps a second alert from re-installing. The threshold and key are
parameters, not constants.

### 12.3 A rover with goals, plans and a reversible choice

```vagent
module mars:exploration
import std:agent
import std:math (Point)
import mars:hardware (Chassis, Spectrometer)

event HazardDetected { kind: Str, severity: Int }
event MissionCommand { target_id: Str, location: Point }
event EmergencyHalted { kind: Str }
event ScienceLog { target_id: Str, mineral: Str }
event SampleSecured { target_id: Str, total: Int }
event SurveyFailed { target_id: Str, cause: Str }

capacity Mobility {
  effect def follow(route: [Point])      // fails if any leg loses traction
  effect def halt()
  def battery(): Float
  def position(): Point
  def cost(route: [Point]): Float
  def direct_route(to_point: Point): [Point]
  def ridge_route(to_point: Point): [Point]
}
capacity Science {
  def spectra(target_id: Str): Str
  effect def drill_core(target_id: Str)
}

goal Survey(target_id: Str, location: Point) perform
goal Reach(location: Point) achieve {
  succeed when position() == location
}

agent MarsSurveyor(hazard_limit: Int, reserve_floor: Float) {
  uses Mobility, Science
  install Chassis()
  install Spectrometer()
  var budget: Float := 0.0
  var samples: Int := 0

  on HazardDetected as h [h.severity > hazard_limit] priority 100 {
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
    let mineral := spectra(target_id) ? (tab(match("SPEC:")) & tab(upto(";")))
    emit ScienceLog{target_id: target_id, mineral: mineral}
    drill_core(target_id)
    samples := samples + 1
    emit SampleSecured{target_id: target_id, total: samples}
  }

  // Choose a route inside one statement: a failed arm restores budget.
  plan travel for Reach(location) {
    let route := (budget <- battery() - cost(direct_route(location))
                  & budget > reserve_floor & direct_route(location))
               | (budget <- battery() - cost(ridge_route(location))
                  & budget > reserve_floor & ridge_route(location))
    follow(route)
  }
}
```

What happens, step by step:

1. `MissionCommand` adopts `Survey`. Its `Resume` arrives next turn; `survey` is the only plan.
1. `achieve Reach(location)` adopts `Reach` as a subgoal and suspends `Survey` at step 0.
1. `Reach`'s turn evaluates the route statement. The direct arm sets `budget`; if
   `budget > reserve_floor` fails, the trail restores `budget` and the ridge arm runs. The statement
   ends, its mark is discarded, and `budget` keeps the chosen arm's value. This is the only place
   `<-` matters, and it is within one statement (V2).
1. `follow(route)` is an effect at statement level (V6). If a leg loses traction, the plan fails.
   `Reach` has no other plan, so it fails, and `Survey`'s `achieve … else` runs: it emits
   `SurveyFailed` and fails `survey`. The rover stays where it stopped; nothing pretends to undo the
   drive.
1. A `HazardDetected` arriving between turns runs at the next turn boundary, halts, and drops every
   intention. Queued `Resume` messages for them then find nothing.

`Chassis` and `Spectrometer` come from `mars:hardware`, whose skills call native host functions.
That binding is device code and is outside this document.

______________________________________________________________________

## 13. Refused alternatives

| Alternative                                                   | Refused because                                                                                                                                   |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| Agents as cells in one shared arena (1.0's population rank)   | breaks share-nothing and the per-actor grant; one agent's fuel exhaustion would undo another's turn                                               |
| Holon children as sub-structures of the parent's arena        | children would share the parent's failure domain and fuel; a holon could not be distributed                                                       |
| Speculation across turns (1.0's shadow cells, `ArenaSlice`)   | needs a mark live across turns, which `checkpointView` refuses; marks are LIFO truncation and cannot be per-intention without new arena machinery |
| Persisting a generator's choice point at a suspension point   | a choice point is a C++ frame and an arena mark; neither is portable (`mesh_actor.hpp:92-93`)                                                     |
| Continuation-passing transform so suspension can sit anywhere | the compiler would have to reify generator state as cells; possible later (U-A7), not needed for the examples                                     |
| Free-standing `commit` (Prolog cut over side effects)         | round 1 found it placed before the fallible action in every example; V5 and V6 make the safe order the only expressible one                       |
| A priority mailbox                                            | needs a change to `enqueue` in C++ for an ordering handlers already get within a turn; preemption by dropping intentions covers the use case      |
| Per-event `promote` to a `Store`                              | R8: agent working state is not hypertime; one promotion per event per agent would flood the spool                                                 |
| vagent as a VQL extension (`vql-agent-experiment.md`)         | VQL's deferred-evaluation model has no intention that outlives a query; agent turns would need a second runtime under the query engine            |
| A dimension per entity kind with no reuse                     | 1.0 declared seventeen; fields, frames and event payloads fit `d.vars`/`d.values`, choice and trail are the VM's and the arena's, so ten remain   |
| `every e1 do e2` for loops that act                           | the arena trail undoes the body's writes on every resumption (V6a); `for` walks without a choice point                                            |
| Positional event patterns                                     | silently rebind when an event's fields are reordered                                                                                              |
| `&&`/\`                                                       |                                                                                                                                                   |

______________________________________________________________________

## 14. Properties, with their conditions

- **No data race between agents.** Each agent's arena is touched only by its own turns on the
  runtime's owner thread (`mesh_actor.hpp:107-109`). Within an agent, turns are sequential.
- **An agent never blocks.** No vagent construct waits inside a turn. Waits end the turn (§6.6).
- **Every wait is bounded.** `ask` and delegated `achieve` require a deadline; a subgoal ends or is
  dropped. A cycle of agents waiting on each other ends when the first deadline elapses. Without
  §11.2's timers, this property does not hold.
- **A turn is atomic.** Its writes stand together or are undone together (`discard`/`release` of the
  turn mark), and its messages leave only if it succeeds (V5). An effect is the exception: a
  hardware action performed before a later hard failure in the same turn is not undone.
- **A turn is a function of its message, its wall time and the agent's state.** `Clock.now()` reads
  the turn's time, and nothing else in the language reads a clock or randomness. Replaying a turn
  from a checkpoint with the same message and time reproduces it.
- **Not claimed: lifetime replay.** The mesh does not log inbound messages, and agent state is not
  in the spool (V8). Recording the inbound stream as a rank would make replay possible (U-A8).

______________________________________________________________________

## 15. Breakages and costs

- **Two turns per message.** Every `ActorSend` produces an `ActorDeliverValue` reply, which the
  sender handles as a short turn. The cost is one decode and one mark per message sent.
- **Relay doubles hops.** Emitting into a space hosted elsewhere is two sends, and the host is a
  bottleneck and a single point of failure for its space.
- **Reclamation needs quiescence.** A continuously busy agent never reaches a checkpoint, so it
  never compacts, and its arena grows by every consumed event and finished intention (§4.4, U-A1).
- **Maintain goals cost every turn.** Each `holds` condition runs at the end of every turn.
- **Belief queries are linear** in the predicate's clause count until convergence U1 (Vlog §6.3).
  vagent is a fourth consumer of U1, after VPL, Vlog and vicon.
- **Suspension is restricted** to step paths (§6.6). A plan that must wait inside a loop over a
  generator has to be split into a goal per element.
- **Single-owner societies only** until §11.3 (c).
- **Fuzzing.** The parser, the event decoder and the outbox drain are new input surfaces and get
  `tests/fuzz/` targets alongside the existing three.

______________________________________________________________________

## 16. Reconciliation with the convergence

- **R5 (`noCell == 0`).** The C++ adapter and compiler use `std::optional<CellRef>`, as
  `ArenaManifold::link` returns. vagent's failure is the absence of a result, not a zero.
- **R6 (scalars).** Fields, event fields, priorities and ordinals are scalar cells.
- **R7 (a `SetLink`'s subject is its chain).** Not touched: vagent writes no Structure operations.
- **R8 (two regimes).** Everything vagent builds is arena state (V8). The only way into a `Store` is
  the explicit `StoreWriter` capacity and `zigzag::promote`, subject to `PromotionBudget`
  (`arena_manifold.hpp:232-234`). Whether an agent acting for its owner counts as a user-generated
  update under R8 is U-A9.
- **R12 (a dimension is a cell).** The ten dimensions are minted as data by `std:agent` (V9).
- **U1 (rank indexing).** A consumer, not a resolver (§15).

______________________________________________________________________

## 17. Open questions

- **U-A1. Reclaiming a busy agent's arena.** Options: a reachability sweep over the arena (new arena
  machinery, O(live cells)), or a copy-to-fresh-arena step the adapter runs between turns, which
  needs no quiescent mailbox, only a turn boundary.
- **U-A2. Cross-owner spaces.** Change (c) in §11.3: what capability a non-owner presents, and who
  issues it.
- **U-A3. Large payloads.** Events over the frame limit as torrents named by `infoHash`, received
  through `ActorDeliverWriteSet`.
- **U-A4. Swarm spaces.** Binding a space to a `mesh_swarm` topic over BEP 46, so membership is the
  topic's and no host relays.
- **U-A5. Commitment strategies.** Blind and open-minded commitment as goal modifiers.
- **U-A6. One module path spelling across the family.**
- **U-A7. Suspension anywhere.** A continuation-passing compilation of generators into cells.
- **U-A8. Recorded inbound streams** for lifetime replay and testing.
- **U-A9. Agents and R8.** When an agent's beliefs or results may become hypertime in the owner's
  store.
- **U-A10. A test harness.** One agent on one arena with a manual clock, mock skills installed by
  `install`, and assertions on emitted envelopes. The turn model makes this direct; the language for
  it is not yet specified.

______________________________________________________________________

## 18. Comparison

| Property         | SARL / Janus                | Jason (AgentSpeak)            | GOAL                         | vagent                                          |
| ---------------- | --------------------------- | ----------------------------- | ---------------------------- | ----------------------------------------------- |
| beliefs          | none built in               | Prolog-like belief base       | Prolog knowledge and beliefs | Vlog facts in the arena                         |
| goals            | none built in               | achievement and test goals    | declarative achieve goals    | achieve, perform, maintain                      |
| plan selection   | n/a                         | first applicable, overridable | rule order                   | first applicable untried, fixed                 |
| commitment       | n/a                         | single-minded                 | blind until achieved         | single-minded                                   |
| failure handling | exceptions                  | `-!g` failure plans           | goal remains                 | `PlanFailed`, reselection, `else`               |
| in-plan search   | none                        | none                          | none                         | goal-directed generators, reversible assignment |
| organisation     | contexts, spaces, holons    | via JaCaMo                    | none                         | contexts, spaces, holons (single owner)         |
| distribution     | Janus network               | via JADE or JaCaMo            | none                         | Spool-Mesh actors                               |
| maturity         | production compiler and IDE | mature interpreter            | mature interpreter           | specification; no compiler exists               |

Where vagent is weaker: no implementation, no tooling, two messages per send, single-owner spaces,
restricted suspension points, and reclamation that depends on quiescence. Where it is different:
in-plan search with automatic undo of reversible writes, deterministic turns, and agent state held
as quotable cells.

______________________________________________________________________

## Appendix: Versioning and Change History

**2.0 — Turn-bounded speculation, one actor per agent, BDI made explicit.** Rewritten after a
five-perspective review (technical writing, agent-oriented programming, ZigZag structure, Vortex and
Spool-Mesh, language ergonomics). Withdrawn from 1.0 as not built or not workable: `ArenaSlice`; the
`#Abort` and `#Trap` opcodes; the Ingestion Gate and Grant Renewal protocols; shadow cells and the
`d.trail`, `d.shadow`, `d.choice`, `d.source`-as-scan, `d.event` and `d.agent` dimensions;
topological GC; per-event `promote`; lifetime replay; "asks the C++ core for nothing"; the
`MeshLensChannel` as an event channel (it is a UI proxy-cell channel); fire-and-forget completion on
`ActorAck`; free-standing `commit`. Added: the rulings (§3), the turn and persistence model (§4),
fan-in-free structure through clone views (§5), beliefs, goal kinds, plans `for` goals, selection
functions, commitment, suspension points and a failure taxonomy (§6), performatives and scopes (§7),
a grammar that parses its examples with an explicit precedence table and newline rule (§8), the owed
C++ including two `mesh_actor.cpp` retention fixes (§11), refused alternatives (§13), and
conditional properties (§14).

**1.0 — The SARL metamodel on Vortex.** Initial draft.
