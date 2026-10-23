# Round 2 critique: ZigZag purist (vagent-agent-language.md v2.0)

Paths are relative to the repo root. "Doc" is `design/vagent-agent-language.md`.

## Status of round-1 BLOCKING and MAJOR findings

| R1         | Topic                                                    | Status                                                                                                                                                                                                                                          |
| ---------- | -------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 BLOCKING | Fan-in on `d.capacity`, `d.context`, `d.trail`, `d.grab` | RESOLVED for capacities (V10, §5.3); the others are deleted. Residual fan-in through values is finding 6 below.                                                                                                                                 |
| 2 BLOCKING | `link()` used as insert                                  | RESOLVED (§5.1 names `insertIntoRank` / `rankTail`+`link`; removal described correctly, checked against `arena_manifold.hpp:582-617`).                                                                                                          |
| 3 BLOCKING | Cross-arena ranks                                        | RESOLVED (V1, handles as values, no `-d.holon`, §13 row 1-2).                                                                                                                                                                                   |
| 4 MAJOR    | Topological GC                                           | RESOLVED as a withdrawal (§4.4). The replacement (checkpoint and resume) has its own structural problems, findings 1 and 2.                                                                                                                     |
| 5 MAJOR    | Persistence vs R8                                        | PARTLY. §4.3 table and V8 added. But the checkpoint is itself an implicit `promote`, which V8, §14 and §16 do not admit (finding 2).                                                                                                            |
| 6 MAJOR    | Speculation duplicates the arena                         | RESOLVED (V2, V3; `d.trail`, `d.shadow`, `ArenaSlice` gone).                                                                                                                                                                                    |
| 7 MAJOR    | Dimension proliferation, no genesis                      | RESOLVED in the count (ten, §5.1 with a "why not existing" column, R12 via V9). The `d.intention` vs `d.cursors` refusal reason (a cursor has a program counter, V4) is accepted. Missing structure for handlers and plans remains (finding 5). |
| 8 MAJOR    | State in payloads                                        | PARTLY. Events, fields, priority are now cells (§5.5, §9.3). Values aliasing (6), namespace collisions (8), queue and timers (3, 4) remain.                                                                                                     |

## 1. MAJOR. The capacity master link drags the whole program into the checkpoint …

Full title: 1. MAJOR. The capacity master link drags the whole program into the checkpoint (§5.3,
§4.3, §4.4, §9.1)

`checkpointView` hands the arena and `stateRoot` to `publishValue`, which calls
`promote(store, parent, arena, root, {limits.operations})` (`vortex/mesh_value.cpp:169`).
`promote`'s contract is "only what is reachable from [root] along any dimension is promoted"
(`zigzag/arena_manifold.hpp:~820-830`). §5.3 says a capacity is "a capacity cell exported as a
symbol of its module on the module's `d.vars` rank" (that is `VortexCore::exportSymbol`,
`vortex_core.cpp:117-125`), and each skill instance reaches it through
`+d.capacity -> view -(-d.clone)-> master`. Reachability is undirected over dimensions, so from the
agent cell the walk goes view, master, the module's `d.vars` rank (every export of the module), each
export's `d.values` entry opcode and its `d.spin` streams, and the module's `-d.stdlib` neighbour
and thence `home_` and `d.dims`. The "Compiled program: named, not copied" row of §4.3 is therefore
false for any agent that has installed a skill. §5.6 was careful (ordinals, "nothing in an intention
is a CellRef into the program") and §5.3 undoes it.

Fix. A capacity needs no link into the module. At `install`, mint a *capacity key cell* in the
agent's own arena (content: the capacity's qualified name, an R6-style text span; one per capacity
per agent, found by walking `d.skill` or kept on a rank of the agent cell). Skill instances link to
views on the key's `d.clone` rank, dispatch compares `cloneMaster(view)` to the key looked up by
name. The module symbol and the key are matched by name, as ordinals are. Add an invariant to §5.1:
"no agent-state cell has a link of any dimension to a cell of the compiled program or `home_`", and
a test that publishes a checkpoint of an agent with two skills and asserts the operation count is
independent of program size.

## 2. MAJOR. Checkpointing is an implicit `promote` with a 65,536-operation ceiling …

