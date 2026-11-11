# Round 1 critique: System Architect (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 3 BLOCKING, 6 MAJOR, 7 MINOR.

Note: `vortex_stdlib.cpp` was being edited concurrently during review (5,624 → 6,265
lines); all repo citations re-verified against the latest snapshot.

## BLOCKING

### B1 — §5.1 (lines 233–236): the ops spool cannot open a read-only shipped `ops.nodes`

`SegmentedOpsSpool::openActiveSegment` does `::open(path, O_RDWR | O_CREAT, 0644)` with no
fallback (`segmented_ops_spool.cpp:603–611`); on EACCES it returns false and `Store::load`
throws "cannot read the operations" (`store.cpp:4406–4412`). `make install` writes `-m 644`
assets under root-owned `$(DESTDIR)$(appdir)` (`Makefile:1786,1842–1848`) → non-root `xuzz`
gets EACCES: `ImageThenCpp` silently falls back — dead shipped asset — and `Image` reports
a misleading "cannot read". `SegmentedPrimediaSpool` already solves this (O_RDWR→O_RDONLY
fallback, `readOnly` flag, flush early-return — `segmented_primedia_spool.cpp:220–241,
352–353`; `hpp:167`).

**Fix:** new proposal — `SegmentedOpsSpool::openActiveSegment` gains the `O_RDONLY`
fallback + `readOnly` member; append/flush refuse when readOnly (~30 lines mirroring the
primedia spool). Collapses U-X4 into v1.

### B2 — §5.2 step 3 + P26: ref-valued operand constants are Int64; "no Int64 translation heuristic anywhere" is false

`opref(target)`/`contCell()` are `makeScalarCell(Int64(ref))` — "~37 `opref`/`contCell`
sites in `buildGoalModule` alone" (`vortex_stdlib.cpp:3494–3510`); dim constants are
`lit(Int64(dims().scan/grab))` (`:4057–4059`). Promote writes Int64s verbatim
(`arena_manifold.cpp:1969–1971`); consumers decode via `toInt`/`resolveCell`/`refArg`
across ~40 sites in `vortex_vm.cpp`/`vortex_stdlib.cpp`. Stale values that name live arena
cells misbind *silently* — no control-flow-bearing routine executes correctly post-graft.
Operand cells are also unreachable orphans (`bindInput` links only the slot,
`vortex_core.cpp:418–452`) — the marker edge is load-bearing for *reachability* too.

