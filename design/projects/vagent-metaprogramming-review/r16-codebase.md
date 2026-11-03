# r16-codebase: Round-16 codebase verification of `design/vagent-metaprogramming.md` v1.15

**Orchestrator summary (≤200 words):** The R15-A fix lands correctly — the legs now
classify by unreachability (:2157-2171): the `+d.clone →` `cap$N`-slot edge is real
(`vortex_vm.cpp:816-825`, `fromNegward(false)`=POS, `dim_vector.hpp:85-86`), runs
state→program (binding=master, slot=clone), sits on a dim the walk vocab lacks
(:1524-1526,:1541), and `masterOf` is correctly demoted to the operand decode
(`cloneMaster`=`rankEnd(NEG)`, `arena_manifold.cpp:1009-1012`,
`vortex_core.cpp:344-346`,`:358-365`). Both changelog echoes and the confession are
consistent (:3666-3671, :3659-3665, :3730-3732). R15-B's uniform marked keyspace and
every adopted minor check out; **all v1.15 citations verify exact**. But the R15-B
blanket rule sweeps the recipe tuple's **`dim` field** into the position keyspace —
a dim's durable identity is its *name* per the doc's own adjacent rule, and a
position folds to a `dimensionNamed`-invisible orphan twin. **1 MAJOR, 1 MINOR,
1 NIT. VERDICT: NOT CONVERGED.**

## Citation verification — v1.15 delta cites (all pass)

- `vortex_vm.cpp:816-825` — `Clone`: `cloneTail = *c` (:815) walks
  `linked(cloneTail, dims().clone, false)` (`false`=POS, `fromNegward`,
  `dim_vector.hpp:85-86`) and `link`s the clone at the rank tail (:821-822). "Clone
  appends it at the master's posward tail" exact (:2163).
- `arena_manifold.cpp:1009-1012` — `cloneMaster` = `rankEnd(*this, ref, cloneDim,
  DimVector::NEG)` — NEG-ward, exact (:2166-2167, :3703).
- `arena_manifold.cpp:78-105` — `ensureDimension`: `dimensionNamed` → `arenaDims_`
  cache → mint + `d.dims` rank link. Exact (:1183, :1537-1538, :2185, :2260, :3716).
- `arena_manifold.cpp:1764-1768` — discovery walk: relocation-handle targets enqueued
  (:1759-1763), then `d.dims`-dim edges skipped outright (:1765-1768). Session-minted
  dims fold off the `d.dims` rank — exact (:2262, :3718).
- `arena_manifold.cpp:1934-1954` — promote: `Int64` verbatim (:1934-1936); `OpHandle`
  writes `isEphemeral(target) ? real.at(target) : target` (:1947-1951);
  `ForeignOpHandle` (:1952-1955). Exact (:601, :2271, :2907, :3713).
- `arena_manifold.cpp:2010-2017` — "links last" comment — supports :2120's "promote
  emits links last". Exact.
- `vortex_core.cpp:181`,`:202` — `masterOf(cell)` at the read path and `writeValue`
  funnel. Exact (:1202, :2944).
- `vortex_core.cpp:344-346` — `masterOf` = `cloneMaster(...).value_or(cell)`. Exact.
- `vortex_core.cpp:354-374` — `wingOf` operand lambda (`asInt64`→`CellRef`→`contains`
  →`.value_or(masterOf(slot))`, :358-365), `linked(opcode, dims_.grab, side)` (:366),
  `inputsOf`=POS (:372-374). Exact.
- `compiler.cpp:769-772` — `dimConstant = literal(int64(findOrMintDimension(name)))`.
  Exact (:1180, :2045, :2206).
- `store.cpp:123` (`Store()` no-id), `:135` (`documentId_` two-arg ctor), `:4450`
  (`documentId_ = tables.documentId` directory load). Exact (:639, :1580, :3691-3693).
- `publication.cpp:1097` — `historyFromSeal` mints `std::make_unique<Store>()`.
  Exact (:642, :1583, :2828, :2894-2899).
- `mesh_actor.cpp:260-271` — `receiveValue` → `checkpoint` →
  `WorkerContext(checkpoint->manifold.get())`; `importValue` is not called on this
  path (called only at `mesh_value.cpp:531`, a different entry). "overlays without
  `importValue`'s remap" exact (:390, :2110, :2266-2267, :3333).
- `mesh_value.cpp:485-501` — unmarked `OpHandle` demoted to `None` (:485-490);
  relocation-marked cells re-minted `OpHandle` naming `local.at(*target)` (:494-501).
  Exact (:2268-2269).
- Walk vocab (:1524-1526, :1541-1545) — `d.clone` still absent; `d.dims` the named
  exclusion citing `arena_manifold.cpp:1766`. Supports the R15-A claims.

