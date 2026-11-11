# R14 — ZigZag topology/promotion critique (v1.13)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.13 deltas in my lane: the R13-A
`d.captured`-drop/content-carried referent (schema :1050-1063, mechanism :1139-1167, genesis
(d)/(e) :2192-2234, dim lists :95, :533, :1018, :1025, :2788, :3028, changelog :3548-3580), the
comparand `globalKeyOf` arm (:2083-2098), the pronoun/reference-side fixes (:1598-1607,
:1984-1987), the equal-text labels + direction fix (:1741-1744, :1765-1776, :2178-2184), the
agent-legs restatement (:2114-2121), the alias conditional-presence/join multiplicity
(:1193-1205, :1747-1749), the faithful-and-divergent dedupe restatement (:1225-1242,
:2186-2191), the `documentId` register item (v) (:2829-2835), and the `weight_of`/`set_weight`
seed fix (:2445-2448, :2648).

Verified against `arena_manifold.cpp:78-105` (`ensureDimension` name arm — `dimensionNamed` →
`arenaDims_` → mint + `d.dims` rank append), `arena_manifold.cpp:1759-1764` (`promote`'s
relocation arm enqueues `handleTarget` into `seen`/`frontier`, `!contains` → `nullopt`),
`arena_manifold.cpp:1797-1809` (the ephemeral-handle mint gate), `arena_manifold.cpp:1947-1951`
(`makeOpHandle` writes `isEphemeral(target) ? real.at(target) : target`),
`manifold.cpp:1115-1122` (`handleTarget` returns `valueBits` for `OpHandle` kind, `nullopt`
otherwise — the (e) kind-guard's no-op basis), `mesh_value.cpp:485-501` (`importValue`: unmarked
`OpHandle` → `None`; relocation handle → `OpHandle` naming `local.at(target)`, target
containment required), `mesh_value.cpp:323-362` (`receiveValue` rebuilds the manifold verbatim
— handles arrive carrying promoted targets), `mesh_actor.cpp:260-273`, `compiler.cpp:769-772`
(`dimConstant` = `int64(findOrMintDimension(name))`), `compiler.cpp:531,781,795` (shared
`cells_->zero`/`minusOne` literals — routine shared referents), `mesh_evaluator.cpp:725-735`
(per-reference `"cell"` marker mint precedent), `foreign_op_handle.cpp:98-117`
(`isCellRelocationHandle` = `OpHandle` kind + `d.mesh.relocation`→`"cell"`),
`vortex_core.cpp:366-369` (`wingOf` absent-head `{}`), `:384-390` (`bindInput` first slot),
`arena_manifold.cpp:1347-1377` (displaced-occupant clearing), `publication.cpp:853-869`
(`globalKeyOf` arms), `:985-987` (`sealableOps` skip), `:1047-1050` (the `localScroll` +
`breakMarkerScroll` remap emplaces), `publication.hpp:447` (`breakMarkerKey`),
`scroll.cpp:13-22` (`scrollKey`), `store.cpp:1461-1469` (`Store::scroll` disengage arms),
`store.cpp:123-125` (`Store()` mints `userPermascroll_` :124, `chronofilade_` :125, no id
parameter — `documentId_` lives at `store.hpp:1435` with the two-arg ctor at `:132-133`),
`publication.cpp:1097` (`historyFromSeal` mints `Store()`), `vortex_vm.cpp:98-100`
(`setOpcodeKind`), `vortex_stdlib.cpp:1953` (`textOf(sym) == symName`).

## Verdicts on the r13 items

### R13-A — drop `d.captured`, carry the referent as content: **resolved — sound end to end**

The mechanism now coheres under M14 on every edge it still writes:

- **`d.refFor` sole correlation, occupancy sound** (:1051-1054, :1139-1147). The record writes
  `+d.refFor → slot` onto the slot's `-d.refFor`, free by construction: one ref-capture record
  per `+d.refs` ref-record, one refs record per marked slot ("a slot pairs with at most one
  ref-record" :1146-1147 — `bindInput` mints a fresh slot per input, `vortex_core.cpp:384`, so
  slots are per-edge unique). The three edges converging on one slot land on three distinct
  dims: `+d.ref` → `-d.ref`, `+d.refFor` → `-d.refFor`, the opcode wing `+d.grab` → `-d.grab` —
  no arm contends, at emission and at every replay (copy slots are fresh). `d.captured` is
  struck from every minted-dim list (:95, :533, :1018, :1025, :2788, :3028); it survives only
  in the change-history recounts (:3510-3513, :3548-3560). The three restatements agree:
  diagram :1050-1059, mechanism :1147-1165, genesis :2200-2229.

