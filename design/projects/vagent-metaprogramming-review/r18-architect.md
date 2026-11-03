# R18 — System Architect critique (v1.17)

Mandate: (1) verify R17-B landed *correctly* — file-before-pin at spawn (`getOrCreate`'s
rebuilt fold IS the injected base), file-before-first-resolution at emit (before the compile
arena's `initGenesis`), whole-vocabulary enumeration (`mintNamedDimension` V10 /
`d.store-refs` / `dimConstant` included). (2) Hunt a major only in pre-existing mechanisms:
is the seam chain image-seal dims → spawn seed → verify → P16 → genesis → turns closed
end-to-end, or does a layer still hide a needed object/order. Re-verified against the tree:
`dimension_registry.cpp:163-217` (`getOrCreate` — `held` early-return :198-200,
`sliceGenesis` :202-204, `makeDimension` :205, `rebuildManifold` :207, `setStore` :208,
in-place `manifold = std::move(newManifold)` :214, `registerDim` :215);
`store.cpp:1057-1091` (`makeDimension` — `setLink` tail-append on `dimsDimension_` :1077-1088
+ `registerDim` :1089, a real persistent `d.dims` op in the spool);
`manifold.cpp:766-829` (`dimensions()` walks `rankAfter(*home_, *dimsDim_)` :771-775;
`dimensionNamed` registry fast-path gated by `contains` :787-791, rank walk + `registerDim`
:802-820); `arena_manifold.cpp:54-105` (`dimensionNamed` base-first :58-70, `ensureDimension`
find → arenaDims_ → mint + `d.dims` rank append :78-105), `:289-290`,`:366` (`attach`/proxy
`d.stores`/`d.store-refs`), `:1765-1768` (the `d.dims` discovery skip), `:1867-1868` (`real`
identity arm), `:1934-1936` (Int64 verbatim), `:1947-1951` (`OpHandle` → `real.at`),
`:2024-2052` (the `real.find(edge.dim)` emission gate), `:1083-1098` (`asInt64`/`handleTarget`
kind gates); `vortex_core.cpp:14,:16-27,:29-70,:72-98,:348-352,:380-414` (`initGenesis` in
ctor, `mintNamedDimension` re-binds via `baseDimensionNamed` → `arena_.base()->
dimensionNamed`, `findOrMintDimension`, `bindInput`/`bindOutput` always mint
`makeScalarCell(int64(operand))`); `compiler.cpp:769-772` (`dimConstant` =
`int64(findOrMintDimension)`); `vortex_vm.cpp:662-677` (`Linked` decodes `toInt(inputs[1])`
positionally), `:37-67` (`toInt`/`resolveCell`), `vortex_core.cpp:294-311` (`render` falls to
`textOf` for non-numeric kinds); `mesh_value.cpp:415-509` (`importValue` re-mints relocation-
marked cells `OpHandle` naming the local twin :495-500), `:227-228,:241` (`sealLocalSpool`
whole spool); `mesh_actor.cpp:260-273` (resume's `WorkerContext(manifold)` +
`setDefaultSpanReader` post-construction); `vql/compiler.cpp:1128-1174` (replicate-dimensions
precedent — verbatim int64 contents there too, :1150-1151); `mesh_evaluator.cpp:720-736`
(the emit-side `setValueBits(OpHandle)`+marker precedent).

## Verification — R17-B landed correctly on all three clauses

**1. File-before-pin at spawn** (`:119-120`, `:2281-2283`, `:2991-2999`). The text now
states the ordering *and* its mechanism: "FILES … BEFORE the folded base is pinned —
`getOrCreate` move-assigns its `Manifold&` argument (`dimension_registry.cpp:194-217`), so
the rebuilt fold the filing returns IS the base-injected fold". Correct as verified: :214's
in-place move preserves object identity, so the pinned `const Manifold* base_` sees the
filed cells; `setStore(&store)` (:208) makes `Manifold::dimensionNamed`'s store-keyed
registry fast-path (`manifold.cpp:787-791`) and rank walk work on the fold; `held()`
(:198-200) returns early for names already filed in the adopted segment (no re-mint, no
rebuild). The causal order is now fully determined: fold → file → pin → (arena) bind — my
r17 intra-step-ordering minor is discharged, since the binds are arena operations that
require the pinned base.