## Ruling-adoption verification

- **R15-A — landed and sound.** :2157-2171 restates by unreachability; every clause
  verifies mechanically (edge identity `+d.clone →` slot, state→program direction,
  dim absent from vocab, `masterOf`=operand decode). The frontier-consumer gloss —
  "`closed`'s frontier enumeration, which does read the `cap$N` slot links and
  decode them through `masterOf`, not this test" — matches :1898-1900 (frontier lists
  "the emission's wing-dim `cap$N` capture-slot links") and §5.3's lambda decode
  (:826). Changelog echoes rewritten in the same form and confessed.
- **R15-B — landed in form** (:1144-1155, :2048-2051, :2907 P16 row, diagram
  :1055-1069): fields carry marked `int64` positions, own `+d.recipe` records the
  inventory, `+d.values` demoted to decorative, shared endpoints re-join on one key —
  *except the `dim` field, Finding 1.*
- **All adopted minors verified in place:** (e) writes `Int64` of the resolved
  `DimRef` with the orphan rationale (:2259-2264); (d) "never the endpoint value"
  scoped to the `+d.name` exception (:2244-2247); arm (c) "a dim referent" (:1876,
  :1066, :3538); pairing named `+d.ref`/`+d.refFor` (:1907-1908, :2274-2275);
  `+d.values` defers to map membership (:1172, :2244); `set_weight` refusal +
  `weight_of` disengage + row gloss (:1125, :2505-2509); position-carriage scoped to
  ref slots with `cap$N` re-clone (:1191-1193); `*user-alias*` (:1776); `wingOf` cite
  moved to `:373` (:1163 — see Finding 3); sketch serial (:2706); retro-edit
  confessions (:3727-3734).

## New findings (v1.15 deltas only)

### Finding 1 — MAJOR

