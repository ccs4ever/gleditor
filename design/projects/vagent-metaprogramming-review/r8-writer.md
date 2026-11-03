# r8-writer — convergence critique of `design/vagent-metaprogramming.md` v1.7

Scope: per-finding verdicts on r7-writer's R7-1..R7-4 + nit bundle against v1.7 (2,673 lines,
`bd2b791e` → `3feb0065`, +169/−61), new-defect sweep of the deltas (:235-259 M6, :352-388 M11,
:583-598 §4.5, :1273-1280 §6.5, :1325-1396 sideness/home, :1452-1576 descriptors + name-records,
:1608-1700 pump + genesis (a)–(f), :1727-1778 §6.7, :2184-2218 inventory/register, U-M11,
changelog :2622-2673). New cites verified against the tree.

## Verdicts on R7 findings

| R7 | Verdict |
| --- | --- |
| R7-1 `src:`/`reify:` reservation contradiction | **Fixed.** The "a module literally takes that name … dropped patch" arm is deleted (:1540-1542 now reads "call records always carry the `module:`-qualified spelling while label records carry `src:`/`reify:`/`#label` head-label spellings"); :1749-1751 reserves all three prefixes "at module declaration" as a compile-time refusal, matching the changelog's side. `patch:` is folded into both places with its own correct classification. One new wording slip in the fix — see R8-2. |
| R7-2 genesis (b) `opcodeMap_` rationale | **Fixed.** :1671-1675: "The dependency is the empty map, not stale indices: `opcodeMap_` is per-`WorkerContext` and empty after `resume`, and this pass is what `code` re-emission and `getOpcodeKind` … need filled". §6.5 :1274-1280 carries the same account and explicitly rejects re-running `mintOpcode` (ephemeral twins, `vortex_vm.cpp:73-82` verified). |
| R7-3 `d.name` rank-dim claim | **Fixed.** `d.name` is out of the bare list (:1328 keeps `+d.name`) and :1332-1333 adds the exact correction: "`d.name` is not one: it is a single-link label dim (`vortex_core.cpp:61`), already covered as `+d.name`". |
| R7-4 M6 "`run` emits a `#Call`" | **Fixed.** :236-240: "the operand slot is head-valued (§6.2) — and pushes the §5.8 `#Call` **frame**: the call/frame convention, not a `Call`-opcode operand — `Call` executes `toInt(inputs[0])` (`vortex_vm.cpp:854-856`)" — verified (`case Call` :854, `toInt` :856). The price line :258-259 drops "and a `Call`". §11's "same-cursor `#Call`" (:2244) is now consistent under the frame-convention gloss. Residual loose phrasing in §9.1 — nits. |
| nit: P16 row head/op hop | **Fixed.** :2218: "`+d.values` → the op-or-head hop → `writeValue(slot, int64(entryOp))`". |
| nit: capture-wing doubling | **Fixed.** :1380-1383: items are "capture-linked binding cells", the anchor parenthetical holds "a program-side capture wing" — wing as anchor once, cells as items. |
| nit: patch-target descriptor resolution | **Fixed — and expanded.** :1454-1462 splits clause targets (`plan_named` verbatim `+d.name`) from method/symbol targets (`symbol_named`'s module-`d.vars` walk, "a method has no `+d.name` label to match"). New edge case surfaced — see R8-3. |
| nit: "pre-head convention" | **Fixed.** :297: "the op-or-head resolution rule §6.1 states". |
| nit: pump-ordering claim | **Fixed.** :1610-1617 re-attributes: "the ceremony claims the ordering itself — the flag's `publishValue` call is issued from the same `Executor` callback … synchronously"; "current `pumpOne` gives no such ordering on its own … *proposed adapter discipline*, not an attribution to the pump (changelog v1.7)" — and the 1.7 entry records it (:2666-2668). |
| nit: `src:` shorthand vs `#label` | **Fixed.** :1503-1504: "a literal operand → the head's actual `+d.name` label — a `code"#label …"` literal's user label, else its `src:`". |
| nit: "Both match labels verbatim" | **Fixed.** :1770-1778 splits by representation: symbol names are cell *content* (`exportSymbol` mints `makeCell(name)`, no `+d.name` edge), clause names are `+d.name` label cells; "Both compare the name string verbatim — it is the cell carrying it that differs". |
| nit: `(§6.6)` self-reference | **Fixed.** No `§6.6` self-citation remains inside §6.6's span (:1308-1716). |
| nit: unchronicled v1.6 edits | **Partially.** The pump-ordering claim is now chronicled (1.7 entry). The "module vars"→"member vars" rename and `cap$N` alias rephrase remain unrecorded anywhere — residual, carried. |

## Disposition adoptions verified

R7-A (store lineage): `job.base` prefix at :593-598/:1341-1358, `sealLocalSpool` whole-spool
(`publication.cpp:611-617` verified — `sealableOps(store, opsAlreadySealed)` over the full op
count), `rebuildManifold` `birthOp = opIndex` (`manifold.cpp:435` inside :427-442 ✓), prefix-less
fail-closed :1367-1375, genesis "arrives, not constructed" :1650-1656, byte-identical + `setOpcodeKind`
pass :1342-1343/:1667-1675. R7-B (e-after-d reorder): (d) replay :1678-1681, (e) sweep last
:1682-1699 with the copy-inheritance dependency named. Minor batch all present: slot-role writeback
:1529-1536, `plan_named`/`symbol_named` run-operand arm :1505-1506 + headless-`run` :1122-1126/
:1550-1554, `home` persistent :1385-1389 (`vortex_core.cpp:30-36` `initGenesis` adopts `*base->home()`
verified; `manifold.cpp:450-451` `home_ = opIndex` ✓), `OpHandle`/`ForeignOpHandle` :363-364/:2218,
`1..E` throughout, U-M11 :1762-1765/:2446-2452, nested-label `+o:c` arms :1732-1738, memo
"no record for keyless attach" :298-299.

## New findings

**R8-1 MINOR — "(step (e) above)" is a forward reference mislabeled.** :1570-1571: "a *learned*
re-point replayed at genesis step (d) is picked up exactly because the `+d.refs` sweep runs after
replay (step (e) above)". The (a)–(f) enumeration is at :1657-1700, *below* this sentence — no
step letters exist above it. Added this round; should read "(step (e) below)".

**R8-2 MINOR — `patch:` is filed under "the emitter's automatic prefixes", contradicting :1541.**
:1749-1751: "The emitter's automatic prefixes are **reserved at module declaration**: `src:`, `reify:`
and `patch:` may name no module". `patch:` is not an emitter prefix — it is not a head label at all
(:1541: "`patch:` is a *clause*-label prefix that patch-target names use, never a head label"), and
clause `patch:<n>` names are user-supplied through `learn_*` arguments (:1086-1087, §9.3), not
emission output. The reservation is right; the subject is wrong — "the label prefixes `src:`/`reify:`
and the `patch:` clause-name convention".

**R8-3 MINOR — the target descriptor's "resolved qualified name — `<module>:<Agent>.<member>`" does
not cover learned-clause targets.** :1454-1455 presents the three-part form unconditionally, but a
`learn_where`/`learn_body` retargeting a *learned* clause resolves through `plan_named`'s verbatim
arm: the clause's `+d.name` is `patch:near_water` (conventional naming, :1086-1087; §9.3), the
module arm fails since `patch:` is reserved, and the recorded name is `patch:near_water` — not
`<module>:<Agent>.<member>`. The doc's own split (:1746-1748: the whole string is the name when the
first segment is not a module) makes that path legal and named; the descriptor claim should say
"the resolved name — qualified for program targets, the verbatim `patch:` label for learned ones".

