# Review: vortex-stdlib-prebuilt-store

Target: `design/vortex-stdlib-prebuilt-store.md` Workflow: `vortex-design-critique` (six critics,
iterate to `VERDICT: CONVERGED`, gate = 0 BLOCKING + 0 MAJOR)

## Rounds

| Round | Writer                    | Vortex                    | Codebase                  | Architect                  | Ergo                       | Zigzag                   |
| :---- | :------------------------ | :------------------------ | :------------------------ | :------------------------- | :------------------------- | :----------------------- |
| r1    | NOT CONVERGED (2B/2M/13m) | NOT CONVERGED (3B/5M/8m)  | NOT CONVERGED (2B/6M/14m) | NOT CONVERGED (3B/6M/7m)   | NOT CONVERGED (4B/7M/5m)   | NOT CONVERGED (3B/7M/6m) |
| r2    | NOT CONVERGED (0B/2M/7m)  | NOT CONVERGED (0B/5M/9m)  | NOT CONVERGED (0B/3M/10m) | NOT CONVERGED (0B/4M/13m)  | NOT CONVERGED (0B/4M/7m)   | NOT CONVERGED (1B/3M/5m) |
| r3    | NOT CONVERGED (0B/1M/7m)  | NOT CONVERGED (0B/4M/6m)  | NOT CONVERGED (0B/2M/5m)  | NOT CONVERGED (0B/5M/11m)  | NOT CONVERGED (0B/1M/7m)   | NOT CONVERGED (0B/2M/5m) |
| r4    | NOT CONVERGED (0B/2M/16m) | NOT CONVERGED (0B/1M/5m)  | NOT CONVERGED (0B/1M/3m)  | NOT CONVERGED (0B/3M/12m)  | NOT CONVERGED (0B/3M/7m)   | NOT CONVERGED (0B/2M/6m) |
| r5    | **CONVERGED** (0B/0M/9m)  | NOT CONVERGED (0B/2M/12m) | NOT CONVERGED (0B/1M/7m)  | NOT CONVERGED (0B/2M/5m)   | NOT CONVERGED (1B/5M/9m)   | NOT CONVERGED (0B/2M/6m) |
| r6    | NOT CONVERGED (1B/3M/11m) | NOT CONVERGED (1B/4M/7m)  | NOT CONVERGED (0B/1M/5m)  | NOT CONVERGED (1B/4M/5m)   | NOT CONVERGED (1B/3M/7m)   | NOT CONVERGED (1B/3M/5m) |
| r7    | NOT CONVERGED (1B/4M/9m)  | NOT CONVERGED (1B/3M/4m)  | NOT CONVERGED (9B/6M/5m)  | NOT CONVERGED (1B/5M/8m)   | NOT CONVERGED (1B/2M/7m)   | NOT CONVERGED (0B/2M/5m) |
| r8    | **CONVERGED** (0B/0M/5m)  | NOT CONVERGED (0B/1M/4m)  | NOT CONVERGED (0B/1M/1m)  | NOT CONVERGED\* (0B/1M/5m) | NOT CONVERGED\* (0B/3M/5m) | NOT CONVERGED (0B/1M/3m) |
| r9    | **CONVERGED** (0B/0M/3m)  | **CONVERGED** (0B/0M/2m)  | NOT CONVERGED (0B/1M/1m)  | **CONVERGED** (0B/0M/3m)   | NOT CONVERGED (0B/2M/2m)   | **CONVERGED** (0B/0M/3m) |
| r10   | **CONVERGED** (0B/0M/2m)  | **CONVERGED** (0B/0M/2m)  | **CONVERGED** (0B/0M/1m)  | **CONVERGED** (0B/0M/2m)   | **CONVERGED** (0B/0M/1m)   | **CONVERGED** (0B/0M/1m) |

## Outcome

**Review converged at Round 10 on revision 9.** All six critics report zero BLOCKING and zero MAJOR
findings. Nine final MINOR notes are dispositioned in `r10-disposition.md`; non-substantive final
polish is applied. P26-P34 are ready for user approval. This is design review completion, not
runtime implementation or testing.

