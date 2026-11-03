# r18-codebase: Round-18 codebase verification of `design/vagent-metaprogramming.md` v1.17

**Orchestrator summary (≤200 words):** R17-A lands correctly and is
mechanically sound: field identity is carried by the member's `+d.step`
position within the four-member tuple run (:393-395, :1078-1083, :1186-1189,
:2131-2133); `+d.name` on a `+d.recipe` member is reserved to the `dim`
field's dim-name channel (:1085-1088, :1120-1121, :1195-1197, :2132-2133,
P16 :3070); the name-record discriminator is `+d.refs`-scoped
(:1072-1077, :1102-1103, :1119-1121, :2115-2116, :3070). The M14
double-booking and (e)'s `from`/`dir`/`to` mis-key are dead. R17-B's
ordering pins verify mechanically — `getOrCreate` move-assigns the
`Manifold&` (`dimension_registry.cpp:214` inside :194-217) so the filed fold
IS the injected base; every v1.17 citation is exact. **One MAJOR:** the
filed-set enumeration still is not "the whole minted-dim vocabulary" —
`std:agent`'s thirteen `ensureDimension`-bound names
(`vagent-agent-language.md:224-235`), resolved at the same spawn-seed, are
nowhere in it, while `vortex_core.cpp:50-69`'s seventeen *system* dims are
mislabeled "the `mintNamedDimension` V10 names". **1 MAJOR, 4 MINOR.
VERDICT: NOT CONVERGED.**

## Citation verification — v1.17 delta cites (all pass)

- `vortex_core.cpp:50-69` — `initGenesis`'s `mintNamedDimension` calls,
  `d.grab` (:50) through `d.role` (:69), seventeen system dims. Exact — but
  see Finding 1: these are *not* the V10 names.
- `vortex_core.cpp:14,:29-70` — ctor calls `initGenesis()` (:14); body
  :29-70 resolves `home_`/`dims_.dims` from `arena_.base()` then mints the
  seventeen. Exact for "before `initGenesis` freezes home_/dims".
- `dimension_registry.cpp:194-217` — `getOrCreate(store, head, manifold,
  dimName)`: `held` (:198), `sliceGenesis` (:202-204), `store.makeDimension`
  (:205), `rebuildManifold` (:207) + `setStore` (:208), **`manifold =
  std::move(newManifold)` (:214)**, `registerDim` (:215). "Move-assigns its
  `Manifold&` argument … the rebuilt fold the filing returns IS the
  base-injected fold" — exact.
- `store.cpp:1057-1091` — `makeDimension`: `makeCell` (:1066), `d.dims`
  rank-tail append (:1078-1088), `registerDim` (:1089), `d.dims`-genesis
  refusal (:1059-1063). Exact.
- `vql/compiler.cpp:1128-1141` — `// 1. Replicate dimensions`: arena-cell
  walk, `name.starts_with("d.")` → `getOrCreate(store, ver, manifold, name)`
  → `dimMap`. Exact.
- `compiler.cpp:769-772` — `dimConstant` lambda →
  `literal(int64(core_.findOrMintDimension(name)))`. Exact.
- `manifold.cpp:771-775` — `dimensions()` = `rankAfter(*this, *home_,
  *dimsDim_)` inside the `home_ && dimsDim_` guard. Exact.
- `arena_manifold.cpp:289-290` — `attach()` calls
  `ensureDimension("d.stores")` (:289) and `ensureDimension("d.store-refs")`
  (:290). Exact; `d.stores` is also initGenesis's (:68), so the *new* name
  attach can mint is `d.store-refs` — the cite is honest.
