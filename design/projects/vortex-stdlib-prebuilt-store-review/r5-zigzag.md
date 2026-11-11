# Round 5 critique: Xanadu/ZigZag Purist (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/2M/6m. **VERDICT: NOT CONVERGED.**

Verified end-to-end: two-cell holder topology, §5.2.3 class order vs
`link()`/`rankContains`/`insertIntoRank`, mark replay + `isCellRelocationHandle`,
`initGenesis` adopt-or-mint, `SetValue` last-write-wins, image validation, config seam.
R4 dispositions largely honored; purity holds.

## MAJOR

### M1 — The moved harvest/restate helpers cannot see `d.mesh.relocation` in the specified scratch arena

P26's DRY commit moves `refCell`/`markedRelocationSlots`/`harvestRelocationTargets`
verbatim into `relocation_marks.hpp`, but every dim lookup resolves through
`arena.dimensions()` — and the doc mandates both `findOrMintDimension` (files only the
`d.dims` rank, never `arenaDims_`) and a null-base scratch `WorkerContext` where
nothing calls `ensureDimension`. `ArenaManifold::dimensions()` returns
`base_->dimensions()` ∪ `arenaDims_` — the filed name table (`arena_manifold.cpp:34–46`).
So in the scratch arena `arena.dimensions()` is empty → `markedRelocationSlots` returns
nothing (`lineage.cpp:213–221`; same lookup drives `dimOf` :145–149, dense arm :194–203,
`restateRefSlots` guard :150–157, `harvestRelocationTargets` :239–249) → marked-Int64
referents never reach `extraRoots`, dense restate early-returns, ref Int64s promote
verbatim naming source-arena refs → §5.2.3's map-miss rule refuses every marked ref at
graft. The image cannot be produced; P26's two pinned clauses are mutually inconsistent.
Works in vagent today only because `dimensionVocabulary`+`fileNameSet`
(`lineage.cpp:426–461`) pre-file the vocabulary into a *base* store — a null-base
scratch context lacks that privilege. Same latent blindness already in-tree:
`executor.cpp:564,634,2924` probe `arena.dimensionNamed("d.mesh.relocation")` and miss
rank-filed dims. Graft side is immune: `isCellRelocationHandle` resolves by
`dimensionsOf(cell)` text match (`foreign_op_handle.cpp:98–117`).

**Fix:** in P26's DRY clause, moved helpers resolve the mark `DimRef` via
`VortexCore::findDimension`/the `d.dims` rank — or detect marks by `dimensionsOf`-text
match like `isCellRelocationHandle` — never `arena.dimensions()`; state the scratch
generator arena has no `arenaDims_`/`base` dim table so `arena.dimensions()` is not a
legal mark lookup there.

### M2 — "The reachable driver is `bindXanaduStore`" is false; "fixed by the same resync" overclaims

`bindXanaduStore` never reaches `bindManifold`: `engine_.reset()`
(`zigzag_visualizer.cpp:364`) → new `UnifiedTransclusionEngine` (:370) →
`ensureVortexHost` (:392) → `vortex_host_ = make_shared<VortexHost>` (:2857–2867) — a
fresh ctor; `bindManifold` has zero in-tree callers. The resync, `ChoiceStackLive` gate,
and post-swap clears are specified against an entry point no shipped flow exercises;
§11's swap gate is their only coverage. Separately, "(named, fixed by the same resync)"
overclaims the retained-host exposure: a retained `vortex_host_` `shared_ptr` copy
(`vortexHost()`/`attachVortexHost`, :2845–2855) dangles `base_`/`baseManifold_` between
`engine_.reset()` (:364) and member replacement (:392) — the resync never runs on it;
host *replacement* retires the pointer, which happens anyway. The R4-5 disposition said
"out-of-scope-but-noted"; revision 4 upgraded it to "fixed by the same resync", which is
wrong.

**Fix:** correct the driver sentence — document-rebind replaces the host and never
invokes `bindManifold`; the resync serves direct `bindManifold` callers (none shipped)
and the multithreaded-host direction, §11 as sole exercised coverage. Restore
"out-of-scope-but-noted" for retained-host dangling `baseManifold_`/`base_` or pin the
real fix (host invalidation/generation token on retained copies).

## MINOR

m1. "equivalently `linked(member, dim, NEG).has_value()`" is not equivalent to
    `rankContains` — `rankContains` walks both directions from the *anchor* proving
    membership on *that* rank (`manifold.cpp:697–736`); `linked(NEG)` proves only
    non-head on *some* chain on that dim — a member on a disconnected same-dim chain
    would pass and be silently skipped. Drop "equivalently" or scope the shortcut.
m2. `perma->size()` comparison direction unpinned — pin `size() < expectedScrollBytes`
    → `ScrollTruncated`; `>` passes (a grown writable dev-tree scroll is legitimate).
m3. P30 Location omits `vortex_core.{hpp,cpp}` — the genesis resync is necessarily a
    `VortexCore` method (`home_`/`dims_` are private ctor-cached, `vortex_core.cpp:16`,
    `initGenesis` :31–74); the register requires exact files.
m4. The resync inherits `initGenesis`'s mid-rank-insert sever on skewed bases —
    `mintNamedDimension` links at `lastDimCell` (adopted-walk position, not rank tail,
    `vortex_core.cpp:18–28,51–73`); under a document base missing a mid-list genesis
    dim, a fresh mint splices mid-rank and `link()`'s splice clears the displaced
    reciprocal edge, orphaning the base tail in the overlay. Fix: resync mints append
    at `rankTail`; name the skewed-base orphan-tail case a known limitation.
m5. "a later user-document `promote` re-reads them" overstates — `promote` reads marks
    only via the OpHandle arm (`arena_manifold.cpp:1772–1777`); marked Int64s are
    re-read only by mark-aware harvesters (the `relocation_marks.hpp` machinery — the
    same helpers whose rank-blindness is M1). Scope the sentence.
m6. §11 gate 6's "negward … posward" arms are a flat-rank vestige — under the
    private-rank holder the foreign record's shared-rank position is meaningless;
    reword to a single arm: a foreign `"ops"`-texted record minted on `home +d.vars`
    must not register — position is irrelevant by construction.

VERDICT: NOT CONVERGED
