# R17 — ZigZag topology/promotion critique (v1.16)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.16 deltas in my lane: the R16-A
one-`+d.refs`-inventory fix (:1055-1066, :1164-1178, :2092-2105, :2132-2135, :2148-2150,
:2313-2332, plus echoes :382-390, :497-500, :1937-1938, P16 :2992), the R16-B `dim`-endpoint
name-keying and `d.dims` filing (:1072-1074, :1174-1178, :1214-1218, :2238-2243, :2926-2938,
echoes :92, :110-113, :140, :541-547, :1572-1575, :3178-3182), the `cap$N` replay palette
restatement (:1225-1230), the `masterOf`/`cloneMaster` traversal correction (:2213-2221, :836),
and the chained-capture edge-direction scoping (:2214-2215).

Verified against `vortex_vm.cpp:816-825` (`Clone` walks `linked(cloneTail, dims().clone, false)`
posward and `link(cloneTail, dims().clone, false, cloneCell)`s at the tail — the `New`+`Link`
replay shape the doc now names is exactly this append), `arena_manifold.cpp:1009-1012`
(`cloneMaster` = `rankEnd` NEG-ward — a `-d.clone` backlink traversal, as now stated),
`:344-346`+`:357-365` (`masterOf` = `cloneMaster().value_or(cell)`; `wingOf`'s operand lambda —
`asInt64` fails → `masterOf(slot)` at `:364`, new cite `:358-365` correct), `:366`/`373`
(`wingOf`'s `linked(opcode, dims_.grab, side)` at `:366`, `inputsOf`'s POS delegation at `:373`
— the v1.16 cite correction lands on the right line this time), `arena_manifold.cpp:1766`
(`textOf(edge.dim)=="d.dims"` skip inside `dimensionsOf`), `:1778` (`discover(edge.dim, false)`
— session-minted dims used on emitted edges DO land in `real`, so `real.at(dimCell)` in the
OpHandle arm at `:1947-1951` succeeds and the "promoted twin" the name-keyed arm bypasses
really is folded), `:2030-2033` (the `real.find(edge.dim)`/`real.find(edge.pos)` link-fold
gate), `arena_manifold.cpp:56-105` (`dimensionNamed` consults `base_->dimensionNamed` first,
then `arenaDims_`; `ensureDimension`'s mint arm appends to `d.dims` off `homeCell`),
`manifold.cpp:782-822` (base `dimensionNamed`: `DimensionRegistry::get` fast path +
`dimensions()` walk + `d.alias`), `dimension_registry.hpp:113-116` /
`dimension_registry.cpp:195-216` (`getOrCreate(Store&, Manifold&, name)` → `store.makeDimension`
at `:205` + `registerDim` — the doc's call chain is real), `vql/compiler.cpp:1128-1140` (the
"replicate dimensions" precedent, verbatim `getOrCreate(store, ver, manifold, name)` per
`"d."`-texted arena cell), `vortex_core.cpp:384-390` (`bindInput`'s `opcode +d.grab → slot`).

## Verdicts on the r16 dispositions

### R16-A — one `+d.refs` marking inventory: **resolved — the walk is complete**

Every clause of the ruling is in the text and the ceremony is closed:

- **One rank.** "The marking inventory is **one rank**: every marked cell, slot or `+d.recipe`
  field, carries its `+d.ref` record on the head's `+d.refs` rank" (:2095-2097); the head diagram
  says "`+d.ref→`the cell genesis must rewrite — slot or `+d.recipe` field alike, this the ONE
  marking inventory" (:1055-1057); §4.5 (:384-389), the fragment-emission note (:497-500), and
  the P16 row (:2992) all echo "the ceremony's **one** inventory rank".
- **`+d.recipe` demoted to schema.** "The `+d.recipe` member *is* the field cell, the member
  record carrying the field's schema (name → tuple position), never a second marking rank"
  (:2097-2098, :1168-1171, :1069-1071).
- **Pairing named.** Member-identity (the field cell IS its `+d.recipe` member) plus the
  `+d.ref`-analog: "its `+d.ref` record on `+d.refs` the inventory pairing every marked cell
  gets" (:1070-1071, :2097-2098) — the same `+d.ref` record shape slots already had.
- **Complete walk.** Step 3 now reads "walks each registered head's `+d.refs` ref-records — the
  one rank enumerating every marked cell, slot and `+d.recipe` field alike" (:2131-2133): for
  each `+d.name`-less record, `+d.ref` leads to the marked cell — marked cells and ref-records
  are bijective by step 1's rule, so every marked cell is reachable. Restore (:2148-2150:
  "every marked cell reachable, since the `+d.refs` inventory enumerates slots and `+d.recipe`
  fields alike") and genesis (e) (:2313-2317: "the `+d.refs` ref-records the one inventory
  covering `+d.recipe` field cells beside the slots under the one-inventory rule") are scoped
  the same, and (e)'s field writeback is explicit — "the endpoint riding the slot's — or the
  `+d.recipe` field's — content" (:2319-2320), the dim arm writing "into the record's
  `+d.refFor`-paired slot or its own field cell respectively" (:2327). The R15-B keyspace-split
  defect is dead: marked recipe fields now reach `OpHandle` kind, promote translates them
  through `real` (:1947-1951), and (e) rehydrates them in the same position keyspace.

No residual gap: `Linked`-born ref boxes stay the declared carve-out (:2106-2107); marked
"record subjects" are covered by the same uniform rule (:2094-2095).

### R16-B — `dim` endpoint name-keyed + `d.dims` filing: **resolved**

- The `dim` endpoint field "carries the dim *name* — `+d.name` on its recipe record — and
  replay resolves `ensureDimension(name)` exactly as the `dimConstant` arm" (:1174-1177,
  :1072-1074, :1214-1218); its field cell stays marked like every field, the dim name living on
  the `+d.recipe` member, not the `+d.refs` record (:2092-2094) — so the ref-record/name-record
  discriminator is undisturbed.
- (e)'s arm correctly exempts "a marked cell whose `+d.recipe` side bears `+d.name` — the
  `dim`-field member itself, or the slot's `+d.refFor`-paired ref-capture record" and writes
  `Int64` of the `ensureDimension`-resolved `DimRef` "into the record's `+d.refFor`-paired slot
  or its own field cell respectively" (:2321-2328).
- The spawn-seed step FILES the minted-dim set on the persistent `d.dims` rank —
  `DimensionRegistry::getOrCreate` → `Store::makeDimension` per name on the kept lineage Store,
  the `vql/compiler.cpp:1128-1141` precedent (:2926-2938); genesis (b)'s find "landing on the
  canonical cell rather than re-minting an orphan twin" (:2238-2243) is mechanized:
  `dimensionNamed` consults `base_->dimensionNamed` first (:59-70), which hits the registry
  fast path or the `dimensions()` walk where `makeDimension`+`registerDim` landed the name.
- The orphan analysis is accurate: `discover(edge.dim, false)` (:1778) folds in-use session
  dims into `real`, so `real.at(dimCell)` at `:1949-1951` succeeds and "a session-minted dim in
  use accreting one such orphan per checkpoint off the folded `d.dims` rank" (:2328-2330) is
  the honest bound.

### Minors — all verified applied

- `masterOf`/`cloneMaster`: "a `-d.clone` *backlink* traversal on a dim outside the walk's `+d`
  vocabulary, never a `+d` step this walk can take" (:2218-2221) — accurate against
  `rankEnd(NEG)` (:1010-1012); the `wingOf` echo carries the same restatement and the corrected
  `:358-365` cite (:836).
- `cap$N` palette: "minted `New` and appended at the binding's `d.clone` rank tail by `Link`,
  the `Clone` op's own append shape (`vortex_vm.cpp:816-825`; the §11 refusal covers `Clone` as
  the *graph* copy primitive, not the capture-slot mint where the alias is the semantics)"
  (:1226-1230) — matches the code exactly, and `masterOf`'s flat NEG-ward decode keeps
  tail-appends semantically identical to `Clone`.
