# Round 6 disposition

Target: `design/vortex-stdlib-prebuilt-store.md` revision 5 → revision 6.

Reports: `r6-writer.md` (1B/3M/11m), `r6-vortex.md` (1B/4M/7m), `r6-codebase.md`
(0B/1M/5m), `r6-architect.md` (1B/4M/5m), `r6-ergo.md` (1B/3M/7m),
`r6-zigzag.md` (1B/3M/5m). Raw: 5 BLOCKING, 18 MAJOR, ~40 MINOR.
Deduped: **7 clusters, all accepted — convergent spec amendments.** Every
BLOCKING/MAJOR mechanism was re-verified against the live tree before acceptance
(`ensureVortexHost` unconditional replace :2857–2866 with ~11 call sites;
`bindXanaduStore`/`attachVortexHost`/`bindManifold` all `void`-shaped;
`hydratePrefix` `outstandingMarks_ = 1` permanent floor :2192; `build*Module`
private `vortex_stdlib.hpp:527,547–567` + ctor unconditional `bootstrap()`
:1484–1487; manifest/`"ops"` tail inside `bootstrap()` :1507–1553; `solve()` →
`solveQueryHelper` dispatch :4926; `~VortexVM` unconditional
`setWriteGuard(nullptr)` :76–82; `release()` restores only the 4 trailed vectors
:1671–1699; `WorkerContext.arena` per-context `worker_context.hpp:70`).

## R6-1 (BLOCKING — 6 critics): the gate covers 2 of ~9 host-drop sites, and the refusal has no channel

The round's dominant defect — the R5-1 fix was placed one level too high.
`ensureVortexHost` unconditionally replaces `vortex_host_` whenever `engine_` is
non-null (`zigzag_visualizer.cpp:2857–2866`), called from `adoptDocument` :306
(the funnel behind `adoptXanaduStore` :354–360, `adoptXanaduDocs` :424–428,
`populateFallbackStructure` :212, `xuzz_app.cpp:629,:836,:3008`) and six command
paths (:3345,:3396,:3594,:3618,:3640,:3661,:3690). The doc even *concedes* the
path at :719–721 ("unconditionally replaced at the next
`adoptDocument`/`bindXanaduStore`") while omitting it from the gate — an
internal contradiction the writer flagged. And every gated API is `void`/`this`
— a refusal can't reach `bindZigzag`, whose bookkeeping (:1829–1841) runs
unconditionally → old document displayed while index/map/linkContext claim the
new one. `extraContexts_` marks live on separate arenas
(`worker_context.hpp:70`), invisible to `primary_.arena.outstandingMarks()`.

**Disposition — revision 6 rewrites §5.2.5a as a funnel-gate:**

1. The liveness check moves to the single replacement choke point:
   `ensureVortexHost` gains a *replace arm* — `if (engine_ && vortex_host_)`
   checks `arena().outstandingMarks()` (public accessor, not private
   `primary_.arena`) plus each `extraContexts_`'s arena count; refusal returns
   `unexpected(ChoiceStackLive)` and the retained host stays bound. Every
   replacement path funnels through it: `adoptDocument` (:306),
   `bindXanaduStore` (:392), `attachVortexHost`, and the six command paths.
2. Signatures become `std::expected`: `bindXanaduStore`, `adoptDocument`,
   `adoptXanaduStore`/`adoptXanaduDocs`, `attachVortexHost`, `bindManifold`,
   `ensureVortexHost`. `bindZigzag` (and every `views.documentActivated`
   handler) checks the result — on refusal it skips *all* bookkeeping
   (`zigzagStoreIndex`/`sliceHeads`/`map`/`linkContext`/`bridgeCoordinator`/
   `anchorPresentation` unchanged) and sets `commandBarFeedback_` to the named
   cause + recovery; the refusal also lands in `status()`.
3. Command-path policy stated: a command-entry `ensureVortexHost` that would
   replace a marks-live host refuses to *replace* — the command proceeds on
   the retained host and the refusal is reported via `commandBarFeedback_` +
   the command's own expected result. (Honest alternative rejected: "cursors
   can't outlive a command" — `goalProduce`/`goalBoundedFirst`/`goalResults`
   suspend marks across commands deliberately; the doc's own model says they
   can and do.)
