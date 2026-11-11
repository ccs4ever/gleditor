# Round 6 critique: Programming Language Design & Ergonomics (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 1 BLOCKING, 3 MAJOR, 7 MINOR.

## BLOCKING

### 1. (§5.2.5a, lines 596–601; §5.4, line 777; §11, lines 1359–1361) `ChoiceStackLive` has no delivery channel — callers desync the "old document retained" promise

**Problem:** The design puts the liveness gate at `bindXanaduStore`/`attachVortexHost` entry and promises "refuses the rebind with a named cause … the old host, engine, and bound document are all retained." But `bindXanaduStore` returns `void`, and the doc never specifies how the refusal reaches the caller. Two readings are both broken: (a) `bindXanaduStore` early-returns and merely *records* the refusal — then no caller can observe it; (b) it throws `StdlibImageRefused` — which propagates through `bindZigzag` into `views.documentActivated`/key-dispatch with no catch in the chain, i.e. terminate. Worse, under reading (a) the stated contract is already false: all 9 `bindZigzag` call sites run their bookkeeping unconditionally after the bind, so a refused rebind leaves the visualizer showing document M while `zigzagStoreIndex`, `sliceHeads`, `map`, `linkContext`, `bridgeCoordinator`, and `views.anchorPresentation` all claim document N. The §11 swap gate's arm (a) also cannot *observe* the refusal it asserts ("refuses `ChoiceStackLive` at entry") through a `void` API. House rule (AGENTS.md): "one that can refuse returns `std::expected` of it" — the signature must change, and every caller path must honor it.

**Evidence:**
- Doc (:596–601): "the `ChoiceStackLive` gate sits at `bindXanaduStore`/`attachVortexHost` entry: `vortex_host_->primary_.arena.outstandingMarks() != 0` checked *before* `engine_.reset()`/the host drop refuses the rebind with a named cause … (the recovery contract: drain or close the produce cursors, then re-issue the switch — the old host, engine, and bound document are all retained)" — mechanism of "refuses" never named.
- `apps/common/ui/slice/zigzag_visualizer.cpp:362–394` — `void ZigzagVisualizer::bindXanaduStore(...)`: `engine_.reset()` at :364, `engine_ = std::make_unique<…>` at :370, `ensureVortexHost()` at :392. `void` return.
- `apps/xuzz/xuzz_app.cpp:1826–1841` — `bindZigzag` calls `zigzagPresentation->bindXanaduStore(...)` at :1827, then unconditionally: `zigzagStoreIndex = storeIndex` (:1829), `sliceHeads[storeIndex] = version` (:1830), `map.setStoreIndex/setCurrent` (:1831–1832), `linkContext.setManifold` (:1833), `bridgeCoordinator.synchronize()` (:1834), `views.anchorPresentation` (:1838). 9 call sites (:1892, :1969, :2103, :2177, :2308, :2663, :3021, :3786, :4018).
- `apps/common/xanadu/vortex/vortex_host.hpp:79` — `void bindManifold(...)` — same void-refusal problem on the in-place path ("the `bindManifold` path records identically", doc :782–783 — recorded on the instance, observable only via `stdlibStatus()`).
- The refusal surface already exists — `commandBarFeedback_` is set by every execute handler and rendered in the HUD (`zigzag_visualizer.cpp:3590–3694`, rendered ~:2393–2446) — but no bind path assigns to it, and the doc only wires the command bar for *init-time* `Image`-mode refusals (doc :807–808), not mid-session switch refusals.
- `tests/zigzag/test_visualizer.cpp:80–88, 91–100` — the test harness calls `bindXanaduStore` directly; arm (a) is executable but has no refusal value to assert on.

**Fix:** Change the signature and update the call chain: `std::expected<void, StdlibImageRefused> ZigzagVisualizer::bindXanaduStore(...)` (and `bindManifold` likewise), per the house `std::expected` rule. On the unexpected arm: `bindZigzag` must skip *all* of :1829–1841 (index/map/link-context/anchor bookkeeping), set `commandBarFeedback_` to the named cause + recovery ("document switch refused — N live produce cursor(s); drain or cancel the producing flow, then retry"), and return. Name `adoptDocument` (`zigzag_visualizer.cpp:215–308`, which reassigns `engine_` at :267 before `ensureVortexHost` at :306) as a third gated rebind path with the same contract. §11 arm (a) then asserts `EXPECT_FALSE(result.has_value())` + cause + old-document retention via `zigzagStoreIndex`/`sliceHead()` unchanged.

