# Round 5 disposition — vortex-stdlib-prebuilt-store.md

Round 5: **1 BLOCKING, 15 MAJOR, ~54 MINOR** across six critics (writer
0B/0M/9m **CONVERGED**; ergo 1B/5M/9m; zigzag 0B/2M/6m; architect 0B/2M/5m;
codebase 0B/1M/7m; vortex 0B/2M/12m). Findings deduplicate into 7 clusters +
a minor batch. Every cluster was spot-verified against the tree before
accepting. All accepted; none refused. Revision 5 follows.

## R5-1 (BLOCKING) — The swap story's reachability and coverage are wrong (ACCEPT)

*ergo B1; zigzag M2; architect M2; codebase m2, m6; vortex M1, M2 — five critics
converged.*

Three sub-defects, one fix:

1. **`ChoiceStackLive` can never fire as specified.** `bindManifold` has **zero
   in-tree callers** (decl `vortex_host.hpp:79`, def `vortex_host.cpp:149–158`).
   The shipped document-rebind path — `bindXanaduStore`
   (`zigzag_visualizer.cpp:362–394`) — calls `engine_.reset()` then
   `ensureVortexHost()` → `vortex_host_ = make_shared<VortexHost>` (:2857–2866):
   wholesale **host replacement**, silently destroying the outgoing host's live
   `choiceStack_`/produce cursors before any gate could run. Also the doc's
   "the reachable driver is `bindXanaduStore`" is thereby false twice over.
2. **The gate misses marks that survive.** Even where `bindManifold` is used,
   `choiceStack_` on `primary_.vm` is not the coverage set: `vplEngine_`'s
   `ownedVm_` is a second `VortexVM` over the same arena (no `vm()` accessor),
   produce cursors and stray `mark()` callers are non-VM marks, and post-swap
   `ensureCells` reads the *fresh* arena where `outstandingMarks()` is trivially 0.
   Three critics independently arrived at the same mechanism: **check
   `arena().outstandingMarks() != 0` pre-swap** — every choice point and produce
   cursor takes an *arena* mark (`vortex_vm.cpp:317–326`), so the count subsumes
   every VM and every stray holder with no accessor.
3. **Engine-level arena-keyed caches dangle unnamed.** `vplEngine_.defaultDim_`
   (`vpl_engine.hpp:140`), `env_`/`dependencyStack_`/`cellDependencies_`,
   `vqlEngine_.env_`/`perspectiveClones_`/`functors_`/`sessionMounts_`/`userDefs_`
   (`vql_engine.hpp:225–238`), `vqlCompiler_.varBindings_`
   (`vql/compiler.hpp:116`), and user entries of `customActionRoutines_`/
   `macroRegistry_` all hold `CellRef`s into `primary_.arena` and alias fresh
   cells post-swap. `initHostServices` re-registers only built-ins.

**Disposition — revision 5:**

- The user-facing gate moves to the outgoing host: `bindXanaduStore` and
  `attachVortexHost` check `vortex_host_->primary_.arena.outstandingMarks() != 0`
  **at entry, before `engine_.reset()`** — `ChoiceStackLive` refuses the rebind,
  old host + engine + bound document all retained; recovery contract stated
  ("drain or close the produce cursors, then re-issue"). `StdlibImageRefused`
  gains an environmental payload (live choice-point/mark count) — the
  dir+env-var `what()` shape doesn't apply.
- `bindManifold` keeps its entry check (same `outstandingMarks()` mechanism,
  pre-move-assign) + genesis resync + clears, honestly framed: the in-place-swap
  primitive with **zero in-tree callers today**, exercised only by the §11 swap
  gate, and the mechanism the multithreaded-host direction
  (`design/vortex-multithreading-study.md`) generalizes.
- Retained-host exposure restored to "out-of-scope-but-noted" (per zigzag M2):
  a `vortex_host_` `shared_ptr` copy held beyond `engine_.reset()` keeps a
  dangling `baseManifold_` — host *replacement* retires it; the resync does not.
- Engine caches: draw the boundary explicitly — "arena-keyed caches outside
  `VortexCore`/`VortexVM`/`VortexStdLib` are caller-managed; document rebind
  rebuilds `vqlEngine_`/`vplEngine_`/action registrations" — with the
  boundary's consequence named (`vqlEngine_.env_` is session state; it is
  dropped, not remapped) and §11 asserting it (post-swap `defaultDim()`
  resolves the new `d.1`, env lookups miss rather than alias).
