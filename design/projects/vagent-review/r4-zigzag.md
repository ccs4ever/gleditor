# Round 4 critique: ZigZag purist (vagent-agent-language.md v3.1)

"Doc" is `design/vagent-agent-language.md`; code paths are under `apps/common/xanadu/`.

## Status of round-3 BLOCKING and MAJOR findings

| R3      | Topic                                                                          | Status                                                                                                                                                                                                                                       |
| ------- | ------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 MAJOR | V11 leaves no reference to a cell (spawn placeholder, Intention, Timer, Space) | RESOLVED. V11/V16: identities are names or scalar ids, values copied; the spawn placeholder disappears because the name is known at once (§8.6).                                                                                             |
| 2 MAJOR | Handler order global vs one rank per source                                    | RESOLVED. §5.7 k-way merge on a `priority` scalar on each handler cell, priced O(k). The supertype link is still unplaced (finding 4).                                                                                                       |
| 3 MAJOR | Plan ordinals unstable under mount                                             | RESOLVED. Qualified names in `plan`/`tried` (§5.6, §5.7).                                                                                                                                                                                    |
| 4 MAJOR | Cursor scope grows; field access through `Resolve`                             | RESOLVED in design. Activation frames on `+d.stack`, adapter unlinks cursor `+d.vars`/`+d.stack` (§4.2 step 7, §5.8), field read/write rows in §9.1. Whether the walk of "the owner's `d.vars`" needs an opcode is the Vortex critic's lane. |

R3 minors 5-11 are addressed (`$turn`, `$` prefix, projectProvenance, subgoal-chain walks and
`rankTail` priced, clause layout, two peer cells). Finding 10 (find-or-mint on resume) is stated in
V10 and I accept it. Two of them are only half done and return below (findings 1 and 5).

## 1. MAJOR. `d.source` is used but nobody mints it in an agent arena; "eleven …

Full title: 1. MAJOR. `d.source` is used but nobody mints it in an agent arena; "eleven dimensions"
is wrong (§3 V10, §5.1, §5.2, §6.1, §10 genesis)

The doc reuses `d.source` for a belief's sources (`+d.source` rank on every clause, §5.2, §6.1) and
lists it under "reused with their existing meanings" (§5.1). Reused from where?
`VortexCore::initGenesis` mints

```text
d.grab d.step d.spin d.stack d.contract d.clone d.cursors d.cache d.vars d.values
d.pinning-cursors d.name d.stdlib d.clause d.scan d.stores d.role
```

(`vortex/vortex_core.cpp:49-66`) and no `d.source`. The only mints of it are `result_slice.cpp:34`
(a `Store`, not an arena) and `ArenaManifold::projectProvenance` (`zigzag/arena_manifold.cpp:138`),
which §5.1 itself says agent arenas never call. So in an agent arena the dimension does not exist
unless `std:agent` genesis mints it, and genesis is specified (V10, §10) to mint exactly the eleven.
The round-3 fix ("agent arenas never project provenance") removed the clash but also removed the
only source of the dimension. As written, the first `believe` has no dimension cell to link along.

Fix. Add `d.source` to V10 and §5.1's table (twelve dimensions, find-or-mint, "from a clause to the
peer cells that assert it", why not `d.member`/`d.holon`: those are space and holon membership), or
rename to avoid the clash with the provenance meaning and keep the count honest. Update every
"eleven" (§1 is silent, V10, §10 genesis, §16 R12, change history). Also list in §5.1 every other
reused dimension that `VortexCore` does not mint: `d.step` (see finding 2) is the same case if lists
use it.

## 2. MAJOR. The cell layout of a list, map, record or event-field value is never given …

Full title: 2. MAJOR. The cell layout of a list, map, record or event-field value is never given (§3
V11, §5.5, §5.6, §8.7)