- **External/state-side referents**: slot `int64` in-session, `*handleTarget` after promotion
  (:1158-1160) — and the promotion path checks out: the marked slot re-kinds `OpHandle`,
  `promote`'s relocation arm enqueues `handleTarget` into `seen`/`frontier`
  (`arena_manifold.cpp:1759-1764`) so the referent folds inside the same segment, and
  `makeOpHandle` writes `isEphemeral(target) ? real.at(target) : target` (:1949-1950) — the
  fold position in both cases (ephemeral via `real.at`; band cells fold verbatim). Genesis (e)
  writes `Int64(*handleTarget)` back into the slot (:2202-2204) — `handleTarget` reads
  `valueBits` on `OpHandle` kind (`manifold.cpp:1115-1122`), which *is* the promoted target on
  the `receiveValue`-rebuilt manifold (handles arrive verbatim, `mesh_value.cpp:345-359` —
  the doc's contrast with `importValue`'s `local.at(target)` re-mint/`None` demote at
  `mesh_value.cpp:485-501` is stated accurately at :2208-2214). On replay-installed copies the
  arm no-ops by kind-guard (`New`/`Link`/`Value` cannot mint handles, :2225-2227) and the
  external slot was `Value`-written `int64(*handleTarget_donor)` at (d) — "the identical bits"
  (:2222-2224), donor handles still live because (e) runs last. Sound.

