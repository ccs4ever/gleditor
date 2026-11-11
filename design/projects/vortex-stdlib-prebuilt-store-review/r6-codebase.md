# Round 6 critique: Codebase Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 1 MAJOR, 5 MINOR.

**Verified load-bearing claims (no defect found):** `bindManifold` declared `vortex_host.hpp:79`, defined `vortex_host.cpp:149–158`, zero in-tree callers; shipped path replaces `vortex_host_` wholesale (`bindXanaduStore` `zigzag_visualizer.cpp:362–394` → `engine_.reset()` :364 → new engine :267/:370 → `ensureVortexHost` :2857–2866 → `make_shared<VortexHost>` :2859 → `bindStore` :2861–2863 → `loadConfigFromStore` `vortex_host.cpp:169–172`); first-context chain `xuzz_app.cpp:1588` → ctor :74 → `populateFallbackStructure` → `adoptDocument` :212/:215–308 (`ownedStore_` :244–247, `engine_` :267, `ensureVortexHost` :306); `WorkerContext` 4th field `stdlib` `worker_context.hpp:59–62` → ctor `bootstrap()` `vortex_stdlib.cpp:1484–1487`, second run `initHostServices` `vortex_host.cpp:117–118`, third per `VPLEngine::ownedStdlib_` `vpl_engine.cpp:54–55,67–68,79–80,92–93`, plus `makeWorkerContext` `:113`; `outstandingMarks()` `arena_manifold.hpp:749–752`; `pushChoicePoint` stores `core_.arena().mark()` `vortex_vm.cpp:317–326` (`ChoicePoint.mark` `vortex_vm.hpp`); produce cursors hold a sentinel choice point `vortex_stdlib.cpp:4210,4247,4272` (sentinel minted `:3488`); `installWriteGuard` last-wins `vortex_vm.cpp:74,84–95`, `vplEngine_` after `primary_` `vortex_host.cpp:100–103` so `ownedVm_`'s guard wins on `primary_.core`; engine caches verified (`vpl_engine.hpp:140–144`, `vql_engine.hpp:225–239`, `vql/compiler.hpp:116`, `vortex_host.hpp:368–369`); `exportSymbol` appends unconditionally `vortex_core.cpp:123–131` (non-idempotent bootstrap claim exact); `findModule` home `+d.stdlib` `vortex_core.cpp:104–110`; `getOpcodeKind` content fallback `:186–196`; `mintOpcode` `:157–166`; wings `vortex_core.cpp:418–452`; `wingOf`/`masterOf` `:373–399`; `runPipeline` `limit=1000` `vortex_vm.cpp:431`; `registerProgramImage` `:121–155` (`rankAfter` walk `:131–137`); scoped `varsRecord` `vagent/records.hpp:97–120` (unconditional `findOrMintDimension("d.owner")` `:101`, `+d.owner` Int64 arm `:113–118`); `finalizeProgramImage` `vagent/fragment.cpp:438–458`; `release()`/`intern()` scratch rollback; `promote` bidirectional + `isEphemeral` gate; `MicroversionId` positional `microversion.hpp:63–205`; `MicroversionId::Segment.number` public `microversion.hpp:73–77`; `zzResolveDim` walks `d.dims` rank `vortex_stdlib.cpp:5959–5974`; `candidatePreds` `:4887–4909`; `vortex_param` wings `:709–781`; `call()` map-miss fallback `:2503–2540`; `exportStandardLibraryToStore` `:5674`; `SovereignStoreLibraryRoundtrip` `tests/xuzz/vortex_stdlib_test.cpp:674`; `SegmentedOpsSpool::openActiveSegment` `O_RDWR|O_CREAT` no fallback `segmented_ops_spool.cpp:603–611`; `Store::load` throw `store.cpp:4406–4412`, `ops.export`→`adoptOpRecords` `:3836`; primedia `O_RDONLY`-on-EACCES `segmented_primedia_spool.cpp:220–241`, `readOnly` member `hpp:167`, silent no-write `flush()` `:352`; `loadConfigFromStore` callers `xuzz_app.cpp:1596`, `:4226`, `vortex_host.cpp:165`; settings arm `xuzz_app.cpp:4219–4228`; `attachVortexHost` zero callers `zigzag_visualizer.cpp:2845–2848` (`hpp:246`); `assetPath` SDL-backed `paths.hpp:42–50`; `Makefile:1786,1842–1848`; `build-msys2.sh:52–54`; `smoke-test.sh:48–55`; `kVortex*` `system_docs.hpp:261–266` + `SettingSpec` block `system_docs.cpp:818–828`; `vortex.stdlib_source` absent today (correctly introduced as new via P31); `std::shared_ptr<State>` consistent (:440,:1222); `VortexStdLib::stdlibStatus()` pinning (:791–793) consistent; `lineage.cpp:564` WorkerContext instance; `manifold.cpp:679–688` 2-degree splice; `pushChoicePoint` `!contains` throw `vortex_vm.cpp:303–306`; `xuzz_vortex_test.cpp:320` explicit bootstrap noted.

## BLOCKING

None.

## MAJOR

### 1. (§4 generator recipe, line 274) `stdlib.bootstrap()` step instructs the double-run it forbids

**Problem:** Step 2 of the generator recipe reads as an imperative post-construction call — "Scratch `WorkerContext` → `stdlib.bootstrap()` builds the arena image — **exactly once**" — but under the doc's own P30 the scratch context's ctor has already run the single build: `WorkerContext` ctor → `ensureStdlib()` → `ensureCells()` → `stdlibPresent()`? no → image arm refused under forced `Cpp` (§5.4 row: `Cpp | — | … | C++`) → `bootstrap()`. A literal reading performs the second run of a non-idempotent builder; the failure is silent — `resolve()` first-matches duplicates — and the shipped artifact would carry doubled symbols/ops.

