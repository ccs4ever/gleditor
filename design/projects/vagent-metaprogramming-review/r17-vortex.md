# Round 17 critique (vortex role): vagent-metaprogramming.md v1.16

Lane: VortexCore/VM execution semantics, store/spool positions, handle
relocation, promote, genesis pass order. Convergence check on the v1.16
deltas: the one-`+d.refs`-rank marking inventory (R16-A), the name-keyed
`dim` endpoint plus `d.dims`-rank filing at image-seal (R16-B), and the
adopted minors (`+d.clone →` first-capture scope, `wingOf` `:366`,
`masterOf`-as-backlink-traversal, `cap$N` re-capture palette, `set_weight`
junction dispatch) — plus a fresh defect hunt over the `git diff`
(b7e2a655→6ee49636, 30 hunks).

## Verification of the r16 dispositions

### R16-A — one `+d.refs` marking inventory: verified, mechanically complete

The unification says: every marked cell — wing slot or `+d.recipe` field —
carries its `+d.ref` record on the head's `+d.refs` rank; the `+d.recipe`
member *is* the field cell, the member carrying only the field's schema
(name → tuple position). Walked end to end, the machinery closes:

- **Emission** (:499-503, :1055-1066, :1086-1090, :2092-2102): the field
  cell is minted `int64(srcPos)` + `d.mesh.relocation→"cell"` marker (same
  carrier as slots) + `+d.ref`→field from a `+d.refs` ref-record — the
  record `+d.name`-absent ⇒ REF-record (:1066), marked under the same
  discriminator. The member keeps schema duties only.
- **Re-kind** (:2131-2134): the `+d.refs` walk hits the field's record,
  follows `+d.ref`→field, `setValueBits(OpHandle, *asInt64(field))` —
  identical to the slot path.
- **Promote BFS**: head → `+d.refs` rank (vocab dim, :1561) → record →
  `+d.ref`→field — `+d.ref` is *not* in the walk vocabulary, but the field
  is `+d.recipe`-reached regardless; `isCellRelocationHandle` →
  `handleTarget` enqueued (`arena_manifold.cpp:1759-1764`); the marker
  edge and `+d.ref`/`+d.recipe` edges fold through `real`-translated
  `setLink` (:2024-2046).
- **Fold**: `OpHandle`→`makeOpHandle(version, isEphemeral(target) ?
  real.at(target) : target, textOf)` (:1946-1951) — the field lands
  `OpHandle`→promoted endpoint; deferred-mint order handles ephemeral
  targets (`:1797-1828`, `isCellRelocationHandle` + `seen` gate :1804-1806).
- **(e)** (:2313-2332): the sweep is now stated covering both arms — "the
  `+d.refs` ref-records the one inventory covering `+d.recipe` field cells
  beside the slots" — per ref-record `Int64(*handleTarget)`, with the
  stated exemption for "a marked cell whose `+d.recipe` side bears
  `+d.name` — the `dim`-field member itself, or the slot's
  `+d.refFor`-paired ref-capture record". The name arm then writes
  `Int64(ensureDimension(name))` "into the record's `+d.refFor`-paired
  slot or its own field cell respectively" — dispatchable: `+d.ref`→cell,
  check `linked(cell, +d.name)` (dim-field) or `linked(cell, -d.refFor)` →
  record → `+d.name` (slot). Complete and enumerable.
- **P16 row** (:2992) and the §6.6 step-1 enumeration (:2085-2102) carry
  the same one-rank rule; no stale "own `+d.recipe` record as inventory"
  survives (grepped: every occurrence corrected or confessed).

### R16-B — name-keyed `dim` endpoints + `d.dims` filing: verified for the filed set; the fold is closed by pre-emption, and the coverage boundary is where the hole now lives

The machinery is real and the cites check:

- `Store::makeDimension` (`store.cpp:1057-1088`) mints the name-texted
  cell and `setLink`s it onto the persistent `d.dims` rank tail
  (:1081-1088) + `registerDim`; `dimsDimension_` is recovered on spool
  replay (:278-285,:328-335), so a `historyFromSeal`-applied lineage store
  accepts `makeDimension` — and `getOrCreate` calls `sliceGenesis` if it
  did not (`dimension_registry.cpp:202-204`).
- `DimensionRegistry::getOrCreate` (`dimension_registry.cpp:194-217`) is
  find-or-mint: `held()` → registry cache →
  `manifold.dimensionNamed(name, store)` — the persistent `d.dims`-rank
  walk (`manifold.hpp:471-478`, "walked posward from home") — then
  `store.makeDimension(head, name)` on miss, rebuilding `manifold` in
  place. Idempotent per name.
