# R4 (impl-plan) — ZigZag/promotion-semantics critique

Lane: cell-graph topology — dimension discipline, filing/promotion ordering, link
eviction, genesis sequencing. Target: `design/vagent-metaprogramming-implementation.md`
v4. Reference: `design/vagent-metaprogramming.md` v1.19.

Method as r1–r3: every new/changed cite re-verified against the tree. This round's
new verifications: `arena_manifold.cpp:1733-2055` end-to-end again (discovery
`:1755-1788`, `edge.dim` mint-through `:1781`/`:2030-2042`, `!isEphemeral` skip
`:1867-1870`, `holdsOwn`/`isProxy` link gate `:2020-2053`), `arena_manifold.hpp:604-618`
(`unlink` = `link(…, nullopt)` — no chain repair), `arena_manifold.hpp:833-846`
(the `@param root` reachability text sits at `:833-836`, refusals at `:837-838`),
`arena_manifold.cpp:30-105` (`dimensions()` base∪`arenaDims_`, `ensureDimension`
name-arm, `dimDims && home` rank gate), `store.cpp:1057-1090` (`makeDimension` =
unconditional `makeCell`+`setLink`+`registerDim` — no name check), `manifold.cpp:782-821`
(`dimensionNamed` registry+`d.alias` walk), `dimension_registry.cpp:194-217`
(`getOrCreate` rebuild+`setStore`), `store.cpp:508-539` (`rebuildManifold` self-attaches
`store_` both arms), `vortex_core.cpp:14-70` (ctor→`initGenesis`; `mintNamedDimension`
resolves `baseDimensionNamed` first `:18-21`; 17 mints `:50-69` + `home` `:35` +
`d.dims` `:44` = nineteen names), `worker_context.hpp:51-60`, `mesh_actor.cpp:467-479`
(pop at `:468`, `executor_` at `:474-476`), `:659-662` (`pending` = `mailbox.size()`),
`:182-190`/`:255-266` (resume take-or-verify), `mesh_actor.hpp:81` (non-optional
`message`), `mesh_evaluator.cpp:733-735`, `or_parallel.cpp:181-184`, `dim_cap.cpp:83-85`,
`command_transaction.cpp:16-18`, `sql_promotion.cpp:140`, `mesh_value.cpp:470-476`,
`mesh_jobs.cpp:304-318`, `publication.cpp:1093-1103`/`1129-1131`,
`store.cpp:1388-1427`/`1418`/`1425`, `store.cpp:3836-4018` (`adoptOpRecords`),
`store.cpp:121-123`, `vagent/compiler.cpp:769-772`, `overlay.cpp:44-82`,
`vortex_core.cpp:95-98` (`findOrMintDimension`). All land.

## R3 closure verification — all items closed

| r3 item | v4 disposition |
| ------- | -------------- |
| **M3-1** (promote-visible class) | **Closed in substance; the two-root tail of the fix is flawed — M4-1.** The class clause landed exactly (:421-429): content ops only for `isEphemeral`, link-writes only on `holdsOwn`, shadowed base cell gets no content op, removed base edge never un-emitted (cites verified). Mint-and-repoint for counters landed (:425-427) — mechanically sound: the repoint is a link-write on a held/shadowed record cell → emitted; the fresh ephemeral's content op carries the value. Replay-vs-tombstone for removals landed (:428-429) — right for `learn_plan`, mis-paired for `unlearn` (m4-2). `root` = agent cell named with spec cites (:429-430). |
| **m3-2** (`+d.name` exemption, phase-6 ref arm) | **Closed.** "per-ref-record `Int64(*handleTarget)` writebacks *minus the `+d.name`-bearing records the dim arm owns* (spec :2394-2396)" (:535-537) — the double-write hazard named verbatim. |
| **m3-3** (filing-per-cycle) | **Closed.** "Filing re-enumerates per cycle — the same name-table sweep runs before each `rebuildManifold`, filing only what the rank lacks, so session mints *and* the (b)/(e) genesis re-mints file rather than orphan per fold (each `makeDimension` is a permanent rank append, so the sweep is idempotent by name)" (:397-400). Verified: `makeDimension` appends unconditionally (`store.cpp:1066-1090`), so the idempotence is the sweep's name-check — correctly attributed. The trailing residual sentence was not re-scoped for this — m4-1. |
| **m3-1's root naming** | **Closed with a flaw** — see M4-1. |
| n3-1 (`or_parallel.cpp` cite) | **Closed — and my r3 nit was wrong.** v4 kept `:181-184`; the four `ensureDimension` calls do sit at `:181-184` on the current tree (`dimOr_`:181 … `dimSubmission_`:184). The drift item I filed was stale or mis-read; v4 is correct. |
| n3-2 (`arena_manifold.hpp` signature cite) | **Closed** — `:844-846` is the `[[nodiscard]] std::optional<Promoted>` + signature span, exact. |

