# Round 6 critique: Vortex/Spool-Mesh Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 1 BLOCKING, 4 MAJOR, 7 MINOR.

Verified before writing: every re-checked citation in revision 5 holds —
`ArenaManifold::outstandingMarks()` (`apps/common/xanadu/zigzag/arena_manifold.hpp:749–752`),
choice-point marks (`apps/common/xanadu/vortex/vortex_vm.cpp:317–326`),
`bindXanaduStore` → `engine_.reset()` at `:364` then host replacement at `:392`
(`apps/common/ui/slice/zigzag_visualizer.cpp:362–394`),
`attachVortexHost` assign-and-return (`:2844–2848`),
`ensureVortexHost` unconditional `make_shared` (`:2857–2866`),
`installWriteGuard` last-wins (`vortex_vm.cpp:74,84–95`; `vortex_vm.hpp:197–201`),
`primary_` before `vplEngine_` in `VortexHost`'s ctor (`vortex_host.cpp:100–104`),
`ownedVm_`/`ownedStdlib_` over the shared core with only a `stdlib()` accessor
(`vpl/vpl_engine.cpp:77–86`, `vpl/vpl_engine.hpp:79,129–136`),
engine caches (`vql/vql_engine.hpp:230–238`, `vql/compiler.hpp:116`, `vpl/vpl_engine.hpp:140–144`),
ctor-cached `home_`/`dims_` (`vortex_core.cpp:16,31–74`; `vortex_core.hpp:335–336`),
`mintNamedDimension` adopted-walk vs `mintDimension` `rankTail` append
(`vortex_core.cpp:18–28` vs `:76–89`),
`varsRecord` mint always adds `+d.owner` (`vagent/records.hpp:97–120`) vs the unscoped
three-line shape already used at `vagent/fragment.cpp:39–49`,
`registerProgramImage` `"ops"` lookup on the holder's private rank (`vortex_vm.cpp:121–155`),
`solve()` building `candidatePreds` from cached refs before `solveQueryHelper`
(`vortex_stdlib.cpp:4881–4929`), the xuzz seam chain
(`xuzz_app.cpp:1588–1599`; `zigzag_visualizer.cpp:74,:157–213,:215–308`),
`loadConfigFromStore` call sites (`vortex_host.cpp:165`; `xuzz_app.cpp:1596,4226`),
`hydratePrefix` (`arena_manifold.cpp:2168–2193`), and the dev-tool WorkerContext
constructions (`or_parallel.cpp:345–351`; `vagent/lineage.cpp:564`;
`mesh_evaluator.cpp:743` still bare-mints `d.mesh.relocation` as P26 says).
The architecture — virgin scratch → whole-space promote → sealed manifest → mark-scoped
graft → post-mark register/rebind → per-instance cache split — is coherent and the
edge-taxonomy, identity token, sentinel re-verify, and `shared_ptr<State>` snapshot
claims all check out. What remains are four coverage/mechanism holes and one path
that cannot be built as written.

## BLOCKING

### 1. (§6.4/P32, lines 871–885, 1244–1248) `--module`'s dev-only scratch has no way to mint a module

**Problem:** The dev-scratch shape is specified as `ArenaManifold` + `VortexCore` +
`VortexVM` **without** `VortexStdLib` — yet the only code that mints a module is the
private `VortexStdLib::build*Module` family, and every `VortexStdLib` construction
bootstraps all 21 modules. The specified object set cannot produce the output the
dev loop depends on.

**Evidence:** The doc: "the tool constructs `ArenaManifold` + `VortexCore` +
`VortexVM` **without** `VortexStdLib` — no bootstrap — mints only the named dev
module(s)" (§6.4) and "edit the `build*Module` C++ → `vortex-stdlib-gen --module
std:mymod`" (§6.4); P32: "`--module <name>` (whole-space image on a **dev-only
scratch** — `ArenaManifold`+`VortexCore`+`VortexVM` with *no* `VortexStdLib`)".
But `buildMathModule`…`buildHypertimeModule` are declared `private` at
`vortex_stdlib.hpp:527,547–567`, write instance state (`routineBindings_`,
`goalHalt_`/`goalFail_`/`goalSentinel_`, `exportSymbol`, `getOrCreateModule`), and
`VortexStdLib::VortexStdLib` calls `bootstrap()` unconditionally
(`vortex_stdlib.cpp:1484–1487` → `:1507–1553`). Under P30 the ctor becomes
`ensureStdlib() = ensureCells + rebindInstance`, and `StdlibPolicy` offers only
`Image`/`ImageThenCpp`/`Cpp` — no mode yields a stdlib-less instance whose builders
can still run, and `ensureCells`'s `stdlibPresent` probe (`resolve("std:math/abs")`)
cannot be faked on an empty scratch. P32's Location lists only
`tools/vortex_stdlib_gen.cpp` (new) + Makefile — no `vortex_stdlib.{hpp,cpp}` — so
the enabling change (a bootstrap-suppressing construction mode plus a public
single-module mint entry, or externalized builders) sits outside every numbered
proposal.

