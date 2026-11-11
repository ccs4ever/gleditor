# R5 (impl-plan, FINAL) — ZigZag/promotion-semantics critique

Lane: cell-graph topology — dimension discipline, filing/promotion ordering, link
eviction, genesis sequencing. Target: `design/vagent-metaprogramming-implementation.md`
v5 (commit `1e6ee056`). Reference: `design/vagent-metaprogramming.md` v1.19.

Method: full `git diff de7e8b69..1e6ee056` of the plan reviewed hunk-by-hunk; every
new/changed cite re-verified against the tree. New verifications this round: spec
`:375-377` (`:375` "program cells are therefore never *republished*", `:377` "a
session graft is gone at `resume`"), spec `:634-635` ("`promote` walks only what the
agent cell reaches" `:634`, "never enters the promoted set" `:635`), spec `:1322-1324`
("`patch:<n>` label is unreachable — `patch:` is refused as a `name` argument"),
spec `:1791-1799` (state-side reachability + prune + "die with the incarnation"),
spec `:2265-2266` (two-root sentence — sits inside the (a)-walk passage, names
§6.6's vocab), spec `:1870-1871` (`d.code`/`d.patch`/`d.synapse` on the agent cell),
spec `:2465` (`d.code` agent → fragment heads), spec `:1300` region
(`+d.vars → [key] +d.values → scalar` record shape, counter on the *agent cell's*
`d.vars` rank), spec `:2397-2402` (the spec's own per-checkpoint residual +
filed/unfiled rebind split), spec `:456-470` (opaque-`Code`-actuals at `:464-465`),
spec `:784-786` ("a `splice` is never a place … `splice c := 3`" at `:786`),
spec `:736-738` (peeling rule), spec `:952-960` (baseline check at `:959-960`),
spec `:2163-2172` (pending/wake/veto passage). Code: `arena_manifold.hpp:833-836`
(`@param root` reachability text — `:833-835`, exact), `arena_manifold.hpp:616-618`
(`unlink` = `link(…, std::nullopt)` body `:615-618`), `arena_manifold.cpp:91-103`
(`dimDims` `:91`, `home` `:92`, gate `:93`, append `:94-102`), `:34-46`/`:87-88`
(`dimensions()`/`intern`+`makeCell`), `:1755-1788` (discovery incl. `d.dims` skip
`:1766-1768`, `discover(edge.dim,false)` `:1781`), `:1867-1870` (`!isEphemeral`
self-map), `:2020-2053` (`holdsOwn` gate `:2021-2023`, dedupe `:2036-2038`,
emit `:2040-2042`), `store.cpp:1057-1090` (`makeDimension` unconditional mint —
`makeCell` at `:1066`), `vortex_core.cpp:35`/`:44`/`:50-69` (home + `d.dims` +
17 `mintNamedDimension` calls), `worker_context.hpp:52-60`, `mesh_actor.cpp:467-477`
(pop `:468`, `executor_` `:474-476`), `mesh_actor.cpp:659-662` (`pending` =
`mailbox.size()`). All land.

## R4 closure verification

| r4 item | v5 disposition |
| ------- | -------------- |
| **M4-1** (two-root promote) | **Closed — the fix is correct and complete.** v5 :441-450: "`root` is the **agent cell** alone (spec :375, :634-635): program-side state is *never* a promote root — bare grafts must die with the incarnation (spec :375-377, :1791-1799 — that boundary is what makes `learn` = graft + record a distinction), graft-adds would carry without prune-removes, and replay-installed copies would double under (d), whose alias re-match can't see a promoted `patch:<n>` label (spec :1322-1324)." All three failure modes carried correctly; the harmful "once per runtime root / harness session root linked to both" is gone; spec :2265-2266 is correctly re-attributed to "(a)'s classification walk, not to promote"; the agent-hung durable inventory (`d.code`/`d.patch`/`d.synapse`/`d.vars`) is correctly invoked against the old "anything unreached drops silently" tail; the one program-rooted promote is named as the one-time image seed (`:449-452`), matching R4-A's carve-out. Verified: nothing durable drops because all durable *session* state is agent-hung — the filed `d.dims` rank on `home` is store-level ops written pre-promote, not arena-overlay payload, so it is not "dropped" by an agent-rooted walk. |
| **m4-1** (residual re-scope) | **Closed.** v5 :410-414 now reads: "an unfiled session-minted dim costs one `dimensionNamed`-invisible orphan at its *first* fold — the first-use edge split: `makeDimension`'s store cell `B` while the session's edges named arena cell `A`, and `discover`→promote mints `A'` carrying them (`store.cpp:1066`, `arena_manifold.cpp:1781`,`:2030-2042`) — not a per-checkpoint accrual." The mechanism and all three cites land exactly. On close read the "unfiled" qualifier is also *defensible*, not just tolerable: within the sentence's scope ("names whose edges can fold") every session-arena mint sits in `arenaDims_`, gets enumerated by the pre-`rebuildManifold` sweep, and is unfiled only at edge-write time — filed dims pay the same one-orphan first-fold cost for their pre-filing edges, and dims the sweep can never enumerate (foreign-space dims) can't emit foldable edges at all (`contains` fails in `discover` → `dim == real.end()` → emit skipped at `:2031-2033`). So first-fold-only is exact for the whole reachable class; the per-checkpoint regime the spec's `:2399-2400` describes survives only where the plan's filing sweep is absent (the spec's own model has no per-cycle filing — the cite correctly anchors the residual's home, not its rate). |
| **m4-2** (unlink-vs-tombstone) | **Closed.** v5 :437-441: "`unlearn`'s mid-rank record drops are repoints (link-writes, `unlink` = `link(…,nullopt)` does no chain repair, `arena_manifold.hpp:616-618`) that promote fine, but its *tail* drop is a true removal where replay is the resurrection hazard and only tombstone form works." The mid-rank/tail split is exactly the fix; `learn_plan`'s supersede no longer stands as the removal example (it stays correctly inside (d) at `:469-470`). |
| **m4-3** (sweep token in the driver chain) | **Not applied — declined.** The disposition's minor bundle does not list it, and the driver chain at :424-430 still runs `promote` → `rebuildManifold` → fresh `WorkerContext` → (a)–(e) with no sweep step. The sweep's seat remains inferable from the filing bullet ("the same name-table sweep runs before each `rebuildManifold`", :404-407). Stands as a declined minor — the reader sequencing the loop from the driver bullet alone still drops it. |
| **n4-1** (`:833-836` cite) | **Closed** — `:446` now cites `arena_manifold.hpp:833-836` for the reachability text; `:426` keeps `:844-846` for the signature. Both exact. |
| **n4-2** ("identical" `overlay.cpp` loop) | **Not applied — declined.** ":511" still says "the identical mint→rebuild→re-pin loop" (a Store-level overlay-release precedent, not the arena/genesis loop). Stands as a declined nit. |

## Disposition minor-bundle items in/near my lane — all verified landed

- `d.vars` repoint precision (:434-435): "the keying record's `+d.values` edge
  repoints at (spec :1300)" — the `+d.vars → [key] +d.values → scalar` record shape
  is at spec :1300-1303. Lands.
- `dimDims && home` conjunct restored (:394-395, `(:91-103)`) — the gate block is
  exactly `arena_manifold.cpp:91-103`.
- "nineteen cells, eighteen filed names (`home` is a plain genesis cell, not a
  dimension)" (:400-401) — verified: `home` `makeCell` at `vortex_core.cpp:35`,
  `d.dims` at `:44`, 17 `mintNamedDimension` calls `:50-69` (d.grab…d.role incl.
  the vicon-added `d.scan` at `:67`) = 19 cells, 18 dim names. Exact.
- spec `:635-636` → `:634-635` (:441) — lands.
- spec `:464-470` → `:456-470` for opaque actuals plus `:784-786` for `splice c := 3`
  (:170-172) — the never-a-place rule is §5.2's all-splices rule; both cites land.
- spec `:952-955` → `:952-960` (:205) — the `choiceDepth`/`outstandingMarks`
  end-of-run check sits at `:959-960`; the widened span now covers it. Improvement.
- `'splice' Postfix` + peeling (spec `:736-738`) added at :107-109 — lands; the
  `splice inc(count)` call-shaped-operand-refusal consequence is spec-exact.
- `run`/`graft` refusal-arm split at :178 — `run` arm in phase 3, `graft` arm in
  phase 4. Lands.

## Findings — none blocking or major

### Nits

- **n5-1 — plan:560** — the pending/veto cite narrowed from v4's `:2163-2170` to
  `:2165-2169` and now shaves both ends of the claim bundle: `pending(pid) == 1`
  is at spec `:2164` (one line above the span) and the explicit ordering veto
  ("**No turn — message or P4 wake — runs between the flagged turn's end and
  `publishValue`**") is at `:2171-2172` (two below). The middle of the bundle —
  `pending` exposes `mailbox.size()`, `checkpointView` refuses a non-empty
  mailbox (the self-send veto mechanism), the P4-wake mint — does sit inside
  `:2165-2169`, so nothing is misattributed; restoring `:2163-2172` recovers the
  two edge lines. One-token cite widen, no mechanism impact.
- **n5-2 — carried, declined** — m4-3 (sweep token absent from the driver bullet's
  cycle chain at :424-430) and n4-2 ("identical" at :511) remain unapplied per the
  r4 disposition; recorded here for the record, no relitigation.

## Majors-only pass on v5's edited text — clean

Every v4→v5 hunk in and around my lane was re-derived against the code, not just
cite-checked:

- The new root paragraph (:441-450) introduces no new error. "All durable state
  already hangs on the agent cell's ranks" is exact in context: the only thing an
  agent-rooted `promote` adds to a same-`Store` cycle is arena-overlay session
  state, and the durable slice of that is agent-hung by construction (the spec's
  state-side test prunes at the program-side boundary, `:1791-1799`); the filed
  `d.dims` rank and the folded prefix are store-level and pre-promote, so nothing
  durable drops. The image-seed exception is correctly scoped ("the only
  program-rooted promote is the one-time image seed", :449-452).
- The residual sentence's scoping holds under the enumerate-then-file sweep
  (analysis under m4-1 above): first-fold-only is exact for the whole
  edges-can-fold class; the never-enumerable corner cannot carry foldable edges.
- The mid-rank/tail `unlearn` split is mechanically exact: mid-rank excision
  re-points a `holdsOwn`-visible link-write (emits), the tail drop is the removed
  base edge that never un-emits — so the resurrected record re-enacts under (d)
  absent tombstone form. Correct pairing restored.
- Filing arithmetic exact: 19 genesis cells (`home` + `d.dims` + 17 mints), 18
  filed names; `dimDims && home` is the correct gate conjunct; the rank-append
  idempotence-by-name claim still holds (`makeDimension` never name-checks,
  `store.cpp:1057-1090` — the sweep's name-check is what makes it safe).
- Phase 5 Exit's new "orphan-twin test asserting one cell per filed dim name
  after a fold" (:521) matches the re-scoped residual — one filed `B` per name,
  dead `A'` beside it. Consistent.
- No inverted dependency, no untestable exit, no cite off by more than the n5-1
  edge-shave anywhere in the diff.

## Counts

- BLOCKING: 0 — MAJOR: 0 — MINOR: 0 — NIT: 2 (n5-1 cite edge-shave; n5-2 carried
  declined items).

Top issues: (1) n5-1 — the `:2165-2169` spec cite at plan:560 drops the
`pending(pid) == 1` line (:2164) and the no-turn veto (:2171-2172); widen to
`:2163-2172`. (2) n5-2 — two declined r4 items stand: the filing sweep is still
absent from the driver bullet's cycle chain (m4-3, minor), and `overlay.cpp:46-82`
is still called "identical" to the genesis loop (n4-2, nit).

VERDICT: **CONVERGED** — M4-1 fixed correctly and completely (the root is the
agent cell alone; program-side is never a promote root, with the graft-must-die,
asymmetric-carry, and replay-double mechanics all stated correctly); both minors
landed exactly; the remaining items are declined-or-nit class. The plan's
promote/durability text now carries v1.19 faithfully.