Disposition minor-bundle items in my lane also verified landed: never-the-arena-`d.dims`-rank
guard + interned-content naming (`:386-389`, spec's `:2303-2306` wording carried);
`OpHandle` "do exist today" correction + post-replay kind-guard (`:482-486`,
`mesh_evaluator.cpp:733` verified); `pending(pid)==1` recast as the *flag-time*
queue-depth test with the pop-before-callback ordering (`:540-544`, `mesh_actor.cpp:468`
vs `:474-476` verified, spec `:2163-2170` carried correctly); (e)'s resolver naming
(`symbol_named` module-`d.vars` walk / label→head, `:470-473`); the emission contract's
two dim-record kinds (`:236-238`); the driver's `genesis.{hpp,cpp}` TU home (`:412-415`);
`Makefile:698`, `store.cpp:3836-4018`, `vagent_compiler_test.cpp:22-53`,
`Makefile:1330-1334`, §2-glossary, ~25 call sites, "nineteen names", `WorkerContext`
base-is-rebuilt-fold gloss, `vagent/prelude/` convention wording — all applied.

## Findings

### M4-1 — MAJOR — plan:429-434 — the two-root answer republishes the class its own first half says never republishes

**Claims:** "`root` is the **agent cell** (spec :375, :635-636 — program cells never
republish; they ride the folded prefix and (d) re-enacts), and session mutations span
two disjoint roots — `program()`/`agent_cell()` are distinct adapter-bound roots never
`+d`-reached from each other (spec :2265-2266) — **so the cycle promotes once per
runtime root, or under a harness session root linked to both**" (:429-434).

**Code/spec:** the inference is wrong about what a program-rooted promote carries. All
durable session state already hangs on the agent cell's ranks — `d.code` is explicitly
"agent → fragment heads" and "hangs on the agent cell, not arena `home`" (spec :2465,
:1161); the top-level ranks `d.code`/`d.patch`/`d.synapse` sit on the agent (spec
:1870-1871); clause-correlated weight records key on *the agent's own* `d.vars` (spec
:2650-2651). What sits under `program()` in-session is the *installed* mutation surface
— grafted copies and repointed clause edges — which is deliberately **not** carried by
promote: "a session graft is gone at `resume`" (spec :377) is the whole `graft`/`learn`
split, and `learn`'s installs re-derive through the agent-side `d.patch` record + (d)
replay. Promoting the program root reaches every shadowed program cell's link-writes
and every ephemeral under them (`arena_manifold.cpp:1755-1757` stops only at
*unshadowed* base cells; `isEphemeral` mints follow), so: (i) session `graft_*` installs
become durable — the phase-4 verb split collapses; (ii) `learn_handler`'s replay
"simply re-appends" (spec :2364-2365) on top of the promoted copy → duplicate handler;
(iii) `learn_plan`'s replay re-matches the *alias* verbatim, and `patch:<n>` labels are
unreachable by alias match (spec :1322-1324), so the promoted copy is no hit → replay
appends a second clause beside it; (iv) a "harness session root linked to both" has the
identical reachable set → identical failure. The cited spec line (:2265-2266) is from
the **(a) classification walk**, where it grounds "no agent-rooted second walk is
owed" — the opposite inference: for promote, no program-rooted walk is owed precisely
*because* program-side changes re-derive.