4. `extraContexts_` aggregated: the gate sums
   `arena().outstandingMarks()` over `primary_` + each extra context (public
   accessors, no new API); where the host object hides them, the visualizer
   consults `vortex_host_->primaryContext()` + the worker-context vector.
5. Null-host guard: `vortex_host_ == nullptr` ⇒ count 0 ⇒ no refusal.
6. P30's file list gains `zigzag_visualizer.cpp` replace-arm line numbers and
   `vortex_host.hpp` signature changes; §11's swap gate asserts on
   `EXPECT_FALSE(result.has_value())` for every gated path plus a new arm:
   produce cursor suspended → `executeLogicQuery` → documented refusal +
   retained host.

## R6-2 (BLOCKING — 4 critics): `--module`'s dev-only scratch cannot mint a module or a manifest

`build*Module` are private `VortexStdLib` members behind an unconditional ctor
`bootstrap()` (`vortex_stdlib.cpp:1484–1487`); the `opsHead` start, `"ops"`
record, and `stdlib.image` holder are all emitted inside `bootstrap()`'s driver
(:1507–1553). A scratch "without `VortexStdLib`" emits neither modules nor
manifest — the artifact fails §5.1's own `FormatRefused` and has no `"ops"` to
register, contradicting §6.4's "its own grafted `stdlib.image` holder". No
`StdlibPolicy` mode yields a suppressed instance, and P32's Location omits
`vortex_stdlib.{hpp,cpp}` entirely.

**Disposition — revision 6 names the mechanism:**

1. P32 gains the enabling clause: a bootstrap-suppressed `VortexStdLib`
   construction — a ctor tag / `Bootstrap::Suppressed` arm under
   `StdlibPolicy{mode=Cpp}` — that skips `ensureStdlib` entirely
   (`status().source` records `none`), plus a public
   `buildModule(std::string_view)` facade dispatching to the named private
   `build*Module`.
2. The manifest-emission skeleton is factored: `beginImageOps()` (opsHead +
   `+d.step` chain at start) / `buildModule(name)` per named module /
   `endImageManifest()` (holder + `keyRecord(holder,"ops",opsHead)` + field
   records) — `bootstrap()`'s driver becomes those three calls over the 21 base
   modules; the `--module` path calls them over the named subset only.
3. §6.4 reworded: "`vortex-stdlib-gen --module` constructs a
   bootstrap-suppressed `VortexStdLib` — `bootstrap()`'s record-emission
   skeleton minus the base `build*Module` calls" — and the self-contained
   limit gains its failure surface: the generator exits non-zero naming the
   first unresolvable `std:*` symbol a dev builder consults.
4. P32's Location adds `vortex_stdlib.{hpp,cpp}`; §5.1's `FormatRefused`
   keeps biting genuinely manifest-less folds.

## R6-3 (MAJOR — 4 critics): the status builtin is unreachable exactly when needed

"The throw fires only at public use-points — `call()`/`solve()`/routine
entries" vs "its `solveQueryHelper` dispatch arm answers **before** the
use-point throw" are call-order-inverted: `solve()` dispatches
`solveQueryHelper` at :4926, so any entry throw precedes every dispatch arm.

