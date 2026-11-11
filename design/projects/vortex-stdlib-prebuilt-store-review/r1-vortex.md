# Round 1 critique: Vortex & Spool-Mesh Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 3 BLOCKING, 5 MAJOR, 8 MINOR.

## BLOCKING

### 1. (§3.1 line 158, §5.2 step 4 line 263, P28 line 441) `"ops"` enumeration is in the image's index space; `registerProgramImage` decodes in the arena's

**Problem:** `registerProgramImage` reads each enumeration member's `asInt64` and casts to a
`CellRef` valid *in the calling arena* (`vortex_vm.cpp:121–155`). The enumeration in a
shipped store names store-fold positions; under §5.2's graft they name ephemeral copies —
in a base-backed arena they resolve through `slot()`→`base_` to *unrelated document cells*
(silent registration of wrong cells; `registerProgramOps` is not atomic, `:99–118`), in a
baseless arena `contains` fails loudly after partial registration. The shape's native
habitat is the overlay model — vagent's `seedProgramImage` restates Int64 positions through
the twin map (`lineage.cpp:396–411`) and `registerProgramImage` runs on arenas *overlaying*
the image (`genesis.cpp:518–523, 538`).

**Fix:** Either (a) enumeration members carry the `+d.mesh.relocation → "cell"` mark;
graft step 3 restates each marked Int64 to `map[imageRef]`, then `registerProgramImage`
works verbatim; or (b) OpHandle-valued members + `handleTarget` arm in the member decode.

### 2. (§5.2, P26 lines 405–418) Operand-cell Int64s — `#Jump`/`#Branch`/`#Call`/`#Choice` targets and `contCell` prefills — survive the graft as garbage

**Problem:** Control-flow ops read the operand cell's *rendered scalar*, not the slot:
`#Jump`/`#Branch`/`#Call`/`#Choice` take `toInt(inputs[i])` (`vortex_vm.cpp:1667–1711`);
`buildGoalModule` mints ~40 `opref`/`contCell` `makeScalarCell(Int64(target))` cells
(`vortex_stdlib.cpp:3494–3510`, bound at `:3564–4152`); dim operands are `lit(Int64(dim))`
(`:4056–4059`). Graft copies bits verbatim → arena-epoch garbage. `#Jump`/`#Call` do no
validation → silent wrong-cursor; `#Choice` validates → loud refusal. Every `std:goal`
generator fails or misexecutes.

**Fix:** (a) vagent convention — keep operands Int64, mark `+d.mesh.relocation → "cell"`,
restate at graft (zero VM change); or (b) OpHandle operands + `handleTarget` decode arms at
the `toInt`/`resolveCell` funnels (a real VM change, must be priced).

### 3. (§5.2 step 5, P33 lines 500–509) `goalHalt_`/`goalSentinel_` are unreachable, unexported, and unrebindable

**Problem:** Only `goalFail_` is `exportSymbol`'d (`:4010`); `goalHalt_`/`goalSentinel_` are
linked from nothing — promote's edge walk cannot reach them, so **they are not in the
image**, and no symbol path exists to re-resolve. Post-graft `goalSentinel_` = stale/noCell:
`pushChoicePoint` throws `std::invalid_argument` on `!contains` (`vortex_vm.cpp:303–306`) —
an exception inside `call()`, not a refusal; `getCursorOpcode(cursor) == haltOp` never
matches → produces drain to nothing. §2.2's inventory omits the goal triple and the
`predVortex*` five (`hpp:594–598`; consumed by `candidatePreds` `:4583–4602`).

**Fix:** P33 names every cached ref; image carries anchors — export `std:goal/halt` /
`std:goal/sentinel` symbols or a named vars record the loader reads.

## MAJOR

### 1. (§2.3.1, §12; P26 vs P29 ordering) Parameter cells are link-invisible today; P26 is what puts them in the image at all

**Problem:** Param cells are bare `makeCell()`s named only by wing-slot Int64s
(`vortex_stdlib.cpp:1558–1572,1604–1630`); `promote` discovers cells through edges alone
(`arena_manifold.cpp:1778–1798`) → **the measured 4,296-cell image contains no parameter
cells**. (a) P26's justification should state the `d.mesh.relocation` marker edge is what
pulls operands into the walk (`:1772–1777`) — a reachability fix, not an encoding fix;
(b) P29's `+d.params` records link to exactly these cells — hard dependency on P26;
(c) §12 re-priced: +~1 cell per param + 1 marker edge per slot + opref relocation —
plausibly +30–60% cells.

### 2. (P26 lines 414–416) Marker-dim claim is wrong twice; marker target cell is unspecified