**Fix:** keep `root` = agent cell; recast the tail — the promote root set is the agent
cell plus any *other state-side* anchor a session legitimately roots elsewhere (none
exists in today's vocabulary — every durable rank hangs on the agent cell); `program()`
is deliberately never a promote root — program-side session changes ride the
agent-side `d.patch` records + (d) replay, and a program-rooted (or program-spanning)
promote republishes session grafts that must die at `resume` and double-carries what
(d) re-enacts.

### Minor findings

- **m4-1 — plan:404-409** — the residual sentence wasn't re-scoped after m3-3's
  per-cycle filing landed. "An unfiled session-minted dim accretes one
  `dimensionNamed`-invisible orphan per checkpoint" now misdescribes the surviving
  residual in both halves: (i) *"unfiled"* — per-cycle filing enumerates
  `dimensions()`' base∪`arenaDims_` before every `rebuildManifold`, so every
  session-arena mint gets its **name** filed; the class is now empty except mints on
  arenas the sweep doesn't enumerate (compile arena, foreign spaces) and
  post-last-sweep mints. (ii) *"per checkpoint"* — the real residual under per-cycle
  filing is **one orphan per session-minted name at its first fold**: the arena mint
  cell `A` ≠ the filed cell `B` (`makeDimension` mints store-side, `store.cpp:1066` —
  it never aliases the arena cell), the session's edges named `A`, and
  `discover(edge.dim, false)` puts `A` in `order` so promote mints `A'` and emits the
  edges on dim `A'` (`arena_manifold.cpp:1781`, `:2030-2042`) — the first-use edge
  split. Post-fold `dimensionNamed` returns `B`; the first-session edges are stranded
  on the dead `A'` — the same spec residual (:2399-2402), narrowed, not eliminated.
  One clause: filing makes the *name* converge; each session-minted dim still orphans
  its own arena cell once, carrying its pre-filing edges.
- **m4-2 — plan:428-429** — "`unlearn`'s record drop" is mis-paired with
  `learn_plan`'s supersede under "(d) replay or tombstone-form records". For
  `learn_plan`'s supersede the claim is exact: the folded prefix resurrects the
  declared clause and (d) re-enacts the alias-match unlink (spec :2359-2362). For
  `unlearn`'s record drop the mechanics differ: a *mid-rank* drop isn't an unlink at
  all — `unlink` = `link(…, nullopt)` does no chain repair (`arena_manifold.hpp:616-618`),
  so excising mid-rank is a **repoint**, a link-write that promotes fine; only the
  *tail* drop is a true edge-removal the link loop can't emit — and there "(d) replay"
  is the **hazard**, not a survival path: the resurrected record re-enacts the
  unlearned patch. Since `unlearn` undoes the *latest* patch (the tail member, spec
  :1175), the common case needs the tombstone/record form — or the drop silently
  doesn't persist and the mutation returns at genesis. One clause splitting the two
  examples fixes it.
- **m4-3 — plan:415-421** — the driver's cycle enumerates
  "promote → `rebuildManifold` → fresh `WorkerContext` → re-run (a)–(e)" without the
  filing sweep the filing bullet inserts "before each `rebuildManifold`" (:397-400).
  The sweep's seat in the cycle is inferable but absent from the load-bearing chain —
  a reader sequencing the loop from the driver bullet alone drops it. One token in
  the chain (sweep → promote → rebuild → …) closes it.

### Nits

- **n4-1 — plan:433-434** — "`promote` folds only its `root`'s reachable subgraph,
  `arena_manifold.hpp:837-846`": the load-bearing reachability text is `:833-836`
  ("Only what is reachable from it along any dimension is promoted"); `:837-846` is
  the `@return` (budget/bad-root refusals) + signature. Off by ~4 lines, same block.
- **n4-2 — plan:496** — "`overlay.cpp:46-82` runs the identical mint→rebuild→re-pin
  loop" — the cite is right (`overlay.cpp:44-82` does fold → `dimensionNamed`/
  `makeDimension` → `rebuildManifold` → `setLink` → `rebuildManifold`); "identical"
  overstates — it's a Store-level overlay-release loop, not the arena/genesis loop —
  but as an offered precedent it stands.

## Verified accurate (v4's new promote/durability text carries v1.19 correctly)

- **The promote-visible class clause is exact** — `isEphemeral`-only content ops
  (`:1867-1870`), `holdsOwn`-gated link writes deduped against base (`:2020-2053`),
  `d.dims` skipped in discovery (`:1766-1768`) — all cites land verbatim.
