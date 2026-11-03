# r12-writer — convergence critique of `design/vagent-metaprogramming.md` v1.11

Scope: verification of my r11 major and nit bundle (R11-1 `unlearn`/`plan_named` staleness,
`:186`→`:188`, `store.cpp:123-124`→`:123`, "patch: convention"/`patch:<ordinal>` spellings,
hydrate/rehydrate stems), the r11-disposition minor in my lane (`sealPendingMetadataAsCells`/
`scrollRegistry` enclosing-function pairing), whole-document coherence (`patch:<n>` serial vs
ordinal vs alias, `d.vars` counter vs `reify`, v1.11 changelog narration, mdl, tables/fences/
anchors, "Design draft."), and a new-defect sweep of the v1.11 deltas (`3f9731da` → `894c7f3a`,
+283/−98, 37 hunks).

## Verdicts on r11 items — all fixed

| Finding | Verdict |
| --- | --- |
| R11-1 `unlearn` LIFO/`plan_named` ambiguity | **Fixed.** :1181-1188 now says a second same-*alias* `learn_handler` clause "appends under its own `patch:<n>`, `priority` differentiating the pair at selection — `plan_named` by the alias hits *neither* (zero hits, never an ambiguous first), and `unlearn` picks each twin precisely by its minted label"; the surviving `+d.name` ambiguity is same-named **grafted** clauses only; `unlearn`'s LIFO-by-name "survives only among patches recording one *target* name — stacked replaces on one clause, which is undo order, not ambiguity." The clause arm at :1734-1738 repeats it ("every learned clause answers a *different* name… picks a specific twin out of a same-alias stack in any order"). Consistent with the `plan_named` row (:1089, "learned clauses included, by their minted `patch:<n>` spelling, never by the user alias"), the `unlearn` row (:1103), glossary `patch` (:136), §6.7 (:2171-2179) and the changelog (:3331-3334). One new gap inside the same paragraph — R12-1. |
| `mesh_actor.cpp:186`→`:188` | **Fixed, verbatim-verified.** All four sites now read "first read at `:188`'s `!checkpoint.store` guard" (:1495-1497, :1898-1900, :2683-2684, P16 row :2703) and the changelog narrates it (:3353). The guard `!request.isValid() … || !checkpoint.store ||` is at `mesh_actor.cpp:187-189` with `!checkpoint.store` on :188. |
| `store.cpp:123-124`→`:123` | **Fixed.** :637 and :3283 now cite `store.cpp:123`; `userPermascroll_(std::make_shared<UserPermascroll>())` opens the `Store()` init list at :123 (`chronofilade_` follows :124-125). Changelog :3354 narrates. |
| "patch: convention" / `patch:<ordinal>` | **Fixed.** :2171-2172 now reads "the verb-minted `patch:` clause-label prefix"; the §9.3 comment (:2516) reads "patch:<n> off the d.vars serial". No instance of `patch:<ordinal>` or "patch: convention" remains (surviving "convention" uses are unrelated; :3096/:3196 are old changelog history). |
| hydrate/rehydrate stems | **Fixed, with one residue.** The span-source install is now "given its span source like any restored history" (:611, :1468, :2621, :3273) and :1469-1470 explicitly disambiguates ("`restorePublication`'s hydration pair — a different operation from step (e)'s `OpHandle`→`Int64` *rehydration*"). One bare "**the hydration**" survives at :641 ("a `restorePublication` seed (which would perform the hydration *and* adopt") — see nit bundle. `hydratePrefix` (:1346) is a function name, correct. |
| `sealPendingMetadataAsCells`/`scrollRegistry` pairing (:618-623, :3336-3338) | **Verified against code.** `Store::sealPendingMetadataAsCells` opens at `store.cpp:4020`; the `publishedLocalScroll_ == localScroll && homeCell_ && !externals.empty()` arm spans :4081-4089 and mints `registerScroll` ops per unregistered external scroll — the "mints `registerScroll` ops for `self` into the history" claim is exact. `Manifold::scrollRegistry`'s `resolvedScrollIds` inserts `store_->publishedLocalScroll()` at `manifold.cpp:1503` (function at :1475). The storeTables round-trip is `.publishedLocalScroll = publishedLocalScroll_` (`store.cpp:4176`) and `publishedLocalScroll_ = tables.publishedLocalScroll` (`store.cpp:4454`); `bindPublishedLocalScroll` assigns at :1422. The `self` recovery is accurate: `historyFromSeal` calls `history->addScroll(from)` at `publication.cpp:1098` without returning it; a caller re-add hits `sameContentAs` dedup (`store.cpp:1388-1405`, return `i+1`) so `bindPublishedLocalScroll(store->addScroll(from))` recovers the fold-internal id — `from` lands at `externals[0]` on a fresh `Store()` (externals starts empty, :122-125). |