- **Internal referents**: slot `int64` names the source position, the old→new map translates to
  the copy twin, map membership is the discriminator (:1152-1155, :2221-2222). At (d)-replay
  the map keys the *folded* cells (read off the donor's `*handleTarget`) — internally
  consistent since the recipe replays against the folded head. On the registered head itself,
  (e)'s `Int64(*handleTarget)` names the folded internal cell — "for internal and external
  endpoints alike" (:2203) — verified, since the whole head folds. The optional
  `+d.values`→endpoint edge is occupancy-guarded ("only where the endpoint's `-d.values` is
  unclaimed" :1156-1157) and load-bearing nowhere — the slot's content is authoritative —
  so its loss to a later `+d.values` writer degrades self-description, never correctness.
  Deterministic at emission and mirror-deterministic on the copy (the copy-twin's `-d.values`
  occupancy mirrors the source's under recipe-order replay, including the descriptor's
  `+d.values`→result/produced claims, :1032-1036).

- **Program-side (dimConstant) referents**: `+d.name → dim name` on the *recipe-side*
  ref-capture record (:1161-1165, :2205-2206, :2219-2220), re-resolved through
  `ensureDimension`'s name arm (`arena_manifold.cpp:78-105` verified: `dimensionNamed` →
  `arenaDims_` → mint + `d.dims` append). Correctly placed: `+d.name` on the `+d.refs` record
  is the name-record discriminator (:1044-1049, :1918-1919), so the dim name must not live
  there — a name-record would skip the marker ceremony entirely, and `qualified name or head
  label` shapes can't be confused with a `d.*` spelling. The `+d.name` edge lands on a fresh
  name cell's `-d.name` — free. At (e), program-side slots get `ensureDimension(name)` rather
  than `*handleTarget` (the folded dim-*data* cell is not the receiver's registered dim) —
  the conditional is stated. `closed`'s program-cell refusal (:1860-1862) is consistent: the
  `+d.name` record *is* the name-resolution its "unless name-resolved" clause requires.

- **`closed` frontier**: "reads each ref-capture endpoint through the paired slot's own
  content" (:1166-1167) — reachable via `+d.ref`/`+d.refFor` pairing + slot `int64`, no edge
  needed. Consistent.

- **Stale-alias preclusion**: replay derives copy slots from the *live* handle/content read,
  never the recorded emission-time literal (:2222-2228) — the bug class the endpoint
  derivation exists to preclude, correctly preserved.

### Minors — all applied

`+d.name` direction fixed at :1767 ("each label cell's `-d.name` backlink holding a single
edge"). `patch:`-shadows-`d.vars` corrected at :1771-1773 — the walk matches *content*
(`textOf`, `vortex_stdlib.cpp:1953` verified), "a `+d.name` label shadows nothing". Agent-legs
restatement present at :2114-2121 — but see Finding 1: the fix kept my r13 wording, which
predates the edge-removal it accompanies. Equal-text labels at :1741-1744 and :2181-2184
(two `patch:<n>` cells, re-mint transport, no `plan_named` collision). Comparand takes
`globalKeyOf`'s arm set minus `localScroll` — `breakMarkerScroll`→`breakMarkerKey`
(`publication.hpp:447`, sealed and identity-remapped `:1047-1050`, `sealableOps` skip
`:985-987`), engaged externals via `scrollKey` under the widened `Store::scroll` disengage
(`store.cpp:1461-1469`, "or an id beyond `externals`", any other disengage failing) — the
break-marker hole is closed and stated at :2086-2095. Reference side pinned to the lineage
store's decoded band (:1598-1607, :1984-1987). Alias: conditional presence (:1225-1228,
:1747-1749), "no session resolver consults" (:1196-1197), join multiplicity latest-wins
(:1202-1205), `unlearn` never matches the record's own label (:1764, :1776). Dedupe covers
faithful-and-divergent victims (:1232-1237, :2188-2190). `demo:Forager.travel` →
`learn_plan("Patrol","demo:Guard.cautious",…)` against §9.2's declared clause (:1218-1219).
`documentId`-adoption registered as (v) (:2829-2835) — accurate: `historyFromSeal` mints
`Store()` (`publication.cpp:1097`), the no-id ctor (`store.cpp:123-125`); `documentId_` exists
(`store.hpp:1435`, two-arg ctor `:132-133`) but nothing passes it in. `weight_of` disengage /
`set_weight` seed-mint (:2445-2448) and the §9.3 `else set_weight(c, 0.1)` bootstrap (:2648).

## New findings (v1.13 deltas only)

### 1. MINOR — the agent-legs fix kept my r13 "capture edges" phrasing, which R13-A just abolished: under content-carried there is no slot link for the walk to follow

`:2114-2118`: "reached only where a runtime-grown edge leads to them — … so the walk reaches
them through capture edges — **a fragment's slot link to an agent-`d.vars` binding cell** is
exactly the beyond-band reach the previous sentence covers". In v1.12 `+d.captured` was a real
edge and this was true; v1.13 carries every referent as slot *content* — "no edge at all"
(:1160) — and int64-content references are "invisible to any link walk" (:2159, :1866) by the
document's own rule. So the agent-`d.vars` legs are once again *not reached at all* — the
v1.12 mechanism claim was in fact the right one, now for the opposite reason (no edge exists,
not because the roots are unreachable). The conclusion is unaffected and remains right — the
walk's job is classification, and an agent-cell leg classifies `>E_ops`/state-side regardless.
Fix: "agent-cell legs are reached only by non-capture runtime edges — captures ride slot
*content*, invisible to the link walk — and every such leg classifies `>E_ops`/state-side
regardless, so no agent-rooted second walk is owed."

### 2. MINOR — (d)'s hand-off sentence still names the record as the endpoint source: "the record rather than the donor cell's live value" inverts the content-carried dataflow

`:2192-2194`: "the copy's ref slots derive from the *capture-record endpoints* at replay — …
its rehydration source being the record rather than the donor cell's live value". Under R13-A
the record carries *no* endpoint — only the `+d.refFor` pairing (plus the conditional internal
`+d.values` and program-side `+d.name`); the endpoint *is* read off the paired slot's content /
`*handleTarget`, i.e. exactly the donor cell's live state. The (e) coverage paragraph restates
it correctly at :2216-2224 ("the referent itself is read off the paired slot's *content*"), so
the two paragraphs now disagree about which side supplies the endpoint — an implementer
reading :2194 looks for an endpoint field on the record and finds none. Fix: "derive from the
*paired slot's promoted content* at replay — the record supplies the pairing and the class
(`+d.refFor`, optional `+d.values`, `+d.name`), never the endpoint value".

### 3. MINOR — "cell link"/"capture links"/"capture edges" vocabulary not propagated to three sites

The content-carried rewrite leaves edge vocabulary where slot content is now meant: :137's
glossary ("every outside reference is a state-side *cell link*"; "a program-side *capture
link* without a record is the refused class"), :1170 ("the copy's *capture links* point at the
same definition-site cells" — they are slot positions, not links), and :1863-1864 ("the
enumerable frontier — `+d.refs` records, *capture edges* on the emission's wing/record dims" —
the frontier enumeration now reads slot *content* per :1166-1167, the one site where stale
vocabulary could misdirect an implementer enumerating dims for edges that no longer exist).
Fix: at :1863 "capture *slots* read through the paired slot's content"; at :137 "a state-side
cell *reference* (slot-carried position)"; at :1170 "the copy's capture *slots* name the same
definition-site cells".

## Verified accurate (v1.13)

`d.captured` absent from all live lists and the mechanism; `d.refFor` present in all six
enumerations. `ensureDimension` name arm (`arena_manifold.cpp:78-105`). `promote`'s handle
frontier + mint gate (`:1759-1764`, `:1797-1809`) and `real.at(target)` (`:1949-1950`).
`handleTarget`/`valueBits` (`manifold.cpp:1115-1122`) and the kind-guard no-op on copy slots.
`importValue` vs `receiveValue` path difference (`mesh_value.cpp:485-501` vs `:345-359`).
`isCellRelocationHandle` requirements (`foreign_op_handle.cpp:98-117`). `dimConstant`
(`compiler.cpp:769-772`), shared-literal referents (`compiler.cpp:531,781,795`), per-ref
marker mint precedent (`mesh_evaluator.cpp:725-735`). `wingOf`/`bindInput` cites refreshed
correctly (`vortex_core.cpp:366-369`, `:384-390`). Comparand arm-set and all its cites
(`publication.cpp:853-869`, `:985-987`, `:1047-1050`; `publication.hpp:447`;
`scroll.cpp:13-22`; `store.cpp:1461-1469`; `spool.hpp:19`). `historyFromSeal`/`Store()` cites
for (v) (`publication.cpp:1097`, `store.cpp:123-125`, `store.hpp:132-135,1435`). `textOf`
symbol-content match (`vortex_stdlib.cpp:1953`). `setOpcodeKind` (`vortex_vm.cpp:98-100`).
The (e)-runs-last ordering is load-bearing and correctly stated: (d)'s external replay reads
`*handleTarget` off donor slots that must still be handles.

VERDICT: CONVERGED
