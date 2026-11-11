# Round 7 critique: Codebase Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root. Tally: 9 BLOCKING, 6 MAJOR, 5 MINOR.

This report preserves the critic's classifications. Findings are not accepted until the orchestrator
verifies them; the implementation worktree is changing concurrently. The final MINOR entry is a
verification record, not an actionable defect.

## BLOCKING

### 1. Module inventory and stdlib citation map are wrong

**Problem:** The design lists 21 modules, including nonexistent `std:env`, `std:io`, `std:map`, and
`std:hypertime`. Many stdlib citations are stale; some exceed file length.

**Evidence:** At the critic's reading, `vortex_stdlib.cpp:1459-1497` creates and builds 17 modules.
Constructor: `:1436-1439`; `buildGoalModule`: `:3069`;
`goalProduce`/`goalBoundedFirst`/`goalResults`: `:3782-3856`; `importModuleFromStore`: `:5029`.
Private builders are at `vortex_stdlib.hpp:473-489`, not `:547-567`.

**Fix:** Inventory the actual 17 modules and refresh all stdlib citations by symbol.

### 2. Funnel enumeration names a nonexistent function and miscounts guards

**Problem:** `navigateCellVQL` does not exist. Seven listed sites are described as six unguarded
command paths; one site is guarded.

**Evidence:** `zigzag_visualizer.cpp:3345` is in `translateVQLAndAttachToFocus` (`:3339-3391`);
`compileVQL` calls `ensureVortexHost` only under `if (!vortex_host_)` at `:3394-3397`. The list
`:3345,:3396,:3594,:3618,:3640,:3661,:3690` has seven entries. The replace arm at `:2857-2866` is
unconditional when `engine_` exists.

**Fix:** Use the real function name; separate six unguarded command paths from null-guarded
`compileVQL`, `vortexHost`, and `dispatchAction`.

### 3. Worker arenas cannot be enumerated through the cited public accessors

**Problem:** The gate requires all `extraContexts_` arenas while insisting that existing public
`arena()`/`primaryContext()` accessors suffice.

**Evidence:** `extraContexts_` is private (`vortex_host.hpp:350`). `workerContextCount()` at
`:115-117` gives only a count. `arena()` at `:82-85` and `primaryContext()` at `:101-104` expose
only the primary context.

**Fix:** Propose a host-level aggregate `liveMarks()` method or a context iterator; alternatively
argue a narrower primary-only gate explicitly.

### 4. Decode-boundary constant names do not exist

**Problem:** `kLastContentDecodableOpcode` and `kLastOpcodeKind` are presented as existing constants
but have no definition.

**Evidence:** `vortex_vm.hpp:145-158` contains API declarations. The real inline registration bound
is `OpcodeKind::RunResume` (`vortex_vm.cpp:109-114`); content decoding bounds at `FieldSet` near
`:198`.

**Fix:** Cite the actual comparisons and ordinal values, or explicitly propose named constants.

### 5. isRegistrableOpcode does not exist

**Problem:** Three passages cite a fabricated helper.

**Evidence:** `vortex_vm.cpp:96-123` is `registerProgramOps`; the `asInt64` and `RunResume` checks
are inline at `:108-118`.

**Fix:** Name `registerProgramOps` and its inline predicate, or propose extraction.

### 6. Hydration's seed floor is evictable, not permanent

**Problem:** A fixed baseline of one can become invalid after floor eviction.

**Evidence:** `hydratePrefix` seeds the floor at `arena_manifold.cpp:2183-2188`. `evictOldestMark`
at `:2192-2199` removes the oldest floor and decrements the count.
`VortexVM::stealNearRootChoicePoint` calls it (`vortex_vm.cpp:403-415`, call `:409`). OR-parallel
workers both hydrate and steal.

**Fix:** Track the synthetic floor's actual lifetime, or explicitly exclude hydrated OR-parallel
arenas from the gate. Do not subtract a permanently pinned one.

### 7. VM destructor guard release is conditional

**Problem:** The design says `~VortexVM` unconditionally clears the core guard.

**Evidence:** `vortex_vm.cpp:75-81` clears only under `guardInstalled_`; `vortex_vm.hpp:278-281`
documents the flag's multi-VM rationale.

**Fix:** Explain the existing install-conditional release and distinguish a VM that installed a
guard from one that did not. Audit whether actual guard ownership needs more work.

### 8. promote skips only d.dims, not d.lineage

**Problem:** P27 promises to retain an existing `d.lineage` skip that does not exist.

**Evidence:** `arena_manifold.cpp:1778-1781` skips only edges whose dimension text is `d.dims`;
`d.lineage` has no match in that file.

