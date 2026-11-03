# R17 — System Architect critique (v1.16)

Scope: verify R16-B (the `d.dims`-rank filing — `DimensionRegistry::getOrCreate`/`Store::makeDimension`
on the kept lineage `Store`, the `vql/compiler.cpp:1128-1141` precedent; doc :110-116, :544-548,
:1573-1577, :2241-2243, :2926-2938, :3181-3186, :3861-3866) — does it mechanically close the
orphan-fold (`arena_manifold.cpp:1766`, `:2030-2033`)? Then my three minors (dim mark :2300-2301;
`Linked` "never the twin `*handleTarget`" + accretion bound :2326-2330; re-bind scoped to
rank-resident :1573-1577), the full-ceremony close-out (image-seal dims → spawn seed → verify →
P16 → genesis → turns), and new architectural defects in the v1.16 deltas only. Re-verified
against the tree (`arena_manifold.cpp:54-105` `dimensionNamed`/`ensureDimension`, `:255-272`
`denseOf`/`holdsOwn`, `:1338` `shadow(from)` on `link`, `:1465-1503` `shadow`, `:1755-1784`
discovery incl. the `d.dims` skip, `:1863-1869` `real` identity arm, `:2020-2053` link emission,
`:2091-2095` `arenaDims_` snapshot arm; `manifold.cpp:766-821` `dimensions()`/`dimensionNamed`;
`dimension_registry.cpp:187-217` `getOrCreate` — `makeDimension` + `rebuildManifold` +
`registerDim` + in-place `manifold = move(newManifold)`; `store.cpp:1057-1091` `makeDimension`
(rank-tail append + `registerDim`), `:274-296`,`:300-340` `indexGenesisCells` re-deriving
`homeCell_`/`dimsDimension_` from adopted ops; `vql/compiler.cpp:1128-1141` replicate-dimensions;
`vortex_core.cpp:14-70` `initGenesis`/`mintNamedDimension` (`baseDimensionNamed` first);
`compiler.cpp:769-772` `dimConstant` = `int64(findOrMintDimension)`; `vortex_vm.cpp:662-677`
`Linked` decoding `toInt(inputs[1])`; `mesh_value.cpp:404-411`,`:481-508` `importValue`'s own
`d.dims` skip + unnamed arena dim mints; `mesh_actor.cpp:260-273` resume's rebuild + reader
attach).

## Verification — R16-B mechanically closes the orphan-fold, **on the receiving lineage**

The code chain holds end to end:

1. `Store::makeDimension` (`store.cpp:1057-1091`) mints the dim cell, appends it to the store's
   `d.dims` rank (`setLink(minted, tail, *dimsDimension_, POS, ref)` :1087-1088 — a real
   persistent `d.dims` op in the spool) and `registerDim`s it — i.e., filing writes ordinary ops,
   so the rank membership is *in the sealed/ checkpointed segment* (`sealLocalSpool` seals the
   whole spool, `mesh_value.cpp:227-228`,`:241`) and replays on adoption; `indexGenesisCells`
   (`store.cpp:300-340`) re-derives `homeCell_`/`dimsDimension_` from the adopted genesis ops so
   the lineage store can file post-adopt.
