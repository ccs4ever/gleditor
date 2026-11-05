# R1 zigzag critique — `design/vagent-actor-executor.md`

Reviewer lane: cell-graph topology, dimension/rank discipline, filing and promotion ordering, link
eviction, genesis sequencing — per `brief-common.md`. Every claim below was walked through
`promote()` (`arena_manifold.cpp:1733-2116`), `regenesisCycle` (`lineage.cpp:168-239`), the
`RankView`/`rankTail` semantics (`cell_views.hpp:117-131,237-271`), `spawnCursor`
(`vortex_vm.cpp:233-247`), `link()`/`unlinkRank()` (`arena_manifold.cpp:1347-1378`,
`records.hpp:41-52`), `initGenesis` (`vortex_core.cpp:31-69`), `ActorGrant` (`mesh_actor.hpp:55-65`)
and `checkpointView` (`mesh_actor.cpp:830-855`) rather than inferred.

Counts: **1 BLOCKING, 2 MAJOR, 6 MINOR, 0 NIT.**

What survives inspection: the `d.seen`/`d.evidence` mechanics are expressible on the substrate
(repoint-head+append is real, and `link()`'s mutual eviction does the excision for free), the
evidence-ownership chain survives promote exactly as specced *provided the carrier is a real link
edge*, the tombstone-delisting convention is a faithful extension of the `unlearn:`/`fault:`
precedent, and M0's file-before-first-resolution ordering is the right order. What does not survive:
the `d.agents` "verified safe" proof — it is refuted in its load-bearing clause — and the
`grant.maxLiveCells` bound, which names a field that does not exist.

## Findings

### F1 — BLOCKING — `d.agents` refuted: `home` joins every promote and accretes dead cursor twins

- Spec location: §3.1 `d.agents` bullet (:127-132 "Verified safe"), §6 options table (:341), §9
  (:390-395, :409).

- Problem: the spec's proof covers the membership edge and nothing else. The edge's real cost is
  that `promote` walks **both** directions — `discover(edge.pos,true); discover(edge.neg,true)`
  (arena_manifold.cpp:1794-1796) — so the agent cell's `-d.agents → home` backlink puts `home` into
  `order` *and* `frontier` on **every** agent-rooted promote, unconditionally. From there:

  1. **Home expands exactly when it hurts.** The prune at arena_manifold.cpp:1768-1770 skips a
     persistent non-root cell only when `!from.holdsOwn(current)`. A shadowed `home` expands, and
     `home` is guaranteed to be shadowed: `spawnCursor` appends onto `rankTail(home, d.cursors)`
     (vortex_vm.cpp:242-245), which is `home` itself on the empty rank (cell_views.hpp:253-262), and
     `mesh_actor` spawns/resumes the actor cursor *before* the genesis hook runs
     (mesh_actor.cpp:194-195 vs :200-208; :323-324 vs :333-339). The `+d.agents` mint itself shadows
     `home` too if done in-arena (`link()` calls `shadow(from)`, arena_manifold.cpp:1338).
  1. **Expansion exposes the whole shadow.** `dimensionsOf(home)` returns the shadow's entire copied
     edge set — every rank hanging off `home` is walked: `+d.cursors`, `+d.scan`, `+d.stores`,
     `+d.stdlib`, `+d.pinning-cursors`, `+d.cache`, `+d.dims`-members-as-cells. The ephemeral live
     cursor twin enters `order` and expands in turn, dragging `+d.name`, its Int64 PC, and any
     residual `+d.stack`/`+d.vars` behind it.
  1. **The emit commits.** `home` is a pre-extent persistent ref — genesis mints it first by fiat
     (manifold.cpp:449-451) and `initGenesis` adopts the base's (`vortex_core.cpp:31-36`) — so
     `commitStateSide` returns **Additive** (arena_manifold.hpp:278-282): every changed posward edge
     emits as add/repoint (:2080-2102), and a dropped edge never commits (:2085-2088). The
     `home +d.cursors → twin` edge commits; the twin mints durably (`real.emplace`,
     arena_manifold.cpp:1998-2001; Int64 scalars via :1968-1970).
  1. **The accretion is self-sustaining.** Once `twin_1` is durable, the next incarnation's
     `spawnCursor` appends onto *it* — shadowing the tail twin — and a shadowed persistent cell
     `> imageExtent` re-enters the walk via `extraRoots` even when `home` is never re-shadowed
     (lineage.cpp:190-195). `twin_i +d.cursors → twin_{i+1}` commits under Clears. Every re-genesis
     therefore appends at least one dead twin per cursor spawned that incarnation — including
     stdlib's internal cursors (`vortex_stdlib.cpp:2227,:2245,:3790,:3830,:3855`).
  1. **It is irreversible.** The head edge `home +d.cursors → twin_1` sits on an Additive cell: a
     repoint can move it but a `noCell` clear can never emit. Dead twins can be spliced *out* only
     by code that deliberately unlinks them (repoint commits under Additive; member-edge clears
     commit under Clears on the >extent twins) — nothing in the design or the codebase does this
     today. Tombstone delisting — the spec's answer — does nothing for rank members that aren't
     agent registrations.
  1. **The stated invariant is violated verbatim.** `vagent-metaprogramming.md:1804-1814`: "The
     frames stay out of the durable set because `home` is not link-*reachable from* the agent cell —
     the invariant is about *paths*, not one edge direction: **no *system-minted* edge creates a
     link path from the agent cell to `home`, the `+d.cursors` rank or any `d.stack` cell**." The
     `+d.agents` edge is exactly such a system-minted path (agent `−d.agents`→ home). The sibling
     doc's own fallback — "the prune is what excludes the frames in every case" (:1811-1814) — holds
     only while `home` is *unshadowed*, which normal operation defeats on every cursor append. The
     spec neither cites nor amends the invariant.
  1. **Secondary effect:** `order` grows by `home`'s reachable shadow set plus each accreted twin
     chain every cycle; the walk charges against `budget.maxOps` (65536, arena_manifold.hpp:285) —
     at the limit `promote` refuses (:1798-1800) and `regenesisCycle` fails (lineage.cpp:206-210),
     i.e., the accretion eventually bricks checkpointing itself.

- Evidence: arena_manifold.cpp:1794-1796 (both-directions frontier), :1768-1770 (prune), :1338-1373
  (shadowing + mutual eviction), :2040-2114 (emit: `persistentShadow` policy,
  `isClear && !mayClear → continue`, `setLink` durable commit); arena_manifold.hpp:278-282
  (`commitStateSide`), :285 (maxOps); vortex_vm.cpp:242-245 (tail append), :233-247 (cursor shape);
  cell_views.hpp:253-262 (`rankTail` returns `start` on empty rank); mesh_actor.cpp:194-195,:323-324
  (cursor precedes genesis hook); manifold.cpp:449-451, vortex_core.cpp:31-36 (`home` pre-extent
  persistent); lineage.cpp:190-195,:201-205 (extraRoots + `commitStateSide`);
  vagent-metaprogramming.md:1798-1814 (the invariant); compiler.cpp:1232-1237 (the unlink-the-cursor
  precedent the codebase already uses).

- Fix: pick one, priced:

  1. **Keep the edge, add hygiene.** A pre-promote pass (adapter or a genesis-adjacent step) unlinks
     every `+d.cursors`/`+d.scan` member — the compiler's own precedent is compiler.cpp:1234-1237.
     Variant (a) unlinks only live cursors: new accretion stops, the frozen durable tail stays and
     `rankTail` walks it forever. Variant (b) additionally splices out the dead durable twins —
     feasible because member clears commit under Clears and the head repoint commits under Additive.
     Price: a documented "no rank may grow off `home` unchecked" duty covering stores/stdlib/scan
     and every future home rank; miss a cycle and that cycle's accretion is durable (though
     retro-compactable under (b)).
  1. **Re-anchor the rank.** Put the mind registry where the spec's own invariant allows it — an
     agent-owned rank (`cells"$agent/+d.agents"`) or a dedicated state-side registry cell — keeping
     `home` free of system-minted inbound edges. Price: the `cells"##/+d.agents"` payoff (`##` binds
     `core_.home()`, compiler.cpp:1307-1314) is lost unless `##` gains a second spelling bound to a
     state-side anchor.
  1. **Substrate change.** Give `promote` a traversal-level "endpoint-only" predicate over
     persistent cells (e.g., `ref ≤ imageExtent`, or `== home`): such cells enter `order` as names
     but are never expanded — ephemerals behind them never mint, their diffs never emit. Cleanest
     containment; changes promote for every caller, needs its own verification pass.
  1. In all cases amend `vagent-metaprogramming.md` §6.6: name `+d.agents` the single permitted
     system-minted agent↔`home` edge and record the accompanying `home`-shadow duties.