## Round 1 → revision 1 highlights

- Ref-valued scalars respecced as a *class* (P26): OpHandle wing slots + marked-Int64 ref scalars,
  harvested promote roots, graft-side translation through the cell map.
- `goalHalt_`/`goalSentinel_` exported as `std:goal` symbols (P33); every cached ref enumerated in
  `rebindCaches()`.
- Graft honors edge classes: internal edges replay, membership edges append via `insertIntoRank`,
  `d.dims` edges never replay (strict 2-degree / no rank severing).
- `ensureStdlib` split into `ensureCells()` (per arena) + `rebindInstance()` (per `VortexStdLib`) —
  fixes dead per-instance caches in vprolog/vicon/vpl/vagent.
- v1 update model reduced to honest scope: complete sealed image per release, atomic swap;
  shared-lineage `--extend`/delta transport deferred to U-X7 with the `resolve()` first-match
  shadowing problem named.
- New P34: `O_RDONLY` fallback + `readOnly` for `SegmentedOpsSpool` — a root-owned installed asset
  otherwise cannot be opened at all.
- Generator pins `documentId`/identity; no `publication-state/` (no secret key material) in the
  shipped artifact.
- `imageEpoch` = digest over ordered kind→label tables; `expectedScrollBytes` permascroll detector;
  manifest anchored on `home +d.vars`; headless image-dir resolution chain (no SDL in engine layer).

Disposition: `r1-disposition.md` (17 accepted clusters, 3 refused, U-X7 deferred).

## Round 2 → revision 2 highlights

- Per-ref `"cell"` marker cells (`refCell` convention) — the shared marker was a strict 2-degree
  violation: `link()`'s splice evicts the previous slot's mark (R2-BLOCKING). Record members
  (`"ops"`/`"in"`/`"out"` chains) carry Int64 refs *positionally* — membership is the ref
  declaration (`seedProgramImage` precedent); the graft translates them by walking each grafted
  record chain.
- Signature/`"ops"` records mint **inside bootstrap** — arenas are self-describing in both modes;
  `rebindInstance` is uniform and `Cpp`-mode `routineBindings_` cannot come back empty. Goal trio
  gets `{in:{},out:{}}` `d.params` records.
- Graft runs inside a `mark()` — `discard` on success, `release` on refusal; registration and rebind
  run post-`discard`.
- `opcodeMap_` clears + re-registers on arena swap (`bindManifold` move-assigns the arena; stale
  ints alias fresh cells — `contains()` cannot filter).
- `UserPermascroll::Config` **gains** `readOnly` (new field): ctor skips
  `permascrollState`/`create_directories`/write-back; primedia honors requested `O_RDONLY`;
  ops-spool append/flush/seal *refuse* readOnly (P34 widened, ~60–70 lines).
- Registration scoped to each grafted image's mapped `"ops"` record — foreign `"ops"`-texted records
  on the shared `home +d.vars` rank are never scanned.
- Membership appends pinned: `link(rankTail(anchor,dim), dim, POS, member)`; same-text local members
  shadow behind base `resolve()` first-match — add, never replace.
- Manifest/sig records minted unscoped (no `+d.owner` Int64-ref class); single-owner edge invariant
  stated and asserted.
- Marks-free is a *named refusal* in every grafting mode (`MarksOutstanding`); graft refusal joins
  the §5.4 matrix; `StdlibImageRefused::what()` carries cause/epochs/dir/ remedial env;
  `"vortex.stdlib"` spdlog channel + `status()` accessor.
- Config channel pinned: store keys → process `StdlibPolicy` at `loadConfigFromStore` for
  later-built contexts; precedence explicit-setter > store > env > default;
  `resetStdlibImageForTesting` + `std::once_flag` lazy fold.
- Validation order pinned: dir precheck → open → fold → `expectedScrollBytes` → epoch.
- Repriced ≈ 18–22K ops / 1.2–1.4 MiB (per-ref markers + restate pass); stale citation batch
  re-pinned; module list corrected to 21 in mint order.