**Fix:** Name the mechanism in P32 and add `vortex_stdlib.{hpp,cpp}` to its
Location — e.g. a `VortexStdLib` ctor tag/`Bootstrap::None` mode that skips
`ensureStdlib` entirely (record `source=none`) plus a public
`buildModule(std::string_view)` facade dispatching to the private `build*Module`
and then running the §4 manifest/`"ops"`/`+d.params` emission the bootstrap path
performs at end. Without it, `--module` cannot exist; the alternative — running
`--module` on a full `Cpp` scratch and filtering — re-imports the ~4,300 base
symbol cells the dev-scratch shape exists to avoid (§6.4's own argument).

## MAJOR

### 1. (§5.2.5a/§5.4, lines 596–605, 777–778, 1163–1167) `ChoiceStackLive` has no delivery channel — both gated APIs are `void`-shaped

**Problem:** The gate "refuses the rebind with a named cause" but names no
mechanism for the refusal to reach the caller. `bindXanaduStore` returns `void`
and `attachVortexHost` returns `this` for chaining — an early return is
indistinguishable from success, a `nullptr` return is a sentinel (house rule 3),
and a thrown `StdlibImageRefused` escapes document-open paths that do not catch it.
P30 names the files (`zigzag_visualizer.cpp:362–394,2845–2866`) but not the
channel.

**Evidence:** The doc: "**the `ChoiceStackLive` gate sits at `bindXanaduStore`/
`attachVortexHost` entry**: `vortex_host_->primary_.arena.outstandingMarks() != 0`
checked *before* `engine_.reset()`/the host drop refuses the rebind with a named
cause carrying the live mark count" and "the recovery contract: drain or close the
produce cursors, then re-issue the switch — the old host, engine, and bound
document are all retained". The error-channel paragraph pins only
`ensureCells`→`std::expected<void,StdlibImageRefused>` and the use-point throw —
neither runs at the visualizer layer. Code: `void ZigzagVisualizer::bindXanaduStore(...)`
(`zigzag_visualizer.cpp:362`), `ZigzagVisualizer *attachVortexHost(...)` returning
`this` (`:2844–2848`), `void VortexHost::bindManifold(...)` (`vortex_host.cpp:149`).
If the refusal is a silent early return, the caller (`bindZigzag`/document-switch
bookkeeping) continues registering the *new* document while the visualizer still
serves the old one — a silent inconsistency worse than the crash it prevents.
Also unstated: the `vortex_host_->` deref needs a null-guard (`vortex_host_` is
null whenever `engine_` was null, `zigzag_visualizer.cpp:2858`).

**Fix:** Pin the channel in §5.2.5a and P30: change `bindXanaduStore`/
`attachVortexHost`/`bindManifold` to return
`std::expected<...,StdlibImageRefused>` (fallible ops return `expected`, house
rule) and propagate through the document-open call chain to the command-bar
surface the doc already names for init refusals; or state explicitly that the
visualizer swallows the refusal internally and records it in `status()` —
and say how the caller learns the switch did not happen.

### 2. (§5.2.5a, lines 586–608) The gate misses the unconditional `ensureVortexHost()` — ~7 ungated host-replacement call sites

**Problem:** `ensureVortexHost()` replaces `vortex_host_` on every call — there is
no `if (!vortex_host_)` guard — and it is invoked unconditionally by interactive
command paths the entry gate never sees. A suspended produce cursor or live
choice stack on the outgoing host is silently destroyed not only at document
rebind but at the *next VQL/VPL/logic/macro command*, which is the more common
destruction site.

**Evidence:** `zigzag_visualizer.cpp:2857–2866`:
`vortex_host_ = std::make_shared<vortex::VortexHost>(&engine_->manifold())`
executes unconditionally whenever `engine_` exists. Call sites:
`:306` (`adoptDocument`, ctor path), `:392` (gated `bindXanaduStore`), plus
unconditional calls at `:3345` (`navigateCellVQL`), `:3594` (`navigateVQL`),
`:3618` (`executeVQLScript`), `:3640` (`executeVPL`), `:3661` (`executeLogicQuery`),
`:3690` (`defineMacro`), and the const-cast at `:3396`; only `vortexHost()`'s lazy
accessor (`:2850–2855`) and `dispatchAction` (`:2871`) are null-guarded. The doc's
"the `ChoiceStackLive` gate sits at `bindXanaduStore`/`attachVortexHost` entry"
leaves each of those paths free to destroy live marks — the same corruption class
the gate exists to refuse ("drain or close the produce cursors" presumes cursors
can outlive a command, and they can).

**Fix:** Move the gate inside `ensureVortexHost()` (or into a single
`replaceVortexHost` choke point) so *every* replacement path checks
`vortex_host_->primary_.arena.outstandingMarks()` before `make_shared`; keep the
`bindXanaduStore`/`attachVortexHost` entry checks as the UX-surface early-out.
Alternatively argue — with evidence — that interactive command paths can never
carry live marks (e.g. all produce cursors are drained before command return);
the doc's own "produce cursors hold marks" model says otherwise.

### 3. (§5.2.5a, lines 601–605; P30, line 1166–1167) `outstandingMarks()` is per-arena — `extraContexts_` marks are invisible to the gate

**Problem:** The coverage claim "the count covers `primary_.vm`'s stack,
`vplEngine_`'s `ownedVm_` stack ... and any stray non-VM `mark()` holder" is true
only for `primary_.arena`. Each `WorkerContext` in `extraContexts_` owns a
*separate* `ArenaManifold` — a suspended produce cursor on a worker context's
arena holds a mark the gate never counts, and host replacement (or
`bindManifold`'s `extraContexts_.clear()`) destroys it silently.

**Evidence:** `worker_context.hpp:70`: `ArenaManifold arena` per context;
`makeWorkerContext` appends contexts sharing `baseManifold_` (`vortex_host.cpp:107–115`);
`bindManifold` runs `extraContexts_.clear()` at `:156`. `outstandingMarks()`
returns the single arena's `outstandingMarks_` (`arena_manifold.hpp:749–752`).
The doc acknowledges `extraContexts_` only for the dangling-`baseManifold_`
exposure (line 1185–1186: "outside the re-fire"), not for the liveness gate.
`solveLogicBatchParallel`/`runOne` drive queries on those contexts
(`vortex_host.cpp:995–1034`), so a cursor can genuinely be suspended there.

**Fix:** Aggregate the check: `primary_.arena.outstandingMarks()` plus each
`extraContexts_[i]->arena.outstandingMarks()` (public accessor, no new API), or
state the bounded claim honestly — the gate covers the primary context only and
worker-context cursors carry the same documented exposure as the dangling base.

### 4. (§5.4, lines 788–803) `vortex_stdlib_status` can only "answer before the use-point throw" if the throw is not where the doc puts it

**Problem:** Two sentences cannot both hold literally: "The throw fires only at
public use-points — `call()`/`solve()`/routine entries" and "its
`solveQueryHelper` dispatch arm answers **before** the use-point throw".
`solveQuery` → `solve()` → `solveQueryHelper` (`vortex_stdlib.cpp:4851–4871`,
`:4881–4929`): a refusal thrown at `solve()`/`call()` entry — the natural reading
of "use-points" — never reaches the dispatch arm, making the builtin dead code in
exactly the state it exists to diagnose. The doc states the ordering requirement
but never names where in the call chain the throw lives, and no §11 gate tests
the builtin under a recorded refusal.

**Evidence:** `solve()` builds `candidatePreds` from cached `pred*` refs
(`vortex_stdlib.cpp:4888–4906`, benign under refusal since dead refs never match)
then calls `solveQueryHelper` at `:4926`; builtin dispatch is per-goal inside the
helper (the `vortex_param` precedent at `:709+`). For the arm to answer, the
throw must sit inside per-goal dispatch *after* the `vortex_stdlib_status` arm —
i.e., `solve()`/`solveQuery()`/`call()` must not throw at entry; the refusal
fires when a goal resolves to stdlib machinery.

**Fix:** Pin it in one sentence: "`solveQuery`/`solve`/`call` do not throw at
entry; the recorded refusal throws inside `solveQueryHelper` when a goal
dispatches to non-diagnostic stdlib machinery — the `vortex_stdlib_status` arm
is checked first, the same exemption `resolve()`/`modules()`/`symbolsInModule()`
get at the public layer." Add an §11 gate: under a latched `Image` refusal,
`vortex_stdlib_status(S)` returns the record while `std:math/abs` goals refuse.

## MINOR

1. (§5 invariant, lines 389–399) `hydratePrefix()` permanently seeds
   `outstandingMarks_ = 1` (`arena_manifold.cpp:2168–2193` — clears `floors_`,
   pushes a floor, sets the count with no matching release). Any arena hydrated
   for context seeding (`or_parallel.cpp:345–351`; §7.3 keeps the mechanism)
   fails `MarksOutstanding` forever if a `VortexStdLib` is ever grafted onto it.
   Name the interaction: either `hydratePrefix` balances its floor (mark +
   release) or the doc scopes the gate to marks taken after hydration.

2. (P30 Location, lines 1144–1148) The file list omits `vortex_vm.{hpp,cpp}` —
   clearing `opcodeMap_`/`cursorPC_`/`memoizedOps_` (private members,
   `vortex_vm.hpp:194–196,304–305`) requires a new VM-facing API — and
   `vortex_stdlib.hpp` (`ensureStdlib`/`ensureCells`/`rebindInstance`/
   `rebindCaches`/`keyRecord` declarations). House rule 6 wants exact files.

3. (§5.2.5b, lines 616–619) The swap clear-list names `opcodeMap_`, `cursorPC_`,
   `memoizedOps_` but not `oraclePath_`, `cutCallback_`, `currentDeweyPath_`, or
   `totalCycles_` — a stale `cutCallback_` would fire a dead coordinator binding
   and a stale `oraclePath_` mis-steers backtracking. Clear them or state why
   they are harmless.

4. (§5.2.5b/P30) "the per-instance `rebindInstance` refresh order matters" is
   loose: order matters only if `rebindInstance` re-invokes `installWriteGuard()`
   (its contract says "re-call it when a second VM over the same core
   re-installed", `vortex_vm.hpp:197–201`). If `rebindInstance` repopulates
   `opcodeMap_` in place, the already-installed `ownedVm_` funnel keeps
   consulting the same live map and order is immaterial. Pin which mechanism
   applies.

5. (§5.1/P31) `expectedScrollBytes` "final scroll size" is self-referential
   unless its own write is span-free: state that trailing manifest writes are
   bits-only `SetValue`s (`PrimediaSpan{}` restate, `vagent/lineage.cpp:407–410`
   precedent) so the arithmetic closes, and that `generatorHead`'s text span is
   counted into the size.

6. (§5.2.5a) The gate as written derefs `vortex_host_->primary_.arena`
   unconditionally; `vortex_host_` is null whenever `engine_` was null at the
   creating call (`ensureVortexHost` no-ops, `zigzag_visualizer.cpp:2858`).
   One line: null host ⇒ count 0 ⇒ no refusal.

7. (citation drift) `arena_manifold.hpp:750–752` cited for `outstandingMarks()`
   — the accessor spans `:749–752`; `vortex_stdlib.cpp:4115–4273` for the goal
   helpers actually covers `buildGoalModule`'s bind tail plus the helpers at
   `:4198–4290`. Within the doc's own drift caveat; tighten on next pass.

## Verified-correct highlights (for the record, not findings)

- `keyRecord` is exactly `varsRecord` minus the `+d.owner` arm — the three-line
  shape already exists at `vagent/fragment.cpp:39–49`; `registerProgramImage`'s
  `rankAfter`/`linked(values)` walk never inspects `+d.owner`
  (`vortex_vm.cpp:121–155`), and `owns()` treats unscoped as match-any
  (`vagent/records.hpp:97–120`), so the unscoped manifest registers.
- `rankTail` append for resync mints is the right fix vs `mintNamedDimension`'s
  adopted-walk `lastDimCell` (`vortex_core.cpp:18–28` vs `:76–89`); the
  skewed-base orphan-tail limitation is stated accurately.
- `shared_ptr<State>` + `once_flag` + `optional<expected>` is coherent:
  a mutex-protected snapshot keeps an in-flight graft's `State` alive while a
  test reset swaps the slot; `call_once` gives the once-per-process latch.
- The application seam timing is right: `systemStore(Settings)` is reachable
  above `xuzz_app.cpp:1588`, and the ctor chain
  (`:74` → `populateFallbackStructure` → `adoptDocument` :215–308 →
  `ensureVortexHost` :306) does fire `ensureCells` before any `loadConfigFromStore`.
- `env_` dropped-not-remapped is the only honest semantic — VQL env entries are
  computed cell sets with no name-addressable manifold path to re-resolve.
- The local-image chain (`GLEDITOR_STDLIB_LOCAL_DIR` taken-as-given +
  `paths::dataPath("xuzz","vortex-stdlib-local/")`) matches the
  `paths.cpp:41–48` explicit-directory convention; `dataPath` is header-inline
  (`include/gleditor/paths.hpp`), so the headless claim holds.
- `mark()`/`discard`/`release` graft atomicity, the `insertIntoRank(rankTail)`
  tail-splice (`arena_manifold.cpp:1423–1439`), `promote`'s `d.dims` skip lift,
  `image.cells()` fold-birth order (`manifold.hpp:407`), the first-two-births
  fiat (`manifold.cpp:447–454`), and the whole-space `extraRoots{home,dims_.dims}`
  ordering are all mechanically consistent.

VERDICT: NOT CONVERGED