### F2 — MAJOR — §4.6's invariant names a field that does not exist, and no live-cell measure exists

- Spec location: §4.6 (:303): `liveCells(turn boundary) ≤ grant.maxLiveCells`, "measured where
  `checkpointView` would accept".

- Problem: `ActorGrant` has no `maxLiveCells` — its bound fields are `maxInstructions`, `maxCells`,
  `maxOutputBytes`, `maxSolutions`, `maxMailboxBytes`, `expiresAt` (mesh_actor.hpp:55-65). The
  existing cell bound is `grant.maxCells` → `setExecutionLimits(initial + maxCells)`
  (mesh_actor.cpp:184-192,:310-318) → `allocationLimit_` enforced at `mintSlot`
  (arena_manifold.cpp:1173-1176) — an **allocation ceiling** over total mints, monotonic, not a
  live-cell bound. And `checkpointView` (mesh_actor.cpp:830-855) is a quiescence predicate —
  stopped/running/mailbox/wakes/choiceDepth/marks/contains(stateRoot)/deliveries — it measures
  nothing about cells; the closest existing metric is `arena.cellCount()`/`traversalBound`
  (arena_manifold.hpp:548,:554), which counts *all* slots including shadow copies. So the stated
  invariant is simultaneously unenforced, unmeasurable, and unspellable as written.

