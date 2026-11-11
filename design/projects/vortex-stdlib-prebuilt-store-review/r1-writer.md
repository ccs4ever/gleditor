# Round 1 critique: Technical Writer (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 2 BLOCKING, 2 MAJOR, 13 MINOR.

Reviewer's note on evidence stability: `apps/common/xanadu/vortex/vortex_stdlib.cpp` was
visibly mutating during this review (observed total lengths 5274→5755→6116→6263→6265 lines;
`exportStandardLibraryToStore` seen at 5500, ~5015, ~5369, and ~5500 again; `zzPromote`
absent in one snapshot, multiply-defined in another, defined at ~5803–5805 in the latest).
The doc's cited line numbers were plausibly accurate at its baseline — the drift pattern is
consistent with a reordered file, not fabricated references — but they do not match the tree
now, and they must be re-pinned once the implementation churn freezes. Positions below are
as observed at review time.

## BLOCKING

### 1. (§2.3, lines 98, 108; §9 P26 line 416, P30 line 465) Late-file `vortex_stdlib.cpp` evidence anchors are stale — cited lines land on unrelated code

**Problem:** The "measured gaps" section — the doc's central evidence load-bearing claims —
and three register entries cite `vortex_stdlib.cpp` lines that currently point at Xuzz/Xudu
action tables and contract helpers, not at the named functions. `exportStandardLibraryToStore`
is cited at `:5759–5776` (it sits at ~5369–5386, observed also at 5500 in a concurrent
snapshot); `importModuleFromStore` at `:5778` (actual ~5388–5465); its value-copy arm at
`:5794–5799` (actual ~5405–5409); `zzPromote`'s `d.mesh.relocation` resolution at `:5662`
(actual ~5805–5820, with `zzResolveDim("mesh.relocation")` at ~5814). A reader verifying
§2.3's "measured" assertions lands inside `kXuzzActions`/`kXuduActions` — the evidence is
currently false as cited.

**Fix:** Re-run the evidence pass against the frozen file and re-pin all seven anchors.
Given the observed churn, keep the baseline note that citations were measured at a named
commit — or hold the citation refresh until the implementation branch settles.

### 2. (§2.2 line 88; §5.3 lines 279, 282; §9 P29 line 450) Mid-file `call()`/dispatch anchors are stale

**Problem:** The citations pinning `call()`, its `routineBindings_` fast path and
`inputsOf`/`outputsOf` fallback, the `#NAME` label dispatch, and `solveQueryHelper` are all
off by ~50–500 lines in the current file.

**Evidence:** `call` is defined at `vortex_stdlib.cpp:2032`/`:2041`; the map lookup is at
`:2504` and the fallback block runs `:2524–2540`; opName dispatch begins `:2047` (`#ARRAY_`)
with `#GOAL_` at `:2433`; `solveQueryHelper` at `:357`, `is/2` at `:784`.

**Fix:** Re-pin to `call()` ~`:2032–2541`, dispatch ~`:2047–2503`, `solveQueryHelper` ~`:357`.

## MAJOR

### 1. (§3.1 line 156; §4 line 189; §5.2 line 263; §9 P28 line 441) Manifest-record placement, call-site argument, and authorship are mutually inconsistent

**Problem:** §3.1 places the records on `home +d.vars`; P28 says "the image's stdlib root"
(which `exportStandardLibraryToStore` treats as the *first module cell* — ambiguous);
§5.2 step 4 calls `registerProgramImage(opsRecord)` but the API takes the record-*holder*
cell (`vortex_vm.cpp:122–141`); and the `"stdlib.image"` record's authorship is claimed by
no proposal's Change text — P28 names only `"ops"`, P31 only validates it.

**Fix:** Define "stdlib root" once (recommend: the grafted `home`) and use it identically
in §3.1, P28, and §5.2; call `registerProgramImage(map[home])`; widen P28's Change to both
records (or explicitly assign `stdlib.image` authorship to P31).

### 2. (§9 P26, lines 414–416) P26 Cost claim "(it does — `zzPromote` already resolves it)" is false

**Problem:** (a) `zzResolveDim` is resolve-only, never mints (`vortex_stdlib.cpp:5787–5802`);
(b) it walks the `d.dims` cell on `+d.step` — not the `+d.dims` registry rank
(`vortex_core.cpp:24–26, 80–96`); (c) `zzPromote` runs only under `#ZZ_PROMOTE` dispatch —
`bootstrap()` never invokes it. The dimension enters the image's registry rank only via
P26's own `findOrMintDimension`.

**Fix:** Correct the Cost justification: P26's marking calls
`findOrMintDimension("d.mesh.relocation")`; that is what makes it exist and be promotable.

## MINOR

1. (§5 flow) `(§5.1–5.5)` → `(§5.1–5.3)`.
2. (§6.3) `OpsSegmentHeader` field is `formatVersion`, not `format` (`segmented_ops_spool.hpp:101,106`).
3. (P27 Cost) `initGenesis` mints 19 named dims — "~21–22-cell registry rank", not 18.
4. (§5.1/§6.3) `engineEpoch` used but never defined — declare it in §3.1 or §6.3.
5. (§2.3) `SovereignStoreLibraryRoundtrip` is at `tests/xuzz/vortex_stdlib_test.cpp:667`.
6. (§7.2) Federation block is `arena_manifold.hpp:872–886`, not 869–884.
7. (§5.3) `cliArgs_` is set app-side, not at `vortex_host.cpp:162,185`.
8. (§5.3) `std:xuzz`/`std:xudu` actions are `Nop`-minted and dispatched app-side via
   `registerActionRoutine`/`dispatchAction`, not inside `call()`'s label chain.
9. (P32 Fallback) `:export-lib` drives `exportLibrary` (per-module), not `exportStandardLibrary`.
10. (§4) "the constructor shape verified above" dangles — cite `user_permascroll.hpp:103–114`.
11. (P29) `d.params` arrives unannounced as a new dimension — say the generator mints it.
12. `GEMINI.md` cited but absent from the tree — re-point at R11/house rule 1.5. [NOTE:
    orchestrator verified `GEMINI.md` exists at repo root — this finding is refused.]
13. Loose ends: §6.3/§10 table rows run 115–160 chars; `vortex_stdlib.hpp:234–266` cites
    accessors, members are ~589–607; "~5,100 lines" overstates the build functions
    (~3.7–4.7k interleaved); U-X5/U-X6 have no in-text anchor; "§1.2" of the skill file
    resolves to rule 2 — spell it "§1, rule 2".

VERDICT: NOT CONVERGED