## MAJOR

### 2. (§5.2.5a, lines 596–601; §7.1, lines 944–947) The real host-destruction point `ensureVortexHost` is ungated and unremarked — the recovery contract is silently bypassable

**Problem:** The doc gates `bindXanaduStore`/`attachVortexHost`/`bindManifold` entry, but `vortex_host_` is actually destroyed inside `ensureVortexHost` — `vortex_host_ = std::make_shared<VortexHost>` at :2859, *unconditional* whenever `engine_` exists — and `ensureVortexHost()` is called directly, with no `!vortex_host_` guard, from at least seven command paths: `navigateVQL` (:3594), `executeVQLScript` (:3618), `executeVPL` (:3640), `executeLogicQuery` (:3661), `defineMacro` (:3690), `navigateCellVQL` (:3345), and the const cursor path (:3396). Consequence: a suspended produce cursor (its arena marks live) survives a document-switch *refusal*, only to be silently destroyed by the very next `##`/`>` command — today's status-quo behavior, exactly the "worse than a refusal" destruction (doc :594–596) the gate exists to prevent, still running one seam away. It also gives users an unadvertised bypass of "drain or close the produce cursors, then re-issue": run any logic command and the marks die silently. The doc needs to either gate `ensureVortexHost` itself or state that per-command host replacement remains permitted by contract — and in the latter case admit that produce cursors cannot, by design, outlive the next command, which narrows who the refusal's recovery contract can possibly serve (the in-flight flow that triggered a mid-command rebind).

**Evidence:**
- `apps/common/ui/slice/zigzag_visualizer.cpp:2857–2866` — `ensureVortexHost()` replaces `vortex_host_` unconditionally under `if (engine_)`; no liveness check, no `!vortex_host_` guard inside.
- Unconditional call sites: `zigzag_visualizer.cpp:306` (`adoptDocument`), :392 (`bindXanaduStore`), :3345, :3396, :3594, :3618, :3640, :3661, :3690. Only `vortexHost()` (:2851–2853) and `dispatchAction` (:2870–2871) guard with `if (!vortex_host_)`.
- `apps/common/xanadu/vortex/vortex_host.cpp:100–118` — each fresh `VortexHost` ctor runs `VortexStdLib::bootstrap()` (`vortex_stdlib.cpp:1484–1486` ctor call + `initHostServices` :118) plus `ownedStdlib_` — i.e., per-command host replacement is also why bootstrap runs 3× per *command* today (the §6.5 triple-bootstrap is per `execute*`, not just per document).
- Marks are arena-level: `vortex_vm.cpp:317–326` (`pushChoicePoint` → `arena().mark()`); `ArenaManifold::outstandingMarks()` at `arena_manifold.hpp:749–752`; produce-cursor flow `vortex_stdlib.cpp:4198–4291`.

**Fix:** Name `ensureVortexHost` as the replacement primitive and split the gate by caller: (a) `bindXanaduStore`/`adoptDocument`/`attachVortexHost`/`bindManifold` — entry `outstandingMarks() != 0` refuses the *rebind* (document retention); (b) command-path `ensureVortexHost` — either refuse to *replace* while marks live and proceed on the retained host (strictly better than today's silent kill), or state plainly: "host replacement at command entry remains permitted; a suspended produce cursor cannot outlive the next command — `ChoiceStackLive` exists for mid-command rebinds, and its payload's audience is the in-flight flow." Whichever is chosen, §11 gains a third arm: produce cursor suspended → `executeLogicQuery` → assert documented behavior.

### 3. (§5.4, lines 781–811) The `vortex_stdlib_status` "arm before throw" ordering is impossible as written — pin the exemption at the use-point entry

**Problem:** The doc says "The throw fires only at public use-points — `call()`/`solve()`/routine entries" (:788–789) and simultaneously that the builtin's "`solveQueryHelper` dispatch arm answers **before** the use-point throw" (:800–801). If the throw fires at `solve()`/`solveQuery`/`call` *entry*, a dispatch arm inside `solveQueryHelper` — invoked *by* `solve` (`vortex_stdlib.cpp:4926`) — can never precede it; the builtin is unreachable under `Image`/`MarksOutstanding` hard refusal, "exactly the state it exists to diagnose" (the doc's own R5-5 phrasing). The good news the doc can use: builtin dispatch is **functor-text** matching (`goalFunctor == "vortex_param"` at :710, from `stdlib.functorOf(curGoal.term)` at :401), so `vortex_stdlib_status` needs no exported symbol to dispatch — the exemption is cheap — but it must live *at* the use-point (pre-scan the goal span for diagnostic-only functors) or the throw must be deferred to per-goal routine entry. Also note `call("std:logic/vortex_stdlib_status")` can never work under refusal — `resolve()` honestly returns `nullopt` (doc :789–790) → `{}` — so the builtin is solver-goal-only; say so.