- Evidence: mesh_actor.hpp:55-65; mesh_actor.cpp:184-192,:310-318,:830-855;
  arena_manifold.cpp:1173-1176; arena_manifold.hpp:548-554.

- Fix: either restate against what exists — `arena.cellCount()` (or a defined `liveCells` =
  `cellCount() − shadow/cell-of-record count`) at the turn boundary `≤ grant.maxCells` — or add
  `maxLiveCells` to `ActorGrant` **and** define the measure and the measuring point in the same
  sentence. The drain-mode GC paragraph is real work either way and unaffected.

### F3 — MAJOR — the `d.seen` ring must close through the agent cell, or rank consumers lasso

- Spec location: §3.1 `d.seen` bullet (:116,:133-138) — "bounded ringrank of last K event cells ↻",
  "append posward, evict the head at capacity K".

- Problem: `RankView` terminates only on a dead end, a self-link, a return to the *start* cell, or
  `traversalBound()+1` (cell_views.hpp:117-131,:157-164). If the ring is read in the natural way —
  journal cells `j1→…→jK→j1` with the agent holding only a head pointer `+d.seen→j1` — then
  `rankAfter(agent, d.seen)` walks `agent→j1→…→jK→j1→…` and yields `traversalBound()+1` entries with
  duplicates, and `rankTail(agent, d.seen)` returns an arbitrary ring member — so the "append
  posward" protocol links to the **wrong cell** and every C++ consumer (the §4.4 outbox-style drain
  walks, harness readers) degenerates to O(arena). The ring must thread the anchor:
  `agent→j1→…→jK→agent` — then the ring-closing guard stops the walk at the anchor, `rankTail`
  returns `jK`, and the protocol is exact. (In-graph reads are safe either way — `Linked`
  materializes the rank under a seen-set, vortex_vm.cpp:827-831 — the hazard is the C++ side the
  spec routes the drain through.) The spec's "(Link repoint evicts prior posward edge)" also
  understates the mechanism in its favor: one `link(agent, +d.seen → j2)` fully excises `j1` —
  `link()` clears both displaced ends (`displacedUs`/`displacedIt`, arena_manifold.cpp:1347-1373),
  i.e., repoint-eviction is a complete `unlinkRank`-style splice without a second write. Under
  `commitStateSide` the splice is durable: a shadowed persistent journal cell (`>imageExtent`)
  re-enters via `extraRoots` and its cleared edges emit under Clears; an evicted *ephemeral* journal
  cell simply goes unreachable and is never minted — the boundedness the design wants. The one piece
  missing is the anchor-in-ring statement.

