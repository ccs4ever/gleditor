# Round 6 critique: Technical Writer (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 1 BLOCKING, 3 MAJOR, 11 MINOR.

**Verification summary.** Every R5 disposition was re-verified against revision 5 and the current tree; the vast majority landed correctly: the seam placement above `xuzz_app.cpp:1588` is the only correct one (ctor chain verified `zigzag_visualizer.cpp:74 → :212 → :215–308 → :306 → :2857–2866`); `bindManifold`/`attachVortexHost` zero-caller honesty holds (`vortex_host.cpp:149–158`, `zigzag_visualizer.cpp:2844–2848`); rank-blindness is correctly specified in P26 (`arena.dimensions()` is `base_∪arenaDims_` at `arena_manifold.cpp:34–46`, empty on a null-base scratch; `findOrMintDimension` files rank-only `vortex_core.cpp:91–102`); `localImage` precedence `value_or(mode != Mode::Cpp)` (:733–737) and `GLEDITOR_STDLIB_LOCAL_DIR` (:738–740) landed; `vortex.stdlib_source` is correctly kept out of `fromStore`/`loadConfigFromStore` (`vortex_host.cpp:37–98,160–172`) and its startup-only framing is precise; `stdlibStatus()` on the instance with `optional<source>` and the `local` sub-record is specified; `keyRecord`/`sigCell` unscoped shapes are stated; validation order, `size() != expectedScrollBytes`, and the no-manifest `FormatRefused` are in §5.1; the generator pins `StdlibPolicy{mode=Cpp}` + `localImage=false`; all proposed APIs (`wingOf` OpHandle arm, `Config.readOnly`, `keyRecord`, `d.params`, `ensureStdlib`/`rebindInstance`/`rebindCaches`, `vortex_stdlib_status`) are framed as proposals, not existing code. All ~60 spot-checked `file:line` citations resolve to the claimed symbols except where noted below.

## BLOCKING

### 1. (§5.2.5a :596–597 vs :719–721; §1 :34–36; §7.1 :944–947; §11 :1359–1362) The doc concedes `adoptDocument` replaces the host, yet the `ChoiceStackLive` gate omits it and every unconditional `ensureVortexHost()` caller

**Problem:** The spec gates "`bindXanaduStore`/`attachVortexHost` entry" as *the* host-replacement seam (:596–597, repeated :1164–1165, :944–946, :777) and §1 calls `bindXanaduStore → ensureVortexHost → make_shared` "the shipped path" (:34–36). But `ensureVortexHost` replaces `vortex_host_` **unconditionally** whenever `engine_` exists (`zigzag_visualizer.cpp:2857–2866`), and the document's own text concedes this at :719–721: "`attachVortexHost` … is unconditionally replaced at the next `adoptDocument`/`bindXanaduStore`". So the doc *knows* `adoptDocument` is a replacement path — it uses that fact to bound `attachVortexHost`'s bypass — yet never gates it. `adoptDocument` swaps `engine_` at `:267` and calls `ensureVortexHost` at `:306` (`zigzag_visualizer.cpp:215–308`), with at least six real call sites: `adoptXanaduStore` (`:354–360`), `adoptXanaduDocs` (`:424–428`), `populateFallbackStructure` (`:212`), and in xuzz `xuzz_app.cpp:629–631`, `:836–838` (reached from `:708,:726,:747` — routine lens refresh), `:3008–3009` (the `documentSink`). Additionally ~7 command paths call `ensureVortexHost()` unconditionally (`zigzag_visualizer.cpp:3345,3396,3594,3618,3640,3661,3690`). An outgoing host with a suspended produce cursor (its sentinel `ChoicePoint.mark`, `vortex_stdlib.cpp:4210,4247,4272`) is silently destroyed by every lens re-adopt and every `##` command — exactly the "worse than a refusal" case :594–595 says the gate exists to prevent. As specified, the invariant the gate is for fails on the most common paths, and §11 arm (a) asserts a refusal the spec can never produce on those paths.

**Evidence:** `zigzag_visualizer.cpp:2857–2866` (unconditional replace); `adoptDocument` `:215–308` (`engine_ = make_unique` `:267`, `ensureVortexHost()` `:306`); `ensureVortexHost()` call sites `:306,:392,:3345,:3396,:3594,:3618,:3640,:3661,:3690`; null-guarded callers `:2851–2853,:2870–2871`; doc :596–601 vs :719–721 vs :34–36 vs :944–947. (Corroborates r6-architect B1 and r6-ergo M2 on the mechanism; the additional defect is internal: :721 concedes what :596 omits.)

**Fix:** Name the full replacement surface in one place: `bindXanaduStore`/`adoptDocument` (and its public callers `adoptXanaduStore`/`adoptXanaduDocs`)/`attachVortexHost`/`bindManifold` get the entry `outstandingMarks()` refusal; state the policy for command-path `ensureVortexHost` (either ensure-if-null plus a gated replace primitive, or an explicit "command-entry host replacement remains permitted — a suspended produce cursor cannot outlive the next command"). §11 arm (a) then covers all gated paths.