V11 says "storing a value copies its cells", §15 prices "O(size) per stored list or map", §5.6
stores `goal` (name and copied arguments), `tried` (names), `step` (a list of ordinals), `waiting`
(kind, key, deadline), `$peers` (a map), `$timers` (entries). All of these are compound values. The
doc gives only the scalar case (name cell on `d.vars`, value cell on `+d.values`, §5.2) and the
record case for an intention's `frame` (a value cell whose own `d.vars` rank holds the fields,
§5.6). It never says how a `[T]` is laid out, or a `Map<K,V>`, so a reader cannot tell whether
elements go on `d.values` (they cannot: the value cell's `-d.values` is already its name cell, so a
list on `d.values` would be one rank with its own key), on `d.vars` (collides with record fields) or
on a new dimension. `workers[i]`, `*xs`, `!xs` and `workers ++ [x]` (§12.1) have no stated cost or
shape; `for x in e` keeps "the list and index in the frame" (§6.6) with no rule for how the index
addresses a rank.

The sibling answer already exists: Vlog §2/§3 lays a compound term out as a head cell with the first
argument on `+d.grab` and the rest chained on `+d.step` (Vlog doc line 81), and a belief's head term
is already that shape (§5.2, §6.1 `+d.grab`). Lists as compound terms of a distinguished head, and
maps as lists of two-field records (`key`, `value` on `d.vars`), cost no new dimension and make
`#Unify` work on list-valued belief arguments (`hit(query_id, text)` is fine today, `hits: [Str]` in
an event is not unifiable otherwise).

Fix. Add a short §5.9 "Values": scalar = name cell, `+d.values` value cell (R6); record and event
fields = value cell with a `d.vars` rank; list = a head cell with elements on `+d.grab`/`+d.step`
(Vlog's compound-term layout), so a list *is* a term; map = a list of `{key, value}` records. State
the costs: index and size are O(n) rank walks (priced next to the `rankTail` sentence in §15), `++`
copies. Add `d.step` and `d.grab` to the reused list in §5.1 and to finding 1's mint check (`d.grab`
and `d.step` are in `VortexCore`, so those two are fine).

## 3. MAJOR. "Ready" is state that the checkpoint does not carry, but the adapter …

Full title: 3. MAJOR. "Ready" is state that the checkpoint does not carry, but the adapter derives
its ready list from cells (§4.2 step 9, §5.6, §6.5, §4.2 closing paragraph)

§4.2 says ready intentions and pending timers "are cells and travel with" the checkpoint, and step 9
derives "the adapter's ready list ... from the intention and timer cells". §6.5 says intentions run
in "ready order ... oldest first", and §6.3/§6.5 "mark ready" at many points (adoption, `yield`,
`SubgoalEnded`, `PlanFailed`, plan selection). But the attributes listed in §5.6 are
`id kind goal plan tried step waiting delegator blocked_until frame`. There is no `ready` attribute
and no ready order. The order is therefore either rank position on `d.intention` (but a subgoal is
on `d.subgoal`, and a parent that becomes ready again keeps its place, not "oldest ready first"), or
lives only in the adapter, which is lost on `resume` and breaks §14's "a turn is a function of its
input and the agent's state". After a migration the ready intentions would be rediscovered in rank
order, a different schedule.

Fix. Hold readiness as a scalar attribute `ready` on the intention's `d.vars`: absent when not
ready, else the value of `$turn_count` at the moment it was marked (a monotone scalar already
reserved), and specify the ready list as "intentions with `ready`, ordered by it, ties by walk
order". No new dimension. Say in step 9 that the derived list is a function of cells only, and add
the resume test: checkpoint with two ready intentions, resume, assert the same order.

## 4. MINOR. Program-side structure is incompletely placed (§5.7, §9.2, §5.5)