- The `vql/compiler.cpp:1128-1141` precedent is accurately cited: the
  "Replicate dimensions" loop maps every arena cell whose text starts
  `d.` through `getOrCreate(store, ver, manifold, name)` and writes
  `cellMap[c] = *dim` — the name→canonical-dim map applied at emission
  into the store, exactly the shape the doc borrows.
- On the arena side the resolution order is base-first everywhere:
  `ArenaManifold::ensureDimension` → `dimensionNamed` →
  `base_->dimensionNamed(name, *reader)` before `arenaDims_`
  (`arena_manifold.cpp:55-84`); `VortexCore::mintNamedDimension` →
  `baseDimensionNamed` before minting (`vortex_core.cpp:17-26`);
  `mintDimension`/`findOrMintDimension` the same (`:72-98`). So a filed
  `D_p` is what every session mint path binds — **no `D_e` is ever
  created for a filed name**, which is why the orphan fold is closed by
  pre-emption rather than by fold-side repair.
- Promote then does the right thing without any dim-awareness:
  `discover(edge.dim, false)` lands `D_p` in `order`; the mint loop's
  `!isEphemeral` arm self-maps `real[D_p] = D_p`
  (`arena_manifold.cpp:1864-1867`); `real.find` passes the gate
  (:2029-2034); the edge folds `x -(D_p)→ y` onto the canonical cell.
  Genesis (b) re-finds `D_p` via the received fold's inherited `d.dims`
  rank (the filing ops are in the lineage spool, sealed per checkpoint).
- The `d.dims` exclusion cites are exact: `textOf(edge.dim) == "d.dims"`
  skip at `arena_manifold.cpp:1766`; the `real.find(edge.dim)` gate at
  :2030-2033 — doubly true, since `d.dims`-dim edges are skipped before
  `discover`, so `dims_.dims` itself is never in `real`.
- The name-arm is uniform across base/session dims: for a filed `D_p`,
  `ensureDimension` returns `D_p` (matches `*handleTarget` after the
  self-map); for an unfiled name it re-mints — the split the doc states.

**Does the `d.dims` filing mechanically close the orphan fold?** For
every dim filed before the session's first `ensureDimension` — yes,
completely and cleanly. For a session mint outside the filed set — no,
and that is finding 1.

### Adopted minors — verified

- **`+d.clone →` first-capture scope** (:2213-2215): "the `d.clone`
  rank's posward-tail append — the binding cell's own `+d.clone →` the
  fragment's `cap$N` slot on a *first* capture, the prior clone's on a
  chained one" — exact against `vortex_vm.cpp:814-828` (`cloneTail = *c`;
  `linked(cloneTail, dims().clone, false)` walk to tail; `link(cloneTail,
  clone, POS, cloneCell)`). Both changelog echoes (:3734-3736, :3753-3757)
  carry the same scoping. ✓
- **`wingOf` `:366`** (:1190-1191): `linked(opcode, dims_.grab, side)` is
  at `vortex_core.cpp:366`; `inputsOf`'s POS delegation is `:373` — the
  changelog's reversal note (:3837-3838) is accurate. ✓
- **`masterOf` restatement** (:2218-2221, :832-835): `cloneMaster` =
  `rankEnd(*this, ref, cloneDim, NEG)` (`arena_manifold.cpp:1007-1010`) —
  a `-d.clone` backlink traversal on a dim outside the `+d` vocabulary,
  correctly restated as "never a `+d` step this walk can take" rather than
  "never a traversal". ✓
- **`cap$N` re-capture palette** (:1226-1230): `New` + `Link` at the
  `d.clone` rank tail — `Link(tail, d.clone, POS, cap)` where `tail` has
  no `+d.clone` forward edge, so M14 appends rather than evicts — the
  `Clone` op's own append shape. ✓
- **`set_weight`/`weight_of` dispatch** (:2574-2582): junction
  (`+d.weight`-bearing `+d.synapse` member) keys off its own scalar; only
  clause targets resolve `+d.name`; the `+d.name`-less refusal is scoped
  to clauses. Consistent with §8.1's "`d.weight` is no rank" (:1579). ✓
- **`(d)` install set** (:2308): now names "the `+d.refs` and `+d.recipe`
  records `reify` regenerated on it". ✓
- **Changelog annotations** (:3538, :3590, :3630, :3669, :3712, :3734,
  :3753, :3768, :3779, :3796, :3850-3893): the `+d.values`-carrier
  superseded markers, "program-side"→"dim" retro-fixes, the first-capture
  scoping and `wingOf` reversal are all confessed per convention. ✓

## New findings (v1.16 deltas only)

### 1. MAJOR — the filing closes the fold only for the pre-filed set; unfiled session-minted dims strand their folded rank members on the twin, and the doc calls the twin "dead weight"

