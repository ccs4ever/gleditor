# r5-writer — document-coherence critique of `design/vagent-metaprogramming-implementation.md` (v5)

Verdict: **CONVERGED**. Counts: **0 BLOCKING / 0 MAJOR / 0 MINOR / 5 NIT** (one carried).

Every r4 item verified landed — my m-1 (refusal-edge cite ranges) and n-1 (`:952-960`) both closed,
the entire r4-disposition minor bundle checked clause-by-clause against v5, and the R4-A promote
rewrite — the round's largest single edit — verified end-to-end against spec v1.19 and the tree.
Residual is four fresh nits — one mid-sentence cite truncation (disposition-directed, one line
short of the completion), one forward-reference naming wobble, one topical cite sitting on a
negated predicate, one missing arm-name echo in phase 4's body — plus the carried mdformat wraps.
`mdl -i` clean (exit 0); `mdformat --check --wrap 100` with the three required extensions clean
(exit 0).

## My r4 items — both CLOSED

- **m-1 (refusal-edge cite ranges) — CLOSED** (:167-172). The enumeration now cites `spec :456-470`
  for the four `Code`-splice-`def` edges and `spec :784-786` for `splice c := 3`, re-scoped to
  "§5.2's 'a `splice` is never a place' rule applying to *all* splices, ordinary `let`-bound or
  `Code`-splice alike" (meta m8's correction folded in). Verified verbatim: spec :456-458
  `recursive-template`, :459-461 control-flow bar, :463-465 opaque actuals, :468-470
  `effect`/`native`/`capacity`/`graft_method` bar; :784-786 "a `splice` is never a place: a captured
  name expands to a `cap$N` *binding*, not a re-assignable slot, so `splice c := 3` is a check
  error".
