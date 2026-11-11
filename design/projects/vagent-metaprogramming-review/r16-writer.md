# r16-writer — final coherence critique of `design/vagent-metaprogramming.md` v1.15

Scope: verification of my three r15 minors (`cap$N` position-carriage scoped; the changelog
retro-edit confession; the carried `wingOf`-cite/`set_weight`-gloss/serial-sketch nits), a
final whole-document coherence pass (marked-int64/`+d.recipe`-inventory terminology, the
three-arm `closed`, `masterOf`-decode-vs-edge, the v1.15 changelog incl. confessed
retro-edits, `mdl`, tables/fences/anchors, "Design draft."), and a new-defect sweep of the
v1.15 deltas only (`406d079b` → `b7e2a655`, +164/−75).

## Verdicts on r15 items — all fixed

| Finding | Verdict |
| --- | --- |
| `cap$N` position-carriage (R15-1) | **Fixed, essentially verbatim as proposed.** :1191-1194 now reads "its ref slots carry the *same* definition-site positions the source's did for external referents, its `cap$N` slots re-clone the same definition-site cells, and internal referents name the copy's own twins through the old→new map" — the two capture carriers separated. |
| Changelog retro-edit confession (R15-2) | **Fixed.** The 1.14 entry (:3693-3695) confesses "the 1.13 entry's two remaining retro-edits …: its program-side grouping corrected to the `+d.name`→`ensureDimension` arm, and the (v) narration it omitted backfilled"; the 1.15 entry (:3730-3734) confesses its own set (1.13+1.14 `cap$N`/`masterOf` ingress rewrites, the 1.14 arm-(c)/(d)-hand-off/tuple-keyspace corrections, the 1.11 "program-side referents"→"dim referents" drop). All claimed retro-edits verified actually landed (1.13 :3633-3637, 1.14 :3656-3684, 1.11 :3541). |
| `wingOf` cite `:366-369`→`:373` | **Fixed.** :1162-1163 now cites `vortex_core.cpp:373` — the `linked(opcode, dims_.grab, POS)` line inside `wingOf`, verified. |
| `set_weight` row gloss | **Fixed.** :1125 now reads "…through the focus slot — the keyed `d.vars` record minted on first write, a `+d.name`-less clause refused (§8.1)", and §8.1 states the refusal (:2508-2510): a `+d.name`-less target "resolves no key and is refused like any unresolved focus-slot verb". |
| `patch:<n>` sketch serial | **Fixed.** :2706 reads "// patch:<n> off the agent cell's d.vars serial", matching :2721-2722. |

## Whole-document coherence — final pass

- **Marked-int64/`+d.recipe`-inventory terminology** — uniform across the *declaration*
  sites: :379-383, :493-496, the head diagram :1055-1057 + :1066-1069, :1078-1081 ("the
  field cell's own `+d.recipe` record for the recipe's source-naming cells"), :1144-1155,
  :2041-2055, the P16 row (:2907) and the 1.15 entry (:3707-3715). One break at the
  *operational* site — R16-1 below.
- **`closed` three-arm consistent.** Glossary (:137), §6.6 (:1868-1882), the frontier
  (:1898-1901) and the re-verify (:1905-1908) all state (a) state-side endpoints however
  carried — `cap$N` link or slot-carried `int64` — (b) `+d.refs` name-record, (c) a dim
  referent name-recorded on the paired ref-capture record's `+d.name`, the refused class
  uniform. Verified no arm overlap: a dim cell is not link-reachable *from the agent cell*
  (it hangs on arena `d.dims`, not an agent rank), so arm (a) cannot absorb arm (c).
- **`masterOf`-decode vs edge fully disambiguated.** The R15-A restatement (:2157-2170) is
  mechanically exact: the only capture edge is the binding cell's `+d.clone →` the `cap$N`
  slot (`Clone` appends POS-ward at the master — `vortex_vm.cpp:817-823` verified:
  `linked(cloneTail, dims().clone, false)` is `fromNegward(false)`=POS), running
  state→program on a dim the `+d` vocabulary (:1524-1526) does not list, while the slot's
  `masterOf` is the operand decode (`cloneMaster` = `rankEnd` NEG-ward,
  `arena_manifold.cpp:1009-1012` verified). The frontier's `masterOf` read (:2169-2170,
  :1870) is scoped to `closed` enumeration, not the sideness walk — the right
  disambiguation. Both changelog echoes rewritten and confessed.
