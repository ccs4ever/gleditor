# r13-writer — convergence critique of `design/vagent-metaprogramming.md` v1.12

Scope: verification of my r12 majors/nits (R12-1 dedupe resurrection → durable verb replay,
R12-2 comparand `scrollKey` harmonization, "publish `Store`"→"lineage `Store`", "the
hydration"→"the span-source pair", ordinal-read pin, `real.emplace`, `store.cpp:3840`,
`d.weight` de-rank, `store.cpp:123`→`:124`), whole-document coherence (`d.refFor`/`d.captured`
terminology, record-shape consistency §6.1/diagram/genesis (e)/changelog, agent-cell `d.vars`
pin uniformity, v1.12 changelog narration, `mdl`, tables/fences/anchors, historical entries),
and a new-defect sweep of the v1.12 deltas (`894c7f3a` → `5dd90207`, +219/−77, 32 hunks).

## Verdicts on r12 items — all fixed

| Finding | Verdict |
| --- | --- |
| R12-1 dedupe resurrection (`learn_plan` unlink session-only) | **Fixed.** §6.1 :1201-1223 now scopes the dedupe to "the install goal's own `d.clause` rank — the rank the patch records as the attach parent, never a global scan", makes the match "*any* verbatim hit: declared or grafted, qualified or unqualified" (self-contradiction gone — a grafted clause holds `name` verbatim; a learned clause's `patch:<n>` is unreachable since `patch:` is refused as a `name` argument), and states the durability mechanism: the record's `alias` field is the replay key, genesis (d) "re-enacts each patch's **verb**" (:2150-2154) — `learn_plan` re-matches and re-unlinks, `learn_method`'s unlink-first rides `graft_method` itself, `learn_handler` re-appends; a displaced declared clause folding back at `1..E_ops` "is displaced again rather than resurrecting", and in-session `unlearn` of the displacer does not re-link (:1221-1223). I probed the replay for divergence — grafted targets (absent at replay, same end state), same-alias re-`learn_plan` (ordinal order preserves in-session match outcomes), `unlearn`-then-relearn (declared clause dead in-session stays dead post-genesis), and prune-interleavings (prune is session-only; the replayed unlink reproduces its effect) — all converge; no residual fork. |
| R12-2 genesis (a) comparand vs §6.6 | **Fixed.** (a) :2053-2066 now states the comparand canonically: "op kind plus every non-scroll field verbatim, plus scroll *identity* by key — never the raw remapped `ScrollId`… but `scrollKey(*store->scroll(id))` (`scroll.cpp:13-22`) for an external id… `Store::scroll`… disengages on `localScroll` (`store.cpp:1461-1469`; `localScroll == 0`, `spool.hpp:19`), so local-scroll ids compare as ids" — and §6.6 :1585-1587 cites "(a)'s stated comparand, applied verbatim". One comparand, stated once. Verified: `scrollKey` def spans `scroll.cpp:13-22`; `Store::scroll` returns `cpp26::optional<const Scroll&>` disengaging at `store.cpp:1463`; `localScroll == 0` at `spool.hpp:19`. |
| "publish `Store`" → "lineage `Store`" (:1955) | **Fixed.** :1955 now reads "the adapter constructs and keeps the lineage `Store`". No live instance of "publish `Store`" remains; surviving hits are v1.8 changelog history (:3269) and the v1.12 narration (:3497). |
| "the hydration" → "the span-source pair" (:643) | **Fixed.** :643 reads "which would perform the span-source pair *and* adopt `pub.storeId`". The hydrate/rehydrate stem split (:1519) remains clean. |
| Ordinal-read pinned (:1192) | **Fixed, better than asked.** :1191-1193: "`<n>` read off the patch *record's* `ordinal` field, joined from the alias by matching the `alias` key-record on each `d.patch` member (`cells"$a/+d.patch"` — the only name→clause join; `clauses_of` + `label_of` enumerates learned clauses without it)" — the disposition's "patch-rank alias field → `plan_named`" spelled end to end. |
| `real.emplace` (:1583) | **Fixed, verbatim-verified.** `real.emplace(arena, arena);` sits at `arena_manifold.cpp:1867` inside the cite's `:1867-1868`. The other `real.at` sites are distinct and correctly cited (`:1950` OpHandle target, `:2041` `setLink`). |
| `store.cpp:3841`→`:3840` (:2105) | **Fixed, verbatim-verified.** `opsSpool.contains(record.produces)` is at `store.cpp:3840` inside `adoptOpRecords` (:3836). |
| `d.weight` de-ranked (:1493-1495) | **Fixed.** The rank list now reads `d.patch`, `d.synapse`, `d.vars`, `d.belief` — "`d.weight` is no rank, it is the §8.1 scalar edge dim on junctions and `d.vars` records" — consistent with §8.1 :2399-2403. |
| `store.cpp:123`→`:124` (:639, :3385) | **Fixed, verbatim-verified — and it corrects my own r12 verification.** `store.cpp:123` is the `Store::Store()` signature line; `userPermascroll_(std::make_shared<UserPermascroll>())` is at `:124`, `chronofilade_(std::make_unique<…>())` at `:125`. The doc's changelog claim "the `userPermascroll_` mint, `:125` `chronofilade_`" (:3496) is exact. Both live cites updated (:639, :3385), and the v1.11 entry's false parenthetical "(`:124` is `chronofilade_`)" removed (:3456) — a factual correction in history, acceptable. |

## Whole-document coherence

- **`d.refFor`/`d.captured` terminology** — uniform across the status table (:95), the
  `ensureDimension` mint list (:533-534), the adapter-bindings enumeration (:1018, :1025), the
  head diagram (:1052-1056), §6.1's record shape (:1136-1156), genesis (e) (:2165-2167,
  :2179), P16-adjacent text and the changelog (:3461-3468) — *except two missed enumeration
  sites*, finding R13-1. Record-shape descriptions agree: §6.1's `+d.refFor`→slot /
  `+d.captured`→endpoint-or-earlier-record / `+d.name`→dim-name triplet matches the diagram's
  compressed form and (e)'s rehydration read; the shared-referent one-hop indirection
  discriminated by recipe-chain membership is sound (no referent is on the head's recipe
  chain — recipe cells are emitter bookkeeping, never fragment referents).