**2. File-before-first-resolution at emit** (`:118-119`, `:2283-2285`, `:2999-3006`). "the
artifact's own emission files the same vocabulary the same way before ITS lineage's first
resolution — before the compile arena's `initGenesis` (`vortex_core.cpp:14,:29-70`)". Correct:
`initGenesis` runs inside the `VortexCore` ctor (:14); `mintNamedDimension` checks
`baseDimensionNamed` *first* (:18-21 → `arena_.base()->dimensionNamed`, :348-352 → the
store-keyed `manifold.cpp:824-829` overload, which needs `store_` — supplied by
getOrCreate's :208). Filing before the ctor makes every V10 name re-bind the filed cell and
link nothing; `ensureDimension` resolves `base_` first (:58-70); `findOrMintDimension` the
same (:95-98→:73). Consequence the text claims and the code delivers: `dimConstant` writes
`int64(filed DimRef)` — a *persistent* position, verbatim-surviving `promote` (:1934-1936)
and position-stable across adoption (`birthOp = opIndex`, `manifold.cpp:427-442`,
`manifold.hpp:295`); `Linked`'s positional decode (`vortex_vm.cpp:673`) then hits the filed
cell directly. This is exactly the fix my r17 major demanded and the wording is now right.

**3. Whole-vocabulary enumeration** (`:113-121`, `:2277-2281`, `:2980-2989`). "every name
the lineage's arenas can resolve: the bind set … plus attach()'s `d.store-refs`
(`arena_manifold.cpp:289-290`), `initGenesis`'s `mintNamedDimension` V10 names
(`vortex_core.cpp:50-69`), and every `dimConstant`/`findOrMintDimension` mint
(`compiler.cpp:769-772`) — the `d.dims`-rank member set `dimensions()` walks
(`manifold.cpp:771-775`), every mint path appending there". Verified against every mint
path: `mintNamedDimension` appends `d.dims` (:24), `ensureDimension` appends (:91-103),
`makeDimension` appends (:1087-1088) — the same rank `dimensions()` walks (:774-775). I
audited every fixed-name `ensureDimension` site in the tree for coverage completeness:
`d.authorship`/`d.source` (`arena_manifold.cpp:129,:135`) and `d.quote-origin` (:423) mint on
the lineage arena but their cells are `provenanceCells_`-marked and skipped in discovery
(:1771 `isProvenanceCell`) — their edges never fold, so filing is moot; `d.op-label`
(`mesh_value.cpp:473`) mints on the throwaway import arena (`:416`), not the lineage;
`d.or`/`d.branch`/`d.frontier`/`d.submission` (`or_parallel.cpp:181-184`),
`d.actor`/`d.ticket`/`d.signature` (`dim_cap.cpp:83-85`), `d.target`/`d.action`/`d.reply`
(`command_transaction.cpp:16-18`), `d.promoted` (`sql_promotion.cpp:140`) all mint on
non-lineage worker/staging/transaction arenas — the residual class, not the fileable set.
The one fixed runtime name whose edges DO fold is attach()/proxy's `d.store-refs` (:366 —
proxy links reach the fold via `isProxy` handling :1871+), and it is listed. **The concrete
enumeration is complete for fold-reaching edges.**

**4. Residual honesty.** "a name first minted mid-session stays outside the filed set — its
orphan accretion the honest residual" (:3008-3010) composes correctly with (e)'s scoped
"an *unfiled* session-minted dim … re-bound off the filed rank for a filed name, re-minted
only for an unfiled one" (:2376-2380) — my r17 wording landed verbatim. Changelog
(:3990-4028) is faithful, including the retro-edit confessions. Consistent restatements at
`:95`,`:113-121`,`:552-558` — no stale "at seal" survives for the filing event.

## Seam-chain assessment — the emit side still hides a needed pass

image-seal dims → spawn seed → verify (a) → genesis (b)–(e) → turns: the dim half is now
mechanically closed on both lineages. But the *same seam* has a non-dim half — the declared
image's own `int64` ref slots — and there a needed step is still unstated on both sides.

## Findings

### 1. MAJOR — the declared image's non-`dim` `int64` ref slots have no stated finalization: genesis (e) is bounded away from declared heads by the `d.code` invariant, and the emit-side post-fold writeback that could seal them is nowhere named (§6.6 (e) `:2359-2360`; the mark rule `:2115-2130`; §4.5 `:620-621`; genesis `:2196-2201`)