**Evidence:**
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:401–404` — functor text extracted per goal; :709–710 — `if (goalFunctor == "vortex_param" && goalArgs.size() == 4)` — pure text dispatch.
- `vortex_stdlib.cpp:4881–4928` — `solve` builds `candidatePreds` and calls `solveQueryHelper`; an entry-level refusal throw would fire before :4926.
- `vortex_host.cpp:932–945` — `solveLogic` catches `std::exception` → `std::cerr` → returns `{}`; a thrown `StdlibImageRefused` surfaces in xuzz only as "no solutions"/stderr, never as the status record.
- `apps/common/xanadu/vprolog/compiler.cpp:237,244` — vprolog reaches `solveQuery`/`solveOnce` — the same use-point boundary.

**Fix:** Pin the ordering: "`solve`/`solveQuery`/`solveOnce` consult the recorded refusal at entry; if every goal's functor is `vortex_stdlib_status` (arity 1, text match — no stdlib symbol required), the query is answered from the recorded status without firing the throw. `call(path)`/`solve` of any other goal throws the recorded `StdlibImageRefused`." One sentence, and the R5-5 fix actually holds.

### 4. (§6.4, lines 856–893; §5.4, lines 733–740) The dev loop's `--out` and `GLEDITOR_STDLIB_LOCAL_DIR` are two ends of one path — and an explicit `GLEDITOR_STDLIB_LOCAL=1` + absent dir skips *silently*

**Problem:** The dev loop is "edit `build*Module` → `vortex-stdlib-gen --module std:mymod --out <local-dir>` → restart" (:884–886), but `<local-dir>` must equal the directory the *app* resolves — `paths::dataPath("xuzz","vortex-stdlib-local/")` or `$GLEDITOR_STDLIB_LOCAL_DIR` (:738–740). The doc never says `--out` names the image directory itself (vs. a parent), nor that the app must see the same path. A dev who writes `--out /tmp/patch` and launches without `GLEDITOR_STDLIB_LOCAL_DIR=/tmp/patch` gets the default dir → absent → "absence skips silently (absence is the default)" (:866–867) — no warning, nothing logged, and on `vpl`/`vagent`/`vicon` (no `solve*` surface, :803–806) no way to see `local.present=false`/`local.dirResolved` at all. The explicit-opt-in case deserves the same logic the doc applies to `image absent` (:773–775: "absence is *the* packaging failure an installed app most needs flagged"): `GLEDITOR_STDLIB_LOCAL=1` + a missing/empty resolved dir is expressed intent, not "the default".

**Evidence:**
- `paths::dataPath` — XDG data dir under `XDG_DATA_HOME`/`~/.local/share` (`include/gleditor/paths.hpp:62–141`); *not* the asset-dir chain, so `$GLEDITOR_ASSET_DIR` does not relocate the local image — worth saying, since the doc teaches the asset chain two bullets earlier.
- "absence skips silently (absence is the default)" (:866–867) — correct for end users, wrong for an explicit `GLEDITOR_STDLIB_LOCAL=1`.
- Status reachability is xuzz/vprolog-only (:803–806): "vpl never calls `solve*`, vagent runs its own solver, vicon compiles only — those three get the C++ accessor plus the `"vortex.stdlib"` startup log line". A silent skip logs no line, so the dev-loop observation step fails on exactly the tools whose users the doc addresses (:858).

**Fix:** State `--out` semantics ("names the image directory the app will resolve — `ops.nodes`+`permascroll/` live directly inside it") and the pairing rule ("set `GLEDITOR_STDLIB_LOCAL_DIR` for the *app* to the same path, or use the default data dir"). Add the failure mode: "an explicit `GLEDITOR_STDLIB_LOCAL=1` with an absent/unreadable resolved dir warns on `"vortex.stdlib"` naming `local.dirResolved` (dev intent expressed); unset-and-absent stays silent." And make the startup log line unconditional for the local decision — "local: grafted|skipped|suppressed, dir=<resolved>" — so all five consumers can see where the app looked.

## MINOR

### 5. (§5.4, line 799; §5.2 step 5, lines 574–575) `vortex_stdlib_status` registration/introspection parity is implied, not enumerated

**Problem:** Dispatch needs nothing (text match — MAJOR 3), but `symbolsInModule`/`vortex_function` visibility requires the same four steps as `vortex_param`: `createPredicate` + `exportSymbol(mod, "vortex_stdlib_status", …)` in `buildLogicModule` (`vortex_stdlib.cpp:3420–3463`), a `predVortexStdlibStatus_` cached member resolved post-graft (the "predVortex* five" enumeration at :574–575 becomes six), and optionally a `candidatePreds` entry (:4887–4909). Also pin the module path — `std:logic/vortex_stdlib_status` is the precedent (`std:logic` is where `vortex_param` lands, :3420–3463) — and whether a Cpp-built or grafted arena carries it (it must, since the term answers on *instance* state).

**Fix:** One line in §5.4: "registered `createPredicate`+`exportSymbol` in `std:logic` beside `vortex_param` (`vortex_stdlib.cpp:3434–3435`); `predVortexStdlibStatus_` joins the P33 cached-ref rebind list — so it survives `rebindCaches` post-graft and appears in `symbolsInModule`."

### 6. (§5.4, lines 726–732) `GLEDITOR_STDLIB_DIR` granularity unstated; `GLEDITOR_DATADIR` is build-time, not runtime

**Problem:** The precedence bullet lists `GLEDITOR_STDLIB_DIR` "taken-as-given" without saying whether it names the image directory itself or a parent containing `vortex-stdlib/` — the `assetPath("vortex-stdlib")` dir setter (:713) implies leaf-level, but a user will reasonably try both. Separately, `GLEDITOR_DATADIR` sits in the runtime precedence chain, but it is a *compiled-in* define (`src/paths.cpp:65–69` — only consulted `#ifdef GLEDITOR_DATADIR` and only when the directory exists); users cannot set it at launch. One parenthetical each.