- `vortex_stdlib.cpp:4051` — `makeVar`'s `CellRef nameCell =
  core_.arena().makeCell(name)` + `d.name` link — the fresh-name-cell
  precedent. Exact (pre-existing cite at :302, unchanged).
- `arena_manifold.cpp:78-105` — `ensureDimension`: `dimensionNamed` →
  `arenaDims_` → `intern`+`makeCell`+`arenaDims_`+`d.dims` rank-tail link.
  Exact (r17-verified, unchanged).
- `arena_manifold.cpp:1766`,`:2030-2033` — `d.dims`-dim skip in discovery;
  `real.find(edge.dim)`/`real.find(edge.pos)` fold gate. Exact — and
  `discover(edge.dim, false)` at :1781 is precisely how an unfiled dim
  enters `order` and mints as an orphan twin.
- `arena_manifold.cpp:1764-1768` — relocation-target enqueue + the
  `d.dims` skip; the accretion cite. Exact.
- `arena_manifold.cpp:1009-1012` — `cloneMaster` = `rankEnd` NEG-ward;
  `vortex_core.cpp:358-365` — `wingOf`'s operand lambda (`asInt64` →
  `contains` → `.value_or(masterOf)`); `vortex_vm.cpp:815-829` — `Clone`'s
  posward-tail walk + append. The split cites are all exact.
- `vortex_stdlib.cpp:603`,`:628` — "Preconditions along -d.contract"/
  "Postconditions along +d.contract" comments beside
  `preconditionsOf`/`postconditionsOf`; anchors are op cells
  (`contractOf` NEG/POS, `vortex_core.cpp:464-470`). Exact.
- `store.hpp:450-457` — `rebuildManifold` returns `Manifold` by value;
  `arena_manifold.hpp:752` — `const Manifold *base_{nullptr}`. Exact.
- `publication.cpp:1097` — `historyFromSeal` mints `Store()` internally, no
  id parameter; `store.cpp:135` — `documentId_ = documentId` in the two-arg
  ctor; `store.cpp:1127-1128` — `restorePublication`'s
  `Store(userPermascroll, pub.storeId)` precedent. All exact.
- `mesh_actor.cpp:271-273` — `WorkerContext(manifold.get())` then
  `arena.setDefaultSpanReader(store.get())` — "passes the manifold alone,
  installs the reader after". `worker_context.hpp:52-54` — two-arg
  `(baseManifold, reader)` ctor. Exact.
- `arena_manifold.hpp:170-186` — `Mark` fields, no dimension field.
  `arena_manifold.cpp:1347-1377` — `setOneSide(..., noCell)` single-occupancy
  eviction. `mesh_evaluator.cpp:735` — per-reference `makeCell("cell")`
  marker. `foreign_op_handle.cpp:98-117` — `isCellRelocationHandle` both
  overloads. All exact.
- `manifold.cpp:824-829` (corroborating the fix): single-arg
  `dimensionNamed` returns `nullopt` when `store_` is null — so
  `VortexCore::baseDimensionNamed` re-binds filed dims only because
  `rebuildManifold`/`getOrCreate` set `store_` on the fold
  (`store.cpp:511`,`:532`; `dimension_registry.cpp:208`). The doc's
  "the filed fold IS the injected base" is the load-bearing half of this —
  verified.

## Fix verification

- **R17-A (`+d.step` positional carrier + `+d.name` reservation + `+d.refs`
  scoping) — landed correctly.** The disposition offered the `+d.values`
  scalar cell *or* tuple order; v1.17 took tuple order and pinned it
  consistently at all four carrier sites. No second `+d.name` edge exists to
  collide under M14 (`arena_manifold.cpp:1347-1377`); (e)'s arm key — "a
  marked cell whose `+d.recipe` side bears `+d.name`" (:2369-2371) — can no
  longer fire on `from`/`dir`/`to` members, which carry no name edge at
  all. The `+d.refs` scoping is in the diagram (:1072-1077, :1102-1103),
  §6.1 (:1119-1121), §6.6 step 1 (:2115-2116) and the P16 row (:3070) — the
  `dim`-field member stays marked via its `+d.ref` REF-record while its
  member-self carries `+d.name`. My r17 Finding-1 is closed.
- **R17-B ordering — landed and mechanically airtight.** Spawn-seed:
  `getOrCreate`'s `manifold = std::move(newManifold)` (:214) means filing
  returns the fold that gets base-injected — "the rebuilt fold IS the
  injected base" is exact, and a post-pin filing is genuinely invisible
  (`base_` is pinned `const Manifold*` at `attach`, `arena_manifold.cpp:
  279-281`). Emit: file before the compile arena's `initGenesis` →
  `mintNamedDimension`'s `baseDimensionNamed` check (`vortex_core.cpp:18-21,
  348-352`) re-binds filed cells — which requires `store_` on the base,
  which `rebuildManifold` always sets (`store.cpp:511`,`:532`).
- **R17-B coverage — one hole: Finding 1.**

## New findings

### Finding 1 — MAJOR

- **Location:** :113-122 (§1), :2277-2287 (genesis (b)), :2980-2995 (§10.1
  spawn-seed), :3944-3947 and :3991-3996 (changelogs); contradicts the
  doc's own :89/:295 ("the thirteen dimensions (V10)") and :1610
  (`d.belief` an agent-anchored rank).
- **Problem:** The landed enumeration of "the whole minted-dim vocabulary —
  every name the lineage's arenas can resolve" names three classes: the
  spawn-seed bind set (the twelve apparatus dims + roots + validate cells),
  `attach()`'s `d.store-refs`, "`initGenesis`'s `mintNamedDimension` V10
  names (`vortex_core.cpp:50-69`)", and every
  `dimConstant`/`findOrMintDimension` mint. **V10's thirteen —
  `d.behavior`, `d.skill`, `d.capacity`, `d.space`, `d.member`, `d.holon`,
  `d.intention`, `d.subgoal`, `d.outbox`, `d.belief`, `d.pending`,
  `d.source`, `d.extends` — appear nowhere**, yet
  `vagent-agent-language.md:227-228` binds them by
  `ArenaManifold::ensureDimension` "before a turn first runs in an arena
  (after spawn and after `resume`)" — a dim resolution inside exactly the
  spawn-seed window the invariant covers, not a mid-session residual.
  Compounding it, the enumeration mislabels `vortex_core.cpp:50-69`'s
  seventeen *system* dims (`d.grab`…`d.role`) as "the `mintNamedDimension`
  V10 names" — V10's names are the thirteen (this doc itself says "the
  thirteen dimensions (V10)" at :89/:295; the sibling doc says
  "`initGenesis` mints neither `d.source` nor `d.extends`, which is why
  they are on the list", :233). Under the closed reading of the
  enumeration, the names literally labeled V10 elsewhere are the ones left
  out. Mechanically: unfiled → V10 bind's `ensureDimension` mints ephemeral
  cells → every `+d.belief`/`+d.intention`/`+d.member`/`+d.space`/
  `+d.behavior`/`+d.skill`/`+d.outbox`/etc. edge keys on them → at
  `promote` the twin is minted off-rank (`edge.dim` discovered :1781,
  `+d.dims` membership edge skipped :1766, dead `real.find` :2030-2033 for
  its own membership link) → `dimensionNamed`-invisible orphan twin →
  **the entire durable agent state — beliefs, intentions, memberships,
  skills, outbox, pending, subgoals, sources, extends — becomes dead rank
  structure at the first checkpoint**, precisely the failure class the
  filing machinery exists to kill, for the most durability-critical names.
  This silently contradicts the sibling doc's V9/V10 durability model
  (checkpoint promotes reachable state; the V10 test asserts one cell per
  name across resume) and the enumeration's own closure claim.
- **Evidence:** `vagent-agent-language.md:224-235` (the thirteen + bind
  timing/mechanism), `vortex_core.cpp:50-69` (the seventeen — mislabeled),
  `arena_manifold.cpp:78-105` (mint path), `:1766`/`:1781`/`:2030-2033`
  (orphan mechanics), doc :2980-2990/:2277-2287/:113-122 (the closed
  enumeration), doc :1610 (`d.belief` acknowledged in-model).
- **Concrete fix:** extend the enumeration — "…`initGenesis`'s
  `mintNamedDimension` *system* names (`vortex_core.cpp:50-69` — the
  seventeen, not the V10 thirteen), `std:agent`'s thirteen
  `ensureDimension`-bound names (`vagent-agent-language.md:224-235`), the
  `ensureDimension` apparatus names (the spawn-seed bind set plus
  `attach()`'s `d.store-refs`), and every `dimConstant`/
  `findOrMintDimension` mint…" — or make the set operational: "every name
  the lineage's arenas resolve before the first turn — the full spawn-bind
  set, `std:agent`'s included". Also tighten the emit-side "every
  source-named `dimConstant` name" (:3004-3005) to cover kernel-internal
  `findOrMintDimension` mints (`d.extends` is compiler-flattened, not
  source-named) — or state the emit walk is the
  `vql/compiler.cpp:1128-1141`-style `d.*` sweep, which covers them.

### Finding 2 — MINOR

- **Location:** :1591-1594 (sideness walk vocabulary).
- **Problem:** "`d.source` because it anchors on clause cells" — `d.source`
  anchors on *asserted belief* clauses (`clause → the peer cells that
  assert it`, `vagent-agent-language.md:480`,`:509`,`:711`), state-side
  cells under the agent's `+d.belief` that `program()` can never reach;
  declared program clauses carry no `+d.source`. The vocabulary entry is
  dead weight — harmless today, misdescribed justification, and latent
  over-classification if a program-side clause ever grew a `+d.source`
  (state-side peers would sweep program-side).
- **Concrete fix:** drop `d.source` from the `program()` walk, or re-anchor
  the justification ("belief-clause provenance, unreachable from
  `program()` — kept for forward-proofing only" or move it to the
  agent-cell legs list where it can actually occur).

### Finding 3 — MINOR

- **Location:** :393-394, :1080, :1187, :2132 ("the member's `+d.step`
  position in the chain carrying the field identity").
- **Problem:** the `+d.recipe` `+d.step` chain is mixed-kind (tuples +
  literal contents + capture records + ref-capture records, :1089-1092), so
  absolute chain position mod 4 does not recover field identity — the
  operative identity is the member's position *within its four-member tuple
  run*. The "four `+d.step`-consecutive members in `(from, dim, dir, to)`
  order" clause rescues intent, but "position in the chain" loosely invites
  an absolute-index misreading.
- **Concrete fix:** "the member's position *within its tuple's four-member
  run* its field identity" — and, if intended, one clause naming how a
  reader delimits tuple runs (record-kind discrimination), which the recipe
  has never spelled out (pre-existing gap, now closer to load-bearing).

### Finding 4 — MINOR

- **Location:** :1163 (`set_weight` table row) against §8.1 :2624-2636.
- **Problem:** the table shorthand "the keyed `d.vars` record minted on
  first write" predates the three-arm dispatch — a junction target keys off
  its own `+d.weight` scalar, never a `d.vars` record, and a `+d.name`-
  bearing non-junction non-clause keys a record by label verbatim; the row
  glosses only the clause arm plus the refusal.
- **Concrete fix:** "…the keyed `d.vars` record minted on first write
  (junctions write their own `+d.weight` scalar; a `+d.name`-less clause
  refused — the full dispatch, §8.1)".

### Finding 5 — MINOR

- **Location:** :3710-3711 (1.11 changelog entry) against :1090-1092,
  :1206-1207, :2367-2369.
- **Problem:** the 1.11 entry still says "one recipe record per `+d.refs`
  ref-record" — narrowed by 1.17 to *slot* ref-records ("a `+d.recipe`
  field's ref-record pairs with nothing") in the operative text but left
  unannotated in the changelog, unlike this doc's usual retro-edit hygiene.
- **Concrete fix:** annotate the 1.11 entry: "(scoped to *slot* ref-records
  by 1.17 — a field's ref-record pairs with nothing)".

## Non-findings verified (delta claims that hold)

- **`+d.name` reservation is collision-free end-to-end:** memo records on a
  clause's `+d.code` chain (`+d.name → "test"/"body"`, :299-304), the
  descriptor's `+d.name → "Int"`, head labels, `patch:<n>` labels and
  `+d.refs` name-records are all off-`+d.recipe`, so the member-scoped
  reservation touches nothing else; (e)'s arm key "a marked cell whose
  `+d.recipe` side bears `+d.name`" now covers exactly the `dim`-field
  member and `+d.refFor`-paired dim ref-capture records.
- **Re-minted name cells on `reify`** (:1257-1261): every `+d.name` cell —
  the `dim`-field member's dim-name cell included — is re-minted verbatim
  under the `patch:<n>` rule, never re-linked off the donor's occupied
  `-d.name`; consistent with M14.
- **`closed` arm (c) + re-verify** (:1945-1949, :1979-1981): the
  `dim`-field member form is included and the member's `+d.refs` record
  stays a REF-record — matches the emission rule.
- **`(d)`'s `Int64(DimRef)` mirror** (:2347-2350): the `dim`-field member's
  own cell takes the same `Int64(DimRef)` writeback as the paired slot —
  consistent with "the member *is* its field cell".
- **`set_weight` three-arm dispatch** (:2624-2636): junction → own
  `+d.weight`; clause → qualified `+d.name` → `d.vars` record; `+d.name`-
  bearing other → `d.vars` record by label verbatim; `+d.name`-less clause
  refused — consistent with §8.1's synapse bullet and the glossary.
- **`+d.clone →` direction scoping** (:2249-2254, :3816-3817, :3836-3838,
  :3884-3886): "state→program on a first capture (program→program on a
  chained one)" — matches `Clone`'s walk-from-master-to-tail
  (`vortex_vm.cpp:815-829`: first capture `binding +d.clone → cap$N`,
  chained `cap$M +d.clone → cap$N`, `cap$N` cells all program-side).
- **Genesis (e) re-bound/re-mint** (:2376-2380): "re-bound off the filed
  rank for a filed name, re-minted only for an unfiled one" — matches
  `ensureDimension`'s `dimensionNamed`-first path (`:78-105`) and the
  orphan mechanics; the accretion bound is `*unfiled*`-scoped.
- **Ordering on both lineages:** `baseDimensionNamed` →
  `Manifold::dimensionNamed(name)` needs `store_` on the fold
  (`manifold.cpp:825-826`); `rebuildManifold` sets it unconditionally
  (`store.cpp:511`,`:532`) and `getOrCreate`'s returned fold sets it
  (`dimension_registry.cpp:208`) — the doc's move-assign claim is the exact
  mechanism that makes the injected base resolve filed names.
- **Mid-session mints are honestly residual:** `projectProvenance`'s
  `d.authorship`/`d.source`/TSV names (`arena_manifold.cpp:129-186`),
  `d.quote-origin` (:423), `d.shared_*`/name-matched space dims
  (:678,:713) mint inside session operations — covered by "a name first
  minted mid-session stays outside the filed set".

VERDICT: NOT CONVERGED
