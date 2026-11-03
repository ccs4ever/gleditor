# r5 — Vortex / VM-semantics critic (final round)

**Verdict: CONVERGED — 0 majors.** R4-A landed correctly in every load-bearing
particular: `root` is the agent cell alone, program-side is never a promote
root, the one-time program-rooted image seed is named as the sole exception,
and the durability rationale is stated as the mechanism it is — each of its
three legs re-verified against the spec today. My n1's `arena_manifold.hpp`
half landed; its *spec* half propagated an off-by-one that was my own r4
error (the original `:635-636` was right). One residual minor on the orphan
residual's spec cite; nothing mechanically wrong in v5's edited text.

Counts: **0 BLOCKING / 0 MAJOR / 1 MINOR / 3 NITS**

Plan refs are `design/vagent-metaprogramming-implementation.md` v5; spec refs
`design/vagent-metaprogramming.md` v1.19; code cites re-verified today
(`arena_manifold.*` lives under `apps/common/xanadu/zigzag/`).

---

## Verified — M1 fixed correctly (plan :441-450)

The old clause ("promotes once per runtime root, or under a harness session
root linked to both") is gone; nothing matching `runtime root`/`session
root`/`once per`-as-promote survives a grep of the file. The replacement,
clause by clause:

- **"`root` is the **agent cell** alone (spec :375, :634-635)"** — :375
  verified verbatim ("the checkpoint promotes what is reachable from the
  agent cell", M11). The spec half of the cite is off by one — see n1.
- **"program-side state is *never* a promote root"** — the ruling verbatim;
  and the boundary is now argued from the mechanism, not asserted.
- **"bare grafts must die with the incarnation (spec :375-377, :1791-1799 —
  that boundary is what makes `learn` = graft + record a distinction)"** —
  :377 "so a session graft is gone at `resume`"; :1791-1799 the "pruning is
  load-bearing … die with the incarnation at `resume`" text re-verified; the
  `learn` = graft + record definition sits at :373-374, inside M11 with the
  cited range. Correct.
- **"graft-adds would carry without prune-removes"** — mechanically exact:
  `unlink` exists only as arena sugar over `link(…, nullopt)`
  (`arena_manifold.hpp:616-618`, re-verified); `Store` has no unlink op, so a
  removed base edge is never un-emitted. A program-rooted promote would make
  the program side add-only durable — the worst pole, correctly named.
- **"replay-installed copies would double under (d), whose alias re-match
  can't see a promoted `patch:<n>` label (spec :1322-1324)"** — verified:
  :1322-1324 is the `learn_plan` verbatim-alias dedupe, "a learned clause's
  `patch:<n>` label is unreachable — `patch:` is refused as a `name`
  argument". A promoted copy on the `d.clause` rank can't be deduped away by
  the re-match, so (d) installs a twin each genesis. Exactly the failure the
  ruling predicted.
- **"nothing durable drops silently: all durable state already hangs on the
  agent cell's ranks (`d.code`/`d.patch`/`d.synapse`/`d.vars`, spec
  :1870-1871, :2465)"** — :1870-1871 names `d.code`/`d.patch`/`d.synapse` on
  the agent cell; :2465 is the `d.code` (agent → fragment heads) bullet.
  `d.vars` is not inside either cited line — its agent-cell cite is
  :1294-1295/:2504-2509 (which the plan itself cites at :355, :504). Claim
  true; cite list one short — n2.
- **"spec :2265-2266's two-root vocabulary belongs to (a)'s classification
  walk, not to promote"** — verified: the "distinct adapter-bound roots
  never `+d`-reached from each other" text sits inside the per-cell
  classification paragraph (:2256-2266, "The walk's remaining role is
  per-cell classification"), the comparand's *walk* — not a promote license.
- **"The only program-rooted promote is the one-time image seed named
  below"** — the seed is named two lines later (:451-452): "the initial
  image band comes from `promote` of the compile arena or `adoptOpRecords`
  (`store.cpp:3836-4018`)". Matches R4-A's exception clause verbatim.

The changelog (:11-14) states the fix accurately: "program-side is never a
promote root; program-side persistence is (d) replay's job".

## Verified — n1 fix, `arena_manifold.hpp` half

- "promote folds only its `root`'s reachable subgraph" now cites
  **`arena_manifold.hpp:833-836`** — the `@param root` doc text ("Only what
  is reachable from it along any dimension is promoted"), re-verified at
  :833-836.
- The signature cite **`arena_manifold.hpp:844-846`** was correctly retained
  at :426 for `promote(Store&, parent, arena, root)` (the signature spans
  :844-846).
- The disposition split my suggested `:833-846` into the two precise ranges —
  better than my suggestion. This half is done.

## Verified — the rest of the r4 minor bundle in my check-zone

- **`d.vars` repoint precision** (:434-435): "the keying record's `+d.values`
  edge repoints at (spec :1300)" — verified :1300's "each scalar field its
  own `+d.vars → [key] +d.values → scalar` record" against the counter at
  :1294-1295. Exact.
- **`unlearn` drop classes** (:437-441): mid-rank drops are repoints that
  promote fine; the tail drop is a true removal → tombstone only; `unlink` =
  `link(…,nullopt)` does no chain repair (`arena_manifold.hpp:616-618` —
  re-verified, the sugar is exactly :616-618). `learn_plan`'s supersede no
  longer mis-paired as the removal example; its re-match lives in (d)'s
  replay bullet (:470-471). Matches the disposition.
- **First-fold orphan residual** (:410-418): the mechanics verify —
  `makeDimension`'s store cell `B` at `store.cpp:1066` (the `makeCell`
  mint), `discover(edge.dim, false)` at `arena_manifold.cpp:1781`, the
  `real`-translated link-emit at `:2030-2042`; and `arena_manifold.cpp:
  1765-1768` (the walk's `d.dims`-edge skip, the spec's own cite at
  :2400) is why a filed name never pulls the session cell in. One wart —
  see m1.
- **Nineteen cells, eighteen filed names** (:399-401): 17
  `mintNamedDimension` calls (`vortex_core.cpp:50-69`) + `home` (:35) +
  `d.dims` (:44) = nineteen cells; filed = 17 dims + `d.dims` = eighteen,
  `home` a plain genesis cell — verified in r4, unchanged and right. The
  `dimDims && home` conjunct is stated (:394-395, gate at :91-103 —
  conjunct itself at :91-93; fine).
- **`'splice' Postfix` + peeling rule** (:107-109): spec :736-738 covers
  `splice f(args)` instantiation (:737-738) and opens the peeling-rule
  sentence — the rule is *named* at :739 ("the arglist belongs to the
  `splice` (the peeling rule)"). Covers the substance; :736-739 would be
  the tight range — n3.
- **`run`/`graft` refusal-arm split** (:177-178): phase 2 now promises the
  `run` arm in phase 3, the `graft` arm in phase 4 — the meta m9 fix. Phase
  3's :273-274 states the *rule* for both ("`run`/`graft` on a
  `Code`-parameterized (splice-only) fragment is a named refusal"), which
  is fine: only `run`'s arm is exercisable there, and phase 4's verbs
  inherit the stated rule. Not a defect.
- **`splice c := 3` cite** (:170-172): spec :784-786 verified — "A `splice`
  is never a place: a captured name expands to a `cap$N` *binding*, not a
  re-assignable slot, so `splice c := 3` is a check error". The plan's
  "all splices, ordinary `let`-bound or `Code`-splice alike" gloss is the
  correct generalization (the rule is stated unqualified at §5.2).
- **`static` baseline cite** (:205): spec :952-960 verified — the
  `vm_.run(cursor, staticFuel)` / result-read / no-mark discipline at
  :952-956 and the `choiceDepth() != 0`/`outstandingMarks() != 0` refusal
  at :958-960. Distinct from :960-961 (the mandatory-operand rule), which
  :107 correctly cites separately.
- **`spec :456-470`** (:170-171): all four M13 edges verified — recursion
  ban (:456-459), control-flow bar (:460-462), opaque `Code` actuals
  (:463-465), `effect`/`native`/`capacity`/`graft_method` bar (:469-470).

## New findings

### MINOR

- **m1 — `spec :2397-2402` sits against the quantifier it contradicts**
  (:414). "an unfiled session-minted dim costs one
  `dimensionNamed`-invisible orphan at its *first* fold … — **not a
  per-checkpoint accrual** (spec :2397-2402; …)". The spec text at
  :2398-2399 says the opposite rate: "an *unfiled* session-minted dim in
  use accreting one such orphan **per checkpoint**". The plan's claim is
  correct *under its own per-cycle filing* (:404-407 — the sweep files the
  name at the next cycle, so "unfiled" is a first-fold-only condition);
  the spec's per-checkpoint rate describes the permanently-unfiled case
  the sweep removes. The cite supports the *mechanism* (orphan twin beside
  the canonical re-resolved cell, :2397-2402) but reads as support for the
  *rate* it contradicts. Fix: scope the cite — e.g. "(spec :2397-2402 —
  the mechanism and the per-checkpoint rate for a dim that *stays*
  unfiled; the per-cycle sweep is what collapses it to one)". One word of
  scoping; not a mechanical error.

### NITS

- **n1 — `spec :634-635` is my own off-by-one, propagated** (:441). My r4
  n1 asserted the "Program cells do not *republish*: `promote` walks only
  what the agent / cell reaches" sentence spans :634-635. It does not:
  :634 is the blank line after the boundary table; the sentence spans
  **:635-636** — the range v4 originally had. The disposition applied my
  misreading. Fix: revert to `spec :635-636`. My error, owned.
- **n2 — `d.vars` outside the cited lines** (:447-448). The four-rank list
  cites :1870-1871 (covers `d.code`/`d.patch`/`d.synapse` on the agent
  cell) and :2465 (`d.code`); `d.vars`-on-agent-cell lives at :1294-1295 /
  :2504-2509 — which the plan itself cites at :355 and :504. Add
  `:2504-2509` to the pair, or drop `d.vars` from the parenthetical.
- **n3 — peeling-rule cite under-ranged by one** (:108-109): the rule is
  named at spec :739; `:736-739` is the tight range. Also at :425,
  "session mutations live in the `ArenaManifold` overlay … so each cycle
  promotes them into the fold" — `them` reads as all session mutations,
  but bare grafts are session mutations that deliberately do *not*
  promote; the boundary sentences two lines later fix the scope, so
  harmless — a "the durable ones" would tighten it.

---

## Spot-check status

Newly verified for v5: spec :373-377 (M11 + "gone at `resume`"), :634-636
(the republish sentence — establishing my own n1 misread), :736-740
(peeling rule), :784-786, :952-961, :1294-1304 (`+d.values` record shape +
agent-`d.vars` counter), :1317-1324 (alias dedupe / `patch:<n>`
unreachable), :1870-1871 (agent-cell top-level ranks), :2255-2269
(two-root vocabulary inside the classification walk), :2393-2404 (dim arm
+ the per-checkpoint accrual clause), :2458-2470 (`d.code` durable bullet);
`arena_manifold.hpp:615-618`, `:824-846`; `arena_manifold.cpp:1760-1783`,
`:2026-2045`; `store.cpp:1058-1091` (`makeDimension` mint :1066, tail
append :1088). Carried from r4 and unchanged in v5: the promote-visibility
mechanics (:1867-1870, :1984-2003, :2020-2053), the unfiled-mint site
list (`mesh_value.cpp:473`, `or_parallel.cpp:181-184`,
`dim_cap.cpp:83-85`, `command_transaction.cpp:16-18`,
`sql_promotion.cpp:140`), the (e)/P16 arm splits, and the
`pending(pid)`-sampling text (:556-560).

## Bottom line

R4-A is the rare fix that not only removes the wrong clause but states the
right reason: program-side persistence is (d) replay's job, and the plan
now says *why* each direction of a program-rooted promote would break
(add-only durability, dedupe-blind `patch:<n>` twins, the `learn` =
graft + record distinction collapsing). The residual is one cite
scoping (m1) and one self-inflicted cite off-by-one (n1). Ship it.