**R8-4 MINOR — the head-layout restriction's rationale names a non-existent matching class.**
:1558-1560: the label→head walk matches `+d.name` "only over cells in the head layout … — the shape
restriction that keeps clause labels and qualified symbol names on the same dim from matching".
Clause labels (qualified names as `+d.name` cells) do need excluding, but *symbol* names are cell
**content**, never `+d.name` (:1773-1774 states exactly this) — they cannot match a `+d.name` walk
regardless of layout, so the restriction has no work to do on them. Drop "and qualified symbol
names" or re-aim it at plan-clause labels.

**Nit bundle:**

- :1691-1692 — "The sweep runs last for two reasons that meet at copy-inheritance": only the first
  reason is copy-inheritance (slots inherit the registered head's contents); the second —
  name-records must see post-installation symbols — is ordering visibility, not copy-inheritance.
- :1658 — "the persistent high-water below which program cells sit equals `E`": cells sit at
  positions `1..E` *inclusive*; :1339 correctly says "at or below". "Below" shaves position `E` off.
- :1396 — "the sideness test (below)": the sideness test is *above* (:1326-1367); "(below)" can only
  attach to the refusal detailed at :1484-1486, and reads wrong as written. Pre-existing.
- :983 — "(link eviction, §4.3)": §4.3 is the trail section; eviction is §6.1/M14/`link`'s
  `arena_manifold.cpp:1347-1377`. Pre-existing wrong xref.
- :1733-1735 — "labels from its **innermost** `src:` site" mislabels the convention: per §5.1
  `src:m:l:c` is the *enclosing* literal's own position and `+o:c` arms resolve inward — the label's
  `src:` component is the outermost site, the *resolved* position the innermost. Also `src:m:l:c+o:c'…`
  uses a stray-prime metanotation labels can't literally carry.
- :2031 — "a `Call` + spill instead of inlining": post-R7-4 this re-reads as the opcode; `#Call`
  frame + spill is the reconciled spelling.
- :1520 — `vortex_vm.cpp:854-857` vs `:854-856` (:240, :1533) for the same `Call`/`toInt` fact —
  trivial range inconsistency (the accurate core is :854-856).
- :1748 — "`patch:near_water` is just a label" inside the `code_named` scope note is vacuous:
  `patch:` labels are clause labels and `code_named` walks `d.code` heads, so the example string
  can never resolve. Illustrative but misleading.
- :1764/:2450 — "a foreign head outside it classifies prefix-less": prefix-less is a property of a
  *lineage* (the fail-closed arm), not a cell classification; shorthand leaks the term.
- Changelog :2658 — "`arena_manifold.cpp:1952-1954`" is narrower than the text's `:1934-1954`; the
  source shows Int64-verbatim :1935-1937, `OpHandle` :1947-1952, `ForeignOpHandle` :1953-1956 —
  the changelog cite lands mid-`ForeignOpHandle`-arm only.
- Changelog :2639 — "**(e) the `+d.refs` name-record sweep last**" narrows the sweep: (e) covers
  ref-records (`Int64(*handleTarget)`) *and* name-records.
- Unchronicled this round: the new memo keyless-record sentence (:298-299); the carried member-var/
  `cap$N` omissions from v1.6.
- "(e) `+d.refs` rehydration, last" (:1682) — "last" is loose with "(f) turns" following; means
  last genesis pass. Harmless but imprecise.
- "r7-B" (:2640) — lowercase r where the disposition says R7-B; earlier entries used the same
  lowercase style for "r6 disposition" so it is consistent-enough style, noted for completeness.

## Mechanical checks

`mdl -i`: **clean, exit 0**. 22 fence markers = 11 tagged pairs, balanced. All 16 TOC anchors
resolve; heading numbering sequential §1–§15 + appendix. Register table 6 columns on both P15/P16
rows; §4.5 boundary table 6 columns on all rows. Cites verified on the delta: `arena_manifold.cpp:
1934-1956` (Int64 verbatim :1935-1937, `OpHandle` :1947-1952, `ForeignOpHandle` :1953-1956),
`vortex_vm.cpp:854-856` (`Call`), `:98-100` (`setOpcodeKind`), `vortex_core.cpp:30-36`
(`initGenesis` → `home_ = *baseHome`), `manifold.cpp:435` (`birthOp = opIndex` within :427-442),
`manifold.cpp:450-451`, `mesh_actor.cpp:199,215` (the `programRef` pin comparisons),
`mesh_actor.cpp:258-271` (`receiveValue` + `WorkerContext` on the received manifold),
`mesh_value.cpp:485-501` (importValue demotes unmarked `OpHandle` → `None`, re-mints marked cells
to local twins), `publication.cpp:611-617` (`sealLocalSpool` seals `opsAlreadySealed..opsTotal`),
`dim_vector.hpp:23` (`noCell = 0`), `arena_manifold.hpp:468-470` (`refOf = ephemeralBit | dense`),
`manifold.hpp:154-158` (`ephemeralBit`/`isEphemeral`).

**Changelog 1.7 accuracy:** every claim verified against the diff — lineage reframe, genesis
reorder, slot-role writeback, headless-`run`, `patch:` reservation, `plan_named` unification,
persistent `home`, `d.name` correction, `#Call` reconciliation, U-M11, nested labels, descriptor
split, and each nit-bundle item — except the `:1952-1954` narrowing and the "name-record sweep"
shorthand (nits above). Omissions are additive clarifications only (keyless-memo sentence; the two
carried v1.6 items).

VERDICT: NOT CONVERGED (minor level) — all r7 findings resolved with correct cites; 4 new minors +
a 13-item nit bundle, all inside v1.7 delta text, headed by the "(step (e) above)" wrong-direction
reference (R8-1) and the `patch:`/emitter-prefix misclassification (R8-2). The lineage reframe is
internally coherent — no stale "image-first"/"do not checkpoint"/`0..E` survives outside historical
changelog entries; §4.5, M11, §6.6, register rows and the adapter inventory all read the same
prefix model. No majors; defect surface still contracting.