- **v1.15 changelog narrates correctly** — every adopted r15 item appears (the two majors
  with verified cites `arena_manifold.cpp:1764-1768`/`:1934-1936`, all twelve minors, the
  confessed retro-edits).
- **Mechanicals.** `mdl -i` exit 0. 22 fence markers = 11 balanced pairs. 7 table blocks
  (:4, :79, :123, :598, :1100, :2905, :2926). 16 TOC anchors ↔ 16 content headings (15
  numbered + Appendix). Version 1.15 (:5); "Design draft." intact (:6).
- **Code cites spot-verified this round:** `ensureDimension`'s name-first arm
  (`arena_manifold.cpp:78-105`); `d.dims`-edge skip in `promote` (:1766-1768); verbatim
  `Int64` fold (:1934-1936); `handleTarget` enqueue (:1759-1762); `Link`/`Break`/`New`
  binding `DimRef d = toInt(inputs[1])` (`vortex_vm.cpp:620`,`:643`,`:729`); `DimRef =
  CellRef` (`dim_vector.hpp:22`); `dimConstant` mints `int64(DimRef)` ref cells
  (`compiler.cpp:769-772`).

## New findings (v1.15 deltas only)

**R16-1 MAJOR — the re-kind walk is `+d.refs`-bounded; the `+d.recipe` field cells it now
marks can never reach `OpHandle` kind.** v1.15 extended the marked set to "every `+d.recipe`
field cell naming a source-cell position" (:380, :493, :1066, :1147, :2051-2054, :2907,
:3707-3710), declaring "the `+d.recipe` field record standing as their inventory beside
`+d.refs`" (:2052) and "the inventory the ceremony walks beside `+d.refs`" (:3710). But
§6.6's operational step was not extended: the turn-end re-kind pass "walks each registered
head's `+d.refs` ref-records and `setValueBits(OpHandle, target)`s every marked ref cell …
the walk is `+d.refs`-bounded" (:2083-2086). The doc itself forecloses the alternative
discovery path — "the marker alone is undiscoverable (no per-cell dimension enumeration,
U-M7)" (:1080) — so enumeration *must* walk the inventory, and a `+d.refs`-bounded walk
cannot reach a `+d.recipe` record. Consequence: recipe field cells stay `Int64`-kind;
`promote` folds `Int64` verbatim (`arena_manifold.cpp:1934-1936`); post-resume they hold the
source arena's dense ids and replay reads dead/aliased positions — the exact verbatim-fold
failure R15-B abolished, persisted for the very class the fix added. The downstream passes
inherit it: the restore pass guards on "`OpHandle` kind" (:2099-2100) and genesis (e) writes
`*handleTarget` (:2254-2255) — neither exists for a cell never re-kinded. §3's summary
"re-kinds each marked cell `OpHandle`" (:384) states the correct scope and contradicts the
step. **Fix:** the re-kind walk enumerates both inventories — `+d.refs` ref-records plus the
`+d.recipe` field records — and the restore/(e) enumerations ride the same enumeration;
drop or rescope "the walk is `+d.refs`-bounded".