**Evidence:** `src/paths.cpp:41–48` (`GLEDITOR_ASSET_DIR` literal, no existence check — "taken-as-given" verified), :65–69 (`GLEDITOR_DATADIR` build define), :72–75 (`assets` fallback), :84–102 (per-process caching).

**Fix:** "`GLEDITOR_STDLIB_DIR` names the image directory itself; `GLEDITOR_DATADIR` is a *build-time* define compiled into `assetDir()`, not a launch-time override."

### 7. (§5.2.5a, lines 597–600; §5.4, lines 787–788) The `ChoiceStackLive` payload is honest but the recovery instruction has no enumeration surface

**Problem:** "carrying the live mark count" is comprehensible but only weakly actionable: nothing in xuzz/vprolog lists *which* produce cursors/marks are live, so "drain or close the produce cursors" can't be aimed — and (per MAJOR 2) the marks can't outlive the next command anyway, so the realistic audience is the in-flight flow that triggered a mid-command rebind (a VQL script driving `std:xuzz/next_doc`, a produce flow crossing a document activation), not a person at a keyboard. The what() should say so: "`ChoiceStackLive`: N live arena mark(s) — resume or fail the suspended producing flow (e.g. `std:goal/resume`/`#GOAL_*`), or let it complete, then re-issue" — and name that no cursor-enumeration surface exists in v1 (or add `liveMarks` to `status()`). Contrast: image refusals carry "resolved dir + remedial env var" (:786–787) — genuinely remedial; the environmental payload deserves the same honesty about who can act on it.

**Fix:** Rewrite the recovery contract naming the audience and the reachable remedy; optionally expose `outstandingMarks()` in `stdlibStatus()` so the count is inspectable in-band rather than only in the exception text.

### 8. (§5.4, lines 793–794) `status().local`'s four-state + `present` axes overlap ambiguously