- Chained captures scoped: "the binding cell's own `+d.clone →` the fragment's `cap$N` slot on
  a *first* capture, the prior clone's on a chained one" (:2213-2215) — `Clone`'s
  `link(cloneTail, …, cloneCell)` links predecessor→successor, so clone→clone is right.
- `wingOf` cite `:373`→`:366` — `linked(opcode, dims_.grab, side)` is at `:366`; `:373` is
  `inputsOf`'s POS delegation. Correct.
- "dim mark" wording sweep (:2301, glossary :140, changelog retro-edits) applied.

## New findings (v1.16 deltas only)

### 1. MAJOR — the `+d.recipe` member's field-schema carriage "(name → tuple position)" is unmechanized, and the obvious carrier is foreclosed: `+d.name` on the `dim`-field member already carries the dim name under single occupancy

Member-identity collapsed the schema record into the marked field cell — "the member record
carrying the field's schema (name → tuple position)" (:387, :1070, :1168-1169, :2097-2098) —
but no edge or positional rule ever says *how* the member says which `(from, dim, dir, to)`
field it is. The two live readings both fail or are unstated:

- **Schema name rides `+d.name`** (the doc's universal name edge — `+d.name` carries qualified
  names, labels, dim names everywhere): impossible uniformly, because the `dim`-field member's
  single `+d.name` edge already carries *the dim name* (:1072, :1214, :2093-2094) — a cell has
  one `+d.name` neighbour, so "dim" and "d.values" cannot coexist. And if non-dim members
  carried `+d.name→"from"` etc., genesis (e)'s discriminator — "a marked cell whose `+d.recipe`
  side bears `+d.name`" (:2322) — would fire on **every** field member, writing
  `Int64(ensureDimension("from"))` into the `from`/`dir`/`to` endpoint fields (silently minting
  dims named `from`/`to`/`dir` and corrupting every recipe at genesis), while `closed`'s
  name-resolved test (:1947-1949) would over-count every tuple field as name-resolved.
