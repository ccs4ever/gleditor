# R16 — ZigZag topology/promotion critique (v1.15)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.15 deltas in my lane: the R15-B
marked-int64 `+d.recipe` keyspace fix (:1144-1155 plus its inventory/marketing echoes at :379-383,
:494-497, :1055-1071, :1078-1081, :2051-2055, P16 :2907), the R15-A unreachability restatement
(:2157-2171 plus the two changelog echoes :3642-3650, :3660-3667), and the map-membership
internal/external discriminator (:1170-1175, :2243-2245).

Verified against `arena_manifold.cpp:1934-1936` (`Int64` folds verbatim — the dead-id arm),
`:1947-1951` (`OpHandle` → `real.at(target)` — the translated arm), `:2020-2052` (edge retarget
through `real`), `:1764-1768` (the `d.dims` frontier skip — session-minted dims fold only via
`discover(edge.dim, false)`, never as rank members), `:1009-1012` (`cloneMaster` = `rankEnd`
NEG-ward on `d.clone`), `vortex_vm.cpp:816-825` (`Clone` walks `d.clone` posward and appends at
the tail), `vortex_core.cpp:344-346` (`masterOf` = `cloneMaster().value_or(cell)`), `:366`+
`:373`+`:376` (`wingOf`/`inputsOf` entry — the `linked(opcode, dims_.grab, side)` read is at
`:366`, `inputsOf`'s POS delegation at `:373`), `:384-390` (`bindInput`'s `opcode +d.grab → slot`),
`foreign_op_handle.cpp:98-117` (`isCellRelocationHandle` requires `OpHandle` kind *and* the
`d.mesh.relocation`→`"cell"` marker edge — kind first), `dim_vector.hpp:22` (`DimRef = CellRef`,
so a marked `dim` field's position is a lawful `Link` operand), `compiler.cpp:769-772`
(`dimConstant` = `int64(findOrMintDimension)`), `ensureDimension`'s name arm
(`arena_manifold.cpp:78-105`: `dimensionNamed` → `arenaDims_` → mint+`d.dims` append).

## Verdicts on the r15 dispositions

### R15-B — uniform marked-int64 `+d.recipe` keyspace (:1144-1155): **partially resolved — the marking and inventory exist, but no ceremony pass walks them**

What v1.15 got right:

- The field cell *is* marked under P16 — every `+d.recipe` field naming a source-cell position
  gets the per-ref-cell `d.mesh.relocation`→`"cell"` marker (:1068-1071 diagram, :379-383,
  :494-497, :2051-2054, P16 :2907). The `+d.values` edge is demoted to decorative
  self-description under the one-record-one-edge rule, "never load-bearing" (:1151-1155) — the
  mixed edge/`int64` carrier is abolished.
- The keyspace logic is coherent *once the field is OpHandle-kind*: it reads the live ephemeral
  position in-session (`asInt64`) and `*handleTarget` = the endpoint's promoted position at (d)
  (:1148-1149) — the same space the marked ref-box slots read (:2276-2278), so a shared endpoint
  re-joins on the same key at replay and the old→new map ("covers every copied graph cell",
  :1171-1172) translates it (:1189-1190). The kind-aware read protocol is stated.
- The map-membership discriminator (:1170-1175) is consistent: map keys are source-graph
  positions, external referents' positions are by construction absent (distinct live cells hold
  distinct dense ids — no false membership), and the `+d.name`-bearing dim record is keyed out
  ahead of the test (:2245-2247), so membership discriminates only the two arms it should.
  The r15 vortex-minor — "the conditional `+d.values` misclassifies internal-with-claimed" —
  is fixed: `+d.values` no longer classifies at all, it merely annotates internal endpoints
  (:2244-2245).

What does not close — **Finding 1** below. The marking rule and the inventory claim were
extended; the three passes that *consume* the inventory were not.

### R15-A — agent-legs by unreachability (:2157-2171): **resolved — accurate**