**Problem:** `local:{present, grafted|skipped|failed|suppressed, dirResolved}` — when is it `skipped` vs `suppressed` vs `present=false`? Reading the doc: `suppressed` = `Image`-mode base refusal (:866), `skipped` presumably = `localImage` resolved false (`Cpp` mode / explicit `0`) and/or absent. But "absent" is itself a state users must distinguish from "off" (MAJOR 4's footgun lives exactly here). Pin: `local.state ∈ {absent, skipped(policy off), grafted, failed(rolled back), suppressed(base refused)}` with `present` as the file-level observation — or fold `present` into the state enum and keep `dirResolved` always populated.

**Fix:** Enumerate the states and their triggers in one table row each; state that `dirResolved` is always populated when the chain ran (so "wrong dir" is always diagnosable).

### 9. (§6.4, lines 881–884) The self-contained-scratch limit names the constraint but not its failure surface

**Problem:** "dev modules must be self-contained — no stdlib ops are bound at build time in a stdlib-less scratch" — editing `std:math`'s internals works (its builder's `resolve("std:math/*")` calls hit the scratch's own minted module), but a new module whose builder resolves `std:math/abs` gets `nullopt` — and what happens then is unstated: silent skip, `nullopt` deref crash, or a named error. For a dev-facing tool, the failure should be named: "`vortex-stdlib-gen` fails with 'builder for `std:mymod` resolved `std:math/abs` — scratch contexts carry no stdlib; self-contained modules only (§6.4)'" rather than whatever the builder happens to do.

**Fix:** One sentence in §6.4 pinning the observable failure (generator exits non-zero naming the unresolvable symbol), plus §11 coverage.

### 10. (§12, lines 1392–1395) Cost accounting should footnote that a `WorkerContext` is per-*command* in xuzz today

**Problem:** "one mark-scoped O(cells) graft per `WorkerContext`/bound arena … the same order as today's bootstrap mint" is accurate but lets a reader think grafts are rare. In xuzz, `ensureVortexHost` constructs a fresh `VortexHost` (fresh `WorkerContext`, fresh `VortexStdLib`) on *every* logic/VQL/VPL command (`zigzag_visualizer.cpp:2857–2866`, call sites :3345–3690) — so "per `WorkerContext`" = per query, exactly like today's per-command 3× bootstrap. Parity holds; the amortization story ("once per process" for the fold) should not let readers over-read it — one footnote: "in xuzz today a `WorkerContext` is built per command, so the graft runs per query — the same cadence and order as the bootstrap it replaces; the *fold* is what is once-per-process."

### 11. (§5.4, lines 696–705) `vortex.stdlib_source`'s default-in-document behavior unstated

**Problem:** The `SettingSpec` registration (beside `system_docs.cpp:818–833`) and "startup-only; applies next launch" notes are honestly specified — verified against the live reload arm (`xuzz_app.cpp:4219–4227` re-applies siblings via `loadConfigFromStore`; the seam read sits above `xuzz_app.cpp:1588`, correctly hoisted per R5-2 since the first `ensureCells` fires inside `make_shared<ZigzagVisualizer>` → `populateFallbackStructure` :74 → `adoptDocument` :212 → `ensureVortexHost` :306). One residual: the doc doesn't say whether the key appears in the *shipped* Settings document. If the default document emits it, users will assume mid-session edits do something (the notes cover this, barely); if it's absent-by-default, the env/default precedence stays clean — say which.

**Fix:** "`vortex.stdlib_source` is absent from the shipped Settings document (absent = env-or-default); a user-added value validates against the registered `SettingSpec` and takes effect next launch — notes say so."

---

**Verification summary:** The settings seam (R5-2) is correctly fixed — verified the full ctor chain `xuzz_app.cpp:1588` → `zigzag_visualizer.cpp:74→:212→:306→:2857–2866` → `vortex_host.cpp:100–118` → `vortex_stdlib.cpp:1484–1486`; the seam placement above :1588 is the only correct one, and the startup-only/live-asymmetry honesty is stated where users look. `attachVortexHost` confirmed zero in-tree callers. `bindManifold` confirmed zero in-tree production callers (decl `vortex_host.hpp:79`, def `vortex_host.cpp:149–158`; `void` return — same refusal-channel problem, folded into BLOCKING 1). `outstandingMarks()` is the right liveness mechanism (arena-level, covers `ownedVm_`, `extraContexts_`, produce cursors, stray marks). The mode/dir/local precedence tables, "taken-as-given" convention (`paths.cpp:41–48`), refusal matrix, `status()` shape with `optional<source>`, the consumer-reachability honesty (:803–806), the dev-only scratch design, and the §11 gate structure are all verified as stated. The remaining defects are all about *delivery and observability* of behaviors the doc correctly names — chiefly that the flagship refusal cannot currently reach anyone.

VERDICT: NOT CONVERGED