- **Schema is positional** (chain adjacency in `(from,dim,dir,to)` order, or a role edge):
  works, and is presumably intended — but it is never stated, and the recipe chain is
  heterogeneous (link-tuple fields interleave with literals, capture records, ref-capture
  records on the same `+d.step` chain, :1067-1084), so even a positional reading needs the
  grouping rule stated.

The defect is new precisely because member-identity is new: v1.15's separate "record cell"
could in principle hold the schema on its own edges; the collapse forces the schema onto the
marked field cell where `+d.name` is already claimed on exactly the member that matters.

**Fix** — name the carrier and fence the discriminator: e.g. "a link tuple's fields are four
`+d.step`-consecutive members in `(from, dim, dir, to)` order — the member needs no name edge
for its field identity; `+d.name` on a `+d.recipe` member exists **only** on the `dim`-field
member, so (e)'s and `closed`'s `+d.name`-keyed tests select exactly it" — or a dedicated role
edge (`+d.grab→` field name is *not* free: `-d.grab` reasoning at :1188-1191 rules wing dims
out for slot cells, and a fresh dim would need minting). Whichever is chosen, state "non-`dim`
members bear no `+d.name`".

### 2. MINOR — arm (c) of the `closed` enumeration still locates dim-referent `+d.name` only on "its paired *ref-capture* record" — omits the `dim`-field member the glossary and the re-verification sentence now include