**Fix:** widen P26 to all ref-valued scalars (marked-Int64 + graft restate, or OpHandle +
`handleTarget` arms at the decode funnels — `resolveCell`, `refArg`, `toInt`-fed sites;
`CellValue` has no OpHandle alternative so arms decode the operand *cell's* kind). Record
the refused alternative.

### B3 — §5 flow, P30, P31, §11.6: `stdlibSource`/`stdlibImageDir` cannot reach the decision

The stdlib decision runs before config exists: `WorkerContext` member-init →
`VortexStdLib` ctor → `bootstrap()` (`worker_context.hpp:59–62`); `setConfig`/
`loadConfigFromStore` arrive later — and `loadConfigFromStore` wholesale-replaces `config_`
(`vortex_host.cpp:169–172`), erasing programmatic settings unless `fromStore` gains
`vortex.*` keys (`system_docs.hpp:135,261–266` convention). `WorkerContext`s are also built
outside `VortexHost` (mesh actors, spool nodes, evaluator runs). §11.6's `stdlibSource=Image`
boot cannot be driven as specified.

**Fix:** lazy decision keyed per graft — `ensureStdlib` resolves image presence under a
default chain (env override + explicit path), with `vortex.stdlib_source`/
`vortex.stdlib_image_dir` settings keys read later as overrides — or plumb `StdlibConfig`
through `VortexHost`/`WorkerContext` ctors.

## MAJOR

### M1 — P31: `assetPath` is not "already a dependency" of the engine layer

`assetDir`/`assetPath` are namespace functions in `src/paths.cpp` which includes
`sdl_compat.hpp` and calls `SDL_GetBasePath` (`src/paths.cpp:14,27,84–102`). Engine binaries
link `$(XUDU_LIBS)` — no SDL — plus hand-picked `src/*.o`; `paths.o` is not among them
(`xuzz_test` `Makefile:1049–1050`; tool links `1123–1301`). Calling `assetPath` inside
`ensureStdlib` pulls `paths.o` + SDL into `xuzz_test`, `vquery`, `vpl`, `vprolog`,
`xudu-swarm-peer`, fuzzers — a layer regression. Only the *inline* `paths::*` helpers are
link-free and used by engine code (`managed_torrent.cpp:8,162`).

**Fix:** resolve the image dir at the app/tool seam, or make the SDL-free parts
header-inline, or add `paths.o` + SDL to every engine link and price it. State which.

### M2 — §5.2 step 4 / P28: the ops manifest cannot be replayed as specified

The record lives on image `home +d.vars` — outside the "+d.stdlib reachable subgraph" step
2 clones. `registerProgramImage` requires `arena().contains(imageRecord)`
(`vortex_vm.cpp:121–126`) and its members are image-space Int64s — verbatim replay either
errors or misregisters colliding cells (unprotecting the write guard,
`installWriteGuard` `vortex_vm.cpp:74–95`).

**Fix:** read the enumeration image-side, translate via graft map, call
`registerProgramOps` on the translated span; or mint a remapped record arena-side.

### M3 — §5.1: image lifecycle/ownership unspecified

"The folded Manifold is held process-wide" — by what? The {permascroll, Store, Manifold}
triple must outlive every context that may graft, including post-`VortexHost` consumers
(actors/nodes/evaluator runs). Interacts with `resetAssetDirForTesting`/`GLEDITOR_ASSET_DIR`
(`src/paths.cpp:99–102`): a cached image folds the *old* asset dir after a test reset.

**Fix:** name the owner (function-local `static` holder + `resetImageForTesting()` hooked
to `resetAssetDirForTesting`) and the open/fold-once discipline.

### M4 — §5.2 step 5 / P33: `rebindCaches` cannot reach all cached refs

`goalHalt_`/`goalSentinel_` minted+cached without export (`vortex_stdlib.cpp:3487–3489`;
`hpp:585–587`) — no `+d.vars` path exists. `routineBindings_` rebuild must `clear()` first
(`bindManifold` re-runs over a fresh arena — stale entries keyed by dead refs accumulate).
Enumerate every `pred*` field's symbol path so the isomorphism gate can assert each rebinding.

### M5 — §4 `--extend`, §6.1, P32: no diff mechanism; append-only records don't update

(a) Nothing computes a structural diff; the machinery that produces a delta is
promote-over-base (cross-store guard at `arena_manifold.cpp:1878–1889`) — name it or defer.
(b) Append-only + first-match = updates never take effect: same-name symbols append on
`+d.vars`, `resolve` returns the *earlier* one; a re-emitted `ops` record is shadowed by
`registerProgramImage`'s first-match too. Release diffs inherit exactly the repoint/delete
question §6.4 defers as U-X1. "~200 lines of driver" prices neither half.

### M6 — P26 cost + P27 cost: the `d.mesh.relocation` premise is false; registry count wrong

`initGenesis` mints 19 dims, no `d.mesh.relocation` (`vortex_core.cpp:52–73`); it exists
only via `findOrMintDimension` in vagent paths or as an unfiled orphan
(`mesh_evaluator.cpp:743`). `zzResolveDim` walks the wrong rank (`d.step` off `d.dims`
cell vs the `d.dims` chain — latent bug). Registry ≈ 21 cells, not 18.

## MINOR

1. (§12) The 8,652-op measurement predates P26/P28/P29 — restate expected total (~13K ops
   ≈ ~900 KiB) or re-measure before keeping "under 1 MiB".
2. (§4) `documentId` is random per `Store` (`document_id.hpp:27,57–62`) → `store.tables`
   differs every run — pin a fixed documentId (and `deviceKeys`).
3. (§5.1) Primedia spool takes a WRITE fd whenever writable + `UserPermascroll` ctor
   `create_directories`s under the asset dir — use the existing `readOnly` flag.
4. (§6) No reset valve for spool growth — regenerating without `--extend` is the
   compaction answer; one sentence.
5. (§5.2/P30) `stdlibPresent()` name-only check (`std:math` module cell) reports
   present-and-broken on partial graft — check a symbol (`resolve("std:math/abs")`) or the
   `ops` record.
6. (P29) "eliminates the last C++ side table" overstates — `routineBindings_` remains a
   derived index; the *authoritative* copy moves to manifold data.
7. Stale line cites across `vortex_stdlib.cpp` (file churning); re-cite at freeze.

Convergent points: `graftImage` *replacing* rather than coexisting with `importModuleFromStore`
is correct — one canonical path, `VortexHost::importLibrary` delegates cleanly. The C++
proposed is genuinely substrate (fold/graft produce the cells Vortex runs on). Store-over-
snapshot and per-context graft are the right calls.

VERDICT: NOT CONVERGED