Full title: 2. MAJOR. Checkpointing is an implicit `promote` with a 65,536-operation ceiling; V8,
§14, §16 say otherwise (§3 V8, §4.3, §4.4, §16)

V8: "vagent never calls `promote` implicitly". §14: "agent state is not in the spool". §16 R7:
"vagent writes no Structure operations". But the only reclamation route the doc offers (§4.4) and
the only migration route (§4.3 "In the checkpoint root: yes") both go through `publishValue`, which
promotes the reachable arena into a Store: one operation per cell, one per link, a splice per extra
span. `ValueLimits::operations` defaults to 65,536 (`vortex/mesh_value.hpp:28`), and `resume` clamps
it further to `grant->maxCells` (`mesh_actor.cpp:~255`:
`limits.operations = std::min(limits.operations, grant->maxCells)`). An agent whose live structure
exceeds that count (roughly a few tens of thousands of cells, since every link is an operation) can
neither migrate nor be compacted. That is a ceiling users cannot work around except by forgetting
beliefs, which the house rules reject, and the doc's own reclamation story depends on it (U-A1 only
discusses the sweep alternative).

Fix. (a) Say plainly in V8/§14/§16 that a checkpoint is a promotion into the *published value's own
transient Store* (not a visited store, so R8 is not violated) and that R7's "no Structure
operations" holds only for the user's stores. (b) Price it in §4.4 and §15: live structure is
bounded by `ValueLimits::operations`, with the count formula (cells + links). (c) Add a setting for
the limit (it is already a struct field) and list lifting it in §11.3 as owed, or record it as an
open question beside U-A1. (d) Reconsider U-A1's second option (copy to a fresh arena) as the
primary route: it avoids the Store round trip and the ceiling.

## 3. MAJOR. State kept in C++ structures that should be cells: timers, intention …

Full title: 3. MAJOR. State kept in C++ structures that should be cells: timers, intention identity
(§6.6, §11.2, §9.1)

The adapter "keeps a timer heap" (§11.2.5). Deadlines on `ask` and delegated `achieve` are the
mechanism for §7.4 and §14 ("every wait is bounded"). `checkpointView` carries the program ref,
spawn request and completed deliveries, nothing else (`mesh_actor.hpp:94-100`). After a resume the
heap is empty, and an intention whose `waiting` field names a correlation id and a deadline waits
forever; the reply may also have been consumed by the old node. This is the "state hides outside the
cells" failure, and it also breaks the stated property.

Likewise, `Resume(i)`, `SubgoalEnded(i, ...)`, `Reply(i, ...)` and timer messages name an intention
`i`. The doc never says how: if by `CellRef`, refs are different after resume (the arena becomes a
copy-on-write view of a rebuilt `Manifold`, `mesh_actor.cpp:~276`).

Fix. (a) Timers are cells: an intention in `waiting` state holds a scalar `deadline` cell (absolute
`Time`, R6) in its frame, and the adapter's heap is *derived* from the `d.intention` rank after
every turn and after `resume` (rebuild = one walk). (b) Each intention gets a monotonically assigned
scalar `id` cell from a counter on the agent cell's `d.vars`; messages carry that id, never a
`CellRef`. (c) State in §6.6 that resume re-arms timers from the frames, and add the check to the
§14 property "every wait is bounded".

## 4. MAJOR. The internal event queue has no structure; `TurnAborted` is queued under …

Full title: 4. MAJOR. The internal event queue has no structure; `TurnAborted` is queued under the
mark it must survive (§6.1, §6.5, §6.7, §4.3, §5.1)