`:1915-1919`: "(c) a dim referent name-recorded on its paired *ref-capture* record's `+d.name`"
— the pairing is `+d.refFor` to a *slot*, which the `dim`-field member does not have (it is its
own member). Thirty lines later the re-verification does include it: "a `+d.name`-bearing
`+d.recipe` record (the `+d.refFor`-paired ref-capture record for a slot, the `dim`-field
member record for a tuple dim endpoint, §6.1)" (:1947-1949), and the glossary row includes it
(:140). The (a)/(b)/(c) enumeration is the normative list the others gloss — fix: "(c) a dim
referent name-recorded on its paired *ref-capture* record's `+d.name` — or, for a `dim` tuple
field, on the field's own `+d.recipe` member record".

### 3. MINOR — P16 row's unqualified "`+d.name` on a record makes it a name-record — no marker, never re-kinded" now contradicts the `dim`-field member, which is a `+d.name`-bearing record yet IS marked and re-kinded

The row (:2992) states the blanket rule in the same breath as "a `+d.recipe` member field cell
itself source-naming" being marked — but a `dim`-field member is literally a `+d.name`-bearing
record that the step-1 rule marks anyway (:2092-2094). The `+d.name` discriminator applies to
`+d.refs` records only; the recipe-side `+d.name` is the *endpoint channel*, not the
discriminator. Scope the row: "`+d.name` on a `+d.refs` record makes it a name-record".

### 4. MINOR — "ref-capture records — one per `+d.refs` ref-record: `+d.refFor`→the same slot cell its `+d.ref` names" presumes every ref-record names a *slot* — stale under the unified inventory

`:1076-1077` and its (e) echo (:2321-2322, "the ref-capture record paired to it by `+d.refFor` →
the same slot cell") say one ref-capture record per `+d.refs` ref-record. Under one inventory a
ref-record's `+d.ref` may name a `+d.recipe` field — which is no slot, takes no `+d.refFor`
pairing (the dim name rides the member itself), and pairing one would contend its `-d.refFor`
and blur (e)'s "slot's `+d.refFor`-paired ref-capture record" test. Fix: "one per *slot*
ref-record" and note recipe-field ref-records pair with nothing.

### 5. MINOR — the copy's regenerated `+d.name`-bearing recipe members never say the dim-name cell is re-minted; `-d.name` single occupancy makes re-linking the source's name cell an eviction

`reify` "regenerates the copy's `+d.refs` (… name-records re-linked) and `+d.recipe` (a
self-describing recipe for the copy, endpoints remapped)" (:1231-1234). The `dim`-field
member's `+d.name`→dim-name cell (new this round) — and likewise the `+d.refs` name-records'
name cells — cannot be re-linked to the source's name cells: the name cell's `-d.name` backlink
is single-occupancy, so the copy's edge would evict the *donor's* `+d.name` and drop the
registered head's dim-name arm. The `patch:<n>` verbatim-re-mint rule (:2285-2288) is the
in-document precedent. Fix: "…name cells re-minted verbatim per copy (the `patch:<n>` label
rule) — never re-linked off the donor's occupied `-d.name`".

## Verified accurate (v1.16)

`Clone`'s posward-tail append shape vs the `New`+`Link` replay recipe (`vortex_vm.cpp:816-825`);
`cloneMaster` = `rankEnd` NEG (`arena_manifold.cpp:1009-1012`) — the "backlink traversal"
wording is now honest; `masterOf`/`wingOf` operand decode and the `:366`/`:373` cites
(`vortex_core.cpp:344-346,357-365,366,373`); `promote`'s `d.dims` skip + `discover(edge.dim,
false)` + `real.find` gate (`arena_manifold.cpp:1766,1778,1947-1951,2030-2033`) — session-minted
dims in edge use do fold into `real`, so the OpHandle arm cannot `real.at`-throw on them;
`dimensionNamed`'s base-first resolution (`arena_manifold.cpp:56-75`,
`manifold.cpp:782-822`) making filed dims findable at genesis; `getOrCreate`→`makeDimension`→
`registerDim` (`dimension_registry.cpp:195-216`) and the `vql/compiler.cpp:1128-1140`
replicate-dims precedent the filing step rides; `bindInput`'s wing edge
(`vortex_core.cpp:384-390`).

VERDICT: NOT CONVERGED