Disposition: `r2-disposition.md` (16 accepted clusters, 1 partial — positional members chosen over
uniform marks).

## Round 3 → revision 3 highlights

- `"ops"` moves under `"stdlib.image"` as the **per-image holder** — 6/6 critics found
  `registerProgramImage(map[home])` cannot scope per record (first-text-match on the shared
  `home +d.vars` rank). `registerProgramImage(map[imageRecord])` now scopes by construction;
  `rebindInstance`/non-grafting instances enumerate all `stdlib.image` records on `home +d.vars`;
  `Cpp` mode mints a dev-fields `stdlib.image` — one uniform mechanism. One `"ops"` record per
  image; `build*Module`s append members.
- Edge taxonomy closed: pre-existing member endpoints (name-merged modules) — skip if edge already
  holds, else named refusal; name-merged cells are a third map-seed class excluded from cloning;
  residual refusal rule; dim classification before `map[]`.
- Arena-swap rebind widened: `opcodeMap_` + `cursorPC_` + `memoizedOps_` clear; `choiceStack_`
  non-empty at swap = `ChoiceStackLive` refusal.
- `PromotionBudget::wholeSpace` pinned: lifts the `d.dims` skip only (`d.lineage` stays skipped) —
  otherwise the registry rank's own edges never export.
- `expectedScrollBytes` reachability fixed: manifest *names* mint early (survive tail truncation),
  Int64 values write last; fallback compares `perma->size()` vs max span-end.
- `vortex.stdlib_image_dir` dropped — dead-on-arrival (fold precedes `loadConfigFromStore`) and a
  store-content→loader trust step. Mode-only store key, Settings-store only,
  `SettingSpec`-registered; precedence setter > env > store > default.
- `vplEngine_.ownedStdlib_` covered by the long-lived-instance invariant + `initHostServices` call
  site + per-instance arena-identity check.
- Generator forces `StdlibPolicy{Cpp}` before the scratch context (else ctor-time graft doubles the
  build).
- Packaging: app seam *must* init the setter from `assetPath` before first context; msys2 staging +
  smoke-test check; `GLEDITOR_STDLIB_DIR` taken-as-given; env spellings
  `image`/`cpp`/`imagethencpp`; image is a checked-in artifact.
- `imageEpoch` gains the decode-boundary constants (`kLastContentDecodableOpcode`/
  `kLastOpcodeKind`) + the authored `OpcodeKind`→label table.
- P26 DRY commits to `common/xanadu/relocation_marks.hpp`; `mesh_evaluator.cpp:743` orphan dim
  migrates to `findOrMintDimension`.
- P34 completeness: `saveOsmicText`, permascroll mutators, request channel, errno scope,
  expected-not-throw, empty-`ops.nodes` naming.
- §12 restated: ~11–13K-cell shared image; Cpp-mode +~4K cells/arena; per-instance `rebindInstance`
  cost named.

Disposition: `r3-disposition.md` (9 accepted clusters, ~30 accepted minors, 0 refused).

## Round 4 → revision 4 highlights

- **Two-cell holder, verbatim** — vortex + codebase + zigzag independently: a member of
  `home +d.vars` cannot own a private `+d.vars` rank (its `+d.vars` edge *is* its rank position), so
  `stdlib.image`-as-one-cell was positional-only and contradicted §4's mint order. Now the literal
  vagent shape: `"stdlib.image"` key record on `home +d.vars` → `+d.values` → **unranked record
  head** anchoring a private `+d.vars` rank holding `"ops"` — `registerProgramImage(head)` gets true
  structural isolation (foreign `"ops"` unreachable, not merely unscanned), no ordering invariant,
  genuine ops-less `unexpected` (prechecked in both call paths).
- **Edge-class precedence pinned** — zigzag + writer independently: *endpoint provenance before
  membership* (a name-merged member edge satisfies both; tail-append would sever it mid-rank — the
  R3 hazard closing on itself) and *edge-existence detects ref-bearing, `ValueKind` dispatches the
  decode* (Int64→`asInt64`; OpHandle→`handleTarget`) — otherwise `asInt64` on OpHandle-marked wing
  slots misdecodes. Residual rule pinned to src/dst endpoints;
  `rankContains`/`linked(member,dim,NEG)` is the "already holds" query.
