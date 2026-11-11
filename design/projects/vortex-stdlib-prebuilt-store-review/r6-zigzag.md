# Round 6 critique: Xanadu/ZigZag Purist (vortex-stdlib-prebuilt-store.md)

Round 6 re-verifies revision 5 against the tree. The graft shape, record topology, private-rank scoping, swap-gate mechanism, and dev-scratch direction are largely sound and honestly bounded. What remains is one internally-inconsistent named path (the only v1 local-image producer), two boundary claims that are weaker than the invariants they assert, and a documentary gap on the purity question this document is being held to.

Tally: 1 BLOCKING, 3 MAJOR, 5 MINOR.

## BLOCKING

### 1. (§6.4, line 873) Dev-scratch `--module` produces a manifest-less, unregisterable image

**Problem:** The named v1 production path for a local image — `vortex-stdlib-gen --module <name>` — is specified as a `WorkerContext` built "without `VortexStdLib` — no bootstrap — mints only the named dev module(s)" (design:873–874). But the `stdlib.image` key record, its private `+d.vars` holder, and the single `"ops"` enumeration are all minted **at `bootstrap()` end** (design:286–289), i.e., inside the very call the dev scratch skips. A stdlib-less scratch that only runs `build*Module` emits module ops and `+d.params` records but never the manifest or the `"ops"` record. The resulting artifact fails §5.1's own validation — a fold carrying no `stdlib.image` key record is `FormatRefused` (design:461–463, the R5-7 rule) — and, worse, §5.2 step 4 has no holder/`"ops"` record to register, contradicting §6.4's assumption that "its own grafted `stdlib.image` holder" exists (design:901–902). The contradiction is structural: `build*Module` are `VortexStdLib` methods, so "without `VortexStdLib`" cannot literally mean no `VortexStdLib` object — it must mean a `VortexStdLib` that runs the named `build*Module`(s) plus the manifest/`"ops"`-record emission tail, minus `bootstrap()`'s base modules. As written, the only way to author a local image in v1 yields an artifact that cannot load and cannot register.