Every operand slot in the sealed image is an emitter-minted `int64` ref cell —
`bindInput`/`bindOutput` write `makeScalarCell(int64(operand))` unconditionally
(`vortex_core.cpp:384`,`:402`), `reference()`/`Jump`/`Branch`/`Choice` targets likewise
(`compiler.cpp:516-518`), `dimConstant` slots too (:769-772). `promote` copies Int64 content
verbatim (`arena_manifold.cpp:1934-1936`), so `int64(ephemeral)` seals dead; the doc's only
machinery for the non-dim class is the P16 mark → `OpHandle` re-kind → genesis
`Int64(*handleTarget)` chain. Follow it for a *declared* cell:

- **Seal `OpHandle`-kind** (marked cells re-kinded before the artifact's own promote,
  `mesh_evaluator.cpp:733-735` precedent): slots arrive `OpHandle(promoted-target)`
  (`:1947-1951`) — and the VM cannot read them: `asInt64` requires `Int64` kind
  (`arena_manifold.cpp:1083-1085`), `render` falls through to `textOf`
  (`vortex_core.cpp:294-311`), so `toInt` yields ~0 — "the VM **never executes**
  `OpHandle`-kind cells" (:2171-2174). They need a genesis `writeValue(slot, Int64)` pass.
- **The only such pass, (e), cannot reach them.** Its stated enumeration is "every
  `learn`-registered head's records **and every replay-installed copy's regenerated
  records**" (`:2359-2360`; P16 register `:3070` repeats "registered heads' *and*
  replay-installed copies' records"). Declared heads are never `d.code`-registered —
  "`d.code` registry (learn-side only)" (`:92`), "the program-side original is never
  registered" (`:1161`), "`run`/`graft_*` never register" (`:2177`) — and *cannot* be:
  the `d.code` reachability invariant exists precisely to keep program-side cells off
  the agent's `d.code` rank (`:1573-1581` — a `d.code` edge to an image head would link
  state→program and drag the skeleton into every checkpoint). The install set is
  replayed `d.patch` copies only. No stated arm enumerates the image's own `+d.refs`
  records.
- **Name-records are no escape** — they are "never re-kinded `OpHandle`" (`:2031-2036`), so
  their slots seal verbatim `int64(ephemeral-calleeEntryOp)`, dead on arrival unless the
  genesis name-rebind fires — the same (e) sweep, the same exclusion. And `Jump`/`Branch`/
  `Choice`/data refs can't ride name-records at all (no symbol to name).
- **`importValue` is no escape either** — its remap re-mints relocation-marked cells
  `OpHandle`-kind naming the local twin (`mesh_value.cpp:495-500`), but `resume`/genesis
  bypass it (`mesh_actor.cpp:260-271`), and the `historyFromSeal`-seeded image bypasses it
  identically (`:1629-1635`). That bypass is (e)'s stated *raison d'être* (`:2384-2390`);
  the image band is subject to the same physics.