- **Mint-and-repoint is mechanically right** — the counter record is agent-side
  (spec :2505-2508: "a durable per-agent counter on the agent cell's `d.vars` rank —
  a module's `d.vars` is program-side and never holds it"); a repoint is a
  `holdsOwn`-visible link-write; the same covers `set_weight`'s junction arm on
  previously-promoted scalars (spec :2666-2668's in-place `Value`-write is the
  anti-pattern named).
- **Filed dims across the second fold: still correct end-to-end.** `makeDimension`
  appends ops (`store.cpp:1066-1090`); each fold replays them once (linear ops, no
  per-fold duplication); `dimensionNamed` resolves registry-fast-map → `d.alias`
  rank walk (`manifold.cpp:782-821`); `held()`'s `contains` check + position-stable
  ids keep registry entries valid across rebuilds of one store; `rebuildManifold`
  self-attaches `store_` both arms (`store.cpp:511`,`:532`); `mintNamedDimension`
  resolves `baseDimensionNamed` before minting (`vortex_core.cpp:18-21`) — the
  "file before `initGenesis`" ordering is real: filed names hand the compile arena
  the canonical cells, so image edges ride the filed dim, never an orphan.
- **Per-cycle filing converges names correctly** — (b)/(e) re-mints land in the
  post-rebuild arena's `arenaDims_`, file at that cycle's own pre-rebuild sweep,
  resolve as filed at the next genesis; `makeDimension`'s unconditional append is
  safely behind the sweep's name-check. The residual m4-1 names is fidelity, not a
  mechanism error.
- **`pending(pid)`** — v4's "flag-time queue-depth test — flagged message
  sole-queued" + "mailbox-empty-at-turn-end is `pending(pid)==0` sampled inside the
  callback" matches the code (`mailbox.pop_front()` `:468` precedes `executor_`
  `:474-476`; `pending` returns `mailbox.size()` `:659-662`) and spec `:2163-2170`.
- **The two `patch:<n>` label cells / verbatim re-mint** (`:460-463`) and the
  (d)→(e) install-set hand-off (`:466-467`) carry spec `:2351-2357`/`:2374-2380`
  exactly; the (e) dim-arm text matches `:2393-2406` verbatim including the
  `+d.name` exception and the every-genesis scope.
- **`wake`/`std::optional` seam** — `ActorExecution::message` non-optional at
  `mesh_actor.hpp:81`; `resume` take-or-verify cites `:182`/`:188`/`:261` all land.
- **All session-mint cites land** — `d.op-label` `mesh_value.cpp:473`,
  `or_parallel`'s four `:181-184`, `dim_cap.cpp:83-85`, `command_transaction.cpp:16-18`,
  `sql_promotion.cpp:140`, `dimConstant` `vagent/compiler.cpp:769-772`,
  `attach()`'s `d.store-refs` `arena_manifold.cpp:289-290`, `job.base`
  `mesh_jobs.cpp:304-318`, `historyFromSeal` `publication.cpp:1097`,
  span-source pair `store.cpp:1418`/`:1425` bodies at `:1388-1425`,
  `adoptOpRecords` `store.cpp:3836-4018`.

## Counts

- BLOCKING: 0 — MAJOR: 1 — MINOR: 3 — NIT: 2

Top issues: (1) the two-root tail of v4's own root fix — "promotes once per runtime
root, or under a harness session root linked to both" republishes program-side session
mutations that must die at `resume` (session grafts become durable; `learn_handler`'s
re-append duplicates; `learn_plan`'s alias re-match can't see its own promoted copy) —
`program()` is deliberately never a promote root since every durable rank hangs on the
agent cell and program-side changes ride records+(d) (M4-1); (2) the residual sentence
still describes the pre-filing world — under per-cycle filing the residual is one
orphan per session-minted *name* at first fold (arena cell `A`≠filed `B`, first-session
edges stranded on `A'`), not "one per checkpoint" for "unfiled" names (m4-1); (3)
`unlearn`'s record drop mis-paired with `learn_plan`'s supersede — mid-rank drops are
repoints that promote fine, and for the tail drop replay is the resurrection hazard,
not a carrier (m4-2).

VERDICT: NOT CONVERGED — every r3 finding closed (m3-2/m3-3 clean, filing-per-cycle and
the exemption landed exactly); the M3-1 fix landed but its two-root tail prescribes the
one thing `promote`+replay jointly forbid; one-clause fixes throughout.