- **Location:** :1144-1150 (§6.1 recipe rule — "every field naming a source cell
  carrying that cell's ref position"); explicit at :2048-2051, :3708 ("tuple
  `from`/`dim`/`to` endpoints … carries the ref position as *marked* `int64`").
- **Problem:** the uniform marked-position rule is wrong for the tuple **`dim`
  field** — a dim reference whose durable identity is its *name*, per the doc's own
  adjacent rule ("a dim referent … keeps `+d.name → the referent dim's name string`,
  resolved at replay through `ensureDimension`'s name arm", :1179-1183; arm (c)
  :1876-1879; (e) :2259-2263). Mechanically: a session-minted dim `D` is a marked
  field's `handleTarget`; `promote` folds it to a twin `T` (`real.at(target)`,
  `arena_manifold.cpp:1950`) that is **name-invisible** — `promote` never emits
  `d.dims` rank edges (discovery skips `d.dims` edges :1766, so the `d.dims` dim cell
  never enters `real`, so the emission's `real.find(edge.dim)` fails :2030-2034) and
  `setLink` never registers dims (`registerDim` exists only at `store.cpp:1052`,
  `:1089` for `makeDimension`/genesis); the fold's `dimensions()` is the `d.dims`
  rank walk (`manifold.cpp:771-775`) so `dimensionNamed` misses `T`
  (`manifold.cpp:795-814`). At replay the field's `*handleTarget` = `T`, so the
  replayed `Link` rides `T` (`Link` takes `DimRef` raw, `vortex_vm.cpp:620`) — while
  every name-resolved consumer binds `ensureDimension`'s fresh canonical `C ≠ T`
  (`arena_manifold.cpp:78-89`), and (e) deliberately writes `Int64(C)` into the
  copy's own `dimConstant` slots. One logical dim becomes two cells in the same
  lineage: replayed structure on `T`, re-bound slots and session ops on `C`, and any
  `linked(x, C)`/`+d`-name traversal misses the `T` edges forever (each later
  checkpoint folds `C` to yet another orphan). The doc's own orphan rationale at
  :2261-2263 ("dead weight beside the re-minted canonical cell") is the exact
  mechanism that makes the `dim` field's position useless — applied to `+d.refs`
  slots but withheld from the `+d.recipe` field doing the identical job. Scope
  honesty: base-registered dims are unaffected (`!isEphemeral → target` passes the
  real ref through :1950), so the break is confined to session-minted dims — but the
  spec's "single position keyspace … every naming already folds into" claim is
  false for them, and an implementer cannot tell which cell a `+d.<name>` traversal
  binds on the lineage.
- **Evidence:** `arena_manifold.cpp:1765-1768` (`d.dims` discovery skip),
  `:2024-2034` (emission requires `real` on both ends → `d.dims` edge never folds),
  `:1947-1951` (`real.at(target)` → twin), `manifold.cpp:771-775`,`:795-814`
  (`dimensions()`/`dimensionNamed` = rank walk + text match), `store.cpp:1052`,`:1089`
  (registration only via `makeDimension`), `vortex_vm.cpp:620` (`DimRef` raw), doc
  :1179-1183, :2259-2263 (the name arm the `dim` field contradicts).
- **Concrete fix:** exempt the tuple `dim` field from the marked-position rule and
  put it on the name channel every other dim reference already rides — the field
  record carries `+d.name →` the dim's name string (no marker, no position), replay
  `ensureDimension`s it to the canonical dim — or state explicitly that replay binds
  the promoted twin *deliberately* to match the sealed image's dim identity, in which
  case the (e) name-arm for `+d.refs` dim records is the arm that is wrong and the
  doc must say how any name-resolved traversal sees twin-riding edges at all.

### Finding 2 — MINOR

- **Location:** :1537-1538 (§6.6 sideness walk, `d.dims` exclusion — carryover
  sentence, but the v1.15 delta at :2261-2263 now contradicts it in-section).
- **Problem:** "a promoted dim cell re-binds rather than re-mints" is false for
  promoted *twins*: `dimensionNamed` resolves only base-registered dims and the
  `arenaDims_` cache (`arena_manifold.cpp:58-74`; fold-side `manifold.cpp:795-814`),
  and a promoted twin sits off the `d.dims` rank unregistered — the doc's own new
  text two passes later calls it "dead weight beside the re-minted canonical cell"
  (:2261-2263). As written the sentence claims the twin re-binds; it re-mints.
- **Concrete fix:** "so a base-registered dim cell re-binds rather than re-mints"
  (or "a dim the base already names re-binds — a session-minted dim's promoted twin
  re-mints, §6.6 (e)").

### Finding 3 — NIT

- **Location:** :1163 (the `+d.grab` correlation refusal).
- **Problem:** the corrected cite `vortex_core.cpp:373` names `inputsOf`'s
  `return wingOf(opcode, DimVector::POS)` — it supplies the POS but the
  "`linked(opcode, dims_.grab, POS)`" the parenthetical quotes lives at :366.
  Defensible as the POS entry, incomplete as evidence for the `linked` call.
- **Concrete fix:** cite `:366` for the `linked` entry (or `:366`,`:373` for both
  halves).

## Non-findings verified (delta claims that hold)

- **R15-A direction check passes on both barriers:** the binding cell is the master
  (state-side), the `cap$N` slot the clone (program-side); `Clone` appends POS-ward
  at the rank tail (`vortex_vm.cpp:815-822`, `fromNegward(false)`=POS) — the only
  sealed edge runs state→program *and* `d.clone` is absent from the walk vocab
  (:1524-1526,:1541). "image-sealed for a declared fragment, never runtime-grown" is
  correct — the edge is minted at emission. A runtime-grown cross-root `Link`
  reaching a leg still classifies it `>E_ops`/state-side, matching "classified
  state-side, never refused" (:2154-2157).
- **`(d)` "only ever annotating an internal endpoint" is a true consequence:** an
  external endpoint's `+d.values` edge would be an edge *into the referent*, which
  the content-carried rule bars (:1165-1168) — the conditional edge exists only for
  internal endpoints where `-d.values` is free (:1173-1175); map membership is
  correctly named the discriminator (:1172, :2244).
- **The marked-field replay chain is coherent:** tuple field marked → re-kinded
  `OpHandle` → folds `OpHandle(real.at(endpoint))` (:1947-1951) → `*handleTarget` =
  promoted endpoint → one keyspace through the old→new map; shared endpoints re-join
  (both marked fields resolve the same `*handleTarget`). This genuinely fixes
  R15-B's mixed encoding for cell endpoints.
- **`*handleTarget` for base dims is harmless:** non-ephemeral targets pass through
  verbatim (`arena_manifold.cpp:1950`), so a marked slot naming a registered dim
  yields the real `DimRef`; the (e) name-arm is correct in both cases.
- **`set_weight`/`weight_of` and the `+d.name`-less refusal** (:2505-2509):
  declared handler clauses named `+d.name`-less and refused like other
  unresolved focus-slot verbs — consistent with the `()`-returning write arm.
- **Carryover observation, not filed:** the deeper engine gap — *every* folded
  dim twin is `dimensionNamed`-invisible, so no name-resolved traversal of the
  sealed image's own persisted edges is possible through `dimensionNamed` at all —
  predates v1.15 (same mechanics since the durability section landed) and is the
  substrate Finding 1's narrow case inherits. If some adapter-side dim-binding
  mechanism exists that makes sealed edges traversable, it is stated nowhere in the
  document.

VERDICT: NOT CONVERGED