- **The session restore can't be borrowed** — "head of the *next* turn" (`:2184`) writes on
  the mutable arena; the artifact emit has no turns. Its store-side analog — a post-fold
  `setValue` pass writing `Int64(real.at(target))` as ops appended inside the band before
  `sealLocalSpool` — is feasible (the emitter owns `real`'s mapping via `Promoted::cells`)
  and is the only shape that leaves the sealed band self-contained — and it is **nowhere
  stated**. The doc's own table requires the image to arrive position-final ("folded back
  verbatim", `:621`,`:2196-2200`); nothing named makes it so.

This is the surviving half of my r17 finding's either/or. The ruling chose
promote-emission + pre-resolution filing — which closes `int64(DimRef)` operands precisely
because filing makes them "seal naming persistent cells" (`:3007`). For every other
image-internal `int64` ref — the `def` bodies' `Call`/`Jump`/`Branch`/`Choice` targets and
wing slots, the declared fragments' internal refs — the promote-verbatim physics is
identical and no stated step produces the final values. An implementer building the
artifact emitter from the text emits either a verbatim-dead band or an `OpHandle` band with
no stated consumer: an unspecified failure at exactly the seam this round froze.

**Fix — pick one and state it:**
(i) *emit-side writeback*: the artifact's own emission ends its promote with a `setValue`
pass over the emitted ref slots writing `Int64(real.at(target))` — name-recorded slots
getting their emission binding `int64(entryOp)`/`int64(head)` — appended inside the sealed
band (the store-side analog of the head-of-turn restore, the same end-state filing gives
`int64(DimRef)`). Declared name-records then keep their emission binding — consistent with
"never registered" — and the doc should say whether post-patch rebinding of *declared*
callers is intended at all (it cannot ride (e): the reachability invariant bars the `d.code`
carrier, so it would need a program-side enumeration such as the `program()` walk).
(ii) *genesis-side coverage*: seal marked slots `OpHandle`-kind and extend the rehydrate
enumeration to the bound `program()`'s `+d.refs`/wing records — noting the records cannot
ride `d.code`, so the pass needs its own carrier (the recorded instruction enumeration's
`+d.grab`/`+d.step` wing walk off each recorded op cell is the natural one — it requires no
`+d.refs` records at all, only kind + target, both already in the band).

### 2. MINOR — "a filing after the pin is invisible to `dimensionNamed`'s base arm" mis-attributes the failure mechanism (§10.1 `:2998-2999`; same parenthetical at `:2282-2283`)

Strictly, a post-pin filing *through the same pinned `Manifold` object* is visible — the
in-place move-assignment mutates the very object `base_` reads. The two real hazards are:
(a) filing on a `Manifold` other than the pinned one (the natural pin-then-file flow), and
(b) filing after `initGenesis`, which froze `home_`/`dims_.*` to arena-minted twins at ctor
time with no re-genesis door (`vortex_core.hpp:268,:282,:287`; the doc itself says so at
`:2967-2969`,`:1672-1679`). The stated rule — file before pin, on the object that becomes
the base — is the correct sufficient invariant; only the *why* is off. **Fix:** "…IS the
base-injected fold — file on any other object and the pin never sees it, file after
`initGenesis` and the frozen `dims_.*` twins never re-resolve".

### 3. MINOR — the concrete enumeration is complete *for fold-reaching edges*, but the doc's coverage sentence reads as a name-census (§10.1 `:2982-2990`; §1 `:113-121`)

"every name the lineage's arenas can resolve" is the right rule and the listed set
(bind names + `d.store-refs` + V10 + `dimConstant`) is verified complete for edges that
can fold. But fixed-name lineage-arena mints exist whose edges cannot fold —
`d.authorship`/`d.source`/`d.quote-origin` (`arena_manifold.cpp:129,:135,:423`), their cells
skipped as provenance at `:1771` — and `d.op-label`, minted on the throwaway import arena
(`mesh_value.cpp:416,:473`). Under a name-census reading they belong in the filed set and
cannot get there (mid-session mints); under the correct reading they are residual by
construction. **Fix:** name the rule by its purpose — "every name whose edges can reach the
fold" — or add the one clause saying provenance/quote/import-scratch dims are excluded
because their cells never fold.

## Nits (carried, still open)

- **Multi-segment `d.dims` rank duplication.** `adoptOpRecords` replays rank links verbatim
  with no name dedup; a lineage stacking two artifacts' genesis dims gets two rank members
  per name and `dimensionNamed`'s `firstOf` binds the earlier while the later segment's
  self-keyed edges name the later. U-M11 (`:2494`) territory; one clause of admission when
  stacked artifacts land.
- **`importValue` still mints received `d.*` cells as unnamed arena cells**
  (`mesh_value.cpp:404-411`,`:481`) — the transport-layer instance of the same rule: a
  received graph's dim-keyed edges fold onto orphans on merge. The filing fix makes the
  asymmetry starker — the lineage files, the wire does not — but the wire's received set is
  unbounded user data, not a fixed vocabulary, so the honest fix is the merge-time
  re-resolution the lineage itself uses; worth naming.

## Structural note

v1.17 is the round the dim seam actually closes — the fix's three clauses are not just
adopted but *right*, verified against every mint path and every read path in the tree. The
remaining defect is the seam's other half: the same "position at seal" invariance the
filing delivers for `int64(DimRef)` has no stated mechanism for `int64(cell)` — the
declared image's wing slots and control-flow targets — where neither emit nor genesis
currently owns the finalization. One sentence of mechanism closes it; leaving it open is
the only thing between this doc and a clean chain.

VERDICT: NOT CONVERGED
