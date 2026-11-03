# r14-writer — convergence critique of `design/vagent-metaprogramming.md` v1.13

Scope: verification of my r13 minors (stale dim enumerations, comparand reference-side, `alias`
"no session resolver" restatement, `patch:`-shadows-`d.vars` removal), R13-A coherence
(`d.captured` excision, `d.refFor` uniformity, phrasing across §6.1/diagram/genesis (e)/glossary/
changelog), mechanicals (`mdl`, tables, fences, anchors, status line), v1.13 changelog narration,
and a new-defect sweep of the v1.13 deltas (`5dd90207` → `f252c2b8`, +~90/−~40 across the
ref-capture restatement, the comparand arm-set, the alias join, the dedupe divergent-victim
cases, the label/serial split, the agent-cell-legs reword, the §10.2 (v) item, the `set_weight`
seed rule, and both changelog entries).

## Verdicts on r13 items — all four fixed

| Finding | Verdict |
| --- | --- |
| Stale dim enumerations (:2788, :3025-3027) | **Fixed.** §10.1's sketch comment now reads `d.recipe/d.refFor/d.mesh.relocation` (:2788) and §14's R12 ruling lists `d.refFor` (:3028) — `d.refFor` restored, `d.captured` absent from both. |
| Comparand pronoun/reference-side (:1597-1607, :1984-1986, :2083-2094) | **Fixed.** :1601-1604 now reads "is not the comparand's *checked* side — it supplies the reference band, never the publish destination"; :1987 "verifies the rebuilt store's decoded `1..E_ops` band against the lineage's decoded reference band — (a)'s canonical post-remap form"; (a) :2080-2083 makes the lineage store the reference and the received store the checked side. One direction, three sites, consistent — and P16's row (:2843) and §10.2's prose (:2811-2812) independently repeat "the verify destination is the received-band store `arena.base()->store()`". The scroll-identity arm is verbatim-verified: `globalKeyOf`'s arm set sits at `publication.cpp:853-869`; `breakMarkerKey` is `publication.hpp:447`; `sealableOps`'s table builder skips `breakMarkerScroll` at `publication.cpp:985-987`; `localScroll`→`selfScrollInHistory` at `:1049` and the `breakMarkerScroll` identity emplace at `:1050`; `Store::scroll` disengages on `localScroll == id \|\| id > externals.size()` at `store.cpp:1461-1469` — the doc's "or an id beyond `externals`" now names my r13 second arm. |
| `alias` "no session resolver" restatement (:1196-1197) | **Fixed, better than asked.** :1195-1197 reads "no session resolver consults the alias — the `d.patch` rediscovery join below and `learn_plan`'s replay dedupe do", and a new clause states the join's multiplicity (:1202-1205): "aliases are not deduped (`learn_plan`'s dedupe hits stored clause `+d.name`s only and `patch:` labels are unreachable), so two live patches can share one alias and the caller picks by `ordinal`, latest-wins" — traced through the session dedupe and it is exact: the second same-alias `learn_plan` finds no clause `+d.name` match (the installed clause is `patch:<n>`-labelled), appends, and both patches carry the same alias. |
| `patch:`-shadows-`d.vars` removed (:1771-1773) | **Fixed.** The false mechanism is gone and replaced by the correct negative: "the content *is* the name the `d.vars` walk matches on `textOf` (`vortex_stdlib.cpp:1953`), a `+d.name` label shadows nothing and no resolver on the symbol arm reads one". `textOf(sym) == symName` verified at `vortex_stdlib.cpp:1953`. |

## Whole-document coherence