- **Ref-capture mechanism verified against code** — `bindInput` writes `opcode +d.grab → slot`
  at `vortex_core.cpp:388-389` (cite `:384-390` covers the write); `wingOf` enters through
  `linked(opcode, dims_.grab, side)` at `:366` with `inputsOf` supplying `DimVector::POS` at
  `:373`; a `+d.grab` correlation edge would indeed collide with the opcode's wing head —
  the M14 argument transfers correctly. The `d.vars` symbol walk reads cell *content*
  (`textOf(sym) == symName`, `vortex_stdlib.cpp:1950-1955`) — load-bearing for R13-4.
- **Agent-cell `d.vars` pin uniform** — every live serial mention now names the agent cell
  (:136, :390-391, :1111, :1178-1179, :1718-1719, :1739, :1807, :2139-2140, :2261-2263,
  :2352-2354, :2617-2618); the module-side exclusion is stated at :2262 ("a module's `d.vars`
  is program-side and never holds it"). One sketch-comment shorthand survives (nit).
- **Verb-replay terminology consistent** — "re-runs each patch's verb" (:393), "re-enacts each
  patch's **verb**" (:2150), "re-runs each patch's recorded verb" (:2206-2207); no "re-runs
  each graft" survives. Per-verb label/serial split (:1738-1750) is internally consistent
  with the `unlearn` refusal (:1794-1797), the record-shape enumeration (:1724-1726) and the
  target-name composition (:1760-1779).
- **v1.12 changelog narrates correctly** (:3460-3498) — every claimed change verified against
  the diff: R12-A ref-capture re-key, R12-B durable dedupe + scope pin + contradiction fix +
  ambiguity restatement, R12-C §10.2 registration (adapter surface (iii) at :2787-2791 plus
  the post-register paragraph :2805-2810), R12-D comparand harmonization, and the full minor
  batch including all six citations.
- **Historical entries intact** — v1.10 (:3390-3396) and v1.11 (:3428-3436) entries still
  narrate the old `+d.values`/`+d.grab` ref-capture shapes correctly *as history*; the two
  retroactive edits (:3385 cite, :3456 parenthetical) are factual cite corrections, not
  narrative rewrites.
- **Mechanicals** — `mdl -i` exit 0; 22 fence markers = 11 balanced pairs; 7 table blocks;
  all 16 TOC anchors resolve against the 16 non-Contents `## ` headings; "Design draft."
  status line intact.

## New findings (v1.12 deltas only)

**R13-1 MINOR — two dim enumerations missed the `d.refFor`/`d.captured` addition.** Every
normative enumeration of the adapter-minted dims gained the two new dims (:95, :533-534,
:1018, :1025), but two recaps still list the old set: the §10.1 sketch comment :2745-2747
("`d.code/d.patch/d.weight/d.synapse/d.entry/d.patch-head/d.validate/d.refs/d.ref/d.recipe/
d.mesh.relocation`") and §14's R12 ruling :2982-2983 (same list ending `d.mesh.relocation`).
Both are completeness inconsistencies — a reader taking either as the mint set drops the two
dims the ref-capture record requires. **Fix:** append `d.refFor`/`d.captured` in both places.

**R13-2 MINOR — "the adapter's seeded store … is not the comparand" now contradicts (a)'s
comparand definition verbatim.** :1581-1584 keeps the v1.11 claim "the adapter's seeded store
holding the *image* lineage is not the comparand: `promote`'s verbatim persistent-ref map
(`real.emplace`…) would write positions into it that never existed there, undetectable by a
comparand reading the wrong store" — while (a) :2053-2055 makes that very store the
comparand's *reference* side: "the adapter decodes its adopted lineage store's own `1..E_ops`
band … and asserts the received store's decoded `1..E_ops` records equal it pairwise". The
harmonization sharpened the contradiction my r12 nit flagged (it was not in the adopted
minors): the seeded store IS one operand of (a)'s comparand — it is not the *checked* side,
and it is never the publish destination, which is the claim the sentence needs. **Fix:**
"is not the comparand's *checked* side — it supplies the reference band — and never the
publish destination: `promote`'s…".

**R13-3 MINOR — the alias is declared "write-only"/"never a session lookup key" in the same
sentence that names a session read of it.** :1187-1193: "the alias is write-only audit
metadata (`learn_*` returns `()`…) — its only two uses are the `d.patch` rediscovery join
below and `learn_plan`'s replay dedupe" — but the join matches the `alias` key-record on each
`d.patch` member, an in-session read keyed on the alias, which is exactly a session lookup.
"Write-only" is flatly contradicted one clause later; "never a session lookup key" survives
only under the narrow reading "no *verb* takes it" — and the glossary `patch` row (:136)
repeats the bare claim without the join's existence qualifier. Same loose edge at :1214:
"the record's `alias` field *is* the recorded superseded-target name" overstates — on a
no-hit append nothing was superseded; the alias is the dedupe *key*, equal to the superseded
name only on a hit. **Fix:** "never a *verb* lookup key — re-found only by the manual
`d.patch` join; `learn_*` returns `()` so the minted serial is never handed back either",
and "the `alias` field *is* the recorded dedupe key — the superseded clause's `+d.name`
verbatim on a hit".

**R13-4 MINOR — `learn_method`'s shadow justification names a mechanism that cannot occur.**
:1746-1747: "a `patch:` label under it would shadow the `d.vars` name walk it is found by" —
but the `d.vars` name walk reads the symbol cell's *content* (`textOf`, `vortex_stdlib.cpp:
1950-1955`), a fact this document itself states at :2289-2290 ("the name lives as the symbol
cell's *content* — `exportSymbol` mints `makeCell(name)`, no `+d.name` edge"). A `+d.name`
label cell cannot shadow a content read; the true reasons the symbol gets no `patch:<n>`
label are that its content already *is* the resolvable name (so no label is needed for
`unlearn`'s content-read symbol arm) and that a `patch:` *content* is what would break the
walk. The conclusion (record-only label) is right; the stated mechanism is wrong and could
teach an implementer a false rule about `+d.name`/`d.vars` interactions. **Fix:** "a `patch:`
label is needless — the symbol's content *is* the name the `d.vars` walk reads — and a
`patch:` *content* is what would shadow it".

**Nit bundle (v1.12 delta text):**

- `:1743` — "the clause's `-d.name` holding a single edge" misattributes the occupancy: the
  constraint barring a shared label cell is the *label cell's* `-d.name` (one incoming edge —
  genesis (d) states it correctly at :2144-2146, "single occupancy would evict off the
  record"); the clause's own `-d.name` being single-edge is a different, non-causal fact.
- `:1744` — "`learn_where`/`learn_body` install nothing" — they install the reified test/body
  head copy at the re-point; "install no *clause*" is the claim.
- `:2061-2063` — the `Store::scroll` disengagement is under-enumerated: it also returns
  `nullopt` on `id > externals.size()` (`store.cpp:1463`), so an implementer guarding only
  `localScroll` still UB's on an out-of-range id — name the second arm or the range check.
  Related: `scrollKey` returns `nullopt` for anonymous scrolls with no segments
  (`scroll.cpp:17-19`), letting two distinct empty scrolls compare equal — worth a clause
  noting the arm (degenerate, but the comparand is a verify path).
- `:1150-1151` — "`+d.captured → the earlier ref-capture record`": the one-hop claim holds
  only if every later sharer points at the *first* (endpoint-holding) record — "the earlier"
  admits chains (record3→record2→record1) that need two hops; pin "the first".
- `:1222-1223` — "returns at the next genesis only if the displacing patch was unlearned
  first" is under-specified: a *replay-dropped* displacing patch (attach parent no longer
  resolves, :2206-2209) also leaves the folded clause standing — "unlearned or
  replay-dropped".
- `:1721-1723` — "Patch and installed clause share the `patch:<n>` `+d.name` intentionally"
  reads as one shared label cell under the new two-cell rule; "carry equal-text `patch:<n>`
  `+d.name` labels" is the v1.12 shape (the intentional *text* sharing is the point).
- `:2602` — sketch comment "`patch:<n>` off the d.vars serial" is the only unpinned serial
  mention (prose beside it at :2617 says "the agent cell's `d.vars` serial").
- `:1142-1143` — the `wingOf` cite `vortex_core.cpp:366-369` shows `linked(opcode, dims_.grab,
  side)`; the `POS` argument is `inputsOf`'s at `:373`, outside the cited range — cite
  `:366-373` or write "`side`, `POS` from `inputsOf`".
- `:2138` — "replayed grafts mint no patch cells" — "replayed patch verbs" keeps the new
  vocabulary (a `learn_*` replay is a verb, the graft is its install step).

VERDICT: CONVERGED — every r12 item verified landed and verbatim-verified against code where
applicable (the dedupe is now durable through the recorded alias under verb replay — probed
for divergence and found none; the comparand is stated once in (a) and cited verbatim in
§6.6; all ten citation/wording nits applied, with `store.cpp:124` correctly superseding my
own r12 verification); `d.refFor`/`d.captured` terminology, the agent-cell `d.vars` pin, the
record shape across §6.1/diagram/genesis (e)/changelog, and the v1.12 narration are coherent;
mechanicals clean — the residue is four MINORs (two stale dim enumerations; the "not the
comparand" sentence now contradicting (a)'s reference side; the alias "write-only" claim
contradicted by its own new join; `learn_method`'s impossible shadow mechanism) plus a
nine-item nit bundle.
