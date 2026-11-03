# Round 13 critique — Programming Language Design & Ergonomics (v1.12)

Scope: verify the four r12 minors against the v1.12 text, assess whether the dedupe
story now reads predictably, and hunt new defects in the v1.12 deltas only
(`git diff 894c7f3a..5dd90207`).

## r12 verdicts

- **R12-F1 (dedupe scope unpinned, MINOR)** — **RESOLVED.** `learn_plan` now
  "dedupes its *alias* verbatim against the clause `+d.name`s **of the install
  goal's own `d.clause` rank** — the rank the patch records as the attach
  parent, never a global scan, which could unlink a clause off a rank the patch
  does not record" (:1201-1204). The scope is pinned to the sane reading and the
  rationale names the cross-rank victim the global scan invited. The example's
  residual — the declared clause it asserts against is never shown to sit on
  `Feed`'s rank — is R13-F3.
- **R12-F2 (alias→`patch:<n>` join unstated, MINOR)** — **RESOLVED.** The join is
  spelled at :1190-1194: `<n>` is "read off the patch *record's* `ordinal`
  field, joined from the alias by matching the `alias` key-record on each
  `d.patch` member (`cells"$a/+d.patch"`)", with `clauses_of` + `label_of` +
  `patch:` prefix as the alias-free enumeration and
  `plan_named("patch:<n>")` as the resolve. The one mechanical hop the alias
  exists for is now on the page (the `+d.vars → [key] +d.values → scalar`
  record shape it matches is defined at :1184).
- **R12-F3 (grafted-only ambiguity under-claim, MINOR)** — **RESOLVED.**
  "The surviving `+d.name` ambiguity is any same-named pair a verbatim
  `graft_*` `name` can create — graft-on-graft, or a graft shadowing a
  declared clause" (:1228-1230), with `plan_named`'s first-hit and
  `unlearn`'s latest-name match both named as misbind arms.
- **R12-F4 (`unlearn(patch_cell)` undocumented, MINOR)** — **RESOLVED and
  hardened.** The table row (:1112) and the mechanism paragraph (:1794-1797)
  refuse a `d.patch` record argument "by its `-d.patch` backlink", with the
  reason stated: the record's own `patch:<n>` label would ride the reverse read
  and "accidentally match the clause-arm patch that installed the same-named
  clause — kind-dependence refused, not exploited". The discriminator is sound
  on the mechanics: every `d.patch` rank member carries a `-d.patch` backlink
  (agent cell for the head, predecessor record after — the rank is a same-dim
  chain per `insertIntoRank`/`rankTail`, :1730-1734), and a clause's backlink is
  `-d.clause`. The one escape I probed — passing an *already-unlearned* (hence
  `-d.patch`-less) record cell whose stale `patch:<n>` label matches a live
  patch's recorded target — is unreachable: any patch recording that target
  name pops before the record itself under LIFO-by-name, so no live patch
  outlives the dead record carrying its target.

## Does the dedupe story read predictably?

Yes, on the mechanism:

- **Unqualified alias** verbatim-matches a grafted same-name on the install
  goal's rank (grafts append at tail, so a declared same-name is always matched
  first when both exist) and unlinks it; spelling no live `+d.name` it
  *appends* under `patch:<n>` (:1204-1212). The v1.11 self-contradiction —
  "can hit a grafted clause" vs "unqualified always appends" — is gone; the
  matrix is stated as "any verbatim hit: declared or grafted, qualified or
  unqualified".
- **Qualified alias** is the only spelling that displaces a *declared* clause,
  since declared labels are qualified and the match is verbatim — the
  dead-name/`tried`-set consequence is named (:1207-1210).
- **Resume**: genesis (d) replays the *verb* — re-matches the recorded alias on
  the re-attached rank and re-unlinks the folded-back declared clause
  (:1214-1219, :2150-2154); the copy's `+d.name` is a verbatim re-mint off the
  record, never a re-link (:2143-2149). The learned clause keeps `patch:<n>`
  and keeps its victim dead — the durable reading is consistent.
- **Discovering what `patch:<n>` displaced**: the `alias` field doubles as the
  superseded-target record (:1214-1215), reachable by `cells"$a/+d.patch"` →
  `ordinal` == n → read `alias`. Legible but hit-conditional — R13-F1.
- **Repeat-alias appends**: because a learned clause's label is `patch:<n>`,
  never the alias, N `learn_plan` calls with one alias stack N clauses — the
  doc's "never a session lookup key" stance covers it; §9.3's
  `learned_rule` belief-guard is the example's own answer to it.

Two residual minors and one unpinned example premise follow, plus nits.

## New findings (v1.12 deltas)

### R13-F1. MINOR — the `alias`-as-superseded-target claim is hit-conditional but stated categorically

"The unlink is **durable through the alias**: the record's `alias` field *is*
the recorded superseded-target name" (:1213-1215). True only when the dedupe
hit; on an appending `learn_plan` (alias spelled no live `+d.name`) the same
field records the user name with nothing displaced — and the two records are
indistinguishable: `{ordinal, target patch:<n>, parent, alias}` is the shape in
both cases, so a user auditing `cells"$a/+d.patch"` cannot tell whether
`patch:<n>` displaced `demo:Forager.travel` or merely carries it. The
displaced-vs-appended fact is recoverable nowhere (the victim is unlinked).
Mechanically harmless — replay re-matches the alias regardless — but the
"write-only audit metadata" contract (:1187) silently over-promises.

Fix: state the conditional — "on a dedupe hit `alias` records the displaced
clause's name verbatim; on an append it records only the user name, and the
record marks no hit" — or, in the document's own more-cells-over-flags style,
carry a `superseded` key-record only on hit so presence answers the audit.