**R16-2 MINOR — the tuple `dim` field has no name arm; a session-minted dim replays to the
orphan twin.** :1145-1149 + :3708 make every `+d.recipe` field naming a source cell —
explicitly "tuple `from`/`dim`/`to` endpoints" — a marked `int64` ref position. Read as the
edge's `DimRef` (the mechanically natural reading: the tuple mirrors
`link(from, dim, dir, to)`, and `DimRef = CellRef`), a `dim` field naming a *session-minted*
dim drags it into the fold via `handleTarget` while `promote` skips `d.dims` edges —
producing exactly the orphaned dim-twin the same revision diagnoses for `Linked`
(":2261-2263 — `*handleTarget` names the dim-twin a session-minted dim leaves orphaned off
the folded `d.dims` rank, dead weight beside the re-minted canonical cell"). `Link` binds a
`DimRef` identically (`vortex_vm.cpp:620`,`:729`), so the doc's own "binds a `DimRef`, never
a promoted position" reasoning applies verbatim — yet no `+d.name`/`ensureDimension` arm is
specified for the tuple field. The failure needs session-built fragment + checkpoint +
post-resume patch replay: narrow, and `closed`'s frontier doesn't enumerate tuple fields
(dims classify state-side anyway, so no refusal). A coherent reading exists — the `dim`
field naming the op's `dimConstant` operand slot (map-translated, its content resolved by
the dim arm) — but the text doesn't state it. **Fix:** one sentence — either the tuple `dim`
field carries `+d.name` → `ensureDimension` like the ref-capture dim arm, or it names the
operand slot rather than the dim cell. Held to MINOR for the coherent-reading escape; the
mechanism lanes may elevate — it is the R15-B successor hole for dim endpoints.

**R16-3 MINOR — residual "program-side" framing of the dim arm.** :2243 calls `+d.name` "the
program-side mark" — the same revision rules "the arm keyed on the record's `+d.name`, never
sideness" (:2263, :3718) and re-keys the arm for image-folded *or* session-minted dims
(:1180); image-folded dims are not program-side under the sideness test. Same stale
qualifier survives in the 1.12 entry ("`+d.name` keeps the program-side arm", :3578), the
1.13 entry ("program-side endpoints ride the `+d.name →` dim-name arm", :3621-3622) and the
1.15 entry's own "program-side writeback" (:3716) — the 1.11 entry's identical qualifier was
retro-edited (:3541), these three were not. **Fix:** "the dim-referent mark (`+d.name`)" at
:2243; annotate or harmonize the three changelog echoes under the established convention.

**R16-4 MINOR — unannotated `+d.values`-carrier narrations in three older entries.** The
1.9 entry's "each *recipe* record's `+d.values` naming the referent fold cell at the
promoted position" (:3449-3451), the 1.10 entry's "the endpoint itself rides the *recipe*
record's `+d.values`" (:3500-3501), and the 1.11 entry's "`+d.values → the referent
endpoint` for internal/state-side referents" (:3540) all narrate the abolished carrier as
load-bearing — the last inside a sentence this revision already touched for the dim
qualifier. Under v1.15 the endpoint rides slot `int64` content and `+d.values` is "decorative,
never load-bearing" (:1153). The doc's convention retro-edits changelog claims that became
false and confesses in the new entry; these three went unmarked. **Fix:** annotate each with
its supersession (v1.13's content-carried endpoint; v1.15's decorative `+d.values`), or
correct and confess alongside the other retro-edits.

**Carried nit bundle (unadopted r15 residue, all still present, all still nits):**
"replayed grafts" verb vocabulary (:2222); "`learn_where`/`learn_body` install nothing"
(:1798 — installs no *clause*); the 1.14 entry's "displacing where one does" compresses "a
same-named *declared* clause" (:3688 — acceptable summary, worth "declared" if touched).

VERDICT: NOT CONVERGED — all three r15 minors verified landed and the coherence surface is
clean except one operational contradiction in v1.15's newest text: the declared dual
marking inventory (`+d.refs` + `+d.recipe`) versus the `+d.refs`-bounded re-kind walk at
:2084-2086, which leaves every recipe field cell `Int64`-kind through `promote` and
re-opens R15-B's verbatim-fold failure for the class the fix added — the implementer cannot
build both statements as written. Residue: three MINORs (the tuple `dim` field's missing
name arm, the residual "program-side" dim-arm framing, the unannotated `+d.values`-carrier
changelog narrations) plus the carried nit bundle.
