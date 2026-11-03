# R18 — ZigZag topology/promotion critique (v1.17, convergence-freeze round)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Round scope: verify the R17-A fix landed correctly plus my r17
minors, then hunt a major **only** in pre-existing mechanisms (the positional carrier vs
`link`-eviction ordering; the whole-vocabulary dim enumeration).

## R17-A — field schema rides positional `+d.step` members: **landed correctly**

All four clauses of the ruling are in the text, uniformly:

- **Positional carrier named.** "the member's own `+d.step` position in the chain carrying the
  field identity — a link tuple's fields four `+d.step`-consecutive members in `(from, dim, dir,
  to)` order" (:1186-1189); echoed in the head layout (:1078-1081: "four `+d.step`-consecutive
  members in (from, dim, dir, to) order, the member's chain position its field identity"), §4.5
  (:392-394) and P16 step 1 (:2131-2133). The order matches the wire signature verbatim —
  `link(CellRef from, DimRef dim, DimVector dir, CellRef to)` (`arena_manifold.hpp:604`,
  `manifold.hpp:356`) — so position → field is the real argument order, not an invented mnemonic.
- **`+d.name` reserved to the dim-name channel.** "`+d.name` on a `+d.recipe` member is that
  dim-name channel alone, so every other member's field identity needs no name edge" (:1195-1197);
  "+d.name on a +d.recipe member reserved to that dim-name channel" (:1085-1088); "`+d.name` on a
  member reserved to the `dim` field's dim-name channel" (:2132-2133); the post-diagram gloss
  (:1119-1121) and the P16 row (:3070: "`+d.name` on a `+d.recipe` member is the `dim` field's
  dim-name channel alone, the member still marked") agree. The double-booking is dead: no
  non-`dim` member carries a schema name on `+d.name`, so (e)'s `+d.name`-keyed arm (:2371-2375)
  and `closed`'s arm (c) select exactly the dim channel — the `Int64(ensureDimension("from"))`
  corruption scenario from r17 cannot arise.
- **Name-record rule scoped to `+d.refs`.** "**`+d.name` present is the discriminator**: a
  `+d.refs` record with `+d.name` is a *name-record*" (:2030-2031); the head layout's `⇒
  NAME-record`/`⇒ REF-record` arms sit inside the `+d.refs` chain block (:1066-1077); step 1 reads
  "whose `+d.refs` record carries no `+d.name`" (:2115-2116); the post-diagram sentence makes it
  explicit — "`+d.name`'s presence **on a `+d.refs` record** is the discriminator" (:1119); the
  P16 row is corrected: "`+d.name` on a `+d.refs` record makes it a name-record" (:3070). The
  marked `dim`-field member no longer trips the blanket rule.

## r17 minors — all landed

- **`closed` arm (c) `dim`-member form** (:1945-1949): "(c) a dim referent name-recorded on its
  paired *ref-capture* record's `+d.name` — **or, for a `dim` tuple field, on the field's own
  `+d.recipe` member's `+d.name`**" — and the REF-record rider now reads "the slot's — or the
  member's — own `+d.refs` record" (:1949). Glossary row agrees (:146).
- **`dim`-member name-cell re-mint** (:1257-1261): "name-records re-linked to **fresh verbatim
  name cells** — every `+d.name` name cell re-minted per copy under the `patch:<n>` label rule …
  never re-linked off the donor's name cell: the name cell's `-d.name` backlink is
  single-occupancy … — **the `dim`-field member's dim-name cell re-minted the same way**". Landed,
  and generalized to *every* `+d.name` name cell — strictly stronger than the ask.
- **Ref-capture scoping to slot ref-records** (my r17 #4): "one per `+d.refs` *slot* ref-record (a
  `+d.recipe` field's ref-record pairs with nothing, the member its own endpoint record — no
  `+d.refFor`)" at :1090-1093, :1206-1208, and (e) :2367-2369. Dead ambiguity removed.
- "(§6.1's uniform position keyspace)" stale phrase dropped at :2136; (e) writes "the member cell
  itself" (:2374-2375); (d) mirrors the `+d.name` exception for the `dim`-field member —
  `Int64(DimRef)` into "the member *being* its field cell" (:2349-2350); `DimRef` "counting" under
  the uniform rule (:2119-2121); chained-capture direction scoped (:2253-2254).

## Carrier-vs-eviction check (the suggested hunt, part 1): **survives**

The positional carrier is not threatened by `link`-eviction (`arena_manifold.cpp:1347-1377`, M14):
members are fresh mints appended tail-ward on `+d.step`, so construction evicts nothing; replay
re-enacts the recorded tuple sequence on fresh copy cells, so a source graph that could only ever
have held one `+d`-neighbour per dim replays to exactly its surviving edge set (two recorded
`A +d.x →` tuples re-enact the same eviction the emission performed). Reify's regenerated recipe
re-runs the same emission order, so copy positions mirror donor positions. The unmarked `dir`
member is outside the `+d.refs` inventory, so re-kind/restore/(e) all skip it — its literal
survives verbatim. No defect.

## New finding

### 1. MAJOR — the filed-set's stated enumeration source cannot produce the claimed vocabulary: the code keeps *two disjoint* dim-name tables, and no accessor/walk named yields their union (pre-existing mechanism, surfaced by the R17-B wording)

The text stakes the enumeration on a rank walk three times: "the whole minted-dim vocabulary —
**the `d.dims`-rank member set `dimensions()` walks (`manifold.cpp:771-775`), every mint path
appending there**" (:113-115, :2277-2280, :2984-2991, :3991-3993). Against the code:

- **`ArenaManifold::dimensions()` does not walk the arena's `d.dims` rank.** It returns
  `base_->dimensions()` ∪ `arenaDims_` (`arena_manifold.cpp:34-46`). The V10 names
  (`mintNamedDimension`, `vortex_core.cpp:16-27`) and `findOrMintDimension`/`mintDimension` mints
  (`:72-98`) are `makeCell` + `link(lastDim, dims_.dims, POS, cell)` — they append to VortexCore's
  **own** rank off its **own** `home_` cell and never touch `arenaDims_` — so in a base-less emit
  arena `dimensions()` misses the *entire* `d.grab`/`d.step`/`d.spin`/`d.vars`/`d.values`/`d.name`/
  `d.clone`/`d.clause`/… vocabulary. `ArenaManifold::dimensionNamed` likewise reads only
  base + `arenaDims_` (`:55-76`) — the arena-local rank is name-invisible to it; only
  `VortexCore::findDimension`'s `rank(arena_, dims_.dims, dims_.dims)` walk (`vortex_core.cpp:87-92`)
  sees it.
- **`ensureDimension` does *not* always append to the rank.** The append is guarded:
  `if (dimDims && home)` (`arena_manifold.cpp:91-103`), and `homeCell()` is `base_->home()` only
  (`arena_manifold.hpp:275-277`, `arena_manifold.cpp:30-32`). In the base-less emit arena **no
  `ensureDimension` mint ever reaches a rank** — the whole adapter bind set (`d.code`, `d.patch`,
  `d.refs`, `d.recipe`, `d.refFor`, `d.entry`, `d.weight`, `d.synapse`, `d.patch-head`,
  `d.validate`, `d.mesh.relocation` — the P16 ceremony's own dims) plus `attach()`'s `d.stores`/
  `d.store-refs` (`:289-290`) live in `arenaDims_` only. "Every mint path appending there" is false
  exactly where the filing matters.
- **The cited accessor is the wrong one and is circular.** `manifold.cpp:771-775` is
  `Manifold::dimensions()` — `rankAfter(home_, dimsDim_)` on the *persistent fold*. `promote`
  emits no `d.dims` links (`arena_manifold.cpp:1766`), so the fold's rank contains only what
  `Store::makeDimension` filed (`store.cpp:1077-1090`) — walking it enumerates the filed set, not
  the set to file.
- **The timing forecloses any walk.** Emit-side filing runs *before* the compile arena's
  `initGenesis` (:2283-2285, :4000-4001) — before the V10 names are even minted — so the
  enumeration can only ever be the static channel list the doc does in fact name; no member walk
  can see unminted cells.

Consequence if the stated walk is implemented literally: whichever half the chosen accessor can't
see goes unfiled — V10 wing dims → post-resume `initGenesis`'s `baseDimensionNamed` misses →
`makeCell` twins → `wingOf`/`inputsOf` resolve on the twin and read every folded op as wingless
(dead image); or the bind set → `ensureDimension("d.patch")` twins at genesis → the `d.patch`
replay walks the twin's empty rank and every learned patch is silently dropped. Both are the
silent-fragmentation class the filing exists to kill.

**Fix:** the channel list the text already names is the correct enumeration — pin it as the
spec and drop the walk claim: "the filed set is the *name list*: the adapter bind set, `attach()`'s
`d.stores`/`d.store-refs` (`arena_manifold.cpp:289-290`), `initGenesis`'s V10 names
(`vortex_core.cpp:44-69` — `d.dims` itself included), every `ensureDimension` callsite name, and
every `dimConstant`/`findOrMintDimension` mint collected off the arena's `d.dims` rank
(`vortex_core.cpp:87-92`'s `rank` walk) ∪ `arenaDims_` keys — **not** `dimensions()`: on the arena
it is base ∪ `arenaDims_` and misses the arena-local rank entirely (`arena_manifold.cpp:34-46`),
and on the fold it walks only already-filed members (`manifold.cpp:771-775`)".

### 2. MINOR — the recipe chain's member-kind discriminator is still unstated, so "four `+d.step`-consecutive members" is not yet decodable (pre-existing since v1.15, now load-bearing)

The positional fix pins *intra*-tuple order but not *inter*-record boundaries: the chain mixes
tuple fields, literals, capture records and ref-capture records (:1078-1093, :1205-1208), and no
member signature is stated for literal vs capture-record members. At a record boundary the
parseable signatures are: `+d.refFor` ⇒ ref-capture record; marked + `+d.name` ⇒ `dim` field (tuple
position 2 — malformed at a boundary); marked, name-less, `+d.refFor`-less ⇒ tuple `from` —
consume four. The hole: an *unmarked* member is a literal or a capture record with nothing to tell
them apart (a `cap$N`-texted capture record is content-indistinguishable from a text literal; if
capture records are marked they collide with `from`). An implementer must invent the tag. **Fix:**
one clause — "a tuple opens on a marked, `+d.name`-less, `+d.refFor`-less member; capture records
carry `<edge>` → the binding cell (state which); literals are the remaining unmarked members".

### 3. MINOR — the named channel list under-covers the non-bind `ensureDimension` callsites

The enumeration's parenthetical names "the `ensureDimension` apparatus names" but enumerates only
"the bind set above plus `attach()`'s `d.store-refs`" (:2980-2984, :3993-3995). Other
`ensureDimension` mints exist: `projectProvenance`'s `d.authorship`/`d.source`
(`arena_manifold.cpp:129-135`), `d.quote-origin` (`:423`), and the `ensureDimension(name)` mints
inside the import loops (`:678`, `:713`). If any such edge can promote into the lineage, its name
must be filed too — either widen the list to "every `ensureDimension` callsite name" or state that
provenance/import dims never edge into the checkpointed graph (plausible but unstated).

## Verified accurate (v1.17)

`(from, dim, dir, to)` order = the `link` signature (`arena_manifold.hpp:604`); `+d.step` chain
eviction discipline; `Manifold::dimensions()` = the `d.dims`-rank `rankAfter` walk
(`manifold.cpp:766-778`) — cite lands correctly for the *base fold*; `getOrCreate`'s
move-assigned rebuilt fold = the injected base (`dimension_registry.cpp:194-216`) —
file-before-pin mechanized correctly; `Store::makeDimension` appends the persistent rank +
`registerDim` (`store.cpp:1057-1091`); `ensureDimension`'s find-or-mint + conditional rank append
(`arena_manifold.cpp:78-105`); the restore/(e) kind-guards keeping `OpHandle`-only writes idempotent
(:2184-2194, :2359-2375).

## Counts

- BLOCKING: 0 — MAJOR: 1 — MINOR: 2

Top issues: (1) the filed-set enumeration's stated mechanism (the `d.dims`-rank/`dimensions()`
walk) cannot yield the claimed vocabulary — two disjoint dim-name tables in code
(`arenaDims_` vs the VortexCore `d.dims` rank) and `ensureDimension`'s base-gated rank append make
"every mint path appending there" false; (2) tuple-boundary member-kind signatures unstated;
(3) `ensureDimension` callsites beyond the bind set (`d.authorship`/`d.source`/`d.quote-origin`)
not covered by the named list.

VERDICT: NOT CONVERGED