2. `getOrCreate` (`dimension_registry.cpp:194-216`) additionally `rebuildManifold`s its `Manifold&`
   argument **in place** (`manifold = std::move(newManifold)` :214) — if the adapter passes the
   manifold that is base-injected, object identity is preserved and the arena's `const Manifold*
   base_` sees the filed cells. Then `ArenaManifold::dimensionNamed` → `base_->dimensionNamed`
   (:58-68) → `Manifold::dimensionNamed`'s registry fast-path gated by `contains(dim)`
   (`manifold.cpp:784-791`) or the `d.dims` rank walk (:802-820, registering hits :817) →
   `ensureDimension`'s find lands (:79-82) — **re-bind, no mint, no twin**.
3. Session edges then carry `edge.dim` = a persistent cell → `real` identity arm
   (`arena_manifold.cpp:1867-1869`) → `real.find(edge.dim)` hits (:2030-2033) → fold emits keyed
   on the canonical cell → next genesis's `ensureDimension` re-resolves it by name. Every
   name-keyed inventory — `d.patch` enumeration for (d), `d.code` heads, the `+d.refs` sweep,
   `+d.recipe`/`+d.refFor` pairing, `d.entry` deref, `d.patch-head`, `d.weight`/`d.synapse`,
   `d.validate`, `d.mesh.relocation`, the agent-anchored V10 ranks — is reachable again, provided
   the name was filed **before the first resolution** (see finding 1 for where that ordering is
   not stated).
4. The orphan mechanism itself re-confirmed, with one refinement of my r16 reasoning: the
   `d.dims`-skip is in *discovery* only (:1765-1768); emission has no `d.dims` arm. A session
   rank-append (`ensureDimension`'s `link(tail, d.dims, POS, dim)` :91-103) *shadows* the base
   tail (`shadow(from)` at :1338 → `holdsOwn(tail)` true), so the emission loop at :2020-2052
   *could* reach it — but the edge still dies at the `real.find(edge.dim)` gate: the `d.dims`
   dim cell is never discovered, since the only edges that would reveal it are the skipped ones
   (:1781 `discover(edge.dim)` runs after the :1766 `continue`). "No `d.dims` rank link is ever
   emitted" stands, now with two independent witnesses.

One **conditional-correct** rider from r16 is discharged: `ArenaManifold::dimensionNamed`
(:54-76) checks `base_` first, so a filed dim resolves without touching `arenaDims_`; the
`arenaDims_` snapshot arm (:2091-2095) preserves filed (persistent) dims verbatim.

## Verification — all three minors landed

1. **"dim mark"** — `:2300-2301` now reads "the record supplying the pairing (`+d.refFor`) and the
   dim mark (`+d.name`)"; retro-edit confessed at `:3772` and in the 1.16 entry `:3875-3876`.
2. **`Linked`/twin + accretion bound** — `:2327-2330`: "`Linked` binds a `DimRef`, never the
   promoted twin `*handleTarget` names, a session-minted dim in use accreting one such orphan per
   checkpoint off the folded `d.dims` rank (`arena_manifold.cpp:1764-1768`), dead weight beside
   the re-minted canonical cell the name arm re-resolves" — my proposed wording, landed verbatim.
   (Now itself shades stale — see finding 4.)
3. **Re-bind scoped to rank-resident** — `:1573-1577`: "a *rank-resident* dim re-binds rather than
   re-mints — while a session-minted dim's promoted twin re-mints: it folds off `d.dims`, which is
   why the minted-dim set is filed on the persistent rank at image-seal, §10.1/genesis (b)".

Also spot-verified sibling minors I was asked to watch architecturally: `masterOf` as a
`-d.clone` backlink traversal on a non-vocabulary dim (`:2219-2221`), `+d.clone →` scoped to first
capture (`:2213-2215`), the `cap$N` re-clone in the fixed `New`/`Link`/`Value` palette
(`:1227-1229`), the one-inventory rule (R16-A) at `:2093-2101` + `:2131-2135` + `:2148-2150` +
`:2313-2327`, and (d)'s install set naming `+d.recipe` beside `+d.refs` (`:2308`). All consistent.

## Ceremony close-out — receiving side closed; the emit side has a new major

image-seal dims → spawn seed → verify → P16 → genesis → turns: the *receiving* half is now
mechanically closed — `d.dims` filing at spawn-seed makes genesis (b)'s `ensureDimension`
re-bind, which makes every (d)/(e) inventory reachable, which is what the whole P16 ceremony
enumerates. The *emitting* half is where the fix as worded does not close: **filing must precede
the emitting lineage's first name-resolution, and "at seal" is the wrong end** — finding 1.

## New findings (v1.16 deltas only)

### 1. MAJOR — the image-seal half states the ordering at the wrong end: "at seal" cannot deliver name-keyed image structure under the doc's own promote-emission model (§10.1 `:2930-2938`; also `:114`, `:544-546`, `:1216-1218`, `:2241-2243`, `:3181-3184`, `:3862-3864`)

The fix's receiving side is ordered correctly (spawn-seed precedes genesis (b)'s first
`ensureDimension`). Its emitting side is one parenthetical: "the artifact's own image emission
files its dims the same way at seal" (`:2932-2933`). Under the doc's own model the image band is
**promote-emitted** — `:2249-2251` "the image's `promote` appends formed a linear `parent` chain"
— and for a promote, a seal-time `getOrCreate` sweep lands a *filed-but-unkeyed* canonical beside
the orphan twin:

- `promote` translates `edge.dim` through `real` (`arena_manifold.cpp:2030-2041`, `dim->second`);
  for an ephemeral session-minted dim `real` holds the freshly minted twin. Filing a canonical
  `d.foo` into the artifact store does not re-resolve the arena's cached `arenaDims_`/`dims_.*`
  refs — nothing in the sweep touches the arena — so the sealed image's edges key on the twin,
  `dimensionNamed` returns the filed cell, and every rank walk under the canonical name misses.
  That is precisely the R16-B split relocated into the image: the program skeleton's
  `d.clause`/`d.vars`/`+d.values`/`+d.spin`/`+d.grab`/`+d.step`/`+d.name`/`+d.entry`/`d.stdlib`/
  `d.contract`/`d.source` ranks, the `d.clone` backlink `masterOf`/`cloneMaster` decodes
  (`arena_manifold.cpp:1009-1012`), all unreadable on receipt.
- It is worse for *content*, which promote copies verbatim (`arena_manifold.cpp:1934-1936`):
  `dimConstant` writes `int64(findOrMintDimension(name))` (`compiler.cpp:769-772`), and `Linked`
  decodes `toInt(inputs[1])` (`vortex_vm.cpp:673`). An ephemeral `DimRef` folds in as a dead
  dense id — and **no genesis arm rewrites it**: (e)'s sweep is bounded to "`learn`-registered
  head's records **and every replay-installed copy's regenerated records**" (`:2313-2314`), and
  declared image heads are never `d.code`-registered (`:2388-2390`, `:2437`). The artifact's
  marked ref cells on declared heads have *no* rehydration pass — the only sound path for
  image-internal `int64` refs is that they already name persistent positions at emission, which
  for dims is exactly what pre-session filing delivers.
- The cited precedent actually demonstrates the *other* emission shape:
  `vql/compiler.cpp:1128-1141` files dims first **because it then mints every cell and link
  through an explicit `cellMap`/`dimMap` remap** — a replication, not a promote. Under
  replication "file first" is sufficient because the remap translates; under promote it is not.
  The doc must pick: either state that artifact emission is the full-remap replication (and say
  how its `int64` contents are translated), or — the natural reading given `:2249` — keep
  promote-emission and state the ordering invariant.

**Fix:** name the invariant once, at §10.1 and §1: *every lineage that will seal `d.*`-keyed
structure files its dim vocabulary on the persistent `d.dims` rank before that lineage's first
`ensureDimension`/`findOrMintDimension`/`mintNamedDimension` resolves the name* — the adapter's
spawn-seed on the receiving lineage; the artifact store's own genesis-equivalent (before the
compile arena's `initGenesis`, `vortex_core.cpp:14,:29-70`) on the emitting lineage. With that
ordering, the artifact's `Linked` dim operands and rank-keyed skeleton arrive verbatim-correct and
"files its dims at seal" becomes the (vacuous) backstop for names first minted mid-session — for
which a seal-time sweep alone still produces a filed-canonical/keyed-twin split and should be
refused or re-resolved, not silently filed.

### 2. MINOR — intra-step ordering and the `getOrCreate` manifold argument are unstated (§10.1 `:2920-2942`)

The comment lists the binds — "program root + agent cell + `d.code`/`d.patch`/… (V10 mechanism,
no-marks path)" (`:2920-2925`) — *before* the filing sentence (`:2926-2938`). Read as sequential
ops, the first `ensureDimension("d.code")` mints the arena orphan, `arenaDims_` caches it, the
filing then mints a second canonical — a one-session split of exactly the kind the step exists to
prevent. Also unstated: `getOrCreate(store, head, manifold, name)` move-assigns its `Manifold&`
(`dimension_registry.cpp:207-214`); the filed cells are visible to the arena only if that argument
*is* the base-injected fold (or the injection happens after filing). **Fix:** "the spawn-seed
files the minted-dim set on the kept lineage `Store` — `getOrCreate` per name against the very
manifold that is base-injected — *before* the bind's first `ensureDimension`, so every bind
re-binds rather than mints".

### 3. MINOR — the filed-set enumeration is a strict subset of the bind list two lines above, and misses the non-`ensureDimension` mints (§10.1 `:2922-2929`)

The bind list names `d.code`/`d.patch`/`d.weight`/`d.synapse`/`d.entry`/`d.patch-head`/`d.validate`/
`d.refs`/`d.ref`/`d.recipe`/`d.refFor`/`d.mesh.relocation` (`:2922-2924`); the filing sentence
enumerates only `d.patch`/`d.code`/`d.refs`/`d.recipe`/`d.refFor`/`d.entry`/`d.weight`/`d.synapse`
plus "every other `ensureDimension`-minted name" (`:2927-2929`). `d.patch-head`, `d.validate`,
`d.ref`, `d.mesh.relocation` ride on the open-ended tail — fine only if the set is stated as *the
complete session-mint vocabulary*: that also sweeps in `attach()`'s `d.stores`/`d.store-refs`
(`arena_manifold.cpp:289-290`) and — the real gap — the **V10 set is not `ensureDimension`-minted
at all** (`mintNamedDimension`, `vortex_core.cpp:50-69`: `d.grab`,`d.step`,`d.spin`,`d.stack`,
`d.contract`,`d.clone`,`d.cursors`,`d.cache`,`d.vars`,`d.values`,`d.pinning-cursors`,`d.name`,
`d.stdlib`,`d.clause`,`d.scan`,`d.stores`,`d.role`). Those are covered only via the image's own
filing (finding 1); the rule should name the coverage once — "every dim name the lineage's arenas
can resolve" — rather than imply the `ensureDimension` subset is the whole set.

### 4. MINOR — the accretion bound now shades stale beside the fix (`§6.6` `:2327-2330`)

"a session-minted dim in use accreting one such orphan per checkpoint … dead weight beside the
re-minted canonical cell" — true only for an *unfiled* session-minted dim; for a filed name no
twin ever mints, so nothing accretes. As written it describes the hazard the filing rule exists
to eliminate, in the present tense. **Fix:** "an *unfiled* session-minted dim in use would accrete
one such orphan per checkpoint" — and note the filed case leaves no twin.

## Nits

- **Multi-segment `d.dims` rank duplication.** `adoptOpRecords` replays rank links verbatim with
  no name dedup; a lineage that ever adopts a second segment carrying its own genesis dims gets
  two rank members per name, and `dimensionNamed`'s `firstOf` binds the earlier while the later
  segment's self-keyed edges name the later — a split the comparand cannot see (both are in-band).
  Only reachable if lineages stack artifacts — U-M11 (`:2448`) territory — worth one clause of
  admission.
- `importValue` still mints received `d.*` cells as unnamed arena cells
  (`mesh_value.cpp:404-411`,`:481`) — the transport-layer instance of the same rule; a received
  graph's dim-keyed edges fold onto orphans on merge. Cross-cutting, as before — but the filing
  fix now makes the asymmetry starker: the lineage files, the wire does not.
- "at image-seal" is used loosely for the filing event throughout (`:95`,`:114`,`:544`,`:1218`,
  `:2241`,`:3182`): the *event* is the spawn-seed (load-time) on the receiving lineage — the
  wording invites the emit-side misreading finding 1 records.

## Structural note

v1.16 is the round the design gets the dim-persistence *mechanism* right — registry-filed,
store-level, no core change, exactly the `vql/compiler.cpp:1128-1141` shape — and gets its
ordering right on the half that was under review (the receiving lineage). The remaining defect is
mirror-symmetric: the artifact's own emission is a lineage too, and the same "file before first
resolution" rule must be stated for it — the deeper invariance the two ceremonies share.

VERDICT: NOT CONVERGED