- **`d.captured` fully excised from live text** — remaining hits are exactly the two
  annotated-history sites: the v1.12 changelog entry now past-tenses it and annotates the
  supersession ("the referent rode a dedicated `d.captured` dim … the collision R13-A then
  proved `d.captured` merely relocates to the shared referent's `-d.captured`, 1.13 dropping
  the dim for a content-carried endpoint", :3510-3513), and the v1.13 entry narrates the drop
  (:3548-3553). Zero hits in normative text, mint lists, the diagram, or genesis.
- **`d.refFor` terminology uniform** — :95, :533, :1018, :1025, :1052, :1139, :1146-1147,
  :2204, :2788, :3028, :3507, :3559 all agree it is the record→slot correlation dim.
- **R13-A phrasing consistent across the four live sites** — §6.1's enumeration (:1147-1167),
  the head diagram (:1054-1059) and genesis (e) (:2200-2206, :2216-2220) all state the same
  three-class split: internal → slot `int64` + old→new map (+`d.values` on the record, one
  record one edge, only where `-d.values` is free); external/state-side → slot `int64` /
  `*handleTarget`, "no edge at all"; program-side → `+d.name → the dim name` re-resolved
  through `ensureDimension`'s name arm. The glossary carries no `d.captured` remnant; its
  "state-side cell link" for captured member-var bindings (:137) is the *edge-capture* class,
  disjoint from the ref-record machinery R13-A touched. One mismatch in the changelog —
  finding R14-1.
- **Premise fix verified** — §9.2 declares `agent Guard`, `goal Patrol() perform`,
  `plan cautious for Patrol` (:2596-2600), so `learn_plan("Patrol","demo:Guard.cautious",…)`
  (:1218-1219) genuinely dedupe-hits the declared clause's qualified name under the
  `learn_plan(goal, name, test, body)` signature (:1733). The `alias`-scoping addition
  (:1747-1749) is consistent: `learn_plan`/`learn_handler` take `name`, `learn_where`/
  `learn_body` take none, `learn_method`'s is functional (:1733-1734).
- **`documentId` arm verified against code** — `documentId_` is `Store`-ctor state
  (`store.hpp:133`, set `store.cpp:133-135`); `historyFromSeal` mints
  `std::make_unique<Store>()` — the no-id default ctor — at `publication.cpp:~1097`; the (v)
  claim is accurate (cite precision nit below).
- **(d)/(e) replay coherence** — (d)'s victim re-derivation (:2185-2190) matches §6.1's
  faithful-and-divergent cases; (e)'s "the slot's own promoted target *is* the referent's fold
  position for internal and external endpoints alike" (:2202-2203) correctly scopes
  `*handleTarget` to the two position arms, and "an equality that holds for the external arm
  only" (:2224) keeps the internal map-rebind honest.
- **`set_weight` seed rule consistent** — §8.1 :2448-2449 ("`weight_of` disengages on no
  record; a `set_weight` to an unkeyed name mints the `d.vars` record first") composes with
  the `Opt<Float>` signature (:1102) and the §9.3 `else` arm (:2651); the §8.2 synapse example
  (:2500-2501) needs no `else` because a junction owns its `FloatCell` by mechanics (:2458).
- **Mechanicals** — `mdl -i` exit 0; 22 fence markers = 11 balanced pairs; 7 table blocks;
  all 16 TOC anchors resolve against the 16 `## ` headings; "Design draft." status line
  intact (:6); version field 1.13; no stray `v1.12`/`1.12` references outside the changelog.
- **v1.12-entry annotation acceptable** — the retroactive past-tense + R13-A pointer inside
  the v1.12 entry is a marked supersession, not a narrative rewrite; consistent with the
  established history convention.

## New findings (v1.13 deltas only)

**R14-1 MINOR — the v1.13 changelog misgroups *program-side* into the slot-int64/`*handleTarget`
arm.** :3555-3557 reads "external/state-side and program-side endpoints ride the slot's own
`int64` ref position in-session or `*handleTarget` after promotion" — but the live restatement
keeps program-side on the `+d.name → the referent dim's name string` arm, re-resolved through
`ensureDimension`'s name arm at replay (§6.1 :1161-1165), and (e) explicitly scopes the
promoted-target read to "internal and external endpoints alike" (:2203). As written the
changelog swaps program-side and internal: internal is the map-rebind class, program-side the
name-resolution class. **Fix:** "internal endpoints rebind through the old→new reify map,
external/state-side endpoints ride the slot's own `int64` ref position in-session or
`*handleTarget` after promotion, program-side endpoints the `+d.name →` dim-name arm
re-resolved through `ensureDimension`".

**R14-2 MINOR — the new §10.2 item (v) is un-narrated in the v1.13 changelog.** The (v)
`documentId`-adoption arm (:2833-2837) is a substantive addition — a fifth named owed-surface
item — yet the changelog's minor enumeration (:3561-3580) never mentions it; every other
adopted item is narrated. **Fix:** append "names §10.1's `documentId`-adoption arm as owed
surface (v) beside the segment-apply seam — `historyFromSeal` mints `Store()` internally with
no id parameter". Adjacent cite imprecision: `store.cpp:124` (:2835) is the default ctor's
`userPermascroll_` *initializer* line; the no-id signature is :123.

**R14-3 MINOR — "a session hit on a grafted twin reduces to the append case" needs the same
replay-dropped-displacer caveat the next clause grants.** :1235-1236 — if the grafted twin
shadowed a same-named *declared* clause that an earlier patch displaced, and that displacer is
replay-dropped on its own dead target, the declared folds back *live* and the twin-hit replays
as a verbatim **displace**, not an append — the identical intervening-drop mechanism the very
next clause states for the session-append case (:1236-1237). The governing sentence ("the
replayed dedupe re-derives its victim from the folded rank, the verbatim alias match *at replay
time*") already covers every case; the "reduces to the append case" gloss narrows it wrongly,
and the changelog repeats the gloss (:3572). **Fix:** "a session hit on a grafted twin
displaced nothing durable (grafts never fold back) — the replay then re-matches the folded
rank, appending where no same-named clause survives, displacing where one does", and drop
"reducing to the append case" from the changelog for the same shape.

**R14-4 MINOR — the bare "never a session lookup key" survives the restatement in two places.**
The r13 finding named the phrase misleading because a session read (the `d.patch` join) exists;
:1196 now defines the precise claim ("no session resolver consults the alias"), but the
identical phrase persists unqualified at :1231 ("the alias is replay-keyed, never a session
lookup key") and in the glossary `patch` row (:136 — the exact site my r13 finding flagged).
**Fix:** mirror the qualifier in the glossary row ("no session resolver consults it —
`learn_plan`'s replay key and the `d.patch` join do") and let :1231 defer to :1196 ("the alias
is replay-keyed — §6.1's no-session-resolver rule").

**Nit bundle (v1.13 delta text + residuals):**

- `:2117-2118` — "a fragment's slot link to an agent-`d.vars` binding cell": post-R13-A "slot"
  names the content-carrying ref-box that takes *no edge*; the reach the sentence needs is a
  capture *edge* (glossary :137's "state-side cell link") — "a fragment's capture edge to an
  agent-`d.vars` binding cell" is the precise claim.
- `:1115` — the `set_weight` table row still glosses "write the `d.weight` scalar through the
  focus slot"; §8.1's new mint-on-miss makes it incomplete for the clause-correlated arm
  (the write may mint the keyed `d.vars` record first).
- Residual unadopted r13 nits, still present: `wingOf` cited `:366-369` renders
  `linked(opcode, dims_.grab, POS)` but `POS` is `inputsOf`'s argument at `vortex_core.cpp:373`
  (:1144); the `scrollKey → nullopt` arm (anonymous segment-less scrolls) is unnamed in the
  comparand — `globalKeyOf`'s shape covers it only implicitly; "replayed grafts" at :2173
  against the verb vocabulary; "`learn_where`/`learn_body` install nothing" :1769 ("install no
  *clause*"); "returns at the next genesis only if the displacing patch was unlearned first"
  :1241-1242 (a replay-*dropped* displacer is a second channel); sketch comment "`patch:<n>`
  off the d.vars serial" :2643 remains the only unpinned serial mention.

VERDICT: CONVERGED — all four r13 minors verified landed (the comparand is one-directional
across :1601/:1987/(a) :2080-2094 with the `globalKeyOf` arm set verbatim-verified against
`publication.cpp:853-869`,`:985-987`,`:1049-1050` and `store.cpp:1461-1469`; the `documentId`
claim verified against `store.hpp:133`/`store.cpp:133-135`/`publication.cpp:~1097`); `d.captured`
is excised from all live text and survives only as annotated v1.12 history; §6.1/diagram/
genesis (e) state the same three-class content-carried split; mechanicals clean — the residue
is four MINORs (the changelog's program-side misgrouping, the un-narrated (v) item, the
"append case" gloss that needs the divergent-case caveat, and the un-migrated "never a session
lookup key" phrasing) plus a nit bundle.