- Evidence: cell_views.hpp:117-131,:157-164,:237-262; arena_manifold.cpp:1347-1373;
  vortex_vm.cpp:827-831; lineage.cpp:190-195; arena_manifold.cpp:2085-2088.

- Fix: amend the `d.seen` bullet to state explicitly that the ring closes through the agent cell
  (`agent +d.seen→j1`, `jK +d.seen→agent`), name the evict op (`Link(agent, d.seen, j2)`) and append
  pair (`Link(jK, d.seen, jNew)` + `Link(jNew, d.seen, agent)`), and note that
  `rankAfter`/`rankTail` consumers depend on the anchor being inside the cycle.

### F4 — MINOR — `d.evidence` confirmed; the carrier must be a real link edge, not an int field

- Spec location: §3.1 (:113-114,:136-137), §3.6 (:231) — "`await` … links `+d.evidence →` the
  journaled `d.seen` cell"; "the `+d.evidence` link owns the cell through promote" (:343).

- Problem: a journaled event cell survives promote iff it lands in `order` — i.e., iff an expanded
  cell references it by a link edge in **either** direction (arena_manifold.cpp:1794-1796). The
  `agent→waiting-record→event` chain works only if the hop to the event is a real edge. An int64
  ref-box field — the `record +d.values → Int64(ref)` shape the record convention uses elsewhere
  (records.hpp:84-101) — is invisible to the walk for the same reason `##` anchors are invisible
  (vagent-metaprogramming.md:1810; `Linked` ref-boxes, vortex_vm.cpp:833-839): the event cell would
  silently drop on the next promote while the awaiting record still names it. The spec *says*
  "links" — make it load-bearing: state that `+d.evidence`/`waiting` is a link edge on the record
  cell, not a `d.vars` field carrying an int. Also worth one clause: survival means "referenced by a
  frontier-*expanded* cell" — a reference through a pruned program-side anchor does not carry (the
  prune at arena_manifold.cpp:1768-1770), which is consistent with the design's intent.

- Evidence: arena_manifold.cpp:1768-1770,:1794-1796,:2089-2102; records.hpp:84-101;
  vagent-metaprogramming.md:1810; vortex_vm.cpp:833-839.

- Fix: one sentence in §3.6 — "the carrier is a real `+d.evidence` link edge (never an int field);
  the event cell survives promote iff some expanded cell links to it in either direction."

### F5 — MINOR — tombstone delisting works; the key spelling and reader-filter duty are unnamed

- Spec location: §3.1 (:130-132), §9 (:341,:395).

