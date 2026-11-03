# Round-17 disposition — orchestrator rulings on v1.16 findings

0 blocking, 2 mechanism-class majors (4 lanes), ~14 minors. Three lanes converged (ergo,
writer, meta). R16-A verified complete; R16-B's filing machinery verified real
(`store.cpp:1057-1091`, `dimension_registry.cpp:194-217`, `vql/compiler.cpp:1128-1141`) and
closes the orphan fold *for filed dims* — the majors are where the filing's coverage and the
schema carrier end.

## Majors — all adopted

**R17-A — the `+d.recipe` member's field-schema carrier is unpinned; `+d.name` double-books**
(zigzag + codebase majors; ergo + meta + writer minors — five lanes). "name → tuple position"
has no named carrier; a `+d.name` edge on the record would collide with the dim member's
dim-name channel (M14 single-occupancy) and make (e)'s `+d.name`-keyed dim arm write
`Int64(DimRef)` into every `from`/`dir`/`to` field. **Ruling:** the field's schema name is
carried as **content** — a `+d.values`-linked fresh scalar text cell per record (the
`makeCell(name)` precedent `vortex_stdlib.cpp:4051`, one record one edge) or positionally by
recipe-tuple order — never `+d.name`; `+d.name` on a `+d.recipe` member is reserved to the
dim-name discriminator alone. Companion scope: "`+d.name` on a record makes it a name-record"
scopes to `+d.refs` records only (:1102-1103,:2992).

**R17-B — the `d.dims` filing's coverage and ordering are unstated** (vortex + architect
majors; meta minor agrees). Session-minted user dims (`dimConstant`/`findOrMintDimension`
`compiler.cpp:769-772`; `mintNamedDimension`-minted V10 names `vortex_core.cpp:50-69`) still
twin — "every other `ensureDimension`-minted name" overclaims; and `getOrCreate`'s rebuilt
fold must *be* the base-injected fold — file-after-pin leaves the arena minting twins
regardless (`dimension_registry.cpp:146-161`). Emit-side mirror: session edges key on the
orphan while `int64(arenaDimRef)` operands stay dead ids — the invariant is "file before the
emitting lineage's first resolution", mirroring spawn-seed. **Ruling:** (i) the filed set
enumerates the *whole* minted-dim vocabulary — the `d.dims`-rank member set plus
`mintNamedDimension`-minted V10 names; (ii) ordering is file-before-pin at spawn-seed
(`getOrCreate`'s fold IS the injected base) and file-before-first-resolution at emit — both
stated in §10.1/genesis (b).

## Minors — all adopted

- **(e) "re-minted canonical cell"** (writer + codebase): filed dims are re-bound, never
  re-minted — :2330.
- **`closed` arm (c) omits the `dim`-field member form** (zigzag + writer): the glossary and
  re-verify include it; the arm doesn't — :1915-1919.
- **`dim`-member name-cell re-mint on `reify`** (writer + zigzag): `-d.name` eviction means
  copies must re-mint the name cell, not re-link — extend the `patch:<n>` precedent.
- **Chained-capture `+d.clone` direction echoes** (codebase): "running state→program" still
  modifies the joint claim — chained captures run clone→clone (:2210-2216,:3737-3741,
  :3801-3805).
- **`set_weight` `+d.name`-bearing non-clause target** (ergo): a head hits neither dispatch
  arm — silently changed from v1.15; state the third case.
- **Citation/phrasing nits**: `:836` cite covers the lambda not `cloneMaster` (meta);
  `+d.refFor`-self/"its own field cell" (meta); slot-shaped (d) exception (writer); "§8.3"
  junction miscite → §8.1/glossary (writer); stale "uniform position keyspace" :2101
  (writer); "one per `+d.refs` ref-record … same slot cell" (zigzag — field ref-records
  aren't slots); filed-set enumeration vs adjacent bind list (architect) — apply all.

## Round status

v1.17 to follow. Two convergent mechanism-classes again — the doc is at 3,893 lines and the
critics are now finding the *enumeration/coverage* layer under each fix. Majors per round:
r13 1 → r14 1 → r15 2 → r16 2 → r17 2 (all pairs of lanes on one mechanism each).