**Evidence:** design:873–874 (dev-only scratch, "no bootstrap", "mints only the named dev module(s)"); design:286–289 (manifest + holder + `keyRecord(holder,"ops",opsHead)` minted "at bootstrap end"); design:284–285 (`opsHead`+`+d.step` chain "minted at bootstrap start"); design:901–902 (local `"ops"` registers via "its own grafted `stdlib.image` holder's private rank"); `vortex_stdlib.cpp:1484–1507` (the `bootstrap()` driver the tail lives in); `vortex_vm.cpp:121–155` (`registerProgramImage` requires an `"ops"` record on the holder's rank and returns `unexpected` without it).

**Fix:** Factor manifest emission out of `bootstrap()` so the dev-only scratch emits it for its module set: specify that `--module` constructs `ArenaManifold`+`VortexCore`+`VortexVM`+a non-bootstrapped `VortexStdLib`, mints `opsHead`+the `+d.step` chain at start, tail-appends each `build*Module`'s op members, then runs the `stdlib.image`/`holder`/`keyRecord` tail — i.e., `bootstrap()`'s record-emission skeleton minus the base `build*Module` calls. State this explicitly; "mints only the named dev module(s)" must be corrected to include the manifest+`"ops"` skeleton or the path is dead on arrival.

## MAJOR

### 2. (§5.2.5/P30, line 597) `outstandingMarks()` conflates the hydration floor with live marks

**Problem:** The `ChoiceStackLive`/`MarksOutstanding` gate is specified as `arena().outstandingMarks() != 0` (design:597,611,1163–1168). But `hydratePrefix` installs a permanent split floor — `outstandingMarks_ = 1` (arena_manifold.cpp:2192) — that is never released and cannot be distinguished from a live choice-stack mark by the raw count. On any hydrated arena the gate reads ≥1 forever: `ChoiceStackLive` refuses every swap, and `MarksOutstanding` refuses every `ensureCells` — directly contradicting the design's claim that a post-swap `ensureCells` reads a fresh arena "where the count is trivially 0" (design:613). This is not hypothetical: the design itself keeps `PrefixSnapshot`/`hydratePrefix` as the §7.3 extra-context seeding mechanism (design:250–255), so the false-positive class is one the document's own machinery can manufacture. Additionally, the gate says nothing about non-VM marks sharing the arena — projection-session checkpoint marks (`projection_session`'s `cps_`), for example — which are a second, unaddressed coverage/false-positive axis depending on which arena they target.

**Evidence:** arena_manifold.cpp:2185–2193 (`hydratePrefix` → `outstandingMarks_ = 1`, `floors_` seeded); design:611–613 ("`ensureCells`'s own entry check reads the *fresh* arena where the count is trivially 0"); design:597–598 (the `!= 0` gate at `bindXanaduStore`/`attachVortexHost`); design:250–255,§7.3 (`hydratePrefix` retained for seeding); `vortex_vm.cpp:317–326` (choice-point marks — the intended covered class).

**Fix:** The gate must count live, undoable marks *above* any hydration floor — e.g., a `liveMarks()`/`outstandingMarks() - floorBaseline` accessor, or tracking the floor as a distinct, named slot so `!= 0` becomes `> 0 above floor`. Alternatively, pin the invariant that `primary_.arena` and the folded image manifold are never `hydratePrefix`-seeded, and say so. Either way, name the projection-session/non-VM mark axis explicitly rather than relying on "stray `mark()` holder" to cover it silently.

### 3. (§5.2.5, line 633) "Caller-managed" engine caches are a purity debt the doc won't name

**Problem:** The boundary clause (design:633–644,1187–1188) honestly lists the arena-keyed caches outside `VortexCore`/`VortexVM`/`VortexStdLib` — `vplEngine_.defaultDim_`, `env_`/`dependencyStack_`/`cellDependencies_`, `vqlEngine_`'s `env_`/`perspectiveClones_`/`functors_`/`sessionMounts_`/`userDefs_`, `vqlCompiler_.varBindings_`, `customActionRoutines_`/`macroRegistry_` user entries — and names the consequence (`env_` dropped not remapped; lookups miss not alias, asserted at §11). What it does **not** do — and what this review requires — is acknowledge that every one of these is a C++ side table holding `CellRef`s, i.e., the exact cells-over-side-tables violation the design is purist about elsewhere (`routineBindings_` is demoted to "a derived index" precisely because "the authoritative copy is manifold data", design:1135). "Caller-managed" reads as "out of scope" when these are *inherited debt the design must interoperate with*, and the doc never states whether the v2 shared-base direction (§7.2) intends to migrate them into cells/dimensions or accept them as permanent exceptions.

**Evidence:** design:633–644,1187–1188 (boundary named, violation not); `vpl_engine.hpp:140–144` (defaultDim_/env_/`cellDependencies_` — `CellRef`-keyed state); `vql_engine.hpp:225–238`; `vql/compiler.hpp:116` (varBindings_); contrast design:1117 (P29 calls `routineBindings_` "C++-only state" as the defect it fixes — the same defect class the boundary tolerates unremarked).

**Fix:** One honest sentence: these caches are pre-existing cells-over-side-tables debt — the same defect class §2.2/P29 eliminates for `routineBindings_` — tolerated in v1 as a caller-managed boundary, with a stated disposition for v2 (migrate to cells under the shared base, or accept as a permanent named exception). The §11 assert already covers the functional half; the missing half is the purity acknowledgment.

### 4. (§5.2/§11.7, line 474) "Bit-identical rollback" is argued over 4 vectors and ignores the untrailed maps

**Problem:** The rollback claim — "rolls back everything — `release()` truncates `slots_`/`links_`/`content_`/`scratch_` … nothing persists past the mark — which is what §11's bit-identical rollback gate requires" (design:474–478) — is justified only over the four mark-covered vectors. `release()` does **not** restore `arenaDims_` (arena_manifold.hpp:847), `boundDimensions_` (:888), `storesRankTail_` (:899), `overlay_`, or the work queues. The graft is safe *today* only because it scrupulously uses rank-filed `findOrMintDimension` (`vortex_core.cpp:99–102`) and never `ensureDimension`/`bindForeignDim` — but that invariant is asserted nowhere, and the in-tree code names the hazard explicitly: `meta.cpp:1499–1507` documents "`arenaDims_` is untrailed" and *refuses a dimension mint under live marks* for exactly this desync (the P17 class). One future `ensureDimension` inside the graft mark silently leaves a `DimRef` in `arenaDims_` pointing at a truncated cell — "bit-identical" while a side-map entry dangles.

**Evidence:** arena_manifold.cpp:1671–1699 (`release()` truncates the 4 vectors + restores live counts/trail/shadow; no `arenaDims_`/`boundDimensions_`/`storesRankTail_` restore); meta.cpp:1499–1516 ("`arenaDims_` is untrailed … a mint under live marks is refused"); vortex_core.cpp:76–102 (`findOrMintDimension`/`mintDimension`/`mintNamedDimension` file on the `d.dims` rank only — trailed, rolled back); design:474–478,1356–1358 (the claim and its gate).

**Fix:** Narrow the claim to "bit-identical over the mark-covered vectors" and assert the no-`ensureDimension`/`bindForeignDim`/`installWriteGuard`-inside-the-mark invariant explicitly — i.e., state that all graft dim mints are rank-filed `findOrMintDimension` so the untrailed side-maps provably go untouched. Strengthen §11.7 to assert `arenaDims_`/`boundDimensions_` are unchanged by a rolled-back graft, not just cell/link counts. That converts an accidental correctness into a stated, testable one — which is what the mark was for.

## MINOR

### 5. (§3.1/P28, line 224) `keyRecord` is correct but its unscoped shared-rank reliance is unstated

**Problem:** `keyRecord` (`makeCell(key)` + `link(rankTail(owner,vars),vars,POS,record)` + `link(record,values,POS,value)`, design:224–226) produces exactly the shape `registerProgramImage` needs — verified — and dropping `+d.owner` is safe for the *private* holder rank. But the `stdlib.image` key record itself rides the *shared* `home +d.vars` rank unscoped, and `varsRecord`'s `owns()` treats a missing `+d.owner` as match-any-owner (records.hpp:103–108). That is harmless today only because `home +d.vars` is documented sparse ("only `stdlib.image` keys ride it", design:905). The doc should pin that `keyRecord` is a private/sparse-rank-only helper — not a `varsRecord` replacement — so a future shared-rank keyed record doesn't inherit an unscoped match-any silently.

**Evidence:** `vortex_vm.cpp:121–155` (the shape is verified compatible); `records.hpp:97–119` (`varsRecord` + `owns()` match-any at :103–108, `+d.owner` rationale at :90–96); design:218–226,905.

**Fix:** One clause: `keyRecord` is used only where the `+d.vars` rank is private (holder) or documented sparse (`home +d.vars`); shared-rank keyed records that must be owner-scoped still go through `varsRecord`.

### 6. (§5.2.5a, line 601) "every produce cursor holds [a mark]" misattributes the mechanism

**Problem:** The gate's coverage argument says "every choice point takes a `core_.arena().mark()` … and every produce cursor holds one" (design:601–603). `spawnCursor` takes no mark (`vortex_vm.cpp:230–244`); the mark is on the `ChoicePoint` the cursor is pushed under (`:320`). The invariant still holds — `goalProduce`/`goalBoundedFirst`/`goalResults` spawn-then-push (`vortex_stdlib.cpp:4206→4210,4246→4247,4271→4272`), so a live produce cursor implies a live choice-point mark — but the mechanism attribution is wrong, and precision matters here because the whole gate is a mark-count argument. Projection-session checkpoint marks sharing the arena are also unaddressed (see MAJOR).

**Evidence:** `vortex_vm.cpp:230–244` (no mark in `spawnCursor`), `:317–326` (`choiceStack_.push_back{...mark=core_.arena().mark()}`); `vortex_stdlib.cpp:4206–4272` (immediate wrap); design:601–603.

**Fix:** Reword to "every choice point and every produce cursor's enclosing choice point takes an `arena().mark()`" — accurate, and it preempts the "does a bare `spawnCursor` slip the gate" question (it does not, because the synchronous `call()` cursors at `vortex_stdlib.cpp:2510,2528` never outlive their call and carry no mark to begin with).

### 7. (§6.5/P29, line 1044) Rank-filed `d.params` is invisible to `dimensionNamed`/`arena.dimensions()`

**Problem:** `d.params` is minted via `findOrMintDimension`, which files on the `d.dims` rank only (vortex_core.cpp:92–102) — it never touches `arenaDims_`. `meta.cpp:1501–1507` names the consequence: "two filing tables exist," and a rank-filed name is invisible to `arena.dimensions()`/`dimensionNamed` (arena_manifold.cpp:34–76 read `arenaDims_`+base only). P26 fixes the `d.mesh.relocation` readers to probe via `findDimension`/`dimensionsOf`-text (design:1044–1050), but the same latent blindness applies to `d.params` and is never extended to it. Under a document base (the `sql_lens` qattr, design:1122–1124) the adoption is harmless because the base files `d.params` in `arenaDims_`; on a base-less grafted arena a `dimensionNamed("d.params")` reader misses.

**Evidence:** `meta.cpp:1501–1516` (two tables + the refusal rationale); `arena_manifold.cpp:34–76` (the `arenaDims_`-only table); `vortex_core.cpp:92–102`; design:1044–1050 (P26 fixes only `d.mesh.relocation`), :1122–1124, :923–924.

**Fix:** Extend P26's "resolve via `findDimension`, never `arena.dimensions()`" rule to `d.params` (and to any new dim the builders mint), or add a line that `d.params` readers are expected to use `core.findDimension` — the same blanket rule the move to `relocation_marks.hpp` already imposes.

### 8. (§4/§5.1, line 290) `expectedScrollBytes=0` under `Cpp` needs its "unexported" scope stated

**Problem:** The `expectedScrollBytes=0` dev placeholder (design:290–295) is coherent only because `Cpp`-mode arenas are never exported and never validated — the field is a placeholder that the shipped image's trailing `SetValue` overwrites. That is true today, but the doc never says `Cpp` arenas are unvalidated; the placeholder reads as a value with a meaning (0 bytes) rather than "unset because never shipped". A verbatim export of a live `Cpp` arena (the deferred U-X8 interactive path) would carry a lying `expectedScrollBytes` through §5.1's check.

**Evidence:** design:290–295, :421–426 (the `size() != expectedScrollBytes` → `ScrollTruncated` validation runs on the folded image, not the live arena); `user_permascroll.hpp:180` (`size()`); design:887–890 (interactive export deferred to U-X8).

**Fix:** One line: `expectedScrollBytes=0` under `Cpp` is an unexported placeholder — the value is only meaningful on an artifact that passed through the generator's trailing `SetValue`, and a `Cpp`-built arena must never be validated or shipped without restating it.

### 9. (P26, line 1050) OpHandle wing slots create a live-arena `findOpHandle` conflation, not just a folded-image one

**Problem:** P26 draws the `findOpHandle`/`versionAnnotation`/`aliases` slot-scan conflation boundary at "the folded image manifold … serves only load→graft" (design:1050–1054). But the marker/OpHandle conversion persists *into the arena* after graft — "Mark edges … replay into the arena like any internal edge — deliberately" (design:495–499) — and `findOpHandle` (`manifold.cpp:1332–1342`) scans `slots` for `valueKind==OpHandle && valueBits==targetOp`. Every grafted wing slot is now an OpHandle cell in the *live* arena, so `findOpHandle`/`versionAnnotationForHandle`/`aliases` on the arena can mis-attribute a wing slot as a real op handle — a conflation the design only owns for the folded image. Compounding, `handleTarget` returns `valueBits` unchecked (`arena_manifold.cpp:1090–1098`).

**Evidence:** `manifold.cpp:1332–1342` (`findOpHandle` slot-scan), `:1350+` (`versionAnnotation`/`aliases`); `arena_manifold.cpp:1090–1098` (`handleTarget` unchecked); design:495–499,1050–1054.

**Fix:** Extend the boundary note to the live arena — grafted OpHandle wing slots now sit inside the scan set of `findOpHandle`/`versionAnnotation`/`aliases`, so either the scans must discriminate (e.g., the `+d.mesh.relocation` marker, or slot role) or the doc names the live-arena mis-attribution as an accepted consequence the way it already accepts it for the image manifold.

## Verified sound (no finding)

- **`keyRecord`/`registerProgramImage` topology** — the three-line helper yields the exact `text=key` + `+d.values→head` + `+d.step` chain `vortex_vm.cpp:121–155` consumes; `+d.owner` omission is safe on the private holder rank and harmless on the sparse `home +d.vars` rank.
- **`sigCell` + `+d.params` walk** — `+d.params→sigCell→"in"`/`"out"→+d.values→+d.step` is structurally realizable; honestly framed as authoritative-records/derived-`routineBindings_` with `rebindInstance` as the (new) consumer. No enumeration leak (`+d.params` is an edge off the sym, not a `+d.vars` member, so `resolve`/`symbolsInModule`/`vortex_param` don't see it).
- **Genesis resync naming** — `mintDimension` correctly uses `rankTail` not `mintNamedDimension`'s adopted-walk `lastDimCell` (`vortex_core.cpp:18–28,76–89`); the skewed-base orphan-tail is correctly named a pre-existing sever-class limitation; `baseDimensionNamed` (:378–380) is the right adopted-dim fallback.
- **`localImage`/`vortex_stdlib_status` purity** — record term + dispatch arm, the `vortex_param/4` precedent; no new arena-state side table. `GLEDITOR_STDLIB_LOCAL`/`LOCAL_DIR` are process config (taken-as-given env), consistent with the named no-store-key policy (design:730–732 — the trust boundary is *document stores*, not env/system-Settings).
- **Swap-gate scoping** — `primary_.arena.outstandingMarks()` correctly covers all VMs/stray marks over that one arena; worker contexts on separate arenas are correctly out of scope for a `primary_.arena` replacement; host-replacement vs `bindManifold` is now honestly split (§5.2.5a/b).

VERDICT: NOT CONVERGED