- Problem: the mechanism is a faithful extension of existing convention — `markUnlearned`/
  `unlearned` mint and test keyed `d.vars` records on `binds_.agentCell` via `varsRecord`
  (meta.cpp:323-335; records.hpp:68-79), `clausesOf` filters tombstoned labels even while rank
  membership persists (meta.cpp:210-218), `dropPatch`'s `fault:<label>` record is the same shape
  (genesis.cpp:219-228), and `commitShadowed`+Clears durable-unlink is the documented path for real
  removals (meta.cpp's unlearn/de-registration comments; records.hpp:41-52). Two things the spec
  leaves implicit: (a) the tombstone's **key spelling** — `varsRecord` dedupes by key text, so pick
  a stable key (e.g. `delisted:` or `unlearn:`-style namespaced) and say so, or two records can mint
  for one delisting; (b) **who reads it** — `cells"##/+d.agents"` returns delisted members verbatim;
  every consumer of the rank (the M0 test, the harness, any `rankAfter` walker) must apply the
  filter, the way `clausesOf` does for clauses. Say both.

- Evidence: meta.cpp:210-218,:323-335; records.hpp:41-52,:68-79; genesis.cpp:219-228.

- Fix: name the key spelling and state the consumer-side filter obligation in the `d.agents` bullet.

### F6 — MINOR — agent-cell Clears depends on the `> imageExtent` seat; the exempt case drops clears

- Spec location: §3.1 (:131 "the agent cell (state-side `Clears` territory)").

- Problem: the tombstone record itself needs only Additive (it is an add). What actually needs
  Clears on the agent cell are the operations that write `noCell` onto *its own* posward edges: a
  `d.seen` drain-to-empty (`agent +d.seen → noCell`), the last-member unlink of `d.outbox`,
  `d.pending`, `d.belief` consumption tails. Those commit only when the agent cell's ref is
  `> imageExtent` (arena_manifold.hpp:278-282,:2085-2088). In the normal lineage that holds —
  `regenesisCycle` mints the agent twin post-image (lineage.cpp:232-235). The exempt case the meta
  doc flags — a program-side `agent_cell()` seated `≤ E_ops` (vagent-metaprogramming.md:1793) —
  flips the policy to Additive and every such clear silently never commits: a stale durable head
  edge resurrects last cycle's member. Worth one stated line so nobody seats the agent cell in the
  image band for convenience.

- Evidence: arena_manifold.hpp:278-282; arena_manifold.cpp:2085-2088; lineage.cpp:232-235;
  vagent-metaprogramming.md:1793.

- Fix: state "the agent cell must sit `> imageExtent` (its `Clears` eligibility is what makes
  drain/unlink commits durable)" alongside the tombstone sentence.

### F7 — MINOR — `ensureDimension` itself does not refuse under marks; the wrappers do

- Spec location: §3.1 (:106-107): "no dim mints under a live mark; `mintMetaBindSet`/
  `ensureDimension` both refuse then".

- Problem: `ArenaManifold::ensureDimension` does **not** check marks — it resolves
  `dimensionNamed`/`arenaDims_`, mints, and appends to the `d.dims` tail unconditionally
  (arena_manifold.cpp:78-105). The marks refusals live one layer up: `mintMetaBindSet`
  (meta.cpp:65-68), `MetaRuntime::resolveDimension` (meta.cpp:806-824), and
  `genesis.cpp::resolveDimension` (genesis.cpp:44-55). Any caller that reaches the arena primitive
  directly — a fragment-side path, a new adapter hook — mints under marks freely and hits the
  `arenaDims_` desync those comments name (the untrailed table). Relatedly, the failure the filing
  order prevents is worth stating: an *unfiled* name used in-graph mints an ephemeral dim cell,
  `discover(edge.dim,false)` still lands it in `order`, and it mints an **orphan persistent twin on
  every promote** — never filed on `d.dims` — which is the precise orphaning the
  file-before-first-resolution ordering (filing sweep before fresh `WorkerContext`/ `initGenesis`,
  lineage.cpp:212-225; two-table resolution genesis.cpp:44-55) exists to avoid. M0's order is
  correct; the citation is not.

- Evidence: arena_manifold.cpp:78-105,:1794,:1998-2001; meta.cpp:65-68,:806-824; genesis.cpp:44-55;
  lineage.cpp:212-225; vortex_core.cpp:31-69.

- Fix: restate as "`mintMetaBindSet` and the `resolveDimension` wrappers refuse; the raw arena
  primitive does not — every dimension-writing path must go through a wrapper."

### F8 — MINOR — `d.evidence` sits inside the retained-evidence `d.evidence.*` namespace

