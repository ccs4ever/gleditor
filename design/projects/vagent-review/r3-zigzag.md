# Round 3 critique: ZigZag purist (vagent-agent-language.md v3.0)

"Doc" is `design/vagent-agent-language.md`; code paths are under `apps/common/xanadu/`.

## Status of round-2 BLOCKING and MAJOR findings

| R2      | Topic                                                      | Status                                                                                                                                                                                                                                          |
| ------- | ---------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 MAJOR | Capacity master link drags the program into the checkpoint | RESOLVED. V12 and `d.capacity` to a per-instance name cell; the diagram's closing sentence is true.                                                                                                                                             |
| 2 MAJOR | Checkpoint is an implicit `promote` with a ceiling         | RESOLVED. V9, §4.4, §11.3 (e), §16 R7/R8 state it. Verified `limits.operations = min(.., grant->maxCells)` at `vortex/mesh_actor.cpp:259`.                                                                                                      |
| 3 MAJOR | Timers and intention identity outside cells                | RESOLVED. §5.6 (`id` scalar, absolute-time deadline cells, heap derived). Caveat in finding 8.                                                                                                                                                  |
| 4 MAJOR | Internal event queue has no structure                      | RESOLVED. `d.pending`, §4.3 row "no (always empty)", `TurnAborted` replaced by the recovery turn.                                                                                                                                               |
| 5 MAJOR | Handler, plan and goal registries undefined                | PARTLY. §5.7 reuses `d.clause` (right: Vlog §6.1 is exactly predicate-and-clauses). Two defects remain: findings 2 and 3.                                                                                                                       |
| 6 MAJOR | Value aliasing under the two-degree rule                   | RESOLVED for values (V11). V11 leaves no way to name a cell at all: finding 1.                                                                                                                                                                  |
| 7 MAJOR | `d.clone` frame sharing                                    | RESOLVED. Frame as clone master, instances as views, `cloneMaster(instance, d.clone)`. Verified: `ArenaManifold::cloneMaster` is `rankEnd(.., NEG)` (`zigzag/arena_manifold.cpp:1004-1008`), so the head of the rank (the frame) is the master. |

## 1. MAJOR. V11 copy-on-store leaves no reference to a cell; the spawn placeholder …

Full title: 1. MAJOR. V11 copy-on-store leaves no reference to a cell; the spawn placeholder,
`Intention`, `Timer` and `Space` values break (§3 V11, §8.6, §6.3, §8.7)

V11: "Storing a value into a field, frame variable, event field, belief argument or outbox entry
copies its cells." §8.6: `spawn` returns "a peer" which is "an unfilled peer cell ... stored and
used in the same turn: messages staged to it reference the cell, and the adapter ... fills the cell"
(doc lines 900-903). Both cannot hold. `var child := spawn Worker()` stores a copy of the *empty*
cell; the adapter fills the original (`ArenaManifold::setContent`, `zigzag/arena_manifold.hpp:576`
makes that possible) and the stored copy stays empty for the agent's life. Staged messages
"reference the cell" but an outbox entry's destination is itself a stored value
(`emit ... for [dest]` copies the list, §12.2), so it is a copy too. The same shape recurs: `adopt`
"returns its handle" (§6.3) and `drop i`, `Timer` (from `Schedules.after`) and `Space` are typed
values (§8.7). A copied intention is a copy of the whole intention subtree (frame, waits), and a
shared one violates V11's own two-degree argument. §5.6 says messages name intentions by `id`, which
implies `Intention` is the id, but nowhere is that said.

Fix. (a) Rule in V11: the types `Agent`, `Intention`, `Timer`, `Space` are *names*, not cells: an
`Intention` is its scalar `id`, a `Timer` its scalar `id`, a `Space` a (host peer, name) pair, and
operations on them look the cell up by walking the rank (O(live intentions), already priced in §15).
(b) For the spawn placeholder, mint the placeholder's content as a *spawn slot number* (a scalar
unique within the agent, from the same counter field as intention ids) and let the adapter, after
the spawn, rewrite every peer cell in the agent's state whose content is that slot (walk `d.holon`,
`d.member`, fields); or better, derive the child's `ActorPid` deterministically before the turn ends
(check `nextSpawn_` and the content hash at `mesh_actor.cpp:303-313`; if the pid is a function of
the request, the peer cell can be filled at `spawn` time and nothing is staged). State which. (c)
Add a test: `var c := spawn W(); emit E{} for [c]` across a checkpoint.

## 2. MAJOR. Handler order is stated as global but the dispatcher walks one rank per …

Full title: 2. MAJOR. Handler order is stated as global but the dispatcher walks one rank per source
(§5.7, §6.5, §8.5, §9.3)

