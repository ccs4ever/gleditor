# Round 1 critique: ZigZag purist (vagent-agent-language.md)

Paths below are relative to the repo root. "Doc" is `design/vagent-agent-language.md`.

Summary: the document's central structural claim (§2.1, §2.3, §6.5, §10.5: "strict 2-degree
invariant, no branching pointers") is not satisfied by its own diagrams. Several lowerings use
`link()` as if it spliced, when `ArenaManifold::link` evicts. The GC section describes a mechanism
that is not built and mis-defines "isolated". The arena/persistence story contradicts R8. Findings
below are ordered by severity.

## 1. BLOCKING. Fan-in on a single dimension violates the two-degree rule (§2.3, §6.5, §10.4, §2.2)

A link is reciprocal: `A.pos(d) = B` implies `B.neg(d) = A`, so a target can be pointed at along `d`
by exactly one source per direction. The doc draws many-to-one pointers in four places:

- **`d.capacity`** (§2.2: "points from a skill cell or behavior cell to the abstract capacity
  contract cell"; §2.3 `S1 -> +d.capacity -> C1`; §6.1 `link(skill_cell, d.capacity, +1, cap_id)`).
  Two skills (or a skill and a `requires` behavior, as in §9.2 and §10.6, where
  `behavior DataReporter { requires Transport }` and `skill ... implements Transport`) cannot both
  reach `C.neg(d.capacity)`. The second `link` evicts the first
  (`zigzag/arena_manifold.hpp:582-592`: "evicts whatever either end held"). Worse, §8.1 chains the
  *capacity cells themselves* along `+d.capacity` inside `std:agent`, so `C` already has a
  `neg(d.capacity)` neighbour (the previous capacity), and the skill pointer cannot fit at all.
- **`d.context`** (§6.5: "All child agents link along `-d.context` to this inner context"). The
  inner context cell has one `+d.context` slot; N children cannot all have it as their negward
  neighbour. §6.1 `link(child_cell, d.context, -1, inner_ctx)` repeats it for every `spawn`.
- **`d.trail`** (§10.3: shadow cell "records a `-d.trail` link to the master belief cell"). Two
  concurrent intentions shadowing the same belief is the stated use case; the master has one
  `+d.trail` slot.
- **`d.grab`** in the §10.4 diagram (`Trailed Shadow Cell -(+d.grab)-> Belief Master Cell`): same
  fan-in on the master, and it contradicts §2.2's own "d.grab ... never overloaded for object
  pointers".

The doc then correctly applies the clone-view fix in exactly one place (§10.5 scan cursors) and
nowhere else.

**Fix.** Use the repository's idiom for "many things name one thing": each referrer owns a *view
cell* on the target's `d.clone` rank (as `d.meta-dims` does, `store-slice-convergence.md` R12), and
the referrer links to its own view. Dispatch compares `cloneMaster(view) == C`. Concretely: skill
`S` carries `+d.capacity -> V_S` where `V_S` is a fresh member of `C`'s clone rank, posward of the
master (never negward: linking negward of the head changes what every member reads,
`vortex-hyperstructural-runtime.md` §5.2). Do the same for `-d.context` (child -> view of the
context cell) and for shadow -> master. Add a fan-in check to §2.1: "no lowering may have more than
one source point at a cell along a (dim, direction); fan-in is always through clone views". Then
re-audit every diagram against it.

## 2. BLOCKING. The lowerings use `link()` as if it inserts; it replaces (§1.2, §2.4, §6.1, §9.1)

`ArenaManifold::link` "evicts whatever either end held" (`zigzag/arena_manifold.hpp:582-592`), and
`VortexCore::link`'s allocation form (`vortex/vortex_core.cpp:141-147`) is just `makeCell()` +
`arena_.link(cell, dim, dir, created)`. So:

- §6.1 `agent`: `link(pop_cell, d.agent, +1, -1)` cuts the population rank at `pop_cell`: the old
  `+d.agent` neighbour is orphaned.
- §6.1 `behavior`, `uses`, `spawn`: `link(agent, d.behavior|d.skill|d.holon, +1, new)` each sever
  the previously attached cell. §9.1 spawns four `WorkerDrone`s; after the loop only the last is on
  the holon rank, and the other three are islands.
- §6.1 `setSkill` "breaks link to old skill cell (`link(old, d.skill, ..., 0)`)" without saying the
  rank must be rejoined; as written it cuts the rank in two.
- §2.4's pseudo-code does get head-insertion right, but only because it saves `old_b` first. Also it
  inserts at the head, which reverses declaration order, while §2.2 calls the rank "dispatch order".

**Fix.** Lower every attach to the existing rank helpers:
`ArenaManifold::insertIntoRank(cell, anchor, dim, dir)` (`zigzag/arena_manifold.hpp:~611`, "without
severing links") for head insertion, and `zigzag::rankTail(arena, anchor, dim)` (used at
`vortex_core.cpp:123`) plus `link` for tail append when order matters (behavior declaration order,
holon children). Say in §6.1 which of the two each construct uses and what that costs (head insert
O(1); tail append O(rank length) unless a tail cell is kept, which would be a new cell on a new
dimension, not a field). Replace the §2.4 pseudo-code with a call to `insertIntoRank`.

## 3. BLOCKING. Cross-arena ranks contradict share-nothing (§1.1, §2.3, §6.1, §6.5, §7.1, §10.4)

Every agent is "an actor ... in an isolated `ArenaManifold`" (§1.1, §7.1; `mesh_actor.hpp` gives
each actor its own `context->arena`, see `mesh_actor.cpp:~665`). Yet the structure links across
agents:

- `d.agent` is a *population* rank of agent headcells (`link(pop_cell, d.agent, +1, -1)`). A cell
  belongs to one arena. Either all agents of a population share an arena (then "share-nothing" and
  "no remote mutation" are false: one agent can walk and relink another's headcell), or the rank
  spans arenas, which the arena cannot represent (foreign cells are proxies attached through
  `d.stores` in `multi_store.hpp`, a different mechanism the doc never mentions).
- `d.holon`/`d.context`: a child "agent" is minted by the parent as a cell in the parent's arena
  (§6.1 `spawn`). But §7.1 says every agent instance is an actor with its own `ActorPid`, grant and
  arena. Is the child in the parent's arena (then it is a data structure, not an actor) or its own
  (then `-d.holon` from child to super-agent is a link across arenas)?
- §10.4 "emits a `HolonGoalFailed` event along `-d.holon`" treats a dimension as a message route.

**Fix.** Pick and state one of: (a) a holon's children are separate actors, each with a private
arena, and the parent's `d.holon` rank holds *handle cells* (cell content = `ActorPid`) for them,
with no `-d.holon` pointer from child to parent (the child learns its parent from its spawn request
and replies by `emit`); the population `d.agent` rank is a per-node registry held by the host, not
by any agent. Or (b) holon children are in-arena sub-structures (a cell tree with no actor
identity). Option (a) preserves §7.2. Rewrite §2.3, §6.5 and the `HolonGoalFailed` path accordingly,
and delete `-d.holon`/`-d.context` as inter-agent pointers.

## 4. MAJOR. "Topological GC" describes a mechanism that is not built, and defines …

Full title: 4. MAJOR. "Topological GC" describes a mechanism that is not built, and defines
"isolated" wrongly (§2.6, §10.4)

- §2.6.3: "The `ArenaManifold` reclaims isolated cells during segment compaction." `compact()`
  (`zigzag/arena_manifold.hpp:669`, comment 656-668, `deadLinks()` at 672) discards dead *link
  entries*; it never frees a cell. Cells leave an arena only by `release(mark)` truncation (LIFO,
  `arena_manifold.hpp:638-640`: "Cells minted under the mark are gone"). I found no reachability
  sweep in `apps/common/xanadu/` (the `vortex-hyperstructural-runtime.md` §5.4 "VQL §5 sweep" is
  specified, not implemented). The header's claim that the doc "rests strictly upon mechanisms that
  are built" is therefore false for §2.6.
- Isolation is the wrong notion. Vortex defines isolated as *all of a cell's links cleared*
  (`vortex-hyperstructural-runtime.md` §1, "geometrically isolated"). §2.6.2 says unlinking an
  intention root "isolates its entire speculative subtree"; it does not, it makes the subtree
  *unreachable* while every internal link stays intact. Collecting it needs a reachability sweep
  over the root set, which is O(live cells), not "O(1)". The only genuinely O(1) reclaim the arena
  has is `release(mark)`.
- §2.6.1 vs §2.6.2 vs §10.4 contradict each other on pinning: §2.6.2 forbids anything but
  `Headcell -> Intention Root` from reaching `d.pinning_cursors`, yet §10.4 "Pinning Cursor
  Handover" hands pinning cursors that point at intentions to successors. §5.6 of the Vortex spec
  says a pinned cursor is attached to a *head cell of an island not otherwise rooted*; the agent's
  own subgraph is rooted by `home`/`d.cursors`/the checkpoint `stateRoot`, so what is the pinning
  cursor in §2.6.1 pinning, and in which arena?

**Fix.** Rewrite §2.6 as: (i) reclamation of speculative cells is `release(mark)` or `discard`; (ii)
reclamation of retired persistent-in-arena structure (finished intentions, detached skills) needs
the reachability sweep, which is *not built* — state it as a dependency (U1-class open item), price
its O(live) cost, and list the alternative of leaving dead structure until the turn's
`checkpointView` rebuild. Drop "O(1)" and "isolated" for unreachable subtrees. Remove the pinning
handover or define what is pinned.

## 5. MAJOR. Persistence story conflicts with R8 and with `release()` semantics …

Full title: 5. MAJOR. Persistence story conflicts with R8 and with `release()` semantics (§1.1.4,
§4.3, §7.3, §10.4, §11.1.3)

- R8 (`store-slice-convergence.md` R8, price paragraph): "A Vortex program's intermediate execution
  is **not** reproducible from the spool", and only a user-generated update persists in a visited
  store. §1.1.4 and §11.1.3 promise "full hypertime provenance" and that an agent's "entire lifetime
  can be replayed deterministically from its genesis microversion". At most the *promoted results*
  replay. Also an agent's autonomous belief update is not "a user-generated update"; the doc must
  carry its own ruling (an agent-authored-ops carve-out, with the `PageBreak`-flood argument priced:
  one `promote` per event per agent).
- §4.3: "`promote()` ... then a subsequent `release()` reclaims unpromoted debris". `release(m)`
  *undoes everything since the mark*, including every structural mutation the handler made in the
  arena (`setSkill`, mailbox dequeue, intention splices). After `promote` the arena's ephemeral refs
  are not stripped of a bit (§4.3 says "strips `ephemeralBit`"): `promote` mints ops and returns a
  *mapping* (`arena_manifold.hpp:808-812, 834`), so the agent's root must be re-bound to persisted
  refs. The success path wants `discard(mark)` (keep the bindings), not `release`. The failure path
  of `promote` (`std::optional`, refuses above `PromotionBudget::maxOps = 65536`) is not handled,
  and a refused promotion is exactly a "ceiling users work around".
- §10.4 says cross-turn intentions are "explicit spatial zzstructures in the `ArenaManifold`", while
  §7.3's `ActorContinuation` carries only `programRef`, `baseRef`, `spawnRequest`, `completed`
  (`mesh_actor.hpp:94-100`) and `checkpointView` publishes one `stateRoot`. Nothing says the
  intention graph, mailbox ring or skill rank is in the published state, so after migration they are
  gone.

**Fix.** Add a "what persists" table with columns: structure (skills, behaviors, intentions,
mailbox, beliefs, outbox), arena or `Manifold`, promoted per turn or never, in `stateRoot` or not.
Fix §4.3's diagram to
`mark -> handle -> {promote ok: discard(mark); refuse/guard-fail: release(mark)}`. Replace "replayed
deterministically" with "the sequence of promoted states is replayable; computation within a turn is
not (R8)". Add a promotion-budget refusal path.

## 6. MAJOR. Speculation machinery duplicates what the arena already is, and invents …

Full title: 6. MAJOR. Speculation machinery duplicates what the arena already is, and invents
unbuilt types (§2.2, §10.3, §10.4)

- `ArenaManifold` already has a conditional trail and copy-on-write: `trail(dense)` is called at
  every link write (`arena_manifold.cpp:844-846`), and `ArenaManifold(const Manifold *base)` shadows
  the slot on first write. §1.4's own row says "`value()` write trailed by enclosing Mark ... none —
  the trail's". But §2.2/§6.1/§10.3 add a *cell-level* `d.trail` ring plus `d.shadow` ring plus
  "Shadow Cells". Two undo mechanisms for one write is a defect.
- `d.shadow` is defined in §2.2 and never used in a lowering or diagram; §10.3 uses `d.trail` for
  the shadow -> master link instead (and see finding 1).
- "private ephemeral `ArenaSlice`" (§10.3, §10.4) is not a type in the tree (I searched `apps/`,
  `include/`, `src/`, `design/`). The only nesting available is an arena over a `const Manifold*`
  base (`arena_manifold.hpp:254-261, 742`), not over another arena. §0 says the design "asks the C++
  core for nothing"; `ArenaSlice` is a C++ addition.
- Intentions run one at a time inside a turn and every mark is closed at the turn boundary (§10.4,
  quiescence), so two intentions never hold speculative state simultaneously. The CoW shadow
  machinery solves a concurrency that the turn model already excludes.

**Fix.** Delete `d.trail`, `d.shadow` and the shadow-cell protocol. `target <- expr` is the arena's
own trailed `link`/`value` write under the enclosing `mark`; "intention-local resolution" is
unnecessary because only one intention runs per turn. If cross-turn speculation is later wanted,
record it as a refused alternative with the reason ("would need nested arenas; see U1").

## 7. MAJOR. Dimension proliferation and overloading; no genesis (§2.2, §8.1 …

Full title: 7. MAJOR. Dimension proliferation and overloading; no genesis (§2.2, §8.1,
vql-agent-experiment.md)

Fifteen new dimensions, several redundant or colliding:

- `d.intentions` vs the existing `d.cursors` (`vortex/vortex_core.hpp:~47`, root set). R8's
  cursor-identity ruling says a cursor cell persists as identity and its position is re-derived. An
  intention *is* a process with a `d.stack`; the doc should either use a cursor on `d.cursors` with
  a goal cell as its subgraph, or record why not (refused alternative).
- `d.holon` and `d.context` both express "inner collective"; see finding 3. `d.context` is also used
  with two meanings: a ± *pointer* (parent/inner context) and, implicitly, a membership relation
  (child -> context).
- `d.agent` is the population rank, the sibling chain among holon children, *and*, in the sibling
  document, the space-member rank (`vql-agent-experiment.md:37`: "Node with member rank on
  `+d.agent`"). One name, three ranks. Decide, and reconcile with that document.
- `d.source` already exists with a different meaning (`result_slice.cpp:34,85`: result cell ->
  provenance source). vagent's `d.source` (§2.2) is a scan cursor -> view link, and §6.1/§10.5 use
  `+d.grab` for that same link, so it is both redundant and colliding.
- `d.choice` mirrors the VM's `#Choice`/`altOp` choice stack. Since every choice point is closed
  before quiescence (§10.4), the rank never survives a turn and only matters inside a mark. Justify
  or drop.
- `d.capacity` is used for three things (finding 1).
- None of the new dimensions are minted: R12 says a dimension is a cell on `d.dims`, minted at
  genesis (vicon does this explicitly for `d.scan`, `vicon-goal-directed-language.md` §8.2 / line
  435). The doc has no genesis section, no `SystemDimensions` additions (`vortex_core.hpp` struct),
  no mention of `dimension_registry`, and no count of what each costs.

**Fix.** Add a "Dimensions minted" subsection with one row per dimension: purpose, direction
semantics, the existing dimension it could have reused and why not, and genesis owner. Target a
reduction to the dimensions that carry a distinct *rank* (likely `d.skill`, `d.behavior`, `d.event`,
`d.space`, `d.holon`, `d.subgoal`, `d.outbox`; capacity binding via clone views; intentions via
`d.cursors`).

## 8. MAJOR. State hides in payloads; Context/Space/Event shapes are unspecified (§3.1, §5, §6.4)

AGENTS.md ("ZigZag cell representation") asks that structure live in cells and dimensions, not in
parsed payloads. The doc never gives cell shapes for:

- an **event**: typed fields (`targetId`, `distance`, `coordinates: [Float;3]`) with only
  "Destructure Payload via `value()`" (§6.4) — i.e., a packed/parsed payload;
- a handler's **pattern, guard, scope, priority** (§6.4 sorts by priority; where does the number
  live?);
- agent **attributes** and skill state (`let mut currentSpeed`, `pendingQueries: [String; [String]]`
  in §9.1 — a map in a payload);
- **Context membership of Spaces** (§3.1: "a logical sphere containing a set of Spaces") — there is
  no dimension carrying context -> spaces; `d.space` hangs off the *agent* (§2.3), and a Space
  shared by several agents cannot sit on several agents' `d.space` ranks (two-degree, again).

**Fix.** Specify: event = a cell on `d.event` whose fields are cells on a field rank (name on
`d.vars`, value on `d.values`, which the Vortex core already has: `SystemDimensions::vars/values`),
so the event is quotable and queryable; handler priority as a scalar cell (R6) on the handler cell;
agent and skill state on `d.vars/d.values` rather than a new mechanism; `d.space` as a rank *of
membership cells* each holding a clone view of the Space cell.

## 9. MINOR. Terminology: rank, ring, headcell (§1.1, §2.5, §6.3, §10)

"Mailbox Ring", "Outbox Ring", "`d.clone` ring", "Dynamic Skill Dispatch Ring" are all open ranks in
the pictures (a chain from the agent cell with an empty negward end). A zzstructure ring is a closed
loop; a rank has a head. `store-slice-convergence.md` and the Vortex doc call `d.clone` a rank
(master = negward-most). "Agent Headcell" is used for the agent root, which is the head of six
different ranks at once; that is legal but should be said once ("the agent cell is the head of its
`d.behavior`, `d.skill`, ... ranks"). Use "rank" unless a ring is truly closed.

## 10. MINOR. `d.clone` state sharing is over-motivated and its direction is under-specified (§2.5)

§2.5 puts the agent state cell "on a `d.clone` ring" for behaviors and spaces to read. Behaviors
already reach the agent cell by walking back along `d.behavior`; no clone is needed. If it is kept,
state which cell is the master (the negward-most) and forbid linking anything negward of it, since
that silently changes what every member reads (`vortex-hyperstructural-runtime.md` §5.2).

## 11. MINOR. Sentinels leak into the pseudo-code (§1.2, §4.2, §2.4)

§1.2 re-states "`noCell == 0`" and `-1` as arguments; §4.2 tests `link(P, d.spin, +1) != noCell`
though `link` is declared to return `std::optional<cell_id>` in the same section. AGENTS.md bans
sentinels outside the wire format. Use `link(P, d.spin, +1).has_value()` and cite R5 for why
`noCell` exists at all (op index 0). The `-1` allocate form is Vortex's spelling; link to
`vortex-hyperstructural-runtime.md` §5.1's note that the wire carries a verb instead.

## 12. MINOR. `d.scan` cursor description is inconsistent (§2.2, §6.1, §10.5)

§2.2 says the cursor reaches its subject through `d.source`; §6.1 and §10.5 say `+d.grab`; vicon
(`design/vicon-goal-directed-language.md:329-341`) uses `+d.grab` with content `&pos`. Also the
clone view must be added *posward* of the master, and the view lives in the subject's rank: if the
subject is a persistent cell, that is a link into a persistent rank (an arena shadow under the COW
base, not a `Manifold` write — R8/R12 refuse an ephemeral target). Say so, and drop `d.source`.

## 13. MINOR. Event-consumption path is untyped (§6.4)

"Skip to Next Event" and consumption both splice cells out of the middle of `+d.event`; say whether
the consumed event cell is unlinked and left unreachable (see finding 4, nothing collects it until a
sweep exists) or moved to a processed rank for the `checkpointView`'s `completed` records.

VERDICT: NOT CONVERGED