- **`MarksOutstanding`/`ChoiceStackLive` are environmental refusals** — refuse `ensureCells` in
  *every* mode: a C++ fallback under a live mark is the identical untrailed-state corruption (ergo).
  `ChoiceStackLive` checked at `bindManifold` entry pre-move-assign; trigger named (produce cursors
  surviving `call()`).
- **Arena-swap made implementable** — architect's deepest find: no identity token exists (arena
  move-assigns in place; no generation counter) *and* `VortexCore::home_`/`dims_` are ctor-only →
  post-swap `resolve()`/`findOrMintDimension` consume dead refs. Now: `base()`+`home()` identity
  token, `bindManifold` runs a `VortexCore` genesis resync before `ensureStdlib` re-fires; driver
  set (`bindXanaduStore` document rebind, incl. today's dangling `baseManifold_`) named.
- **`vortex.stdlib_source` enforcement + timing** — ergo + architect independently: read at the
  *Settings call site* pre-first-context (`xuzz_app.cpp:1589–1596`, never through `fromStore` —
  `bindStore` feeds it document stores, and the first `ensureCells` fires in the `VortexHost` ctor
  before any config load). xuzz-only key, honestly scoped; `StdlibPolicy` is per-field `optional`s;
  bad spellings warn.
- **§6.4 made honest** — ergo + architect: production path named (installed
  `vortex-stdlib-gen --out`; `:export-lib` fails `FormatRefused`; interactive authoring is U-X8);
  local graft gated on stdlib *presence*, opt-in under `Cpp`, generator pins `localImage=false`
  (determinism can't depend on the Makefile's XDG redirect); `Image`+ base-refusal suppresses it.
- **Process plumbing** — `unique_ptr<State>`+`once_flag` carrying
  `optional<expected<Triple,Refusal>>` (latched refusal has a home; immobile flag works under
  indirection); `resetStdlibImageForTesting` covers both base and local holders;
  `makeWorkerContext`'s orphan `bootstrap()` (`vortex_host.cpp:113`) → `ensureStdlib()`; `vicon`
  added to consumers, `vquery` removed; `status()` pinned (`VortexHost::stdlibStatus()` +
  `vortex_stdlib_status` builtin).
- ~45 minors: enum mapping (store refusals → `ImageUnreadable`), `insertIntoRank` mechanism
  corrected, sig-record mint placement, dev-fields appositive, `discard` boundary drawn, name-merge
  candidate pinning, mark-edge replay policy, newer-same-epoch row, §12 restate (~5.3–6.8K
  records+markers), introspection-surface disclosure, cite-drift batch.

Disposition: `r4-disposition.md` (7 accepted clusters, ~49 accepted minors, 0 refused).

## Round 5 → revision 5 highlights

- **The swap story rewritten honestly** (ergo BLOCKING + 4 critics converged): `bindManifold` has
  *zero in-tree callers* — `bindXanaduStore` **replaces** the host wholesale (`engine_.reset()` :364
  → `make_shared<VortexHost>` :2857–2866), so a `ChoiceStackLive` gate pinned at `bindManifold`
  entry could never fire while the real path silently destroyed streaming results. Now: (a) the gate
  sits at `bindXanaduStore`/`attachVortexHost` entry — `outstandingMarks() != 0` before
  `engine_.reset()`, covering *every* surviving VM's stack (`primary_.vm` **and**
  `vplEngine_.ownedVm_`), produce cursors, and stray `mark()` holders via arena marks, no accessor
  needed — refusal retains old host+engine+document with a recovery contract; (b) `bindManifold`
  keeps entry-check + resync + clears as the in-place primitive for the MT direction, §11 as its
  exercised coverage; (c) retained-host `baseManifold_` dangling = out-of-scope-but-noted; (d)
  engine caches outside core/vm/stdlib are caller-managed — `vplEngine_`/`vqlEngine_`/`vqlCompiler_`
  envs, action registries — named boundary + §11 assertions; `installWriteGuard` last-wins.