**Location:** §10.1 sketch :2926-2938 ("and every other
ensureDimension-minted name"); §5.2 :544-546; genesis (b) :2239-2243;
(e) :2328-2330; §6.1 :1214-1218; 1.5-changelog scoping :3363-3364.

**Problem.** The spawn-seed step can only file names known at spawn —
the closed apparatus set (`d.code`/`d.patch`/`d.refs`/`d.recipe`/
`d.refFor`/`d.entry`/`d.weight`/`d.synapse`/`d.patch-head`/`d.validate`/
`d.mesh.relocation`) plus whatever the artifact's own emission files.
But `ensureDimension`/`findOrMintDimension` mints names the set cannot
enumerate: a `cells"…"` fragment's `dimConstant` emission mints *user*
dims — `d.foo` — per `compiler.cpp:769-772` → `findOrMintDimension` →
`mintDimension` (`vortex_core.cpp:72-98`), session-side and post-seed.
For those, the v1.16 machinery still does exactly what R16-B diagnosed:
`D_e` folds to `D_t` (`real.at` mint, `arena_manifold.cpp:1948-1950`),
the session's `d.dims` filing edges die at the `:1766` skip, and
`dimensionNamed`/`ensureDimension`/`mintDimension`/`findOrMintDimension`
post-genesis all re-mint `D_p2` — never `D_t`.

The consequence the doc does not state: **edges folded onto `D_t` are
unreachable by every post-genesis channel.** A pre-checkpoint
`cells"…$x -d.foo→ y"` write promotes to `x -(D_t)→ y`; post-genesis the
query op resolves `d.foo` → `D_p2` and `linked(x, D_p2)` misses it — the
folded rank members are stranded live data, not "dead weight"
(:2329-2330). This is also where v1.16 makes the unfiled residual
strictly worse than v1.15: under v1.15's (e) the `dimConstant` slot
rebound `*handleTarget` = `D_t` — inconsistent namespace, but ops read
the rank that actually held the folded edges. Under v1.16's name arm the
slot binds `D_p2` — a canonical cell whose rank is empty, while the only
copies of the folded `d.foo` edges sit on `D_t`. The divergence moved
from name-space to rank-membership; the doc's "dead weight beside the
re-minted canonical cell" sentence covers the cell and not its edges.

And the coverage claim over-reads: "every other ensureDimension-minted
name" (:2928) filed "at image-seal" cannot include names minted
mid-session — there is no later filing event (`ensureDimension`'s own
`d.dims` filing is arena-local and promote-invisible by design;
`ensureDimension` *finds* on the persistent rank but *mints*
arena-local, a distinction :1217's "ensureDimension find-or-mints on the
persistent rank" glosses). The doc contradicts itself: :1576 and
:2328-2330 concede unfiled twins re-mint per checkpoint; :2928 and
:544-546 read as total coverage.

**Evidence:** twin formation — `arena_manifold.cpp:1766`,`:1948-1950`,
`:2029-2034`; name-only resolution — `arena_manifold.cpp:55-84`,
`vortex_core.cpp:17-26,:72-98`; user-dim mint path —
`vagent/compiler.cpp:769-772` → `vortex_core.cpp:95-98`; persistent
filing — `store.cpp:1057-1088`, `dimension_registry.cpp:194-217`.

**Fix.** Either close it or scope it. The adapter-side closure: extend
the end-of-turn ceremony (the same mutable-arena `Executor` seam as the
re-kind) to file every `arenaDims_` entry whose cell is ephemeral onto
the lineage store (`getOrCreate`) and re-key the session's `D_e` edges
onto the filed `D_p` before publish — per-cell `dimensionsOf`
enumeration is C++-side, U-M7 does not bind the adapter; promote then
folds edges onto canonical `D_p` (`real[D_p]=D_p`) and `D_e` folds to a
genuinely dead orphan. The alternative is a P-register dim-aware `real`
(`real[D_e]=D_p` — cleaner, merges the twin, but a core change). If
neither is wanted, scope the claim honestly: the filed set is closed;
session-minted names outside it lose their folded rank members at each
checkpoint — `cells"…"` user dims become session-scoped — and delete
"every other ensureDimension-minted name".

### 2. MINOR — the file-before-pin ordering R16-B silently depends on is never stated

**Location:** §10.1 sketch :2903-2941; genesis (b) :2239-2243.

**Problem.** The fix works only if `getOrCreate` runs before the folded
base is pinned: the arena's `base_` is a snapshot Manifold, "pinned
node-stable for the arena's life" (:2905-2906), and `dimensionNamed`
reads the snapshot — a filing applied after the pin is invisible to the
session and `D_e` mints anyway. `getOrCreate` even hands back the
rebuilt fold it must be (`manifold = std::move(newManifold)`,
`dimension_registry.cpp:207-214`): the natural implementation files all
names, then injects the last returned fold. But the sketch narrates the
sequence base-injection → "binds program root + agent cell +
d.code/…(V10 mechanism)" → "the spawn-seed step FILES the minted-dim
set" — literally after the binding the filing exists to serve. An
implementer reading the comment chronologically injects first and voids
the whole fix silently — the acceptance fixture still passes if it
never exercises a dim read between seed and first checkpoint, and fails
opaquely otherwise.

**Fix.** One clause: "the spawn-seed files the minted-dim set on the
lineage store *before* the folded base is pinned — `getOrCreate`'s
rebuilt fold is the fold to inject; a filing applied after the pin is
invisible to `dimensionNamed`".

### 3. MINOR — the `+d.recipe` member's schema channel ("name → tuple position") is never named; if it rides `+d.name` it M14-collides with the dim name and over-fires (e)'s exemption

**Location:** :387, :1069-1074, :1168-1170, :2097-2098; interaction:
:2321-2327.

**Problem.** The member "carries the field's schema (name → tuple
position)" — which never says *how* a member knows it is the `dim`
field. If the field-role rides `+d.name` (the natural reading of
"name → …"), two failures follow: (a) the `dim` member's `+d.name` is
already taken by the dim name (:1072, :1175-1176, :1214-1215) — one
`+d.name` target per cell, M14; (b) (e)'s exemption keys on "a marked
cell whose `+d.recipe` side bears `+d.name`" (:2321-2323) — under
role-on-`+d.name`, *every* tuple field bears `+d.name`, so the
exemption over-fires and the name arm writes
`Int64(ensureDimension("from"))` into the `from` field — corrupting the
tuple. The schema is consistent only if field-role is positional (the
`+d.step` chain order) or carried on another dim — the `+d.grab`→role-
label precedent exists two diagram-lines up (:1062). The doc relies on
the invariant "no `+d.recipe` member other than the `dim` field and the
dim-referent ref-capture record bears `+d.name`" but never states it.

**Fix.** Name the carrier — "the member's `+d.step` chain position is
its field-role; `+d.name` on a `+d.recipe` member is reserved for the
dim name" — which also pins the (e) discriminator's invariant.

## Nits (non-blocking)

- :2317-2319 — "the marked cell's own promoted target *is* the
  referent's fold position" keeps slot vocabulary for recipe fields,
  whose target is a tuple *endpoint*, not a referent.
- :2099-2101 — "(§6.1's uniform position keyspace)" is attached to "the
  passes stay `+d.refs`-bounded" — boundedness follows from the one
  inventory; the keyspace is what makes the writeback *value* correct.
  Reattach or drop.
- :2086-2087 — "the uniform rule is *every* `makeScalarCell(int64(ref))`
  the emitter writes" sweeps the `dim`-field's `int64(DimRef)` content
  under "ref" — consistent but worth a "(a `DimRef` counts)" since the
  field's int64 is a dim id, not a cell position.

## Summary for the orchestrator

R16-A verifies end to end: the one-`+d.refs`-rank inventory reaches
every marked cell for re-kind/restore/(e) (`arena_manifold.cpp:1759-1764`,
`:1864-1867`, `:1946-1951`; `vortex_vm.cpp` n/a), the `+d.recipe` member
is the field cell with its `+d.ref` on `+d.refs`, and (e)'s stated
exemption + name-arm writeback into "the `+d.refFor`-paired slot or its
own field cell" is dispatchable and complete. R16-B's machinery is real
(`store.cpp:1057-1088`, `dimension_registry.cpp:194-217`,
`vql/compiler.cpp:1128-1141`) and closes the orphan fold **by
pre-emption** for the filed set — `baseDimensionNamed`/`dimensionNamed`
resolve `D_p` before any session mint, `real[D_p]=D_p` folds edges
canonical. But the closure stops at the filed set: session-minted user
dims (`dimConstant`/`findOrMintDimension`) still twin, and their folded
rank members strand on `D_t` unreachable by every post-genesis channel —
the doc's "dead weight" understates stranded data and "every other
ensureDimension-minted name" overclaims spawn-seed coverage (new MAJOR).
Two MINORs: the file-before-pin ordering `getOrCreate` implies is never
stated (post-pin filing is invisible to the snapshot base), and the
`+d.recipe` member's schema channel is unnamed — a `+d.name`-carried
field-role M14-collides with the dim name and over-fires (e)'s
exemption. Adopted minors all land correctly (`vortex_vm.cpp:814-828`,
`vortex_core.cpp:366`, `arena_manifold.cpp:1007-1010`).

VERDICT: NOT CONVERGED