## Whole-document coherence

- **`patch:<n>` terminology** — "durable monotone serial off `d.vars`, never the rank position" is
  uniform across glossary (:136), §4.5 (:390-391), §6.1 (:1158-1161), §6.6 (:1671-1672,
  :1745-1747), §6.7 (:2176-2179), §9.3 (:2516, :2530), the `learn_*` row (:1102) and the
  changelog (:3314-3321); "ordinal" is consistently defined as the serial value, "alias" as the
  write-only `name`. Surviving "the `d.patch` ordinal" phrasings are v1.10 changelog history
  (:3256-3267), correct as narration. One ambiguous live phrasing — nit bundle.
- **`d.vars` serial vs `reify`** — consistent: "the same per-agent `d.vars` counter `reify`'s
  `<serial>` already rides" (:1158-1159, :1671-1672) and §6.7's "a *durable per-agent counter* on
  `d.vars`… and `patch:<n>`'s `<n>` draws the *same* counter" (:2176-2179); replay consumes none
  (:2068-2069); `unlearn` reclaims none (:1745-1747). Same counter, disjoint label namespaces —
  coherent.
- **v1.11 changelog narrates correctly** — every claimed change verified in the diff: R11-A serial
  + verbatim replay transport + dedupe consequence; R11-B received-band destination + owed
  twin-apply surface; R11-C ref-capture record shape (`+d.grab`/`+d.values`/`+d.name` arms);
  R11-D corrections; the minor batch including the resolve-loop cite (`publication.cpp:1138-1162`
  — loop opens :1138, `metainfo` :1141, `ofTorrentFile` :1146-1148, `resolve` :1150,
  `isVerified`+length :1152 — exact).
- **Mechanicals** — `mdl -i` exit 0; 22 fence markers = 11 balanced pairs; 7 table blocks
  (unchanged); all 16 TOC anchors resolve against the 16 `## ` headings; "Design draft. Nothing
  here is implemented." (:7) still accurate.
- **Renames** — `publish Store`→`lineage Store` applied everywhere except one line-wrapped
  instance (nit bundle). `d.contract` correctly stays program-side with the op-anchor
  justification (`vortex_stdlib.cpp:603`'s `preconditionsOf(*curOp)` / :628's `postconditionsOf`)
  — verified. `exportSymbol`/`makeCell(name)` cite `vortex_core.cpp:119-127` verified (:122);
  `Manifold::store()` non-const `Store*` at `manifold.hpp:486`, `ArenaManifold::store()`
  `const Store*` const-only at `arena_manifold.hpp:269-272`; `scrollKey` at `scroll.hpp:333`;
  `adoptOpRecords` topo-sort with min-`produces` heap at `store.cpp:3836-3940` (opens :3836,
  `opsSpool.contains(record.produces)` :3841, `indexOf`/`idOf` exist); `applyOpsSegment`→
  `adoptOpRecords` at `publication.cpp:1088`; `mesh_actor.cpp:182` signature, `:188` guard, `:261`
  `userPermascrollPtr()` — all verified.

## New findings (v1.11 deltas only)

**R12-1 MAJOR — `learn_plan`'s new dedupe text contradicts its own mechanism twice over**
(:1173-1181). Two defects inside the R11-D consequence sentence:

1. *"An unqualified alias always appends" is false under the stated mechanism.* The dedupe is
   "its *alias* verbatim against clause `+d.name`s" — verbatim matching cannot see qualification.
   Grafted clauses hold `name` **verbatim** on `+d.name` (:1185-1186, stated one sentence later),
   and nothing refuses an unqualified graft name (:1153-1156 gives the qualified motivation but
   no refusal). So `graft_plan(G,"near_water",…)` followed by `learn_plan("Feed","near_water",…)`
   verbatim-hits the grafted clause's label and *unlinks* it — the flatly asserted "an
   unqualified alias always *appends* under its own `patch:<n>`" is wrong. The appositive "a
   *qualified* spelling against a declared clause" narrows the firing rule without stating what
   enforces the narrowing: verbatim match can't tell declared from grafted or qualified from
   unqualified. (A gate is mechanically feasible — declared clauses sit `≤E_ops`, grafted `>E_ops`
   — but the doc doesn't state it.)
2. *The dedupe's durability is unspecified and the default reading undoes it.* Genesis "re-runs
   each graft" (:2122); `graft_plan` is pure append (no unlink step — the unlink is a `learn_*`
   step), so on replay the dedupe-unlinked *declared* clause folds back verbatim at `1..E_ops`
   (the resurrection mechanics :1729-1730 states for handlers apply identically) while the learned
   clause replays beside it as `patch:<n>`. Post-resume `plan_named("demo:Forager.travel")`
   resolves the *resurrected declared clause* — silently reversing "the qualified name then
   resolves to nothing" (:1177) and restoring the superseded clause to selection. Whether replay
   re-applies the unlink (as `graft_method`'s built-in unlink-first would, :1278-1281 — that verb
   self-carries its dedupe) or whether the unlink is session-only is a fork the text does not
   resolve; `learn_method` replays its replace for free, `learn_plan` as specified does not.