Belief events (`+at/3`), `GoalAchieved`, `GoalFailed`, `GoalDropped`, `PlanFailed`, `ChildDown` and
`TurnAborted` are all "queued". The mailbox is the runtime's C++ deque (§2), so these are not there.
Nothing in §4.3 or §5.1 gives them a rank. If they are cells minted during the turn, then (i) they
are dead cells after the turn like the event cell, and (ii) `TurnAborted` ("`release(m0)`: the whole
turn is undone; `TurnAborted` queued for the next turn", §6.7) cannot be a cell written under `m0`,
because `release` truncates it. It must be written after the release, or sent as a self-message by
the adapter. Also, `belief_event_depth` rounds (§6.1) need a defined queue discipline (FIFO rank,
consumed head).

Fix. Say which: (a) a transient `d.event` rank is not needed if in-turn events are a VM-local list
that dies with the turn (then say they are not part of any checkpoint, and `TurnAborted` is an
adapter self-message delivered after `release`); or (b) mint the queue as a rank on the agent cell
and add it to the §4.3 table. Option (a) fits V2 and adds no dimension. Update §6.7 accordingly.

## 5. MAJOR. Handler, plan and goal registries are rank structure the doc never defines …

Full title: 5. MAJOR. Handler, plan and goal registries are rank structure the doc never defines
(§6.2, §6.5, §9.1, §9.3)

§6.2 defines "the plan list" per goal across agent body, behaviors in mount order and imported
modules. §9.3 dispatches over "each handler cell matching the event type name, in (priority,
ordinal) order", with `priority` and `ordinal` on `+d.vars` of the handler program cell. §6.5 says
priority is descending, §9.3 does not say. Which rank do those handler cells sit on, off which cell,
and which dimension? §5.1 lists ten dimensions and none carries handlers, plans or goals; `d.vars`
is name-keyed (`exportSymbol` makes one symbol per name), so "many plans for one goal" and "many
handlers for one event type" are not representable there. An implementer cannot lower §9.3 from this
text. Two further structural points: mounting a behavior needs its handlers visible to the
dispatcher without copying them into the agent (a rank walk across `d.behavior` instances, then the
behavior's program rank), and `ordinal` is the plan list position that intentions store in `plan`
and `tried`; it must be stable across recompilation or checkpoints from an older program break.

Fix. Add to §5.1 a "program-side structure" paragraph or table: for each of handler, plan and goal,
the dimension (new, or `d.clause`-style reuse with justification), the head cell, and the order.
Suggestion: one program-side rank per kind off the agent or behavior program cell (`d.handler`,
`d.plan`), both in declaration order, with plans grouped by goal name through a name cell, so the
plan list for a goal is a walk and the ordinal is the position. Count them in V9 (twelve) or justify
reuse. State that the ordinal is checked against a program digest and a mismatch refuses the resume.

## 6. MAJOR. V10 fixes capacity fan-in only; values reintroduce it (§3 V10, §5.5, §5.6, §8.7)

Everything stored is a `+d.values` link from a name cell (as `exportSymbol` does). A cell has one
negward `d.values` neighbour. The doc routinely names one value twice: `let a := xs` then
`var b := a`; an event field assigned into an intention's `goal` args (§5.6) and into a belief
(`believe hit(r.query_id, h)`); `emit` fields copied from locals; a list passed as a skill
constructor argument and then stored. The doc states copy semantics for handles only ("two handles
naming one agent are two cells"). Lists and maps are "std:collections structures of cells, so their
elements are cells and are quotable" (§8.7), i.e. aliasable. Under the two-degree rule, binding the
same value cell into a second place evicts the first, silently, with `link()` (that is the exact
mechanism of the round-1 finding).

Fix. Add a ruling V13 (or extend V10): every store of a value into a field, frame variable, event
field, belief argument, or outbox envelope is a *deep copy* of the value's cells (scalars by R6, so
one cell), unless the source is explicitly shared, in which case the store links to a fresh clone
view of the source (reads go through `cloneMaster`). State the default (copy), its cost (O(size),
paid by the writer), and that mutation of a shared value is visible to every view. This also settles
the aliasing semantics of `Map<K,V>` and `[T]`. List the compiler obligation in §9.1.

## 7. MAJOR. "Instances share the skill's field frame through `d.clone`" is not what …

Full title: 7. MAJOR. "Instances share the skill's field frame through `d.clone`" is not what
`d.clone` does (§5.3)

Per the Vortex document §5.2 and `Manifold::cloneMaster`, a clone "holds no content of its own and
reads its master's" (`vortex-hyperstructural-runtime.md:748`; `vortex_core.cpp:328`). It shares
*content*, not the ranks hanging off the master. A skill's fields are a `d.vars` rank off a cell
(§5.1), so a second instance that is a clone of the first does not see the first's `d.vars` unless
every reader first resolves `cloneMaster(instance)` and walks the master's `d.vars`. That is
implementable but is not said, and it creates three further problems: (a) which instance is master
is unspecified (the first appended?); (b) removing the master from the `d.clone` rank (uninstall of
the capacity it served, or `install` replacing it) silently changes what every remaining instance
reads, exactly the hazard the Vortex doc §5.2 warns of; (c) `install` of a skill that implements
several capacities then re-`install` for just one removes only some instances (§8.6 "replaces the
skill for each capacity S implements", §5.3 "removes the old instance"), splitting the frame.

Fix. Introduce one *skill frame cell* per installed skill, off the `d.skill` rank only as the master
of a private clone rank, never removed while any instance exists. Fields hang on its `d.vars`. Each
per-capacity instance is a view on that rank (posward), and readers do `cloneMaster(instance)` then
`d.vars`. Uninstall removes the instances and finally the frame. Reject, or define, the partial
replacement case in §8.6. When a skill instance is removed, also remove its view from the capacity
key's `d.clone` rank (or the key's rank grows with every swap, §12.2).

## 8. MINOR. Reserved names in `d.vars` (§5.5, §5.6)

An intention cell keeps `goal`, `plan`, `tried`, `step`, `waiting` and the user's plan-local
variables on the same `d.vars` rank. A plan declaring `let step := 1` or `let goal := ...` collides.
Event cells add implicit `source` and `space`, so a declared event field named `source` (very
common) collides with the runtime's. Fix: the checker rejects declared names in the reserved set,
and §5.5/§5.6 list that set; or put system attributes on a distinct rank off the intention (one more
dimension, consistent with "more dimensions, not overloaded ones").

## 9. MINOR. Intention placement in the diagram vs the text (§5.2, §6.3)

§6.3: `adopt` "appends it to `d.intention`" and `achieve G` "adopts G as a subgoal, links on
`+d.subgoal`". So a subgoal is on both ranks. §5.2 draws `intention Reach` only under `+d.subgoal`.
Pick one and draw it: if both, say that `drop` of a parent walks `d.subgoal` and that the top-level
walk of `d.intention` must skip subgoals (or not). Also say whether a subgoal's parent is found by
walking `-d.subgoal`.

## 10. MINOR. "handle" collides with the existing `OpHandle` / `handleTarget` (§2)

`ArenaManifold::handleTarget` / `Manifold::handleTarget` (`manifold.hpp:440`) return the target
operation of an `OpHandle` cell; `ValueKind` already has that case. The Glossary's handle (a cell
holding an encoded `ActorPid`) is a different thing. Rename to "peer cell" or "actor reference", or
state the difference once.