**Problem:** (a) `zzResolveDim` walks `rank(arena, dims().dims, dims().step)`
(`vortex_stdlib.cpp:5787–5802`) — the dims rank is chained on `d.dims` itself
(`mintNamedDimension` `vortex_core.cpp:24–26`, `findDimension` walks `d.dims` `:93`) — a
latent wrong-rank bug; (b) nothing in `bootstrap()` mints `d.mesh.relocation` — vagent
mints it lazily (`fragment.cpp:122`, `records.hpp:70`, `executor.cpp:675`), the scratch
generator never runs those; (c) `isCellRelocationHandle` requires the marker edge to land
on a cell whose text is `"cell"` (`foreign_op_handle.cpp:24–25, 100–106`) — a fresh marker
per slot is 2 objects, not "one extra link".

**Fix:** P26 uses `findOrMintDimension("d.mesh.relocation")` (filed → exported under P27 →
name-mapped at graft) — *not* the unfiled `arena.makeCell` idiom at `mesh_evaluator.cpp:743`
— and a per-context cached `"cell"` marker cell (unless the shared-marker backlink
eviction forces per-slot markers).

### 3. (§5, P30/P33) `ensureStdlib` has no mark-free precondition; graft and `opcodeMap_` interact badly with the trail

**Problem:** `release()` truncates `slots_`/`links_`/`content_`/`scratch_` to the mark
(`arena_manifold.cpp:1657–1674`) — a graft under `outstandingMarks_ > 0` is rollback-able;
`opcodeMap_` is *not* trailed — registering under a live mark then releasing leaves it
holding refs to truncated slots. Call sites are mark-free today, but the invariant is
unstated (vagent learned the same discipline, `executor.cpp:252–257,3024–3029`).

**Fix:** P30 states `ensureStdlib()`/`graftImage()`/`registerProgramImage` run only with
`outstandingMarks_ == 0` (assert or refuse), and re-graft on `bindManifold` runs before any
cursor spawns.

### 4. (§4 `--extend`, §6.1–6.2, §12) The update path has no diff mechanism; changed symbols keep resolving to release n−1

**Problem:** Nothing computes "the module diff": `exportSymbol` appends unconditionally
(`vortex_core.cpp:123–131`) and `resolve()` takes the *first* text match
(`vortex_stdlib.cpp:2005–2012`). A re-minted same-name symbol appends *after* the old one
→ the update silently does not take effect. Even a full re-mint appended to the same spool
keeps old symbols first on `+d.vars`. Replacing a symbol needs repoint semantics or
`ShadowCommit::Clears` emission (`arena_manifold.cpp:2066–2096`) — unspecified.

**Fix:** Defer `--extend`/delta-transport to v2 with the repoint problem named explicitly,
and ship a fresh sealed image per release (the stated default) — or spec the differ and
symbol-shadow semantics as a priced proposal.

### 5. (P26) `bindInput`/`bindOutput`→OpHandle changes `promote()` semantics for every caller, not just the stdlib export

**Problem:** `bindInput`/`bindOutput` have callers beyond the builders —
`mesh_evaluator.cpp:702`, vagent meta/executor compile paths. OpHandle+marker slots drag
operand graphs into every promoted subgraph via the relocation edge
(`arena_manifold.cpp:1772–1777`) — larger exports on `zzPromote`, `exportModuleToStore`,
every user `promote`. Also: the same marker name serves OpHandle-kind relocation (promote's
gate) and vagent's Int64 position convention (`records.hpp:55–70`, `lineage.cpp:189–249`)
— `harvestRelocationTargets` reads `asInt64`, skips OpHandle slots safely, but the doc
should name the distinction. `rawWingSlotsOf` readers see `OpHandle` kind now.

## MINOR

1. Stale citations across `vortex_stdlib.cpp` (file churning mid-review; re-pin at freeze).
2. (P27) Registry rank ≈ 21 cells (19 dims + `d.dims` + `d.mesh.relocation`), not 18.
3. (§3.1 vs P28) Record placement disagreement — pick `home +d.vars`.
4. (§4/P29) `routineBindings_` is `unordered_map` — emit signature records walking `+d.vars`
   ranks (deterministic), consult the map by key only.
5. (§5.2 step 2) Specify clone order = `image.cells()` fold-birth order; do not copy the old
   importer's `unordered_map` re-link (`vortex_stdlib.cpp:5437–5458`).
6. (§12) Latent ceilings worth listing: `call()` `vm_.run(cursor, 100)` (`:2513,2531`) and
   `runPipeline` `limit = 1000` (`vortex_vm.cpp:431`).
7. (P26) `wingOf`'s OpHandle arm should mirror the Int64 arm's `arena_.contains` check —
   `handleTarget` returns bits unchecked (`arena_manifold.cpp:1090–1098`).
8. (§6.4) The local-image graft is a *different* procedure (name-merge) — `graftImage` needs
   a stated merge-mode parameter and a sentence on `d.*` dimension merging.

VERDICT: NOT CONVERGED
