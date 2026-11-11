# Round 3 critique: Codebase Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/2M/5m.
(Tree verified live: `vortex_stdlib.cpp` now 6,436 lines; `exportStandardLibraryToStore`
`:5674`, `zzResolveDim` `:5959–5974` on the correct `d.dims` rank, `exportSymbol(fail)`
`:4009`.)

All graft architecture anchors verified: `mark`/`discard`/`release`
(`arena_manifold.cpp:1572–1698`), `outstandingMarks()` (`arena_manifold.hpp:750–752`),
`link()` splice eviction (`manifold.cpp:667–688`), `rankTail`/`insertIntoRank`
(`cell_views.hpp:258–262`, `manifold.cpp:739–753`, `arena_manifold.cpp:1423–1439`),
promote `extraRoots` + `d.dims`/`d.lineage` skip (`arena_manifold.cpp:1733–1782`),
OpHandle deferred translation (`:1772–1824,1982–1987`), `registerProgramOps`/
`registerProgramImage` (`vortex_vm.cpp:97–155`), `refCell`/harvest/`seedProgramImage`
(`records.hpp:63–73`, `lineage.cpp:189–249,398–411`), `isCellRelocationHandle` OpHandle
gate (`foreign_op_handle.cpp:100–115`), all second-instance sites, the 3×-bootstrap claim.

## MAJOR

### M1 — §4/P28: "`"ops"` minted at the end of each `build*Module`" reads as 21 same-text records — first-match registers only the earliest module

§3.1 pins a *single* record and P28 costs "one record + ~1,200 member cells", but §4's
literal reading mints one per module → 21 same-text `"ops"` records on `home +d.vars`;
`registerProgramImage` `break`s at the first (`vortex_vm.cpp:131–136`) → 20 modules' ops
never enter `opcodeMap_`. Fails silently — `getOpcodeKind` falls back to content decode
(`:186–196`) so ops still execute; only write-guard protection is missing. Fix: pin the
single-record reading — "the `"ops"` record is minted once on `home +d.vars`; each
`build*Module` tail-appends its op member cells to the `+d.step` chain" — and align P28.

### M2 — §5.2.4/§6.4/§11.5: `registerProgramImage(map[home])` cannot deliver record-scoped registration (fifth independent finding)

Holder-argument first-match walk (`vortex_vm.cpp:121–155`); `map[home]` is shared across
grafts; grafted records tail-append so base's `"ops"` shadows the local's forever; a
foreign `"ops"` ahead on the rank is scanned first — the P28 Problem's own hazard. Fix:
record-addressed path is primary — `linked(map[opsRecord], +d.values)` →
`rankAfter(head, +d.step)` → translate members → `registerProgramOps` (P28's own
"accepted as equivalent" fallback becomes the mechanism); this also makes "absent →
no-op" literal (holder API returns `unexpected`, `vortex_vm.cpp:138–140` — otherwise a
presence precheck is required).

## MINOR

m1. P27 `~:5502` → `~:5674` (`:5502` lands inside `buildBridgeModule`).
m2. P31 `vortex_host_config.cpp` doesn't exist — `fromStore` is `vortex_host.cpp:37–98`,
    `loadConfigFromStore` `:169–172`.
m3. `d.mesh.relocation` has two mint conventions: `mesh_evaluator.cpp:743` mints a bare
    `makeCell("d.mesh.relocation")` orphan (never registered) + bare `"cell"` markers at
    `:755`. Text-based detection tolerates both, but a graft+evaluate arena ends with two
    same-texted dims. Name `:743` for migration to `findOrMintDimension` in P26(c) or
    state non-registry twins tolerated.
m4. `GLEDITOR_STDLIB_SOURCE` value spellings never pinned (`=cpp` vs `=Image` casing
    differs across §11 tests); state accepted spellings + case handling.
m5. Marks-free invariant check-point ambiguous: `graftImage` runs inside a mark so the
    check must read "(at entry, before `graftImage` takes its mark)".

VERDICT: NOT CONVERGED