An implementer cannot satisfy "always appends" and verbatim dedupe simultaneously, and gets no
ruling on the post-resume resurrection. **Fix:** state the gate — e.g., dedupe fires only when
the alias is a *qualified* spelling matching a clause's stored `+d.name` verbatim (grafted
unqualified labels then genuinely always append), or scope it to declared/program-image clauses —
and state the replay consequence: either the patch records the unlink so (d) re-applies it, or
the unlink is session-only and the declared clause coexists post-resume (the handler shadow arm,
:1729-1730, made explicit for plans).

**R12-2 MAJOR — genesis (a)'s comparand omits the scroll-identity check §6.6 adds in the same
breath.** §6.6's verify arm (:1536-1539) specifies "the decoded op records in canonical
post-remap form (op kind plus non-scroll fields, order-vs-order **as (a) states** — scroll
*identity* compared through `scrollKey(*store->scroll(span.scroll))`, `scroll.hpp:333` … since a
lineage differing only in which scroll a span names must not pass)". But step (a) itself
(:1992-1994) still enumerates only "canonical post-remap `OpRecord`/`CompactOpNode` fields (op
kind plus non-scroll fields)" and justifies skipping scroll fields entirely ("the `ScrollId`
remap legitimately rewrites scroll fields"). The cross-reference makes them the same comparand;
(a) — the place an implementer reads — specifies a strictly weaker test that *passes* the
"differing only in which scroll a span names" lineage §6.6 says must not pass. **Fix:** give (a)
the same field enumeration — "scroll fields compared by identity via `scrollKey`, never by raw
`ScrollId`, which the remap legitimately rewrites" — or point (a) at §6.6's statement.

**Nit bundle (v1.11 delta text):**

- `:1891-1892` — "the adapter constructs and keeps the publish\n`Store`" — the one line-wrapped
  instance the publish→lineage rename missed; every other site reads "lineage `Store`".
- `:641` — "which would perform the hydration *and* adopt" — the last unmarked hydrate-stem use
  for the span-source pair; "the span-source pair *and* adopt" or "the restore pair" keeps the
  split clean.
- `:1166` — "`plan_named(\"patch:<n>\")` with the ordinal read off the `d.patch` rank" — "read off
  the rank" invites the rank-position misreading R11-A exists to forbid; the `ordinal` *field* on
  the patch record is the correct source, and `label_of(clause)` yields `patch:<n>` directly —
  "read off the patch record's `ordinal` field, or the clause's own `+d.name` via `label_of`".
- `:1449` — "`+d.code` and its rank dims (`d.patch`, `d.synapse`, `d.weight`, `d.belief` …)":
  `d.weight` is not an agent-cell rank — §8.1 makes it the scalar edge dim on junction cells and
  `d.vars` records (:2312-2319); `d.vars` is the better third rank example (or drop `d.weight`).
- `:1538` — the sketch `scrollKey(*store->scroll(span.scroll))` derefs
  `optional<const Scroll&>` (`store.hpp:1024`); `scroll()` returns `nullopt` for `localScroll`
  and out-of-range ids (`store.cpp:1462-1465`), so a literal implementation UB's on local-scroll
  spans — needs the guard or a local-scroll arm named. "at equal cost" is also loose: `scrollKey`
  computes per span-bearing record — "same order" is the defensible claim.
- `:1528-1535` vs `:1989-1991` — "the adapter's seeded store … is not the comparand" vs (a)'s "the
  adapter decodes its adopted lineage store's own `1..E_ops` band": the seeded store still supplies
  the comparand's *reference* side under (a); precision would be "not the comparand's checked
  side, never the publish destination".

VERDICT: NOT CONVERGED — every r11 item verified landed verbatim (the `unlearn`/`plan_named`
correction, both cite fixes, the `patch:` spellings, the stem split, and the
`sealPendingMetadataAsCells`/`scrollRegistry`/`addScroll`/`storeTables` cites all check out
against code), terminology/changelog/mechanicals coherent end-to-end — but two v1.11 additions
carry new specification defects: the dedupe-consequence paragraph asserts an "always appends"
its own verbatim-match falsifies and leaves the unlink's post-resume fate unstated (R12-1,
MAJOR), and genesis (a)'s comparand specification omits the scroll-identity check §6.6's verify
arm adds while cross-referencing it (R12-2, MAJOR); plus a six-item nit bundle.
