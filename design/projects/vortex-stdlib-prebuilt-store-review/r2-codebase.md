# Round 2 critique: Codebase Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 3 MAJOR, 10 MINOR.

All R1 dispositions landed. API audit clean: `insertIntoRank` (`manifold.cpp:739–753`,
arena overload `arena_manifold.cpp:1423–1439`), `zigzag::rankTail` (`cell_views.hpp:258–266`),
`Manifold::cells()` (`manifold.hpp:407`), `outstandingMarks()` (`arena_manifold.hpp:750–752`),
`MicroversionId::parse`/`str` (`microversion.cpp:110,177`), `UserPermascroll::size()`
(`user_permascroll.hpp:180`), `seedProgramImage` (`lineage.cpp:185–249,390–411`).

## MAJOR

### 1. (§5.1/§4/P34) `UserPermascroll::Config.readOnly` does not exist, and the LMDB `permascrollState` path throws on a read-only dir before the spool's O_RDONLY fallback is ever reached

`Config` has five fields (`user_permascroll.hpp:103–114`) — no `readOnly`. The ctor calls
`permascrollState(config_)` unconditionally (`user_permascroll.cpp:360`) which
`create_directories(publication-state)` (throwing, `:58–59`), `mdb_env_open` RW +
`mdb_txn_begin` (`:66–71` → EACCES throw), and `mdb_put` writes key material on writable
copies (`:148–167`) — all *before* `spool_.openActiveSegment` (`:384`) reaches the
auto-fallback. `chmod 0555` gate fails even after the ops-spool fix.

**Fix:** widen P34: `Config` *gains* `readOnly` (new field); under it skip
`permascrollState` + `create_directories` + write-back; primedia spool honors a
*requested* O_RDONLY (not just EACCES-detected); `Store::save`/`writeSegmentFile` refuse
early on a readOnly spool. Reprice ~60–70 lines.

### 2. (§5.2.4/P28) The all-`"ops"`-records walk is unscoped on a shared rank — foreign records get scanned as opcode enumerations

`home` on a bound arena is the *base* home (`vortex_core.cpp:34–35`); its `+d.vars`
already carries document records; vagent's own `seedProgramImage` writes `"ops"` records
(`lineage.cpp:390`). A foreign `"ops"` record's members are scanned by
`registerProgramOps` (`contains`+`isRegistrableOpcode` only, non-atomic
`vortex_vm.cpp:100–117`) → the wrong-space misregistration P28 exists to kill,
reintroduced at rank level; a malformed foreign member aborts the walk entirely.

**Fix:** register the graft's *mapped* ops records (`map[imageOpsRecord]` per image), not
every `"ops"`-texted record on `home +d.vars`.

### 3. (P29/§5.3/P30) `rebindInstance` rebuilds `routineBindings_` from `+d.params` records, but who emits them in `Cpp` mode is unspecified — the specified fallback can empty the map

If record emission is generator-only, `clear()`+rebuild finds nothing under
`Cpp`/`ImageThenCpp` → `clamp` wrong-arity reborn. State that emission lives inside
`build*Module` (self-describing arenas, uniform rebuild), or that builder-populated
bindings survive when no records exist.

## MINOR

1. `insertIntoRank(cell, anchor, dim, dir)` inserts anchor-adjacent → head-insert
   reverses batch order; pin `rankTail` anchoring; cite `ArenaManifold::insertIntoRank`.
2. `outstandingMarks_` is private — the check uses `outstandingMarks()` (`arena_manifold.hpp:750`).
3. `zzResolveDim` already walks the correct rank — drop/fix §2.3.9.
4. `std:collections` duplicated in §1's list; "std:memoize-first" is mint order
   (`:1508–1530`), `build*Module` calls run math-first (`:1532`).
5. `:export-lib` images carry no `"ops"`/`stdlib.image` records — state whether absent
   manifests are tolerated (`registerProgramImage` returns `unexpected` without one).
6. `vortex.stdlib_*` keys touch `VortexHostConfig::fromStore` + `settings::` constants
   (`system_docs.hpp:262–266`) — existing files missing from P31's Location.
7. `exportLibrary` is `vortex_host.cpp:505`, not `~:533`.
8. `SovereignStoreLibraryRoundtrip` is at `tests/xuzz/vortex_stdlib_test.cpp:674`.
9. §5.2.3 membership bullet lists `d.dims` among append-able anchors while step 1 says
   never replay — reconcile (`findOrMintDimension` appends internally).
10. `refCell` (`records.hpp:63–72`) mints a *fresh* `"cell"` marker per call — per-ref is
    the precedent; a shared marker is a divergence worth naming [see zigzag B1 — it is
    actually impossible under `link()` splice].

VERDICT: NOT CONVERGED