**Disposition — revision 6 pins the ordering:** `solveQuery`/`solve`/
`solveOnce`/`call` do **not** throw at entry; the recorded refusal throws
inside `solveQueryHelper`'s per-goal dispatch, checked *after* the
`vortex_stdlib_status` arm (functor-text match — `vortex_param` precedent
:709–710 needs no exported symbol). `call()` gains its proposed entry throw
but `call("std:logic/vortex_stdlib_status")` still never reaches it under
refusal (`resolve()` → `nullopt`) — stated as solver-goal-only. The builtin is
registered `createPredicate`+`exportSymbol` in `std:logic` beside
`vortex_param` (:3434–3435) with `predVortexStdlibStatus_` joining the P33
cached-ref rebind list. §11 asserts `vortex_stdlib_status(S)` answers under a
latched refusal while `std:math/abs` refuses.

## R6-4 (MAJOR — 3 critics): mark-count precision and rollback scope

- `hydratePrefix` seeds `outstandingMarks_ = 1` permanently
  (`arena_manifold.cpp:2192`, `floors_` seeded) — the universal `== 0` gate can
  never hold on a hydrated arena. The gate counts marks **above the floor**:
  either a `liveMarks()`/`outstandingMarks() − floorBaseline` accessor, or the
  doc pins that gated arenas (`primary_`, folded image, extra contexts) are
  never `hydratePrefix`-seeded — §7.3's seeding mechanism then applies only
  outside the gated set, and the interaction is named explicitly.
- "Bit-identical rollback" covers only the 4 mark-trailed vectors —
  `release()` does not restore `arenaDims_`, `boundDimensions_`,
  `storesRankTail_`, `overlay_`. Narrow the claim to "bit-identical over the
  mark-covered vectors" + pin the invariant that all graft dim mints are
  rank-filed `findOrMintDimension` so the untrailed maps provably go
  untouched; §11 asserts `arenaDims_`/`boundDimensions_` unchanged after a
  rolled-back graft.
- The VM clear-list misses `oraclePath_`, `cutCallback_`, `currentDeweyPath_`,
  `totalCycles_` — stale `cutCallback_` fires a dead coordinator binding. Add
  them or justify per-member.