**Fix:** Correct the premise and either propose lineage exclusion or justify its absence.

### 9. decls.cpp image-record citation is beyond end of file

**Problem:** `decls.cpp:2245-2257` does not exist in the critic's source reading.

**Evidence:** Image record/extent wiring is at `vagent/decls.cpp:2078-2104`; `finalizeProgramImage`
is at `vagent/fragment.cpp:438-459`.

**Fix:** Refresh those citations.

## MAJOR

### 10. overlay\_ is partially trailed; side-map citations are stale

**Problem:** The untrailed-map list overstates `overlay_`'s lack of rollback.

**Evidence:** `release` erases overlay entries via `shadowOrder_` truncation
(`arena_manifold.cpp:1663-1669`). `arenaDims_`, `boundDimensions_`, and `storesRankTail_` are at
`arena_manifold.hpp:836,:877,:888`. The untrailed-dimension comment is at `meta.cpp:1408-1416`;
live-mark refusals are at `:69-72,:1419-1424`.

**Fix:** Distinguish partially restored overlay entries from genuinely untrailed maps.

### 11. Status-ordering citations are stale

**Problem:** The cited ranges no longer identify the status dispatch path.

**Evidence:** At the critic's reading, `solve` is `vortex_stdlib.cpp:4293-4341`, `candidatePreds` is
`:4299-4321`, `vortex_param` dispatch is `:662`, `call` is `:1985`, and predicate export is
`:3018-3019`. Host catch remains `vortex_host.cpp:942-944`.

**Fix:** Refresh by enclosing symbol.

### 12. vicon ownership census is wrong

**Problem:** `apps/vicon/main.cpp:121` constructs `Driver`, not a unique core pointer.

**Evidence:** `vicon/driver.hpp:66-68` owns arena, core, and compiler by value;
`vicon/compiler.hpp:158-160` owns its core reference, VM, and stdlib.

**Fix:** Describe the actual ownership chain.

### 13. Core module/export citations are stale

**Problem:** The underlying non-idempotence claim is correct, but citations drift.

**Evidence:** `getOrCreateModule` is `vortex_core.cpp:110-119`; `exportSymbol`, which always mints
and appends, is `:121-129`.

**Fix:** Refresh the ranges.

### 14. outstandingMarks accessor citation is stale

**Problem:** `arena_manifold.hpp:749-752` is a compact comment, not the accessor.

**Evidence:** `outstandingMarks()` is `:739-741`; count and floors are at `:843-859`.

**Fix:** Correct the citation and identify `liveMarks()` as proposed API.

### 15. Hydration/export direction citations need separation

**Problem:** Hydration range drifts and export/hydrate sites are easy to conflate.

**Evidence:** `hydratePrefix` is `arena_manifold.cpp:2164-2190`. `or_parallel.cpp:210` exports in
`stealChoicePoint`; `:349` hydrates worker arena `:345`.

**Fix:** Cite the two opposite directions explicitly.

## MINOR

### 16. VM citation drift

`spawnCursor`: `vortex_vm.cpp:236-250`; `registerProgramImage`: `:125-159`; missing `"ops"` refusal:
`:142-145`; `installWriteGuard`: `:83-94`; `hasOpcodeKind`: `vortex_vm.hpp:161-163`; runtime
members: `:269-281`.

### 17. Consumer and UI citation drift

`VPLEngine::stdlib_`: `vpl_engine.hpp:136`; three Settings entries: `system_docs.cpp:818-833`;
feedback rendering: `zigzag_visualizer.cpp:2393-2455`.

### 18. rankContains citation names the wrong overload

The graft uses `ArenaManifold::rankContains` (`arena_manifold.cpp:1381-1421`), not
`Manifold::rankContains` (`manifold.cpp:697-737`).

### 19. Private member names blur the proposed access contract

Use "primary context's arena" and "primary VM" when describing access through public APIs; reserve
`primary_.*` for ownership descriptions.

### 20. Verified anchors; no action

The critic verified the substantive host replacement and bind bookkeeping paths, per-context arenas,
non-idempotent exports, unscoped record precedent, relocation consumers, Store/permascroll save and
read-only paths, bare-core consumers, headless early return, asset lookup, packaging targets, and
the principal promotion/handle-translation mechanics. Keep those arguments rather than re-litigating
them without new evidence.

## Critic Conclusion

The architecture and most mechanism reasoning are sound. Convergence is blocked by stale citations
plus nonexistent names and incorrect premises concerning module inventory, worker-arena access,
hydration floors, guard release, and lineage traversal.

VERDICT: NOT CONVERGED