## 11. MINOR. Dead-cell growth per message is by design (§4.2, §4.4, §15)

Every message mints an event cell with its fields, belief-event cells, outbox envelopes and their
fields, and every message is answered by an `ActorDeliverValue` which costs another turn and another
event cell; none is freed before reclamation. Given finding 2's ceiling, a busy agent reaches the
checkpoint ceiling by garbage, not by live data (the reachability walk from the root only promotes
live cells, but fuel `maxCells` counts all). Fix: promote the "copy to fresh arena between turns"
option to the main line (U-A1), with a trigger setting (dead-cell ratio), and say that `maxCells` is
a lifetime figure unless re-armed per turn (it is, §4.2).

## 12. MINOR. `capacity ... extends` and `d.space` cardinality (§8.9, §5.1, §5.4)

`CapacityDecl` has `extends IdentList`, but §5.3 gives each instance one `+d.capacity` link. A skill
implementing `Nav extends Base` must answer `Base` calls too; state that the compiler emits one
instance (view) per transitive capacity. Separately, "every agent hosts exactly one space at
creation" (§5.4) while `d.space` is a rank "of spaces it hosts" (§5.1); say whether `Space` values
from `std:agent` create more, and who may.

## 13. MINOR. Provenance link on facts (§6.1)

`tell` stores the sender's handle "on `+d.source`" of the fact. Correct per `result_slice.cpp:34`,
and fan-in free only because handles are copied per fact (each fact gets its own handle cell). Say
so there; it is a use of finding 6's rule.

VERDICT: NOT CONVERGED