### R13-F2. MINOR — "the uniform `label_of`/`unlearn` bookkeeping handle" contradicts the adjacent new text

v1.12's per-verb split says "every patch record carries its own `+d.name →
patch:<n>` label cell — the uniform `label_of`/`unlearn` bookkeeping handle"
(:1740-1741). Two sentences later the same passage states `unlearn` "matches
every verb by the recorded *target* name, never by the record's own label"
(:1749-1750), and §6.6 *refuses* a `d.patch` record argument outright
(:1794-1797). The record's `+d.name` has no `unlearn` consumer; its actual
consumers are `label_of` (rediscovery on patch cells) and genesis (d)'s
verbatim re-mint transport to the installed copy (:2143-2147). Self-
contradiction inside text added this revision.

Fix: "the uniform `label_of`/genesis-transport handle" (or drop `/unlearn`).

### R13-F3. MINOR — `demo:Forager.travel` asserts the pinned scope's outcome against a clause the doc never declares

"`learn_plan("Feed", "demo:Forager.travel", …)` unlinks the declared clause"
(:1207-1209) now correctly asserts the goal-rank-scoped dedupe — but the
document's only `demo:Forager` is §9.3's (`agent Forager`, :2594-2613), which
declares `def navigate_to` and two `on` handlers and **zero plans**: no
`plan travel for Feed` exists for the alias to verbatim-hit, so against the
shown code the call finds no live `+d.name` and *appends* — the opposite of
the asserted unlink. The premise "a declared `travel` sits on `Feed`'s rank"
is exactly what the pinned scope makes load-bearing, and it is still unstated
(the unaddressed second half of R12-F1's fix). §9.2's
`plan cautious for Patrol` (:2561) does exist in-doc.

Fix: mark the premise in-line — "a declared `plan travel for Feed`" — or
retarget the example to a verifiable declared clause:
`learn_plan("Patrol", "demo:Guard.cautious", …)` unlinks §9.2's declared
clause and leaves the qualified name dead.

## Nits (not findings)

- "the only name→clause join" (:1193) reads ambiguously next to `plan_named`,
  which joins qualified declared names and `patch:<n>` spellings to clauses
  without touching `d.patch`; the intent is the only *alias*→clause join —
  s/`name`/`alias`/ or "the only join from the user `name`".
- :1222-1223 "returns at the next genesis only if the displacing patch was
  unlearned first" — strictly the declared clause folds back at `1..E_ops`
  unconditionally; it *survives replay* only if the displacer is gone. The
  parenthetical "(no replay, no re-unlink)" carries it, but "returns" vs
  "stays returned" is loose.
- :1746-1747 "a `patch:` label under it would shadow the `d.vars` name walk it
  is found by" — the symbol walk reads cell *content* (`makeCell(name)`,
  `vortex_core.cpp:119-127`), never `+d.name`; a `patch:` *content* would
  break the walk, a label would not shadow it. Counterfactual wording loose.
- Carried grammar regression, still present: "the `asInt64 ≤ Linked` value
  scan §6.6's step (b) rules out is the first" (:1427-1428) — needs
  "rules out **being**" or "rules out *as*".
- Carried: "the patch's ordinal" at :2617 where the durable serial is meant —
  "serial" is the word the surrounding text standardized on.
- Carried, pre-dates this delta but worth naming once: §9.3's `on Meal`
  strengthen loop can never fire — `let w := weight_of(c)` yields a
  disengaged `Opt` on any never-weighted clause (the clause-correlated weight
  is a `d.vars` record nothing seeds, :2406-2408), so `if [w]` is always
  false and `set_weight` never bootstraps the first weight. The flagship
  Hebbian example's second half is inert as written; it wants an
  unconditional seed (`set_weight(c, w | 0.1)`-shaped) or a stated default.
- Carried: `label_of` still grouped under `reflect`'s "program-structure
  subset" (:1084) though it reads runtime-minted state-side labels.
- Carried: synapse endpoint domain still lists "a synapse junction's own
  label" among `symbol_named`/`code_named`/`plan_named`-resolvable spellings
  (:2420-2423) — junctions sit on `+d.synapse` and their endpoint names are
  descriptor *content*, in no named resolver's space.
- Carried: `learn_where`/`learn_body` on a *grafted* clause still records a
  `+d.name` target that dies at genesis; the dead-patch fault covers it
  mechanically but the consequence is unstated at the `learn_*` boundary.

## Summary

All four r12 minors landed: dedupe scope is pinned to the install goal's own
`d.clause` rank (:1201-1204), the alias→`patch:<n>` join is spelled as
`d.patch`-rank `alias`-key match → `ordinal` → `plan_named` (:1190-1194),
the surviving ambiguity is correctly restated as any verbatim-graft pair
(:1228-1230), and `unlearn` refuses `d.patch` records by `-d.patch` backlink
with the accidental-match reason drawn (:1112, :1794-1797). The dedupe story
now reads predictably: qualified alias displaces the declared clause on the
goal's rank, unqualified displaces a verbatim graft or appends, `patch:<n>`
survives resume via (d)'s verbatim re-mint plus verb replay that re-displaces
the folded-back victim, and displaced-clause discovery runs through the
`alias` field — with three residual minors: the alias-as-superseded contract
is hit-conditional where stated categorically (R13-F1), the new per-verb text
contradicts itself on whether `unlearn` consumes the record's `+d.name`
(R13-F2), and the `demo:Forager.travel` example asserts an unlink of a clause
the document's own `demo:Forager` never declares (R13-F3). No BLOCKING or
MAJOR issues from this lane.

VERDICT: CONVERGED