- `~VortexVM` unconditionally `setWriteGuard(nullptr)` (`vortex_vm.cpp:76–82`)
  — transient VMs (`vprolog::Compiler` per-solve, `vquery`'s `localVm`) unguard
  a shared core mid-session. P30 names either clears-own-only (guard identifies
  its installer) or resync re-installs.
- `rebindInstance`/`installWriteGuard` ordering: pin whether `rebindInstance`
  repopulates `opcodeMap_` in place (installed funnel keeps consulting the
  same live map — order immaterial) or re-invokes the installer (order
  matters).
- `spawnCursor` holds no mark — the enclosing `ChoicePoint` does; reword the
  coverage line.

## R6-5 (MAJOR — 3 critics): dev-loop UX and status-surface gaps

- `--out` names the image directory itself (`ops.nodes`+`permascroll/` inside);
  `GLEDITOR_STDLIB_LOCAL_DIR` must resolve to the same path for the app —
  stated as the pairing rule. Explicit `GLEDITOR_STDLIB_LOCAL=1` + absent dir
  warns on `"vortex.stdlib"` naming `local.dirResolved`; unset-and-absent stays
  silent. Startup log line always reports the local decision + dir.
- `local.state ∈ {absent, skipped(policy off), grafted, failed(rolled back),
  suppressed(base refused)}` — pinned enum; `dirResolved` always populated.
- `GLEDITOR_STDLIB_DIR` names the leaf image directory;
  `GLEDITOR_DATADIR` is a build-time define, not a launch-time override;
  `assetDir()` takes-as-given only `GLEDITOR_ASSET_DIR` (`paths.cpp:45–48`) —
  the stricter chain is deliberate hardening, reworded.
- `vortex.stdlib_source` is absent from the shipped Settings document
  (absent = env-or-default); user-added values validate against the
  `SettingSpec`, apply next launch.
- `ChoiceStackLive`'s payload audience is the in-flight flow, not a person at
  a keyboard — the recovery wording names resumable produce machinery and
  optionally exposes `liveMarks` in `status()`.
- §12 footnote: a `WorkerContext` is per-*command* in xuzz today — the graft
  runs per query, same cadence/order as the bootstrap it replaces; the *fold*
  is what's once-per-process.
- Consumer enumeration gains `vlace`/`vlacec` (per-job `WorkerContext`s fold
  like every consumer; `vlace::Driver` is the stdlib-free precedent for the
  dev-scratch); bare-`VortexCore` helpers (`xuzz_app.cpp:542,889,907`,
  `lens_file_picker.hpp:28,197`) never fold — "once per process" is
  per-`VortexStdLib`, not per-`VortexCore`.
- Generator recipe: the scratch `WorkerContext`'s ctor-time `ensureStdlib`
  under forced `Cpp` performs the single build — no explicit `bootstrap()`
  (a bare post-ensure call double-mints).
- `vagent/lineage.cpp:564` recategorized: it builds a whole `WorkerContext`
  (outside-`VortexHost` class), not a shared-core second instance — use
  `vprolog/compiler.hpp:78`, `vicon/compiler.hpp:160`, `vpl_engine.hpp:132–136`.

## R6-6 (MAJOR — 1 critic): §11 gate completeness

Skew gate gains three arms: `perma->size() > expectedScrollBytes` →
`ScrollTruncated`; well-formed store without `stdlib.image` → `FormatRefused`;
severe truncation eating early names → `ScrollTruncated` via the span-end
fallback. Swap gate covers every gated path from R6-1 plus the command-path arm.

## R6-7: housekeeping (~25 accepted minors)

- `keyRecord` = hoist `vagent/fragment.cpp:39–49`'s file-local unscoped
  `varsRecord` overload into the shared records header (4 critics found this
  independently); scope it to private/sparse ranks — `owns()` treats absent
  `+d.owner` as match-any.
- `vortex_host_->primary_.arena` → `arena()`/`primaryContext()` public
  accessors (private member).
- Citation fixes: `mesh_evaluator.cpp:743`→`:727`; `executor.cpp:564,634,2924`
  →`:567,:640,:2584`; `or_parallel.cpp:210`→`:349` (`hydratePrefix`);
  `mesh_evaluator.cpp:936`→`:920`; goal-helper ranges →`:4198–4310`,
  `:2460–2489`; `arena_manifold.hpp`→`:749–752`; `deploy`→`registerSubscroll`/
  `ingestSubscroll` `user_permascroll.cpp:561–619`; `store.cpp` split —
  `Store::save`/`writeSegmentFile` `:4093–4125`, `saveOsmicText` `:4320–4340`;
  `loadConfigFromStore` gains the test caller `vortex_host_test.cpp:181`;
  engine-death line `:364` not `:370`.
- P26 extended: `d.params` is rank-filed too — same `findDimension`-not-
  `arena.dimensions()` rule for all graft-minted dims; the `findOpHandle`/
  `versionAnnotation`/`aliases` slot-scan conflation boundary extends to the
  live arena post-graft (grafted OpHandle wing slots sit inside the scan set).
- `expectedScrollBytes=0` under `Cpp` is an unexported placeholder — stated;
  trailing manifest writes are bits-only `SetValue`s (`PrimediaSpan{}`
  restate, `vagent/lineage.cpp:407–410` precedent) so the arithmetic closes.
- P30 Location gains `vortex_vm.{hpp,cpp}` + `vortex_stdlib.hpp`.
- §6.5 gains the `std:goal/halt` + `std:goal/sentinel` symbol-visibility delta.
- Null-host guard clause; `vortex_stdlib_status` purity (record term +
  dispatch arm, `vortex_param/4` precedent).

## Convergence

Every cluster is a spec amendment inside the chosen architecture — no
redesigns. R6-1 tightens the R5-1 fix one level down (the funnel); R6-2
adds one construction arm + one factored skeleton; everything else is
precision. Round 7 critics verify the amendments.
