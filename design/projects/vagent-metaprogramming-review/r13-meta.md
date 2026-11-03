# Round 13 critique — `design/vagent-metaprogramming.md` v1.12 — Metaprogramming Expert lane

Scope: verify R12-B (dedupe durability via verb replay — `alias` as the recorded superseded
target, genesis (d) re-unlink before the (e) `+d.refs` sweep, :1200-1223 / :2137-2153) and its
composition with `plan_named`'s verbatim walk and the quasiquote/splice cycle; verify my r12
minor (alias→clause join, :1190-1194), the "publish `Store`" fix (:1955), and the module-prefix
composition (:1802-1808); hunt new defects in the v1.12 deltas only (full diff
`git diff 894c7f3a..5dd90207` reviewed). Code citations re-verified against
`apps/common/xanadu/`.

## 1. R12-B verification — the fix is mechanically sound and composes

**The mechanism.** `learn_plan(goal, name, …)` records the user `name` on the patch record as
`alias` (:1183-1194), dedupes it verbatim against clause `+d.name`s **of the install goal's own
`d.clause` rank** (:1201-1203 — scope correctly pinned to the rank the patch records as attach
parent), unlinks the first-matched clause, installs the copy labelled `patch:<n>` (:1206-1209).
The durability claim (:1213-1219) is honest: the alias *is* verbatim-equal to the superseded
clause's stored `+d.name`, so genesis (d) can replay each patch's *verb* — re-match the recorded
alias against the re-attached (folded) rank and re-unlink the hit (:2150-2153) — before (e)'s
`+d.refs` sweep. `learn_method` replays its unlink-first inside `graft_method`'s own verb;
`learn_handler` re-appends (:2153-2154). `plan_named`'s verbatim walk then resolves the dead
qualified name to nothing and `patch:<n>` to the clause — identical session and post-resume.

**Composition proof sketch (all four kill-source cases checked).** Rank order is declared
clauses in fold order, then appends at tail (`graft_plan` :1105, :1305; learned :2439), and
appended learned clauses are `patch:<n>`-named — unreachable as alias (`patch:` refused as a
`name` arg :1195-1197). Therefore: (i) a session hit on a *declared* clause re-matches the same
clause at replay (unique declared names per rank ⇒ the unique one); (ii) a session hit on a
*grafted* clause implies no live declared same-name (declared precede appends), so replay
no-hit/append is faithful — the grafted clause is session-only anyway; (iii) a session-only
`prune`/`unlearn`-displacer leaves the declared clause dead in session and live at fold — any
surviving same-alias patch's replayed dedupe re-kills it, reproducing session state exactly;
where none survives, the declared clause's resurrection is the *stated* durable-undo semantics
(:1221-1223); (iv) unlearned displacers can't leave a dangling kill — `unlearn` removes the
patch record, so nothing replays it. Ordinal-order replay preserves verb order ⇒ rank-state
evolution mirrors session. **Yes: a `code"…"` fragment can `learn_plan` over a declared clause
and the shadowing survives resume** — the registered head is `d.code`-promoted, the record
folds back, (d) re-enacts kill+install. Quasiquote/splice never enters clause-label space
(`src:`/`+o:c`/`+n`/`reify:` are head labels only), so the cycle is unaffected. The residual is
only the declared-declared duplicate-name corner (alias records the name, not the twin) —
spec-degenerate since `tried` is a name set (nit 3).

**Re-mint rule correct.** :2143-2149's two-equal-text-cells rule is right and necessary:
re-linking the record's label cell to the copy would hand the label cell a second `-d.name`
edge and evict the record's at the second genesis (`arena_manifold.cpp:1347-1377`).

## 2. My r12 minors and the other named fixes — landed, one residual

- **Alias→clause join — FIXED (:1190-1194).** Spelled exactly as suggested: `<n>` off the
  record's `ordinal` field, joined from the alias by matching the `alias` key-record on each
  `d.patch` member (`cells"$a/+d.patch"`), "the only name→clause join".