## MAJOR

### 2. (§11 gate 3, :1338–1341) The skew gate drops three arms the R5 disposition required and §5.1 specifies

**Problem:** R5-7/R5-11 required the test plan to cover "truncated/grown permascroll, missing manifest". §5.1 delivers the mechanisms — `size() != expectedScrollBytes` → `ScrollTruncated` *including a grown tail* (:425–428), a manifest-less fold → `FormatRefused` (:432–435), and a span-end fallback that keeps the `ScrollTruncated` cause when names are eaten (:429–432) — but §11 gate 3 asserts only "`truncated scroll (`< expectedScrollBytes`)`". An implementer testing to this list verifies `<` and never exercises `>`, the manifest-absent cause, or the span-end fallback — three deliberately-specified behaviors go unverified.

**Evidence:** doc :426–428 (`!=` semantics, grown = refused deviation), :429–432 (span-end fallback keeps `ScrollTruncated`), :432–435 (no-`stdlib.image` → `FormatRefused`); doc :1338–1341 (gate 3 enumerates `<` only and omits the manifest arm).

**Fix:** Extend gate 3: "`perma->size()` **greater than** `expectedScrollBytes` → `ScrollTruncated` (grown is a deviation, §5.1); a well-formed store carrying no `stdlib.image` record → `FormatRefused`; severe truncation eating the early names → `ScrollTruncated` via the span-end fallback, not a generic missing-manifest error."

### 3. (§6.4 :873–874 vs §4 :283–289 vs §5.1 :432–435) "Without `VortexStdLib` — mints only the named dev module(s)" contradicts the doc's own manifest lifecycle

**Problem:** Cross-section inconsistency: §6.4's dev-scratch builds "without `VortexStdLib` — no bootstrap — mints only the named dev module(s)", but §4 places `opsHead`+`+d.step` chain minting "at bootstrap start" (:284–285), the `stdlib.image` key record + holder "at bootstrap end" (:286–289), and the per-module `+d.params` records "at the end of each `build*Module`" (:283–284) — i.e., all manifest machinery lives inside the `bootstrap()` driver (`vortex_stdlib.cpp:1507–1553`) that a stdlib-less scratch never runs, and `build*Module` are `VortexStdLib` members (`vortex_stdlib.hpp` decls) so "without `VortexStdLib`" cannot be literal anyway. A module-only scratch emits neither manifest nor `"ops"` record, and the artifact fails the doc's own §5.1 `FormatRefused` gate while §6.4 :901–902 assumes "its own grafted `stdlib.image` holder" exists. The named v1 local-image producer is internally inconsistent as written.

**Evidence:** doc :873–874, :284–289, :432–435, :901–902; `vortex_stdlib.cpp:1484–1487` (unconditional ctor `bootstrap()`), `:1507–1553` (the driver the record-emission tail lives in); `vortex_vm.cpp:138–141` (ops-less holder → `unexpected`). (Corroborates r6-zigzag B1 and r6-architect M2; the tech-writer frame is the §4↔§6.4 cross-reference contradiction.)

**Fix:** Reword the scratch to "a `VortexStdLib` constructed in a bootstrap-suppressed mode — `bootstrap()`'s record-emission skeleton (opsHead start, `build*Module` ops tail-append, manifest/holder end) minus the base `build*Module` calls" — and add the missing construction arm to P32.

### 4. (§5.4 :788–789 vs :800–801) The `vortex_stdlib_status` "answers before the use-point throw" ordering is self-contradictory as written

**Problem:** "The throw fires only at public use-points — `call()`/`solve()`/routine entries" (:788–789) vs "its `solveQueryHelper` dispatch arm answers **before** the use-point throw" (:800–801): if the throw fires at `solve()` entry, a dispatch arm inside `solveQueryHelper` — invoked *by* `solve` (`vortex_stdlib.cpp:4881–4926`) — can never precede it. The two sentences are call-order-inverted; whichever arm of the intended behavior an implementer picks, the other's stated text is wrong. (Corroborates r6-architect M3/r6-ergo M3 — filed here because it is a literal contradiction between two adjacent spec sentences.)

**Fix:** Pick one ordering and write it: "the use-point consults the recorded refusal at entry *after* a diagnostic-functor pre-scan: a query consisting solely of `vortex_stdlib_status` goals is answered in-band; any other goal throws the recorded `StdlibImageRefused`."

## MINOR

### 1. (§4 :282; P26 :1030) `mesh_evaluator.cpp:743` → `:727`

The bare `makeCell("d.mesh.relocation")` the doc twice cites is at `apps/common/xanadu/vortex/mesh_evaluator.cpp:727` (consumed at `:739`); `:743` is `vplCell`'s signature. Re-resolve both citations.

### 2. (P26 :1048–1049) `executor.cpp:564,634,2924` drifted

