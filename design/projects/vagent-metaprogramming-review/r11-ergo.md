# Round 11 critique — Programming Language Design & Ergonomics (v1.10)

Scope: v1.10 deltas only — verification of R10-F1–F4 against the disposition rulings, plus the
rest of the applied minor batch (walk vocabulary two roots :1401-1405/:1897-1908, (b) recorded
instruction enumeration :1921-1944, ingress-as-classification :1904-1911, `WorkerContext` call
shape, `opcodeMap_` freeze scope). Cross-checked against §6.7 resolver mechanics (:2030-2078),
the patch-record schema (:136, :1587-1646) and §9.3 (:2366-2404).

## r10 verdicts

- **R10-F1 (`patch:<n>` unmintable, MAJOR)** — **RESOLVED as ruled, option (b).** The verb now
  mints the installed clause's `+d.name` itself: "`patch:<n>` (`<n>` = the `d.patch` ordinal the
  call creates — only the verb can mint it, since the ordinal belongs to the patch the same call
  appends)" (:1130-1132), restated at the mechanism (:1589-1591), the glossary (:136) and the
  descriptor arm (learned-clause targets record the verbatim `+d.name` — "the `patch:<n>` the
  learning verb minted", :1617-1618). The user `name` is an *alias* on the patch record
  (:1132-1133, :1591-1592). The reservation is now coherent — `patch:` refused as a user `name`
  (:1133-1135), disjoint from head labels and module names (:1740-1741, :2044-2047) — because it
  guards a live mint, not a dead convention. §9.3 passes `"near_water"` as the alias with a
  correct inline comment (:2383-2385) and prose (:2399-2402). The ordinal correspondence is
  enforced rather than conventional, and `plan_named("patch:3")` works: first-`:` splitting finds
  no `patch` module (reserved), falls to verbatim `+d.name` match (:2040-2043).
- **R10-F2 (`label_of` anchored on `home`)** — **RESOLVED fully.** The row reads
  `cells"$a/+d.code"` off `agent_cell()`, explains *why* (`d.code` hangs on the agent cell, not
  arena `home`, so `##` reads an empty rank) and names the `[Cell]`→`Cell` indexing step
  (:1075). Consistent with the two-root walk vocabulary (:1401-1405) and the §6.7 echo (:2058-2060).
- **R10-F3 (one refusal for two futures)** — **RESOLVED fully, better than asked.** The split is
  named — **fail-until-resume** vs **fail-forever** (:1696-1698) — the emitter's knowledge is
  stated ("knows which future the miss has and warns at emission", :1695-1696), the head-less
  slot's observable content is `None` with no sentinel (:1688), and the slot is declared
  **emission-fixed** — a later `learn` registering `L` does not fill it; only genesis's rebind
  does (:1693-1695). `code_named`'s row also marks its space head-labels-only (:1074).
- **R10-F4 (`unlearn` LIFO same-name limit)** — **text landed, premise broken** — see R11-F1.

## The questions posed

**Does the alias-vs-label split read coherently?** Mechanically yes — the three surfaces agree:
the clause's `+d.name` is always the mint (`patch:<n>`), the patch record carries the alias plus
its own `patch:<n>` label, and `patch:` is sealed out of every user-name spelling. Two residual
incoherences, both narrative rather than mechanical:

1. The alias is **write-only** from the language surface. `plan_named` matches `+d.name` verbatim
   (:1073, :1626-1627); `unlearn` resolves the clause's `+d.name` label cell (:1643-1646); dedupe
   matches the alias against `+d.name`s — never alias-against-alias (:1138). Nothing resolves an
   alias back to its clause, and `learn_*` returns `()` (:1088) so the minted ordinal is never
   handed to the caller. The alias's only truthful description today is "audit metadata on the
   patch record, readable by `cells"$a/+d.patch"` spelunking" — the doc never says so, leaving a
   reader to wonder what their `name` argument *did*. (Its hidden second job — `learn_plan`'s
   dedupe/replacement key — is also worth surfacing; see nits.)
2. One consequence-note was written against the pre-mint world and now asserts behaviour that
   cannot occur (R11-F1).

**Is the §9.3 learn→patch→unlearn workflow still legible end-to-end?** The learn→patch half is:
the alias/label comment sits on the call (:2383-2385), the prose names the rule and the reason
(:2399-2402), and the `Meal` handler demonstrates the `clauses_of` enumeration idiom that is, in
fact, the only discovery path (:2391-2394). The unlearn tail is not exercised and depends on the
unstated rediscovery idiom (R11-F2): having learned `"near_water"` and received `()`, the user
finds their clause again by `clauses_of` enumeration or `plan_named("patch:<n>")` — never by the
name they passed.

**The three unapplied nits — worth taking?**

- *`learn_method`-only qualified-success* (:1699-1701): **take it.** The text implies it —
  qualified resolution must land on a head, `def` symbols are head-less — but never states that
  the only qualified `run`-role record that *ever* binds is one naming a `learn_method`-installed
  (head-valued) symbol. Without the clause, `code_named("demo:Forager.f")` reads as a plausible
  durable spelling when it is a guaranteed patch-drop for every declared `f`. One clause.
- *Emission-refused `patch:` wording* (:1133-1134): **cosmetic — take opportunistically.** "At
  `graft_*`/`learn_*` emission" borrows the fragment-emission sense for what is a runtime verb
  boundary (and `graft_*` never touches emission). "As a `name` argument at the
  `graft_*`/`learn_*` call" is the accurate shape; fix only if the paragraph opens anyway.
- *Qualified-verbatim dedupe consequence* (my r10 nit, still unapplied): **take it.** v1.10's
  "matching its alias verbatim against clause `+d.name`s" (:1138) states the mechanics, so a
  careful reader infers it, but the *failure mode* is still unnamed: `learn_plan("Feed","travel")`
  coexists with declared `demo:Forager.travel` rather than replacing it — the same
  silent-coexistence shape the handler gap now names explicitly. One clause ("the alias must be
  the target's stored `+d.name` verbatim — qualified for declared clauses; an unqualified alias
  appends, it does not replace").

## New findings (v1.10 deltas)

### R11-F1. MINOR — the "same-named learned clauses" limit is stale under the mint

The F4 ruling was applied verbatim (:1140-1145, changelog :3167), but the F1 fix removed its
premise. Under verb-minted labels every learned clause's `+d.name` is a unique `patch:<n>`, so:

- *"A second same-named handler clause appends … so `plan_named(name)`'s first-hit stays
  ambiguous between them"* (:1140-1142) — same-named at the `+d.name` level is unreachable for
  learned clauses (`patch:` is refused as a user spelling, :1133-1135, so no grafted clause can
  collide either). Two same-*alias* learned handlers give `plan_named("foo")` **zero** hits, not
  an ambiguous first — the alias is never on `+d.name`.
- *"Among same-named learned clauses it always pops the latest patch recording that name — the
  clause-cell argument cannot pick an earlier twin out of order"* (:1143-1145) — false under the
  mechanism (:1643-1646 resolves the clause's `+d.name`): passing the earlier twin resolves
  `patch:3` and pops exactly patch 3. `unlearn` is **more** precise than the sentence claims.

The genuinely surviving ambiguity is different in subject: `plan_named(name)` first-hit and
patch-target-name collision among same-named **grafted** clauses (verbatim `+d.name`s — reachable
when `learn_where`/`learn_body` patches two grafted `"foo"` clauses in-session), plus LIFO-by-name
on *repeated patches to one target* — which is correct undo order, not a limitation. As written,
the note misstates observable behaviour of two verbs; an implementer who took the note rather
than the mechanism could even build alias-keyed `unlearn` and *create* the bug it describes.

Fix: re-key the sentence to the mint — "`learn_handler` is deliberately additive: a second
same-alias clause appends beside the first under its own `patch:<n>` label, so `plan_named` by
the user name hits neither (resolve by `patch:<n>` or `clauses_of`), and `unlearn` targets each
precisely by its minted label. Same-`+d.name` ambiguity survives only among *grafted* clauses —
`plan_named(name)`'s first-hit — and `unlearn` pops the latest patch recording a given target
name, which on repeated patches to one clause is simply undo order."

### R11-F2. MINOR — learned-clause rediscovery has no stated idiom

The mint makes a learned clause's name unpredictable (`patch:<n>` for an ordinal the call never
returns — `learn_*` yields `()`, :1088) and the alias carries no lookup semantics (no resolver
consults it; nothing like `patches_of`/`alias_of` exists in the :1064-1091 surface). The doc's
own standard is to name each class's re-resolution path (`reify:` labels are "read off a live
head with `label_of`, never constructed by hand", :2058) — for learned clauses that path is
unwritten. Today it is: `clauses_of(goal)` and discriminate on the `patch:` prefix — but the only
stated `+d.name` reader, `label_of`, is specced for "a head's `+d.name`" (:1075), leaving clause
labels in a spec gap — or `plan_named("patch:<n>")` with an ordinal recovered from the `d.patch`
rank itself. Fix: one sentence in §6.1's naming paragraph naming the idiom (and whether
`label_of` reads clause labels or a clause-name read is owed), plus a half-clause stating the
alias's role — patch-record audit metadata, never resolved.

### Nits (not findings)

- `learn_method` vs `learn_plan` replace-vs-append asymmetry is now structural but undrawn:
  `learn_method` dedupe-*replaces* (the symbol's content **is** the user name), `learn_plan`
  always *appends* (its `patch:<n>` is unreachable as a dedupe key, and `patch:` can't be passed
  in). Same verb family, opposite repeat-call semantics — one clause at :1136-1140.
- The patch cell and its installed clause carry the *same* `patch:<n>` `+d.name` label (:136,
  :1130). Disjointness holds only because `plan_named`'s space is clause-rank cells and the
  head-layout shape guard excludes patch cells (:1758-1761) — worth half a clause saying the
  shared label is intentional (the patch's label names the clause it installed).
- Carryover, still present: the junction's own label listed among resolver-reachable endpoint
  spellings (:2204-2206) — junction cells sit on `+d.synapse`, in no named resolver's space.
- Carryover, unmechanized-but-named: `learn_where`/`learn_body` on a named *grafted* clause
  produces a patch whose target cannot exist post-genesis (grafts are session-only) — the
  dead-patch fault path covers it, but the "learned patch on a session clause dies at replay"
  consequence is never stated at the `learn_*` boundary.

## Summary

All four r10 findings landed: the `patch:<n>` mint is enforced-by-construction with the alias
recorded alongside (:1130-1135, :1589-1593, §9.3 :2383-2385), `label_of` is correctly anchored on
`agent_cell()` (:1075), the `run` refusal split names fail-until-resume vs fail-forever
(:1693-1698), and the `unlearn` limit is stated (:1142-1145). The alias-vs-label split is
mechanically coherent; its narrative cost is that the user `name` is write-only metadata no verb
resolves. Two new MINORs in the deltas: the applied F4 sentence describes pre-mint semantics —
learned clauses can't share a `+d.name`, `plan_named(name)` misses rather than misbinds, and
`unlearn` can pick an earlier twin (R11-F1) — and the learned-clause rediscovery idiom
(`clauses_of` + `patch:` prefix, or `plan_named("patch:<n>")`) is never stated (R11-F2). No
BLOCKING or MAJOR issues from this lane.

VERDICT: CONVERGED