- `installWriteGuard` is last-wins (`vortex_vm.cpp:84–95`): name that the
  core's write funnel consults whichever VM installed last (`ownedVm_`).
- §5.4 parenthetical: `ChoiceStackLive` refuses the *swap/rebind*, not
  `ensureCells`.
- P30 Location gains `zigzag_visualizer.cpp` and `vortex_core.{hpp,cpp}`;
  §11.8 gains a `bindXanaduStore`-driven gate beside the `bindManifold` gate.
- §1's "re-runs every time `bindManifold()` rebinds a document" corrected —
  document rebind replaces the host (ctor path); `bindManifold` is the
  future in-place swap.

## R5-2 — The xuzz seam is one statement too late (ACCEPT)

*architect M1; corroborated by writer m4, codebase m1, ergo (seam-timing verify).*

The first `ensureCells` fires inside `make_shared<ZigzagVisualizer>("")` at
`xuzz_app.cpp:1588` — ctor → `populateFallbackStructure` (:74→:157–213) →
`adoptDocument` (:212→:215–308) → `ensureVortexHost` (:306→:2857–2866) →
`VortexHost` ctor → `WorkerContext` — **before** the doc's cited :1589–1596
window and before any `loadConfigFromStore` (the ctor's `adoptDocument` →
`bindStore` → `loadConfigFromStore(*store_)` already runs on the fallback
document store). A seam in the cited window can never precede the fold, which
latches process-wide — `Image` mode + installed assets = latched `ImageMissing`
before the dir setter could run; `ImageThenCpp` = silently C++ forever.

**Disposition:** seam moves **before `xuzz_app.cpp:1588`** — `session` and
`systemStore(Settings)` are in hand there; the `vortex.stdlib_source` read +
mode setter and the `assetPath("vortex-stdlib")` dir-setter init run
immediately before `make_shared<ZigzagVisualizer>("")`. Re-cite the
first-context driver chain through the ctor path; note the ctor-path host is
discarded by `bindXanaduStore`'s `engine_` replacement (:370) but the fold
latch is process-level, which is what makes the ordering fatal. Fix "already
in hand" → hoisted above :1588 (writer m4 / codebase m1 wording absorbed).
`vortex.stdlib_source` is stated **startup-only** — the Settings-change arm
(`xuzz_app.cpp:4219–4228`) never re-reads it while sibling `vortex.*` keys
re-apply live; a mid-session edit applies next launch (architect m1 + ergo m4:
`SettingSpec` notes carry this).

## R5-3 — P26's moved helpers are rank-blind in the specified scratch arena (ACCEPT)

*zigzag M1.*

`markedRelocationSlots`/`dimOf`/`restateRefSlots`/`harvestRelocationTargets`
all resolve `d.mesh.relocation` via `arena.dimensions()` (`lineage.cpp:145–249`)
— the filed name table (`base_ ∪ arenaDims_`, `arena_manifold.cpp:34–46`). The
doc mandates `findOrMintDimension` (rank-only filing, `vortex_core.cpp:18–102`)
on a null-base scratch `WorkerContext` where nothing calls `ensureDimension` —
so `arena.dimensions()` is empty, the harvest returns nothing, marked Int64s
promote verbatim naming *source-arena* refs, and §5.2.3's map-miss rule refuses
every marked ref at graft. The image cannot be produced; P26's two clauses are
mutually inconsistent. Works in vagent today only because
`dimensionVocabulary`+`fileNameSet` pre-file into a *base* store. Graft side is
immune (`isCellRelocationHandle` matches by `dimensionsOf` text).

**Disposition:** P26's DRY clause gains the sentence — moved helpers resolve
the mark `DimRef` via `VortexCore::findDimension` (the `d.dims` rank) or detect
marks by `dimensionsOf`-text match like `isCellRelocationHandle` — **never**
`arena.dimensions()`; the scratch generator arena has no `arenaDims_`/`base`
dim table. Bonus noted: same fix un-breaks the `executor.cpp:564,634,2924`
probes (pre-existing latent blindness).

## R5-4 — The named local-image path re-imports the whole base stdlib (ACCEPT)

*codebase M1 + ergo M4 + codebase m4/vortex m4.*

