# r5 — system-architecture review (phase ordering and deliverability) — FINAL

**Verdict:** CONVERGED. The lane's sole major (M-1, the spanning-root clause —
predicted in r4 to be one clause from done) landed correctly, both minors
landed, and a fresh majors-only sweep of the v5 delta and the whole dependency
chain finds the plan implementable end-to-end. Trajectory closes: r1 ~12
structural → r2 2 → r3 2 → r4 1 → r5 0.

**Counts:** 0 BLOCKING / 0 MAJOR / 0 MINOR / 1 NIT (carried, cosmetic)

Plan under review: `design/vagent-metaprogramming-implementation.md` v5.
Spec refs `design/vagent-metaprogramming.md` v1.19.

## Directed verification — M-1 (promote root is the agent cell alone): **CLOSED**

v4's "the cycle promotes once per runtime root, or under a harness session
root linked to both" is gone. v5 :441-450 now reads:

- "`root` is the **agent cell** alone (spec :375, :634-635)" — :375 "the
  checkpoint promotes what is reachable from the agent cell"; :634-635
  "`promote` walks only what the agent / cell reaches" (sentence wraps the
  :634/:635 break — cite lands exactly on it). ✓
- "program-side state is *never* a promote root — bare grafts must die with
  the incarnation (spec :375-377, :1791-1799 — that boundary is what makes
  `learn` = graft + record a distinction)" — :377 "a session graft is gone at
  `resume`"; :1798 the installed-copy/memo-record class "die with the
  incarnation at `resume`". Both prongs of the death-boundary argument are
  spec-backed. ✓
- "graft-adds would carry without prune-removes" — consistent with the
  code-verified fact that promote never un-emits an unlink
  (`arena_manifold.cpp:2020-2053`, verified r4). ✓
- "replay-installed copies would double under (d), whose alias re-match can't
  see a promoted `patch:<n>` label (spec :1322-1324)" — :1324 "(a learned
  clause's `patch:<n>` label is unreachable — `patch:` is refused as a `name`
  argument)", inside the verbatim-alias-match passage. The doubling mechanism
  is exactly the r4 analysis. ✓