`arena.dimensionNamed("d.mesh.relocation")` probes now sit at `apps/common/xanadu/vagent/executor.cpp:567,:640,:2584`; `:2924` names no probe (`:683,:1552` already use `findOrMintDimension`, consistent with the doc's "probes … and miss" claim).

### 3. (§7.3 :964–965) `or_parallel.cpp:210` is `exportPrefix`; `hydratePrefix` is `:349`

"`exportPrefix`/`hydratePrefix` … already used by `or_parallel.cpp:210`" — :210 calls `exportPrefix`; the `hydratePrefix` call is `or_parallel.cpp:349` (`workerArena.hydratePrefix`). Cite both or name which is meant.

### 4. (§5.4 :679) `mesh_evaluator.cpp:936` → `:920`

The non-host `WorkerContext` construction in `mesh_evaluator.cpp` is `std::make_unique<WorkerContext>(snapshot, reader)` at `:920`; `:936` is `builder.append`. Same class as the sibling-acknowledged drift.

### 5. (§5 :383–384) `vagent/lineage.cpp:564` is not a shared-core second instance

The sentence lists it among instances that "construct their own [VortexStdLib] over a shared `VortexCore`", but `:564` builds a whole `WorkerContext` — its own arena/core/vm/stdlib over a lineage fold — i.e., the *non-host context* class the doc correctly cites it for at :679. Use `vprolog/compiler.hpp:78`, `vicon/compiler.hpp:160`, `vpl_engine.hpp:132–136` for the shared-core claim; keep `:564` for the outside-`VortexHost` claim.

### 6. (§6.5 :922–927 vs P33 :1266–1267) P33's "(visible `symbolsInModule` addition — documented)" pointer dangles

§6.5's honest-delta list names `d.params`/`d.mesh.relocation` dims, wing-slot marker edges, and the `stdlib.image` key record — but not the two new resolvable symbols `std:goal/halt` and `std:goal/sentinel` (currently unexported, `vortex_stdlib.cpp:4009` exports only `fail`). The "documented" claim in P33 refers to a documentation that is absent; add the symbol-visibility delta to §6.5.

### 7. (§7.3 :962–967 vs §5 :389–399) `hydratePrefix` permanently trips the universal marks gate — the interaction is unnamed

`hydratePrefix` sets `outstandingMarks_ = 1` with no releasable `Mark` (`arena_manifold.cpp:2185–2193`), so a hydrated arena can never satisfy `ensureCells`'s entry requirement `outstandingMarks() == 0` — yet §7.3 keeps the machinery as the extra-context *seeding* path and §3.2 :250–255 keeps it "for memory". The doc's own retained mechanism contradicts its universal invariant with no reconciliation stated. (Corroborates r6-zigzag M2; the doc-consistency angle: §7.3's promise and §5's gate cannot both hold.)

### 8. (§3.1 :222–226; P28 :1090–1092) `keyRecord`'s exact body already exists file-local — uncited

`vagent/fragment.cpp:39–49` implements the identical three-line unscoped shape under the same name `varsRecord(arena, owner, varsDim, valuesDim, key, value)` — the helper `finalizeProgramImage` uses at `:443–457` for the very two-cell convention the manifest copies. The doc derives `keyRecord` only from the *scoped* `records.hpp:97–120` overload, making "`varsRecord` itself cannot mint them" read ambiguously across the two same-named functions. Name the unscoped overload as the precedent (it is presumably the natural hoist into the shared header). (Corroborates r6-codebase m1/r6-architect m8.)

### 9. (§5.2.5a :597) `vortex_host_->primary_.arena` names a private member

`primary_` is private (`vortex_host.hpp`, `private:` ~:337, member ~:346); the public surface is `arena()` (`vortex_host.hpp:82–85`) or `primaryContext()` (`:101–104`). The pseudocode should read `vortex_host_->arena().outstandingMarks()`. (Corroborates r6-architect m6.)

### 10. (§5.4 :728) "`every level is taken-as-given (the `assetDir()` convention, `paths.cpp:41–48`)" mis-attributes the convention

`paths.cpp` takes-as-given only `GLEDITOR_ASSET_DIR` (`:45–48`); the portable/installed/`GLEDITOR_DATADIR` levels are `isDirectory`-guarded fall-throughs (`:50–75`). The doc's stricter all-levels rule is a deliberate hardening, not the cited convention — drop the parenthetical or reword "stricter than `assetDir()`". (Corroborates r6-architect m7.)

### 11. (P34 :1299–1301) Two citations name nothing

"`deploy :578–616`" — no `deploy` exists in `user_permascroll.cpp`; the mutators in that range are `registerSubscroll`/`ingestSubscroll` (`:561–619`). And "`Store::save`, `writeSegmentFile`, and `saveOsmicText` (`store.cpp:4320–4340`)" — `saveOsmicText` is `:4320`; `Store::save`/`writeSegmentFile` are `store.cpp:4093–4125`. (Corroborates r6-codebase m2–m3.)

VERDICT: NOT CONVERGED