The generator's `StdlibPolicy{Cpp}` pin means the scratch context bootstraps the
**full base stdlib**; a whole-space export "after dev additions" carries base +
dev, and at graft the name-merge covers only `home +d.stdlib` members (module
cells) — every base *symbol* tail-appends as a shadowed duplicate (~4,300
cells), contradicting §6.5's "the image removes the duplication". `--module`
can't bound it: `promote` floods `edge.pos` **and** `edge.neg`, so a
module-rooted export reaches the whole library anyway. And `localImage`
defaults/precedence are unpinned (no env/Settings channel, "defaults true for
app modes" vs "opt-in under `Cpp`" contradiction).

**Disposition — revision 5:** pin the dev-only scratch: `--module <name>`
constructs `ArenaManifold` + `VortexCore` + `VortexVM` **without**
`VortexStdLib` (no bootstrap), mints only the named dev module(s), and exports
whole-space — there is no base to flood, so the whole image is the patch.
V1 limit stated: dev modules must be self-contained (no stdlib ops bound at
build time). `--out` on a full `Cpp`-policy scratch remains the *base* image
producer. `localImage` gets its own precedence bullet — explicit setter >
`GLEDITOR_STDLIB_LOCAL=0|1` > `localImage.value_or(mode != Mode::Cpp)` — plus a
`local` sub-record in `status()` (`{present, grafted|skipped|failed|suppressed,
dirResolved}`), a `GLEDITOR_STDLIB_LOCAL_DIR` override (taken-as-given), the
dev loop stated end-to-end (edit builders → `vortex-stdlib-gen --module ...` →
**restart the app**; the fold is once-per-process), the P32 install rule
(`$(OBJDIR)/vortex-stdlib-gen` + `install -m 755` beside `Makefile:1798–1802`),
and §6.5's honesty list extended with the double-enumeration consequence for
any same-text duplicates that do land.

## R5-5 — The status surface cannot describe or reach its consumers (ACCEPT)

*ergo M2 + M3.*

`VortexHost::stdlibStatus()` is xuzz-only; the builtin dispatches inside
`solveQueryHelper` — reachable only via `solve*` (vprolog + xuzz `solveLogic`),
never by `vpl` (no `solve*` call) or `vagent` (own solver), and **unreachable
exactly when needed**: `solve()`'s use-point throw precedes the dispatch arm,
and `solveLogic` swallows it to `std::cerr`. The record itself can't express
"nothing loaded" (`source∈{image,cpp}` has no refusal value) or any local-image
state.

**Disposition:** `stdlibStatus()` moves to `VortexStdLib` (the instance every
consumer owns) as a C++ accessor over the process folded-image state;
`source` → `std::optional`/`none`; `local` sub-record added; the builtin's
dispatch arm **answers before the use-point throw** — the same diagnostic
exemption `resolve()`/`modules()` already get ("answer honestly under
refusal"); term shape pinned (`vortex_stdlib_status(-Status)` record term, the
`vortex_param/4` precedent); each consumer's surface named or scoped honestly
(vprolog/xuzz builtin; `vpl`/`vagent`/`vicon` get the C++ accessor + startup
log line — no fake REPL commands promised); `dirResolved` reports the
*attempted* dir under explicit-setter refusals; a recorded refusal surfaces
once into xuzz's error surface at init (not only at use-points).

## R5-6 — `varsRecord` cannot mint unscoped records (ACCEPT)

*codebase m3.*

`varsRecord` with `mint=true` **always** files `record +d.owner→ Int64(owner)`
(`records.hpp:113–118`) — no scope flag — and unconditionally mints `d.owner`
(`:101`). The "deliberately unscoped" manifest cannot be produced verbatim.

**Disposition:** name the mint path — a `keyRecord(owner, key, value)` helper:
`makeCell(key)` + `link(rankTail(owner,vars),vars,POS,record)` +
`link(record,values,POS,value)` — the three `varsRecord` lines minus the
`+d.owner` arm and its `d.owner` dim mint (keeps the registry at ~21–22 dims).
`sigCell` restated as an unscoped **unranked holder** (a vars record is an
on-rank key cell that cannot own a private rank — writer m7).

## R5-7 — A valid non-stdlib store falls through every validation arm (ACCEPT)

*ergo M5 + zigzag m2 + vortex m6.*

A `$GLEDITOR_STDLIB_DIR` pointed at an ordinary document store folds fine:
genesis present → not `FormatRefused`; complete scroll → `expectedScrollBytes`
can't run (no manifest); `stdlib.image` lookup misses → `imageEpoch` has no
operand. No named cause.

**Disposition:** a successful fold carrying no `stdlib.image` key record →
`FormatRefused` naming "store carries no stdlib.image manifest — not a stdlib
image"; the span-end fallback applies only when the manifest's names may
themselves be truncated; comparison direction pinned `size() != expected` →
`ScrollTruncated` (sealed artifact — an appended tail is a refused deviation);
the `expectedScrollBytes` dev placeholder joins the dev-field list (vortex m6).

## Minor batch (all accepted; folded into revision 5)

- writer m1/vortex m5: `"ops"` mint timing — opsHead + `+d.step` chain mints at
  bootstrap start; the keyed `"ops"` record is minted-and-filed at bootstrap
  end via the record-mint helper (`varsRecord`/`keyRecord` unconditionally
  `makeCell`s the key — a pre-minted record would duplicate).
- writer m2: "covers both holders" → "covers both folded-image states" (§5.1,
  §6.4).
- writer m3 + architect m4: identity token = `arena().base()` +
  `core().home()`/`home_` (not `arena().home()` — `nullopt` for null-base);
  distinguishes **cross-base** moves only; same-base swaps are caught by the
  unconditional re-fire + `stdlibPresent` probe + sentinel; the generation
  counter remains the named alternative.
- writer m5: `vicon` joins the env-or-default consumer list.
- writer m6: `vquery` out of the U-X6 per-app subsetting question.
- writer m8: sentinel re-verify = recorded grafted `stdlib.image` key ref +
  `contains` + `textOf` (map isn't retained).
- writer m9: `baseManifold_` is repointed by `bindManifold` itself (:154), not
  the resync; `extraContexts_` workers share the pre-existing dangling-base
  exposure — bounded claim, no mechanism change.
- ergo m3: §6.5 honesty extended — shadowed same-text local symbols
  double-enumerate in `symbolsInModule`/`vortex_function`/`findall`;
  `stdlib.image` records visible on `home +d.vars` browsing.
- architect m2: per-field setters update through `compare_exchange_weak`
  retry loop on the shared-pointer atomic.
- architect m3: `shared_ptr<State>` acquired per `ensureCells` under the mutex
  (in-flight grafts keep the old `State` alive across a test reset) — or
  quiescent-context contract documented.
- architect m5: `attachVortexHost` noted zero-caller + unconditionally replaced
  at the next `adoptDocument`/`bindXanaduStore`.
- zigzag m1/vortex m2: drop "equivalently" — `rankContains(anchor, member,
  dim)` is required; `linked(NEG)` answers a different question.
- zigzag m4: resync mints append at `rankTail` (not `mintNamedDimension`'s
  adopted-walk position); skewed-base orphan-tail named a known limitation.
- zigzag m5: "re-read by mark-aware exporters (the `relocation_marks.hpp`
  machinery), not by `promote` alone".
- zigzag m6: §11 gate 6 → single arm: foreign `"ops"`-texted record on
  `home +d.vars` must not register — shared-rank position irrelevant by
  construction.
- codebase m5/vortex m1: `release()` truncates `slots_`/`links_`/`content_`/
  `scratch_` — interned bytes roll back with their cells, not "dead spans".
- codebase m7: text/bits skew honesty clause — grafted ref-valued scalars
  carry image-space renderings; bits are authoritative, `textOf`/`xudu-dump`
  readers see stale text. Inert; named.
- codebase m8: `or_parallel.cpp:210`.
- vortex m3: "`loadConfigFromStore` invoked only inside xuzz
  (`xuzz_app.cpp:1596,4226`, `vortex_host.cpp:165`)".
- vortex m8: ops-precheck justification → foreign/malformed `stdlib.image`
  records (per-module images are `FormatRefused` upstream and never reach it).
- vortex m9: residual-rule consequence named — a name-merged dev module's own
  outgoing non-membership edges refuse the local graft (base kept).
- vortex m10: P26 boundary line — `findOpHandle`/`versionAnnotation`/`aliases`
  slot-scans conflate OpHandle wing slots with real handles on the folded
  image; image manifold serves load→graft only.
- vortex m11: `generatorHead` computed arithmetically (positional
  `MicroversionId`: promote head + planned trailing op count) — written,
  never read back.
- vortex m12: unmarked-OpHandle refusal fires only for ephemeral targets
  (`arena_manifold.cpp:1816–1819`; persistent-target handles need no mark).
- ergo m1: builtin arity/shape pinned in R5-5's fix text.
- ergo m5/m9, vortex m6 placeholder: folded into R5-5/R5-7.