"The only edge a §5.3 capture creates is the binding cell's own `+d.clone →` the fragment's
`cap$N` slot (`Clone` appends it at the master's posward tail, `vortex_vm.cpp:816-825`) —
image-sealed for a declared fragment, never runtime-grown, and running state→program on a
`d.clone` dim the walk's `+d` vocabulary does not list besides — while the slot's `masterOf`
resolution is the operand decode §5.3 states (`cloneMaster` = `rankEnd` NEG-ward,
`arena_manifold.cpp:1009-1012`)" — every clause verifies. `Clone` walks
`linked(cloneTail, dims().clone, false)` to the posward tail and links `cloneCell` there
(`vortex_vm.cpp:816-829`); the binding cell is the rank's NEG end, so `masterOf` decodes
slot→binding by `rankEnd` NEG-ward (`arena_manifold.cpp:1009-1012`,
`vortex_core.cpp:344-346`); `d.clone` is absent from the `+d` walk vocabulary; and the
two-root split makes the leg unreached rather than classified-by-ingress. The conclusion —
legs classify `>E_ops`/state-side by unreachability, no agent-rooted second walk owed — is now
argued from reachability, not from the abolished edge class. Both changelog echoes were
retro-edited to match and the retro-edits confessed (:3697-3706, :3730-3734). One residual
wording nit: Finding 2.

### Minors — all verified applied

- (e) program-side writeback names `Int64` of the `DimRef` `ensureDimension`'s name arm
  re-resolves, `*handleTarget` naming the orphaned dim-twin a session-minted dim leaves off the
  folded `d.dims` rank (:2259-2263) — the cite `arena_manifold.cpp:1764-1768` is the `d.dims`
  frontier skip, which is exactly why the fold cell is orphaned. Arm keyed on `+d.name`, never
  sideness. Accurate.
- (d)'s "never the endpoint value" correctly excepts the `+d.name`-bearing dim record whose
  name *is* the endpoint source (:2245-2247); the `+d.ref`/`+d.refFor` pairing is named at
  :1907-1908 and :2274-2275.
- `closed` arm (c) reads "a dim referent" with the re-resolution phrasing (:1876-1879);
  glossary and 1.11/1.14 changelog entries updated to match.
- `cap$N` slots correctly excluded from position-carriage: "its `cap$N` slots re-clone the same
  definition-site cells" beside ref-slot positions (:1191-1193) — `asInt64` fails on them,
  `masterOf` decodes (`vortex_core.cpp:357-365`).
- `set_weight`/`weight_of` `+d.name`-less refusal stated (:1125, :2505-2508); alias scoped to
  *user-alias* (:1776); sketch serial pinned to the agent cell's `d.vars` (:2706); `wingOf`
  cite moved to `:373` (`inputsOf`'s POS delegation — the `linked()` read itself is `:366`,
  close enough to the ruling's pick).

## New findings (v1.15 deltas only)

### 1. MAJOR — the re-kind/restore/(e) passes still enumerate `+d.refs` only: marked `+d.recipe` field cells are never re-kinded, fold verbatim, and the keyspace splits again one level down

v1.15 step 1 (:2051-2054) now marks the recipe field cells and declares "the `+d.recipe` field
record standing as their inventory beside `+d.refs`"; :1078-1081 says "the records are the
inventory the checkpoint ceremony walks". But the ceremony's actual enumeration was not
re-scoped:

- **Step 3, the re-kind pass** (:2083-2086): "walks each registered head's `+d.refs`
  ref-records and `setValueBits(OpHandle, target)`s every marked ref cell … the walk is
  `+d.refs`-bounded." Explicitly bounded to `+d.refs` — the `+d.recipe` field records are never
  visited.
- **Step 5, the restore pass** (:2099-2103): "on every marked cell still in `OpHandle` kind" —
  enumerates marked cells through the same inventory; unstated scope, and moot while step 3
  misses them.
- **Genesis (e)** (:2254-2255): literally named "`+d.refs` rehydration" and sweeps "per
  ref-record `Int64(*handleTarget)`" — recipe fields are not ref-records; the P16 row echoes
  "the post-`d.patch`-replay `+d.refs` sweep" (:2907).

The consequence chain is the R15-B defect relocated: a marked-but-never-re-kinded field cell is
`Int64`-kind at `publishValue` — `isCellRelocationHandle` requires `OpHandle` kind *and* the
marker (`foreign_op_handle.cpp:98-117`), so the marker alone changes nothing — and `promote`
folds `Int64` **verbatim** (`arena_manifold.cpp:1934-1936`). The image's recipe fields therefore
carry dead ephemeral ids. At (d): replay builds the old→new map keyed on those dead ids, while
every ref-capture slot reads `*handleTarget` = a *promoted* position — the two keyspaces split
again, exactly as before. An internal referent's slot position is then absent from the map, is
misclassified external under §6.1's own membership discriminator, and is carried verbatim —
the copy's slot aliases the *source's* fold twin across the copy boundary. In-session the two
spaces coincide (emission position == live position), so the failure is again invisible at
`learn`-time and bites only on the (d) path the fix exists for. The 1.15 changelog's claim —
"the single position keyspace … so shared endpoints re-join at replay" — holds only if the
fields actually reach `OpHandle` kind, which no stated pass accomplishes. `promote`'s gate does
not catch it: the cell is a plain `Int64`, not an unmarked `OpHandle`, so the fold is silent.

**Fix** — re-scope all three enumerations to the unified inventory and name it once: step 3
becomes "walks each registered head's **marked-cell inventory — the `+d.refs` ref-records and
the `+d.recipe` field records** — and `setValueBits(OpHandle, target)`s every marked ref cell
… the walk is inventory-bounded"; step 5 and (e) read "every marked cell **on either
inventory**"; rename (e) "`+d.refs` rehydration" → "marked-cell rehydration (`+d.refs` plus
`+d.recipe`)"; mirror both in the §4.5 summary (:384-389) and the P16 row's "`+d.refs` sweep"
(:2907). The walk is mechanical — `+d.recipe` is a `+d.step`-chained rank off the same head —
and cheap, O(refs + fields) as before.

### 2. MINOR — "`masterOf` … content-side like the ref-capture referent's `int64`, never a traversal" mischaracterizes the decode: `cloneMaster` *is* a link traversal, on a dim outside the vocabulary

`:2166-2168`: "while the slot's `masterOf` resolution is the operand decode §5.3 states
(`cloneMaster` = `rankEnd` NEG-ward, `arena_manifold.cpp:1009-1012`), content-side like the
ref-capture referent's `int64`, never a traversal". `rankEnd` NEG-ward walks `-d.clone` *link*
structure — it is a traversal by any honest reading; what it is not is a traversal the
sideness walk performs, because `d.clone` is outside the `+d` vocabulary. The ref-capture
`int64` genuinely is content — no edge exists to follow — so grouping the two under
"content-side" erases the distinction the very next clause relies on ("`closed`'s frontier …
does read the `cap$N` slot links and decode them through `masterOf`", :2169-2170; likewise
:1870, :1898-1899): the frontier follows `masterOf` precisely because it *is* edge structure
worth decoding, while slot `int64` is content it merely reads. Fix: "— while the slot's
`masterOf` resolution is the operand decode §5.3 states (`cloneMaster` = `rankEnd` NEG-ward,
`arena_manifold.cpp:1009-1012`) — a `-d.clone` backlink walk on a dim outside the walk's `+d`
vocabulary, never a step this walk can take".

### 3. MINOR — the `+d.recipe` record↔field inventory relation is never named: "the field cell's own `+d.recipe` record" implies a pairing edge no text supplies

For `+d.refs` the inventory edge is explicit — the record carries `+d.ref → the slot cell`
(:1046-1048). For the recipe fields, v1.15 says "its own `+d.recipe` record the inventory
entry" (:1147-1148), "the field cell's own `+d.recipe` record" (:382-383, :1079-1080), and
"a `+d.recipe` field record's own source-naming cell" (P16, :2907) — all implying record and
field are *distinct* cells — without naming the edge that leads record → field. The only
compatible reading left is that the marked int64 field cell *is* the `+d.recipe` chain member
("per-field records", :1055-1056; "one record per field", :1066) — a chain member whose content
is the position and whose optional `+d.values` names the endpoint — which works (the walk then
enumerates rank members directly) but contradicts the "record's own … cell" phrasings and
leaves `+d.values` doubly claimed (the member's own edge vs. a parent's). Fix — pick one and
say it: either "the marked field cell is itself the `+d.recipe` record — its chain membership
is the inventory entry" (cheapest — mirrors "the slot *being* the endpoint record" for external
referents, :1178), or name a dedicated pairing dim on the record pointing at the field cell
(`+d.ref` is `+d.refs`' own; `+d.values` is the decorative endpoint edge — neither is free).

### 4. MINOR — "its `cap$N` slots re-clone the same definition-site cells" asserts a replay operation outside the stated `New`/`Link`/`Value` palette

`:1191-1193` newly promises the copy's `cap$N` slots "re-clone the same definition-site cells".
`reify`'s stated palette is `New`/`Link`/`Value` (:1189-1190, echoed :2286), and `Clone` is
expressly refused as reify's copy primitive (:1201-1204). A `cap$N` slot *must* alias its
binding — that is the one place clone semantics is wanted — but the replay must produce it
somehow: either a `Clone` op (legitimate here, since the refusal covers the *graph* copy, not
the capture slots — but then the palette sentence under-states it) or a composed
`New` + `Link(rankTail(binding, d.clone), d.clone, POS, slot)`, which yields the identical
`-d.clone` membership `cloneMaster` decodes (`vortex_vm.cpp:816-825`'s append is exactly that
shape). As written, an implementer cannot tell which. Fix: "…its `cap$N` slots re-clone the
same definition-site cells — minted `New` and appended at the binding's `d.clone` rank tail by
`Link` (the `Clone` op's own append shape; the §11 refusal covers `Clone` as the *graph* copy
primitive, not the capture-slot mint where aliasing is the semantics)".

## Verified accurate (v1.15)

`promote`'s three arms and the verbatim-`Int64` fold that makes Finding 1 load-bearing
(`arena_manifold.cpp:1934-1936,1947-1951,2020-2052`). `isCellRelocationHandle`'s kind+marker
conjunction (`foreign_op_handle.cpp:98-117`) — marker alone is inert. `Clone`'s posward-tail
append and `cloneMaster`'s NEG-ward `rankEnd` (`vortex_vm.cpp:816-825`,
`arena_manifold.cpp:1009-1012`). The `d.dims` frontier skip behind the orphaned-dim-twin
reasoning (`arena_manifold.cpp:1764-1768`). `DimRef = CellRef` (`dim_vector.hpp:22`) — the
marked `dim` tuple field is a lawful `Link` operand for image dims. `ensureDimension`'s
name-first resolution (`arena_manifold.cpp:78-105`). `wingOf`/`bindInput` entry and eviction
exposure (`vortex_core.cpp:366,373,384-390`). The two-root adapter split; map-membership as a
sound discriminator under unique dense ids.

VERDICT: NOT CONVERGED