- **n-1 — CLOSED** (:205). Cite now `spec :952-960`; :959-960 names the `choiceDepth() != 0`/
  `outstandingMarks() != 0` refusal verbatim ("so is a `static` that leaves `choiceDepth() != 0` or
  `outstandingMarks() != 0` at run end").
- **n-2 (carried) — persists.** The same five mdformat slash-wraps, renumbered with the edits:
  `maxExpansionDepth`/`maxSplices` (:137), `consume`/`terminate` (:169), `+d.refs`/`+d.recipe`
  (:240), `d.recipe`/`d.refFor` (:284), `learn_where`/`learn_body` (:338). mdformat-produced,
  CI-clean.

## R4-A — the promote rewrite — landed and verified

The "once per runtime root, or under a harness session root linked to both" text is gone entirely
(grep: no `session root`, no `runtime root` outside :281's unrelated bind-set usage). The rewritten
block (:430-452) reads cleanly in sequence — class first (what *can* emit), then root (what is
*reached*) — and every load-bearing cite verifies:

- "`root` is the **agent cell** alone (spec :375, :634-635)" — :375 verbatim "the checkpoint
  promotes what is reachable from the agent cell"; :635 "Program cells do not *republish*:
  `promote` walks only what the agent" (see n-1 on the truncation).
- "bare grafts must die with the incarnation (spec :375-377, :1791-1799)" — :377 "a session graft is
  gone at `resume`" (the boundary that makes `learn` = graft + record a distinction, exactly as the
  plan glosses); :1791-1798 the state-side definition with "they die with the incarnation at
  `resume`" and the program-side-boundary prune.
- "replay-installed copies would double under (d), whose alias re-match can't see a promoted
  `patch:<n>` label (spec :1322-1324)" — verbatim "a learned clause's `patch:<n>` label is
  unreachable — `patch:` is refused as a `name` argument".
- "`promote` folds only its `root`'s reachable subgraph (`arena_manifold.hpp:833-836`)" — the
  `@param root` doc, verbatim "Only what is reachable from it along any dimension is promoted".
  Correct fix from :837-846 (which had been the `@return` + signature span); the signature cite
  `arena_manifold.hpp:844-846` at :426 is untouched and still exact.
- "all durable state already hangs on the agent cell's ranks (`d.code`/`d.patch`/`d.synapse`/
  `d.vars`, spec :1870-1871, :2465)" — :1870-1871 "the top-level ranks themselves — `d.code`,
  `d.patch`, `d.synapse` on the agent cell"; :2465 opens the durable-layer bullets with `d.code`
  (agent → fragment heads), and the `d.vars` counter rides at :2505-2506 ("a *durable per-agent
  counter* on the agent cell's `d.vars` rank").
- "spec :2265-2266's two-root vocabulary belongs to (a)'s classification walk" — :2265-2266 sits
  inside the (a) walk text ("`program()` and `agent_cell()` are distinct adapter-bound roots never
  `+d`-reached from each other (§6.6's vocab names the two-root split)").

No contradictions: consistent with (a)'s per-cell `≤E_ops`/`>E_ops` classification (program cells
ride the sealed prefix, never republish), (d)'s verb re-enactment (program-side persistence is
replay's job — graft-adds would double if they also promoted), the durable bullet's
"program-side-original-never-registered invariant" (:506-507), (e)'s "never the promoted twin
`*handleTarget` names" (:492-493), and phase 6's (i) pin "folded over the seeded *and filed* lineage
`Store`" (:533-534).

## Disposition minor bundle — all landed

- **`d.vars` repoint precision** (:434-435): "a *fresh ephemeral* the keying record's `+d.values`
  edge repoints at (spec :1300)" — spec :1300 verified (the `+d.vars → [key] +d.values → scalar`
  record shape).
- **`unlearn` drop classes** (:438-441): mid-rank drops are repoints — "`unlink` =
  `link(…,nullopt)` does no chain repair, `arena_manifold.hpp:616-618`" verified verbatim
  (`unlink` returns `link(from, dim, dir, std::nullopt)` at :616-618) — that promote fine; the tail
  drop is a true removal → tombstone only. `learn_plan`'s supersede correctly no longer cited as the
  removal example — it appears only in (d) replay (:469-471, "displacing the superseded declared
  clause again"), where it belongs.
- **Residual re-scoped** (:410-415): "one `dimensionNamed`-invisible orphan at its *first* fold —
  the first-use edge split: `makeDimension`'s store cell `B` while the session's edges named arena
  cell `A`, and `discover`→promote mints `A'` carrying them (`store.cpp:1066`,
  `arena_manifold.cpp:1781`,`:2030-2042`) — not a per-checkpoint accrual". All three code cites
  verified (`makeCell` at store.cpp:1066; `discover(edge.dim)` at :1781; the promote link-write
  remap at :2030-2042). The unfiled-mint enumeration re-verified: `mesh_value.cpp:473`
  (`ensureDimension("d.op-label")` exact), `sql_promotion.cpp:140` (`"d.promoted"` exact),
  `command_transaction.cpp:16-18` (`d.target`/`d.action`/`d.reply` exact), `dim_cap.cpp:83-85`
  (`d.actor`/`d.ticket`/`d.signature` exact), `or_parallel.cpp:181-184` (four `ensureDimension`
  mints, unchanged).
- **"nineteen cells, eighteen filed names"** (:399-401): verified — 17 `mintNamedDimension` calls
  at `vortex_core.cpp:50-69` (`d.grab`…`d.role`), `home` at `:35`, `d.dims` at `:44`; `d.dims` is
  itself a dimension cell (filed), `home` plain → 19 cells, 18 filed names.
- **`dimDims && home` conjunct** (:395): "`(:91-103)`" now spans the `dimDims` fetch (:91), `home`
  (:92), the gate (:93) and the append (:102) — exact.
- **`'splice' Postfix` + peeling rule** (:107-109): spec :736-738 verified verbatim — "when the
  operand's outermost postfix ends in `( args )` the arglist belongs to the `splice` (the peeling
  rule), and any other call-shaped operand is a check error" — matching the plan's "without it
  §9.1's `splice inc(count)` parses as a call-shaped-operand refusal".
- **`run`/`graft` arm promise split** (:177-179): "the `run` refusal arm lands with the member
  surface in phase 3, the `graft` arm with the mutation verbs in phase 4" (meta m9) — see n-3 on
  the phase-4 echo.
- **Cite fixes**: `arena_manifold.hpp:833-836` and `static` baseline `:952-960` verified above;
  `spec :634-635` verified with the n-1 caveat.

## New findings — NIT only

- **n-1** (:441): `spec :634-635` (was :635-636, disposition-directed) — the cited claim "promote
  walks only what the agent | cell reaches" starts at :635 but its completion "cell reaches" sits at
  :636; :634 is the paragraph's blank line. The named phrase is in-range and the paragraph head is
  the right landing spot — a one-line truncation, same class as my r4 n-1. Harmless under the doc's
  starting-coordinate discipline; `:635-636` remains the tighter range.
- **n-2** (:450): "the one-time image seed named below" resolves one sentence later to "the initial
  image band comes from `promote` of the compile arena or `adoptOpRecords`" — the referent is
  unambiguous (the only program-rooted `promote` in the block; `adoptOpRecords` is not a promote),
  but the noun shifts ("image seed" → "image band"). One word closes it ("the initial image band
  seed below" or naming it directly).
- **n-3** (:411-415): the parenthetical "(spec :2397-2402; today's unfiled session mints include …)"
  sits directly on "not a per-checkpoint accrual", while the cited range states the spec's contrary
  figure — "an *unfiled* session-minted dim in use accreting one such orphan per checkpoint"
  (:2398-2399). The plan's first-fold account is disposition-sanctioned and mechanically argued
  in-sentence (the `A`/`B`/`A'` split plus per-cycle filing's (b)-re-mint→next-sweep rescue two
  sentences up), and :2401-2402 does contain the filed-name re-resolution the account relies on — a
  defensible topical pointer. But as placed it reads as spec support for the negated predicate; one
  qualifying clause (e.g., "spec :2397-2402's per-checkpoint figure is the no-rescue accounting the
  sweep bounds") makes the revision explicit.
- **n-4** (:338-342, carried wrap count separate): the phase-2 promise now points the `graft`
  splice-only-fragment refusal arm at "the mutation verbs in phase 4", but phase 4's body lists the
  `graft_*`/`learn_*` verbs without naming the arm — the only statement of it remains phase 3's
  "`run`/`graft` on a `Code`-parameterized (splice-only) fragment is a named refusal" (:273-274),
  where `graft` isn't yet callable. Coherent on the kinder reading (the rule is stated once for the
  pair; each arm is exercised where its verb exists) — a cosmetic echo gap; one clause in phase 4
  (or narrowing phase 3's sentence to `run`) closes it.
- **n-5 (carried):** the five mdformat slash-wraps at :137, :169, :240, :284, :338.

## Consistency checks run clean

- **Preamble** (:11-14) accurately summarizes the round — agent-cell-alone root, first-fold residual
  re-scope, unlink-vs-tombstone, cite fixes.
- **Phase map vs bodies**: all eight rows match; deps linear.
- **No residual spanning-root text** anywhere; (a)'s own "walk rooted at the adapter-recorded
  program root" (:457-458) is the classification walk, correctly retained.
- **Phase-5 exit** "one cell per filed dim name after a fold" (:520-521) — consistent with the
  re-scoped residual (filed → one cell; the unfiled orphan is the first-fold `A'`).
- **Format gates**: `mdl -i` exit 0; `mdformat --check --extensions gfm --extensions dollarmath
  --extensions frontmatter --wrap 100` exit 0.

## Top-3

1. **R4-A verified correct, not just landed** — the agent-cell-alone root is argued from three
   spec-grounded counterfactuals (grafts die at incarnation, carry-without-prune, (d) doubling on an
   unmatchable `patch:<n>` label), closed by the durable-ranks observation, and contradicts nothing;
   the "harness session root" text is fully gone.
2. **All r4 items closed** — both my findings (cite ranges `:456-470`+`:784-786`; `static` baseline
   `:952-960`) and the entire disposition bundle verified line-exact against spec and tree.
3. **Residual is cosmetic** — four nits: the `:634-635` mid-sentence truncation, the "image
   seed"/"image band" naming wobble, the topical `:2397-2402` cite sitting on the negated predicate,
   and the un-echoed `graft` arm name in phase 4 — plus the five carried mdformat wraps.