§6.5 pseudocode: `for handler in handlers(e) by (priority desc, declaration order)`. §5.7: the
handlers of an event type form a rank on `d.clause` ordered by priority then declaration, "a
behavior's program contributes its own ranks, and the dispatcher walks the agent's then each mounted
behavior's". That is a concatenation of per-source sorted ranks, not a priority order. §8.5 gives
`ObstacleAvoidance` a `priority 1` handler and the agent a default (0) handler on adjacent events;
with concatenation, the agent's priority-0 handler for the same event runs before the behavior's
priority-1 one. `extends` (§5.5, a handler for `Alarm` also matches `TargetDetected`) adds a second
axis: the supertype's rank is another rank to merge. Also, `priority` and the declaration index are
nowhere stored as cells (the rank position carries them), so the dispatcher cannot compare across
ranks without reading them off something.

Fix. Either (a) say the dispatcher does a k-way merge across the agent rank, each mounted behavior
rank and each supertype rank, comparing a `priority` scalar cell hung off each handler cell on
`d.vars` (program side, read-only, no V12 issue), with ties broken by source order then position; or
(b) restrict priority to "within one source" and delete the claim of global order from §6.5 and
§8.5. (a) is the better semantics; price it (O(k) per step, k = sources).

## 3. MAJOR. Plan ordinals are unstable under `Behaviors.mount`; plans have names and …

Full title: 3. MAJOR. Plan ordinals are unstable under `Behaviors.mount`; plans have names and V12
already matches by name (§5.6, §5.7, §3 V12, §10)

V12: "plans and handlers by ordinal ... Ordinals are valid only for the same `programRef`". But the
plan list is "agent, then behaviors in mount order, then imported modules" (§6.2), and `Behaviors`
has `mount`/`unmount` at run time (§10). Mounting or unmounting a behavior changes the list while
live intentions hold `plan` and `tried` ordinals: the next reselection skips or retries the wrong
plans, and a checkpoint resumed under the same `programRef` but a different mount history
misattributes them. Separately, §5.7 says "an ordinal is a position on such a rank", but the plan
list for a goal spans several ranks (one per source module cell), so no single rank position is the
ordinal.

Fix. Identify a plan in an intention by its qualified name (`module:Agent.plan_name`, the declared
`plan direct for ...` name, with an implicit suffix for duplicates), held as a text span in `plan`
and as a list of such names in `tried`. This is the same device V12 uses for capacities, costs a
name comparison, and removes the programRef/mount-history dependence. Keep ordinals only as a
transient cache within a turn. Update V4, V12, §5.6, §5.7.

## 4. MAJOR. Local scope lives on the lifetime cursor and only grows; field access …

Full title: 4. MAJOR. Local scope lives on the lifetime cursor and only grows; field access lowers
through `Resolve` of the wrong scope (§4.2, §9.1, §9.3, §15)

`#Bind` mints a name cell and a value cell and prepends the pair to the *cursor's* `+d.vars` rank
(`vortex/vortex_vm.cpp:705-728`); `#Resolve` walks that rank (`:731-745`). vicon lowers both `:=`
and `<-` to `#Bind` (`vicon/compiler.cpp:356-361`) and every call to a `Bind` per argument plus one
for the continuation (§9.2). A mesh actor has one cursor for life (`mesh_actor.cpp:290`,
`spawnCursor`). Nothing in the doc ever truncates that scope. A successful turn's
`clearChoicePoints()` keeps its writes (§4.2 step 7), so after n successful statements the cursor's
`d.vars` rank holds O(n) cells, every `Resolve` that misses (any field or capacity name, since those
are not bindings in this scope) walks all of them, and the rank grows without bound. §15 does not
list this. It is a ceiling users would work around only by restarting the agent.