- Spec location: §3.1 (:114,:116,:136), M0 (:363-364 "file `d.agents`/`d.seen`/`d.evidence` in
  `kAgentDimensionNames`").

- Problem: the evidence subsystem already files `d.evidence.target` (and kin) as dimension names
  (evidence_controller.cpp:1658,:1689,:1761,:1772; query_composition.cpp:609,:613,:631). A top-level
  `d.evidence` is not a literal collision — `dimensionNamed` is exact-match — but it lands inside an
  owned dotted namespace, and a future `d.evidence.<x>` filing beside a top-level `d.evidence`
  journal-edge dim invites reader confusion in folded `d.dims` walks. Also: `kAgentDimensionNames`'s
  comment says "thirteen" (lineage.hpp:64-66) and will go stale when the three names are added — the
  arrays are enumerated verbatim by every filing list (tests' `filedNames()`/`fixedDimNames()`
  enumerate all three arrays), so the code change is genuinely one place, comment included.

- Evidence: evidence_controller.cpp:1658,:1689,:1761,:1772; query_composition.cpp:609,:613,:631;
  lineage.hpp:46-71.

- Fix: either rename the journal-edge dim (e.g. `d.seen.evidence`, keeping `d.evidence` for the
  retained-evidence namespace) or add a sentence acknowledging the prefix share; fix the "thirteen"
  count in the same breath.

### F9 — MINOR — §3.1's diagram anchors the ranks under `home` visually

- Spec location: §3.1 topology block (:109-123).

- Problem: the `├─` branches hang under the `home ─(+d.agents)─► [agent cell]` line, so a literal
  read puts `d.vars`, `d.belief`, `d.seen`, `d.outbox`, `d.code`, `d.patch`, `d.synapse` on `home`.
  The intent is clear from the sibling doc (`d.code`/`d.patch`/`d.vars` are
  `binds_.agentCell`-anchored, meta.cpp:1690,:328; metaprogramming's root model), and the spec's own
  prose agrees — but the ambiguity is load-bearing: on `home` (Additive, F1) those ranks' mutations
  would commit adds/repoints forever with no possible clears — unbounded belief/pending/outbox
  accretion — while on the agent cell (`>imageExtent`, Clears) the drain/eviction protocol works.
  One line of anchoring text removes a whole class of misimplementation.

- Evidence: vagent-actor-executor.md:109-123; meta.cpp:1690,:328; F1 above.

- Fix: label the block "agent cell's ranks" (e.g., `[agent cell]` as the indented node with a
  `+d.agents` inbound edge), or state in prose that every rank below hangs off the agent cell.

## Cell-classification reference (the §3.1 map, per Q7)

| Cell                                                    | Class                                                                                     | Policy under `commitStateSide`                                                            | Note                                                                               |
| ------------------------------------------------------- | ----------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `home`                                                  | base-persistent, `< imageExtent` (manifold.cpp:449-451)                                   | **Additive** — adds/repoints commit, clears never emit                                    | permanently in `order` once `d.agents` exists (F1); expands whenever shadowed      |
| agent cell                                              | ephemeral → durable at first re-genesis (lineage.cpp:232-235); `> imageExtent` thereafter | Additive while ephemeral; **Clears** once durable                                         | exempt ≤E_ops seat flips it to Additive (F6)                                       |
| `d.seen` journal cells                                  | ephemeral per turn → durable while reachable                                              | Additive at mint; Clears once durable                                                     | evicted ephemerals die unminted; evicted durable cells splice via committed clears |
| `d.evidence`-linked event cells                         | as journal cells                                                                          | same                                                                                      | survive iff link-referenced by an expanded cell (F4)                               |
| `d.vars` records (fields, `meta.serial`, tombstones)    | ephemeral → durable on the agent's rank                                                   | same                                                                                      | adds commit under either policy — tombstones are Additive-safe                     |
| `d.belief`/`d.intention`/`d.outbox`/`d.pending` members | ephemeral → durable on agent ranks                                                        | same                                                                                      | drain tails need the agent's Clears (F6)                                           |
| program-side clause/symbol/module cells                 | base-persistent, `≤ imageExtent`                                                          | Additive; usually unreachable or pruned; excluded from `extraRoots` (lineage.cpp:190-195) | re-derived by genesis replay; shadow writes on them never reach `order`            |
| cursor twins + `+d.stack` frames                        | ephemeral per incarnation                                                                 | minted durably via the F1 path — the bug                                                  | the §6.6 frames-invariant violation                                                |
| `d.agents`/`d.seen`/`d.evidence` dim cells              | must be filed persistent before first use                                                 | identity-mapped in `real`                                                                 | unfiled in-graph use mints orphan persistent twins per promote (F7)                |

VERDICT: NOT CONVERGED