- **The xuzz seam moves above `xuzz_app.cpp:1588`** (architect): the *first* context builds inside
  the `ZigzagVisualizer` **ctor** (`populateFallbackStructure` → `adoptDocument` →
  `ensureVortexHost`), one statement *before* the R4 seam — and the fold latches process-wide, so a
  seam at :1589+ means a permanent unset-policy fold.
- **P26 rank-blindness clause** (zigzag): moved helpers resolve the mark dim via `findDimension`/the
  `d.dims` rank or `dimensionsOf`-text-match — **never** `arena.dimensions()`, which is empty on the
  null-base scratch (rank-only filing, no `ensureDimension` callers); without it the harvest returns
  nothing and every marked ref refuses at graft.
- **Local image = dev-only scratch** (codebase): `vortex-stdlib-gen --module` builds
  `ArenaManifold`+`VortexCore`+`VortexVM` *without* `VortexStdLib` — a `Cpp`-policy scratch
  bootstraps the whole base (→ ~4,300 shadowed duplicate symbols at graft; `promote` floods both
  directions so `--module` can't bound it on a full scratch). V1 limit: dev modules self-contained.
  `localImage` gets precedence (setter > `GLEDITOR_STDLIB_LOCAL` > `value_or(mode != Cpp)`),
  `GLEDITOR_STDLIB_LOCAL_DIR`, the dev loop end-to-end (edit builders → `--module` → **restart**),
  the install rule.
- **Status surface made real** (ergo): `stdlibStatus()` on `VortexStdLib` (not the xuzz-only host);
  `source` → `optional`; `local` sub-record; the `vortex_stdlib_status(-Status)` builtin answers
  *before* the use-point throw (the `resolve()`/`modules()` diagnostic exemption); reachability
  scoped honestly (builtin = vprolog + `solveLogic`; `vpl`/`vagent`/`vicon` = C++ accessor + log);
  refusal surfaces once in xuzz's error surface at init.
- **`keyRecord` mint helper** (codebase): `varsRecord` *always* files `+d.owner` — the "deliberately
  unscoped" manifest needs the 3-line scope-less variant; `sigCell` restated as the same unscoped
  unranked-holder shape.
- **Validation gap closed** (ergo): a valid non-stdlib store → `FormatRefused` "no `stdlib.image`
  manifest"; `size() != expectedScrollBytes` → `ScrollTruncated` (sealed artifact);
  `expectedScrollBytes` dev placeholder pinned.
- ~35 minors: `release()` truncates `scratch_` (interned bytes die with cells — bit-identical
  rollback), `rankContains` not `linked(NEG)`, resync `rankTail` append + skewed-base limitation,
  mark-aware-exporter scope, `shared_ptr<State>` per `ensureCells`, CAS write primitive, `vicon`
  consumer list, U-X6 fixed, `or_parallel .cpp:210`, `generatorHead` arithmetic, ephemeral-target
  qualifier, identity token = `base()`+`core().home()` cross-base-only, sentinel = recorded key ref.

Disposition: `r5-disposition.md` (7 accepted clusters + ~35 minors, 0 refused).

## Round 6 → revision 6 highlights

- **The gate moves into the funnel** (5 BLOCKINGs, 6 critics converged): the R5 `ChoiceStackLive`
  placement covered 2 of ~9 host-drop sites — `ensureVortexHost` unconditionally replaces
  `vortex_host_` whenever `engine_` exists (:2857–2866), called from `adoptDocument` :306 (the
  funnel behind `adoptXanaduStore`/`adoptXanaduDocs`/`populateFallbackStructure`/
  `xuzz_app.cpp:629,:836,:3008`) and six command paths (:3345–:3690); the doc even conceded the path
  at :719–721 while omitting it from the gate. Now the check sits at `ensureVortexHost`'s **replace
  arm** — `liveMarks() != 0` aggregated over `arena()` + `extraContexts_` arenas (marks are
  per-arena; null host ⇒ 0) — and every gated API returns `std::expected`:
  `bindXanaduStore`/`adoptDocument`/`adoptXanaduStore`/`adoptXanaduDocs`/
  `attachVortexHost`/`bindManifold`/`ensureVortexHost`. `bindZigzag` skips all bookkeeping on
  refusal + `commandBarFeedback_` surfaces it; command paths refuse to *replace* (proceed on
  retained host) — "cursors can't outlive a command" refused, `goalProduce` suspends marks across
  commands deliberately.
- **`liveMarks()`** (zigzag+vortex+writer): `hydratePrefix` seeds `outstandingMarks_ = 1`
  permanently (:2192) — the gate counts marks above the floor, with `or_parallel`'s transient
  `ShadowDeltaPrefix` arenas (:345–349) named as the only hydration users outside the gated set;
  `extraContexts_` share `baseManifold_` and are never hydrated.
- **`--module` gets a real construction** (4 critics): private `build*Module`
  - unconditional ctor `bootstrap()` + manifest/`"ops"` inside `bootstrap()`'s tail meant a
    stdlib-less scratch produced a manifest-less artifact that fails its own `FormatRefused`. Now: a
    bootstrap-suppressed `VortexStdLib` (ctor tag, `source=none`) + public `buildModule(name)`
    facade + the factored `beginImageOps`/`buildModule`/`endImageManifest` skeleton —
    `bootstrap()`'s driver becomes those calls over the 21 base modules; P32 gains
    `vortex_stdlib.{hpp,cpp}`; self-contained-scratch failure = generator exits non-zero naming the
    unresolvable symbol.
- **Status builtin ordering pinned** (4 critics): `solveQuery`/`solve`/`call` do *not* throw at
  entry — the refusal throws inside `solveQueryHelper`'s per-goal dispatch *after* the
  `vortex_stdlib_status` arm (functor-text match needs no export); `call()`'s entry throw stays but
  `call(path)` on the builtin is unreachable under refusal (solver-goal only); `createPredicate`+
  `exportSymbol` in `std:logic` + `predVortexStdlibStatus_` joins the rebind list. `local.state`
  enum pinned; `liveMarks` optional in `status()`; the startup log always reports the local decision
  \+ dir.
- **Rollback scoped** (zigzag): `release()` covers the 4 mark-trailed vectors only — claim narrowed,
  invariant pinned (all graft dim mints rank-filed `findOrMintDimension`; no
  `ensureDimension`/`bindForeignDim`/guard inside the mark); §11 asserts
  `arenaDims_`/`boundDimensions_` unchanged. VM clear-list
  +`oraclePath_`/`cutCallback_`/`currentDeweyPath_`/`totalCycles_`; `~VortexVM` clears-own-guard or
  resync re-installs (transient VMs unguard a shared core); `rebindInstance` repopulates
  `opcodeMap_` in place (installed funnel keeps consulting the same live map).
- **Dev-loop UX closed** (ergo): `--out` names the leaf image dir + the `GLEDITOR_STDLIB_LOCAL_DIR`
  pairing rule; explicit `LOCAL=1`+absent warns; `GLEDITOR_STDLIB_DIR` leaf-level;
  `GLEDITOR_DATADIR` build-time; the key absent from shipped Settings; `vlace`/`vlacec` consumers;
  bare-`VortexCore` helpers never fold; per-command `WorkerContext` footnote in §12.
- ~30 minors: `keyRecord` = `fragment.cpp:39–49` hoist (4 critics found it),
  sparse/private-rank-only scope; `d.params` rank-filed extension; live-arena `findOpHandle`
  conflation named; `expectedScrollBytes` span-free writes + unexported placeholder; `spawnCursor`
  mark attribution; engine-death `:364`; `vlace` precedent; citation batch (`mesh_evaluator:727`,
  `executor:567,640, 2584`, `or_parallel:349`, `mesh_evaluator:920`, `registerSubscroll`/
  `ingestSubscroll`, `store.cpp` split, `vortex_host_test.cpp:181`, goal-helper ranges,
  `arena_manifold.hpp:749`); §11 skew arms (grown scroll, manifest-less `FormatRefused`, span-end
  fallback) + command-path arm + `expected` assertions; §6.5 goal-symbol delta; end-to-end gate is
  `SDL_VIDEODRIVER=offscreen` windowed — `--headless` returns before `VortexHost` exists (vacuous).

Disposition: `r6-disposition.md` (7 accepted clusters, ~30 minors, 0 refused).

## Round 7 → revision 7 highlights

- Gate document adoption **before mutation**; keep command replace-arm and direct-attach checks.
  Refusal preserves actual host/engine/store pointers, document/focus maps, pane, and bookkeeping.
- Add public host mark aggregation over private worker contexts, and explicit arena hydration-seed
  accounting that remains correct when the seed is evicted.
- Pin visible feedback: the refusing visualizer member writes its private fields and opens the
  command bar. External bind lambdas only consume/propagate expected results.
- Skip commands on refused replacement rather than constructing transient compilers under suspended
  marks. Preserve existing command result types and real embedding-based recovery.
- Specify owner-keyed live write predicates, removing one VM's ability to unguard a surviving VM;
  include core-header costs and tests. Clear choice stacks and oracle cursors on swaps.
- Pin repeatable module selection, canonical build order, checked dependencies, manual construction,
  optional-source absence, and refusal of nonempty generator output directories.
- Reject the codebase critic's wrong-checkout inventory/helper/citation claims: they match the
  default `/data/git/gleditor-restore` tree, not this review's target worktree. Keep the original
  report and disputed tally visible; disposition records independently verified target blob hashes.

Disposition: `r7-disposition.md` (7 accepted clusters, wrong-tree claims explicitly refused).
Revision 7 was reviewed in Round 8; no implementation is claimed.

## Round 8 → revision 8 highlights

- All critics verified the correct worktree; no BLOCKING findings remain.
- Scope stdlib status to `arenaLiveMarks` and initialization failures. Host aggregate replacement
  counts belong to the operation's error/HUD payload, not an inaccessible stdlib back-pointer.
- Name startup error plumbing: the visualizer reads status after constructing the host and owns a
  structured diagnostic latch that survives per-command host rebuilds.
- Remove the unimplemented cancel promise; recovery names owning-runner resumption honestly.
- Add concrete engine/compiler `onArenaSwapped()` APIs and touched files to P30.
- Refuse hydration before mutation when raw marks exist; consume its expected result at the sole
  OR-parallel caller, and specify actual move/seed lifetime rather than nonexistent reset APIs.
- Scope the image mark invariant and gate direct UI compilation even when it does not replace a
  host. Arbitrary direct VM registration/trailing redesign remains outside this migration.

Disposition: `r8-disposition.md` (6 accepted clusters plus minors). The starred verdicts normalize
contradictory critic reports: their text said CONVERGED while reporting MAJOR findings, so the
strict zero-MAJOR gate treats them as NOT CONVERGED. Revision 8 was reviewed in Round 9.

## Round 9 → revision 9 highlights

- Four critics converge; three remaining majors concern VQL coordinator cache lifetime, residual
  swap-refusal status wording, and the primary-only compilation mark domain.
- Add in-place coordinator reset, preserving VQL's reference while clearing mounted-store,
  home/tail, and dimension caches; account for files and post-swap tests.
- Keep swap refusal solely in operation errors/HUD; distinguish primary compilation marks from
  all-context replacement marks.

Disposition: `r9-disposition.md`. Revision 9 was reviewed in Round 10.

## Round 10 → convergence

- All six converge: 0 BLOCKING / 0 MAJOR / 9 MINOR.
- Final notes clarify session mount redeclaration, optional/vector clearing, attachment
  preconditions, the existing dimension limitation, and P30's bounded scope.
- No further critique round is required by the stopping gate.

Disposition: `r10-disposition.md`. Runtime acceptance tests remain an implementation-stage task.