Second, §9.3 lowers a field increment as `Resolve targets_seen → Add 1 → Bind`. `Resolve` looks in
the cursor's scope, not the agent cell's `d.vars` rank, and `Bind` shadows rather than writes. A
field update needs a different lowering (resolve against the owner cell's rank, then `#Value` on the
field's value cell, which is a trailed write when older than the mark). The cross-walk (§9.1) has a
`var` row for declaration but none for assignment, read or call of fields and none for locals.

Fix. (a) Add to §4.2 step 7: "the adapter detaches the cursor's `+d.vars` rank
(`unlink(cursor, vars, POS)`) at the end of every turn, success or failure", and a §14 property that
scope size is bounded by one turn's bindings. (b) Add rows to §9.1 for local `let`/`:=` (a `Bind` on
the cursor scope), field read/write (walk the owner's `d.vars`, `#Value`), and skill-field access
(via `cloneMaster`). (c) Say whether the executor's `Resolve` takes the owner cell as an explicit
scope operand; if it does not today, that is a second owed change and §1's "one opcode" is
understated. I could not confirm either way from `OpcodeKind` (`vortex_vm.hpp:26-96`): there is no
field-store opcode other than `Value`.

## 5. MINOR. The turn's event cell has no home; turn time is written before the cell …

Full title: 5. MINOR. The turn's event cell has no home; turn time is written before the cell exists
(§4.2, §4.3, §5.5)

§4.3 lists "The turn's event cell: unlinked at turn end", but §5.1 gives it no dimension, so there
is nothing to unlink. Step 2 writes the turn time "into the turn's event cell" while step 4 creates
the event cell; and a wake turn on a ready intention (step 4: "or pick the ready intention") has no
event cell at all, yet `Clock.now()` returns "the turn time written in step 2" (§10). Fix. Name the
turn cell: a single `turn` cell on the agent's `d.vars` (reserved name, see finding 6) whose
`d.vars` holds `time` and, for message turns, the event cell on `d.values`; created in step 2,
detached in step 8. Then wake turns have time too, and §4.3's row is accurate.

## 6. MINOR. Hidden fields on the agent cell's `d.vars` collide with user fields (§5.6, §10, §5.5)

`timers` ("owned by `std:agent`", §4.3, §10), the intention id counter ("a counter field on the
agent cell", §5.6) and the emission counter (`ordinal`, §5.5) all live on the agent's `d.vars` next
to user `var`s and agent parameters. `var timers: Int` (or a parameter named `timers`) collides with
the same `exportSymbol` one-name-per-symbol rule that made round 2's finding 8. §5.5 reserves event
field names; nothing reserves these. Fix. List the reserved agent field names in §5.6 (prefix them,
e.g. `$timers`, which the lexer rejects in user identifiers) and add the checker rule to §8.9's
static restrictions.

## 7. MINOR. `d.source` is a provenance dimension in the arena already (§5.1, §6.1)

The doc cites `result_slice.cpp:34` for `d.source` as provenance. `ArenaManifold::projectProvenance`
(`zigzag/arena_manifold.cpp:138-139`) also mints `d.source` and inserts it into `provenanceCells_`,
and `promote` refuses to discover provenance cells (`:1767`), so a `d.source` link on an arena that
had provenance projected is not promoted. Nothing calls `projectProvenance` outside the class today
(grep over `apps src tests`), so the clash is latent. Fix: state that agent arenas never project
provenance, or name the belief-source dimension `d.told` and count it (twelve). The first is cheaper
and sufficient; it needs a test (checkpoint an agent with a sourced belief, resume, assert the
sources).

## 8. MINOR. Derived views must walk `d.subgoal` chains; tail attachment is O(n) (§5.1 …

Full title: 8. MINOR. Derived views must walk `d.subgoal` chains; tail attachment is O(n) (§5.1,
§5.6, §4.2 step 9)

§5.6: a subgoal intention "is reached only along its parent's `+d.subgoal`". So the ready list, the
timer heap, end-of-turn reconsideration (§6.4) and `intentions()` (§10) must walk every top-level
intention *and* its subgoal chain; the doc says "rebuilt from the intention and timer cells" without
this. Also `adopt` "appends to `d.intention`": a rank has no tail pointer, so each append walks the
rank (`zigzag::rankTail`), and the same for every `d.outbox`, `d.pending` and `d.skill` append. For
a busy agent that is O(live intentions) per adopt, which is the same order as the per-turn
reconsideration, so it is acceptable, but §15 should say so. Fix: one sentence each in §5.6 and §15.

## 9. MINOR. Belief clause layout and the arena/limbo difference are not stated (§5.2, §6.1, §5.7)

Vlog §6.1 hangs a clause's head on `+d.grab` and its body on `+d.spin`; the diagram in §5.2 shows
`[at(3,4)]` as a single cell with a `d.source` rank. State that a fact is a clause cell with its
head term on `+d.grab`, no body, and sources on `+d.source`, so `d.grab` is part of the checkpoint
and the promotion cost per belief is the term's cells plus the sources (price it in §4.4). Vlog
§6.2's "retract is a link to limbo, nothing reclaimed" is a Store-regime statement; in the arena the
retracted clause is unlinked and dead (§4.4), which §6.1's "no history of them" half-says. One
sentence.

## 10. MINOR. Resume must find-or-mint the eleven dimensions (§3 V10, §4.4)

V10: `std:agent` "mints ... when it loads". After `resume` the state's links are keyed by dimension
cells that `promote` carried as plain cells (`discover(edge.dim, ..)`,
`arena_manifold.cpp:1761-1763`; only edges *on* `d.dims` are skipped, `:1756-1758`). Loading
`std:agent` into the resumed arena must not mint a second `d.skill`.
`VortexCore::mintNamedDimension` does look up through the base first
(`vortex/vortex_core.cpp:17-19`), which suffices only if `Manifold::dimensionNamed` finds a
dimension cell not on the `d.dims` rank; I did not verify that path (`zigzag/manifold.cpp:785-815`
reads `dimensions()`, which I did not trace). Fix: say that genesis is find-or-mint, and add a
resume test that asserts exactly one cell named `d.skill` after load.

## 11. MINOR. Is a child's peer cell one cell or two? (§5.1, §5.4, §8.6)

A child appears on the parent's `d.holon` rank and in the parent's `inner` space on `d.member`. A
cell may sit in one rank per dimension, so one cell on both is legal; V11 says copies. The
difference matters for `forget`/exit: removing from `d.member` must not disturb `d.holon`. Say "two
cells" (V11) in §5.4.

VERDICT: NOT CONVERGED