(a) §9.2's diagram puts both the guard stream and the body stream on the handler cell's `+d.spin`. A
cell has one posward neighbour per dimension. Vlog §6.1 gives the shape: head on `+d.grab`, body on
`+d.spin` (Vlog doc lines 554-556, 618-620). Put the guard on `+d.grab` (it is the clause's head
test) and the body on `+d.spin`, priority on `d.vars` as now; the same for a plan's `where`. (b)
`extends` (§5.5, §8.5) needs a link from an event type's name cell to its supertype's. §5.7 says the
dispatcher merges "each of its supertypes" without naming the dimension. Same for `capacity extends`
(§5.3 installs one instance per extended capacity). A one-link rank on an existing dimension is
enough, or one of the dimensions already listed; name it. (c) How a behavior or skill instance (on
`d.behavior`, `d.skill`) names its program-side declaration is not stated; the same answer as
capacities works (content = qualified name); say so.

## 5. MINOR. The reserved field list is still incomplete (§5.6, §9.1, §5.5, §6.5)

Reserved names listed: `$name $peers $next_id $spawn_count $turn_count $timers $turn`. Used
elsewhere and absent: `$terminating` (§9.1 `terminate`), the storage of `parent` and `outer` (§8.8:
a name and a (host, space) pair; `self` is `$name`, the other two have no home), and the counters
`dropped_events`, `unhandled_events`, `late_deliveries` ("counted per type", §4.2, §6.5, §6.6),
which are state (per-type maps) and would otherwise be adapter memory outside the checkpoint. Fix:
add them to the §5.6 list (`$parent`, `$outer`, `$terminating`, `$stats` as a map of per-type
counters), and say whether the counters are meant to survive a checkpoint (they are cheap if they
do).

## 6. MINOR. `d.subgoal` is a chain, not "one link" (§5.1 table, §5.2)

The table says shape "one link": "an intention awaits at most one subgoal". But §5.6 and §6.3 chain
it (a maintain goal's restorer, which awaits its own subgoal). Each intention has one `+d.subgoal`
neighbour, so the whole chain is a rank rooted at the top-level intention, with `-d.subgoal`
pointing back to the parent. Say "rank (a chain; each cell's one posward neighbour is its subgoal)".
This also settles that `unlink i` repairs the chain by linking `i`'s parent to `i`'s subgoal only
when `i` is dropped with its subgoals, i.e. never: dropping `i` drops the whole tail.

## 7. MINOR. Per-intention checkpoint cost is not priced (§4.4)

Following house style (cells over packing), an intention is a name cell and a value cell per
attribute: about ten attributes, each with a name/value pair, plus the frame record, plus the
`tried` and `step` lists (finding 2) — on the order of 25-40 cells and as many links. §4.4 prices a
belief but not an intention, and the operations cap is 65,536 until §11.3 (e). One sentence: "an
intention costs about N operations at rest; one thousand live intentions is a third of the cap".
Compute N once the layout in finding 2 is fixed.

## 8. MINOR. Diagram legend wording (§5.2)

The legend defines `(+d.x)` as the posward neighbour. The line
`[instance] ─(-d.clone … master)─ [skill frame ThrusterNav]` reads as the instance's negward
neighbour on `d.clone` being the master, which is true only for the first instance; later instances'
negward neighbour is the previous instance and the master is `cloneMaster` (the rank's negward end,
`zigzag/arena_manifold.cpp:1004-1008`). Write "(-d.clone, rank head = master)". Cosmetic, but the
whole point of the diagram is the two-degree rule.

## Things checked and found sound

- Agent state is one cell with ranks off it; no state cell links into the program (V12); the peer
  cell duplication (two cells per child on `d.holon` and `d.member`) respects
  one-neighbour-per-direction.
- A skill instance sits in one rank per dimension (`d.skill`, `d.clone`, one `d.capacity` link) and
  the frame is the head of its `d.clone` rank, matching `cloneMaster`.
- "A cell exists only where something minted one" and "arena frees only by `release` truncation,
  `compact()` frees no cell" (§4.4) agree with AGENTS.md and `arena_manifold.hpp:656-672`; "unlinked
  and dead until reclamation" is the correct description of arena retraction, and the
  checkpoint-resume reclamation story is consistent with R8 (the promotion goes to the value's own
  store, not a visited one).
- R7: the doc writes no `SetLink` into a user store; the checkpoint goes through `promote`.

VERDICT: NOT CONVERGED