- **"publish `Store`" — renamed (:1955) but the verify clause still dangles (:1961)** — see
  MINOR 1.
- **Module-prefix composition — verified (:1802-1808).** Unchanged and still correct against
  `exportSymbol`'s `makeCell(symbolName)`/`rankTail(moduleCell, dims_.vars)`
  (`vortex_core.cpp:117-126`) and `getOrCreateModule`'s `makeCell(modulePath)` (:107-115).
- **Per-verb label split (:1738-1750), `unlearn` record-arg refusal (:1794-1797, :1112),
  (a) comparand harmonization (:2054-2063, cited `scrollKey`/`localScroll` shapes verified at
  `scroll.cpp:12-21`, `store.cpp:1461-1469`, `spool.hpp:19`), agent-cell `d.vars` serial pin,
  `d.weight` de-rank (:1491-1496), `agent_cell()`-program-side scoping (:2082-2090), §10.2
  segment-apply registration (:2785-2795, :2805-2809)** — all landed as dispositioned.

## 3. New findings (v1.12 deltas only)

### 1. MAJOR — §6.1 :1145-1152 + genesis (e) :2164-2167 — `+d.captured` recreates the M14 collision one dim over; the endpoint's `-d.captured` is still single-occupancy

R12-A re-keyed the referent edge onto a fresh dim to escape `+d.values`' collision, but the
collision the doc itself names — "two records naming one referent collide on the same
single-occupancy slot" (:1148-1149) — lives on the **endpoint's backlink**, and `link` evicts
the target's `-d` neighbour the same way (`arena_manifold.cpp:1351-1366`, `displacedIt =
theirs->neighbor(-dir)` → `setOneSide(*other, dim, dir, noCell)` — the *earlier* record's
forward edge is severed). Two failure arms:

- **Within one recipe, ≥3 sharers.** "A shared referent is named once, later records carrying
  `+d.captured → the earlier ref-capture record` … the replay follows one hop" (:1149-1152). If
  "the earlier record" is the first/endpoint-bearing one, every later record fans into its
  single `-d.captured` slot — record 3's link evicts record 2's `+d.captured`, silently severing
  its pairing. If "earlier" means the *immediately preceding* sharer (a chain — the only
  occupancy-safe reading), then "the replay follows **one hop**" mis-specifies: the walk must
  iterate until a non-record target. Either reading is defective.
- **Across recipes — unhandled.** A state-side referent (a `d.vars` member cell — the doc's own
  example of a live `+d.values` target) captured by two ref-capture records on *different*
  heads collides identically: the "named once" discipline is per-recipe and cannot coordinate.
  The common case is immediate — `reify` "regenerates … `+d.recipe` (a self-describing recipe
  for the copy, endpoints remapped)" (:1163-1165), and an external/state-side endpoint remaps
  to itself, so the installed copy's `+d.captured → X` **evicts the registered head's**
  `+d.captured → X` at every `reify`. The severed record then feeds `noCell` endpoints to every
  later replay — including the recipe replayed at genesis (d)/(e), which derives ref slots from
  "the regenerated ref-records' **capture endpoints**" (:2176-2179). Every `learn` of a
  state-side-capturing fragment poisons its own registration; a second fragment capturing the
  same member corrupts at emission.

This is the doc's own head-recording rule unapplied: "Heads are recorded **by name, never by
link** … a second `+d.*` link to the head cell would sever the first record by eviction"
(:1755-1757) — shared endpoints need the same treatment. **Fix:** restrict `+d.captured →` to
endpoints the recipe owns exclusively (internal cells — remapped per copy, never shared), and
give state-side/external referents a name/route arm like the program-side `+d.name` arm already
provides (e.g., the member's resolvable `module:Agent.member` name or an agent-relative
`cells"…"` route), or a per-record anchor chain documented occupancy-safe; if the
within-recipe hop is kept, pin it as a chain to the *nearest preceding* sharer with a
transitive follow, not "one hop".

### 2. MINOR — §6.6/P16 :1961 — the verify clause's "against it" still dangles

"supplies it as the rebuild's backing store or verifies the rebuilt store's `1..E_ops` band
against it in canonical post-remap form" — "against it" still reads as *against the adapter's
lineage store*, the misread my r12 minor flagged and the disposition adopted ("re-point the
verify clause"). The phrase got "in canonical post-remap form" appended but kept the pronoun;
§6.6 is explicit that the seeded store is *not* the comparand (:1581-1584: the comparand is the
runtime's base-store band vs the decoded records). **Fix:** "or verifies the runtime's rebuilt
store's `1..E_ops` band against the received decoded records in canonical post-remap form".

### 3. MINOR — §6.6 :1721-1723 — "share the `patch:<n>` `+d.name`" is stale after the two-cell split

"Patch and installed clause share the `patch:<n>` `+d.name` intentionally — the patch's label
names the clause it installed" — post-split they share the *text*, not the cell (the very next
paragraph mints "two cells, never one shared", :1741-1743; (d) re-mints a third per genesis,
:2143-2147). Read literally, "share the `+d.name`" re-invites the one-cell design whose
`-d.name` eviction the split exists to prevent. **Fix:** "share the `patch:<n>` label *text*".

### Nits

- :1221-1223 — "returns at the next genesis only if the displacing patch was unlearned first"
  states a necessary condition as if sufficient: a *surviving* same-alias `learn_plan` re-kills
  the folded clause at replay, so it does not return even then. Half a clause: "…and no
  surviving patch's replayed dedupe re-unlinks it".
- :2157-2162 — "(d)'s install set: per patch, the minted install copy" — `learn_where`/
  `learn_body` replayed verbs mint reified head copies too (with regenerated `+d.refs` records
  needing (e)'s rebind); say "every copy a replayed verb mints, install or re-point".
- Replayed-dedupe uniqueness dependency: the alias records the superseded *name*, not which
  same-named twin — under a declared-declared duplicate on one rank, an unlearned earlier
  displacer shifts the replayed kill to a different twin. Degenerate under the spec's
  `tried`-as-name-set semantics (vagent §6.3), but the dedupe now silently assumes at most one
  declared clause per name per rank — worth pinning as the stated precondition.
- :1187 — "the alias is write-only audit metadata" sits tensely beside "replay-keyed" (:1217);
  "write-only *in-session*" is the accurate qualifier.
- `learn_where`/`learn_body` accept a detached-but-`+d.name`-bearing clause cell session-side
  (records a patch whose replay then drops+faults, :2206-2208); consistent outcome, but a
  rank-membership check at accept would refuse the zombie re-point outright.
- Citations: `scroll.cpp:13-22` → the `scrollKey` signature is :12 (function :12-21);
  `publication.cpp:1097` for the `historyFromSeal` `Store()` mint is :1096.
- Carried from r12: the durable counter's cell shape on the agent cell's `d.vars` is still
  unstated — if it is a *named* member it joins the `symbol_named`/first-match walk a user
  member could collide with; say "an adapter-minted anonymous scalar".

## 4. Staging-discipline summary

R12-B lands the load-bearing change correctly: verb replay makes the dedupe durable, the alias
field genuinely is the superseded target (verbatim `+d.name` equality), and the composition
with `plan_named`'s verbatim walk is exact — session-hit and replay-hit coincide in every case
that matters, with divergences confined to sanctioned session-only semantics
(`graft_*`/`prune`/`unlearn`). The EDL now reconstructs the displaced-declared state it
previously lost. The new defect is in R12-A's other half: `+d.captured` moves the collision it
was chartered to remove onto a fresh dim without noting that single occupancy binds on the
*endpoint's* backlink, so shared referents — the case the dim exists for — still evict, worst
at the registered-head/copy boundary where every `reify` severs the record genesis replays.

VERDICT: NOT CONVERGED — 0 blocking, 1 major (`+d.captured` `-d.captured` single-occupancy
collision on shared/cross-head referents severs ref-capture records), 2 minors (verify-clause
"against it" antecedent; stale "share the `patch:<n>` `+d.name`"), 7 nits.