- "`promote` folds only its `root`'s reachable subgraph
  (`arena_manifold.hpp:833-836`)" — :833-836 is the `@param root … Only what
  is reachable from it along any dimension is promoted` doc block, verbatim.
  ✓
- "all durable state already hangs on the agent cell's ranks
  (`d.code`/`d.patch`/`d.synapse`/`d.vars`, spec :1870-1871, :2465)" —
  :1870-1871 "the top-level ranks themselves — `d.code`, `d.patch`,
  `d.synapse` on the agent cell"; :2465 the `d.code` registration bullet;
  `d.vars`-on-agent-cell spec-backed at :2266 and the :1297-1300 counter
  passage. Within promote's cell-graph domain the claim is exact — store-layer
  state (lineage seed, filed `d.dims` names, the `programRef` bind record)
  lives outside the fold by construction and the plan handles it in (a)–(c).
  ✓
- "spec :2265-2266's two-root vocabulary belongs to (a)'s classification
  walk, not to promote" — :2266 is `program()`/`agent_cell()` "distinct
  adapter-bound roots never `+d`-reached from each other", inside the `>E_ops`
  classification text. Correctly re-scoped — it was never a promote license.
  ✓
- "The only program-rooted promote is the one-time image seed named below" +
  :450-452 `promote` of the compile arena / `adoptOpRecords`
  (`store.cpp:3836-4018`) — the ruling's carve-out, preserved. ✓

The fix is the ruling verbatim, with every new cite verified against spec and
code. The spec-harmful alternative is deleted, not hedged.

## Directed verification — m-2 (`+d.values` repoint precision): **CLOSED**

:434-435 — "the `d.vars` serial counter bumps a *fresh ephemeral* the keying
record's `+d.values` edge repoints at (spec :1300)". The record — not "the
rank" — repoints, and :1300 is precisely the
"`+d.vars → [key] +d.values → scalar` record" line. Suggested fix landed
word-for-word.

## Directed verification — m-1 (M13 refusal-edge cite ranges): **CLOSED**

:168-172 — "…opaque `Code` actuals — spec :456-470 — plus `splice c := 3`,
§5.2's "a `splice` is never a place" rule applying to *all* splices, ordinary
`let`-bound or `Code`-splice alike (spec :784-786 — cap$N is not
re-assignable)". Verified: `recursive-template` :456-459, control-flow bar
:460-462, opaque actuals :463-465, and the `effect`/`native`/`capacity`/
`graft_method` bar *begins* at :470 (runs :470-473 — its tail extends three
lines past the range's end, but every named arm starts inside it; adequate as
a starting coordinate per the plan's own cite discipline, :617-618).
`splice c := 3` at :784-786 is exact and correctly re-scoped to §5.2's
all-splices rule per the disposition (meta m8's point).

## Disposition minor bundle — spot-verified

`unlink` mid-rank repoint vs tail tombstone (:437-441): `unlink` =
`link(from, dim, dir, std::nullopt)` verbatim at `arena_manifold.hpp:616-618`;
consistent with promote's emit model. First-fold orphan re-scope (:410-415):
`store.cpp:1066` is `makeDimension`'s `makeCell` (store cell `B`),
`arena_manifold.cpp:1781` the `discover(edge.dim/pos/neg)` walk, `:2030-2042`
the remap + base-skip + `setLink` block. "nineteen cells, eighteen filed
names" (:399-401): `vortex_core.cpp:35` `makeCell("home")` — `home` is a plain
cell, not a dimension; `:44` `makeCell("d.dims")`; `:50-69` the seventeen
system dims (r4-verified). `dimDims && home` conjunct restored at :394-395 —
gate sits at `arena_manifold.cpp:93`, inside the cited :91-103. `'splice'
Postfix` + peeling rule at :106-108 — spec :736-738 verbatim ("the arglist
belongs to the `splice` (the peeling rule)"). `static` baseline :952-960 at
:205 — :957-960 names `choiceDepth() != 0`/`outstandingMarks() != 0` and the
`compiler.cpp:108-110` precedent. run/graft refusal-arm promise split landed:
phase 2 :177-179 promises each arm where the verbs live; phase 3 :273-274
states the shared refusal class once where the target class is defined.

## Majors-only pass — is the plan implementable end-to-end? **Yes.**

Swept the full v5 delta and the phase chain for used-before-built, inverted
dependencies, and untestable exits:

- The entire v5 delta sits inside phase 5's driver paragraph and adjacent
  cites — every piece verified above; nothing else moved.
- Ordering holds: bind set harness-supplied in phase 3, filed in phase 5 —
  and the filing bullet (:396-398) correctly keeps the non-dim members
  (program root, agent cell, `validate` constants) out of the rank, bound at
  (c). Session `learn_*`/`d.patch` records in phase 4 precede phase 5's (d)
  replay that consumes them. Phase 5's promote→rebuild→fresh-`WorkerContext`
  loop uses only in-tree machinery (`overlay.cpp:46-82` named as the running
  precedent); (e)-after-(d) ordering justified by the post-installation
  rebind rationale (:495-496). Phase 6 consumes phase-3's record schema for
  the marker mint and phase-5's filed rank for the dim arm — stated inputs.
- Exits: phase 5's "≥2 re-geneses" (:515) is checkable in the named
  harness loop (the re-link vs re-mint distinction genuinely needs the second
  fold); phase 6's P16 round-trip and phase 7's adapter-gating are honestly
  scoped.
- No exit names machinery its phase doesn't deliver; no Requires reaches
  backward past what earlier phases ship.

## NIT (carried)

- **n-1 (residual)** — `mesh_actor.cpp:473-477` for the `executor_`
  invocation (:557). Verified against the file: `mailbox.pop_front()` :468,
  `try {` :473, the `executor_` call :474-476, catch :477-479. Off by a line
  either side — a starting-coordinate cite, cosmetic only.

## Closing note

Five rounds, and the last defect was the expected one: v4 picked up a
half-analyzed disposition alternative and v5 replaced it with the ruled text,
code- and spec-verified end to end. Every mechanism the plan now sequences —
marshalling, emission contract, session records, the (a)–(e) driver, P15/P16 —
traces to spec lines that check out. This lane converges.