**Evidence:** Doc :359–375 ctor→`ensureStdlib`→`ensureCells`→`bootstrap()` flow; doc :1192–1193 "The `WorkerContext` ctor calls `ensureStdlib` as today"; doc :1189–1191 "`makeWorkerContext`'s explicit `stdlib.bootstrap()` … becomes `ensureStdlib()` or is dropped — the ctor-time call already covers it, and a bare `bootstrap()` would double-mint over a ctor graft"; `VortexStdLib` ctor → `bootstrap()` `vortex_stdlib.cpp:1484–1487`; unconditional append `exportSymbol` `vortex_core.cpp:123–131`; the existing double-run this doc diagnoses (`vortex_host.cpp:118`, doc :160–164).

**Fix:** One sentence — e.g., "the scratch `WorkerContext`'s **ctor-time `ensureStdlib` under forced `Cpp`** builds the arena image — **exactly once**; *no explicit `bootstrap()` call* (P30: a bare post-ensure `bootstrap()` double-mints, §2.3 item 4)". Or route through `ensureStdlib()` itself, which no-ops via the `stdlibPresent` probe (`resolve("std:math/abs")`, symbol real: `vortex_stdlib.cpp:1572`).

## MINOR

### 1. (§3.1, lines 208–226) `keyRecord` described as new; identical body already exists file-local

**Problem:** The proposed `keyRecord(owner,key,value)` helper — "`makeCell(key)` + `link(rankTail(owner, vars), vars, POS, record)` + `link(record, values, POS, value)` — the `varsRecord` pattern minus the scope arm" — is verbatim `vagent/fragment.cpp:39–49`'s file-local 6-arg `varsRecord(arena, owner, varsDim, valuesDim, key, value)`. The doc cites the call sites (:438–458) and `records.hpp`'s *scoped* 5-arg overload (:97–120) but never the unscoped helper at :39–49, and two same-named functions with different signatures/semantics make "`varsRecord` itself cannot mint them" (line 222) ambiguous.

**Evidence:** `vagent/fragment.cpp:39–49` (`makeCell(key)`, `+d.values` link :43–44, `rankTail` append :45–47); `vagent/records.hpp:97–120` (5-arg, `findOrMintDimension("d.owner")` :101, `+d.owner` arm :113–118); `vortex_vm.cpp:128` comment already names "fragment.cpp's varsRecord shape".

**Fix:** "extract `fragment.cpp:39–49`'s file-local `varsRecord` into the shared records header as `keyRecord`" — name the overloads distinctly at each mention.

### 2. (P34, line 1299) `deploy :578–616` names a symbol that does not exist

**Problem:** "the remaining mutators (`clear()` `:456`, `sealIncremental`/write-back `:528–530`, deploy `:578–616`)" — there is no `deploy` in `user_permascroll.cpp`; the range covers `registerSubscroll` (:561–591) and `ingestSubscroll` (:593–619), whose `copy_file`/`ofstream` write-backs are presumably the mutators meant.

**Evidence:** `user_permascroll.cpp:561–619`; grep finds no `deploy` in the file.

**Fix:** Name `registerSubscroll`/`ingestSubscroll` (and their file writes at `:584–586`, `:612–616`).

### 3. (P34, lines 1300–1301) Grouped cite `store.cpp:4320–4340` covers only one of three named functions

**Problem:** "`Store::save`, `writeSegmentFile`, and `saveOsmicText` (`store.cpp:4320–4340`)" — `saveOsmicText` is `store.cpp:4320`, but `Store::save` is `store.cpp:4093` (its `writeSegmentFile` call `:4125`; the impl is `segmented_ops_spool.cpp:726`).

**Evidence:** `store.cpp:4093,4125,4320`; `segmented_ops_spool.cpp:726` / `segmented_ops_spool.hpp:368`.

**Fix:** Split the citation — `Store::save`/`writeSegmentFile` `store.cpp:4093–4125`, `saveOsmicText` `store.cpp:4320–4340`.

### 4. (§5.4, lines 722–723) "invoked only inside xuzz" omits the test caller

**Problem:** "`loadConfigFromStore` is invoked only inside xuzz (`xuzz_app.cpp:1596,4226`, `vortex_host.cpp:165`)" — `tests/xuzz/vortex_host_test.cpp:181` also calls it (and `:282` calls `loadMacrosFromStore`). Shipped-flow claim stands; the exhaustive phrasing does not.

**Evidence:** `tests/xuzz/vortex_host_test.cpp:181`.

**Fix:** "…in shipped code (`xuzz_app.cpp:1596,4226`, `vortex_host.cpp:165`; plus `vortex_host_test.cpp:181`)".

### 5. (§2.2, lines 108–110) Cached-ref consumer line ranges drift ~80 lines low

**Problem:** `(:4115–4273)` for `goalProduce`/`goalBoundedFirst`/`goalResults` — the family actually spans `vortex_stdlib.cpp:4198–~4310` (sentinel pushes `:4210,:4247,:4272`, `goalResume` `setCursorOpcode(goalFail_)` `:4286`); `:4115` lands inside `buildGoalModule`'s `#GOAL_POS` bind block. The `#GOAL_*` default-halt cite `(:2461–2489)` also misses the first `goalHalt_` use at `:2460`.

**Evidence:** `vortex_stdlib.cpp:4198–4236,4247,4272,4286,2460–2487`.

**Fix:** Re-resolve to `:4198–4310` and `:2460–2489`.

VERDICT: NOT CONVERGED
