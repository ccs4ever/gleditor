# Prebuilt Vortex Stdlib Implementation

All proposals P26-P34 in `design/vortex-stdlib-prebuilt-store.md` are accepted by the user. The
design converged at Round 10, revision 9.

## Execution Rules

- Worktree: `metacircular_compiler_implementation_plan`; preserve concurrent Vagent edits.
- Workers use explicit `gpt-6-luna-high` selection, not an inherited default model.
- Give parallel workers disjoint files; the parent owns builds and integration.
- Run headlessly with isolated XDG paths; do not change on-disk format layout.
- Test and review each iteration before treating its proposals as implemented.

## Iterations

| Iteration | Scope                                                                                                             | Status      |
| :-------- | :---------------------------------------------------------------------------------------------------------------- | :---------- |
| 1         | P26 shared relocation helpers and OpHandle wings; P27 whole-space promotion foundation                            | Completed   |
| 2         | Remaining P26 stdlib scalar marks; P28 manifest/op enumeration; P29 persisted routine bindings; P33 cache exports | Completed   |
| 3         | P32 manual generation, verification, deterministic image and build target                                         | Completed   |
| 4         | P34 read-only opens and P31 image discovery/validation                                                            | Completed   |
| 5         | P30 ensure/graft/rebind plus allocator, VM, host, cache and refusal safety                                        | Completed   |
| 6         | Installation/packaging, consumer integration and end-to-end acceptance                                            | Completed   |
| 7         | Peer review (SE + architect + vortex expert), hardening fixes, final verification                                  | Completed   |

The scope of an iteration may be split further when source dependencies require it. The application
stays on the existing C++ path until the image acceptance gates pass.

## Iteration 1

Baseline `make -j$(nproc) xuzz_test` succeeds before migration implementation. The focused baseline
run covers 108 tests: 104 pass and four existing failures remain:

- `VagentLineageTest.CapcallRefResultRestatesThroughRegenesis`
- `VagentLineageTest.CapcallCodeResultRestatesThroughRegenesis`
- `VagentLineageTest.RunSeesEnclosingLetLocals`
- `VagentLineageTest.RunSeesOwnerFieldCaptures`

The first two report unknown `Nav` names in the current compiler tree; all four fail before
migration edits. They are not silently attributed to this migration. Logs are
`/tmp/vortex-stdlib-i1-baseline-build.log` and `/tmp/vortex-stdlib-i1-baseline-test.log`. Source is
already dirty from the concurrent compiler project.

The current session cannot dynamically select the new model-pinned profile. Luna workers therefore
run as explicit-model CLI subprocesses. Temporary configs grant assigned patch writes, deny
shell/fetch access, and disable nested agents. The parent supplies source snapshots because these
workers have no native readers; it retains build, test, and integration ownership.

The first P26 slice deliberately excludes goal-module scalar marking and the stdlib exporter; P27
initially adds the opt-in promotion flag without changing existing export defaults. Later iterations
connect these foundations.

### Implementation and Verification Checkpoint

Luna implemented both assigned slices. New shared helpers live in
`apps/common/xanadu/relocation_marks.hpp`; Vagent's `refCell` remains available through an alias.
Wing slots now use separately marked OpHandles and preserve legacy Int64 reads. Lineage/executor
lookups use rank-aware dimension discovery. Mesh evaluation reuses core wing bindings and files its
relocation dimension. PromotionBudget wholeSpace defaults false, lifts only d.dims exclusion, and
continues to skip d.lineage. Four arena tests and three initial core tests cover it.

The parent reviewed the changes and added a fourth core test rejecting Int64 references that would
truncate into valid CellRefs. Wing decoding now reuses the shared checked decoder.

The expanded **pre-change** baseline has 273 tests, 257 passing and 16 failing. After Luna's changes
the same selection has 280 tests, 264 passing and the **same 16 failures**, with no new failures. A
final rebuild/test including the parent's additional bounds regression succeeds in building and runs
281 tests: 265 pass, with exactly the same 16 baseline failures. All eight new regression tests
pass; no new failure names are introduced.

Artifacts retained for integration:

- Pre-migration source snapshots: `/tmp/vortex-stdlib-i1-baseline/`.
- Expanded baseline log: `/tmp/vortex-stdlib-i1-expanded-baseline-test.log`.
- Initial iteration build/test: `/tmp/vortex-stdlib-i1-build.log`, `/tmp/vortex-stdlib-i1-test.log`.
- Final bounds-fix build/test: `/tmp/vortex-stdlib-i1-build-final.log`,
  `/tmp/vortex-stdlib-i1-test-final.log`.
- Successful Luna worker reports: `/tmp/vortex-stdlib-luna-i1-relocation-context.log` and
  `/tmp/vortex-stdlib-luna-i1-promotion-context.log`.
- Their full source-snapshot prompts, per-worker permission configs, and exported transcripts also
  remain in `/tmp/vortex-stdlib-luna-i1-*`.

With user approval, clang-format 19.1.7 was installed privately at
`/tmp/vortex-stdlib-clang-format-19/clang_format/data/bin/clang-format`. It formatted only
iteration-changed ranges relative to the saved snapshots, plus the new helper header. Markdown
formatting/lint and diff whitespace pass. Broad concurrent dirty changes are not reformatted.

## Iteration 2

Three explicitly pinned Luna workers implemented disjoint stdlib, hydration, and guard slices. The
latter two P30 foundations were pulled forward because they are independent of file loading and
unblock safe registration later. The parent owns integration and the ABI epoch helper.

- Goal op/continuation/dimension scalar references use shared relocation marks.
- A private two-cell `stdlib.image` holder carries one opcode enumeration and manifest fields. The
  C++ development manifest has the engine ABI epoch, content version zero, head `cpp`, and a zero
  scroll-size placeholder. It is not an installable image.
- Each exported symbol carries an authoritative `d.params` signature with private `in`/`out`
  positional-reference ranks. Manual construction and explicit in-arena rebinding are tested.
- `halt` and `sentinel` join the goal exports. Rebinding reconstructs goal and predicate caches.
- One descriptor table defines canonical module names, builder dispatch, and generation order.
- `stdlib_image_format` hashes ordered opcode/Structure labels and both decode boundaries. Manifest
  counts describe those ABI tables, not the number of emitted opcode cells.
- Arena liveness explicitly distinguishes the evictable hydration seed from real marks. Hydration
  refuses raw outstanding marks before mutation, and OR-parallel workers consume the expected
  result. Moves transfer all accounting and leave the source usable.
- Core write guards are owner-keyed. Transient VM construction/destruction cannot remove another
  VM's protection; VMs are non-copyable/non-movable because callbacks capture their identity.

Integration caught an existing topology bug: `from` and `seq` linked the same entry on `d.values`,
so the second export silently unlinked the first. `seq` now has its own Jump entry into the shared
generator graph, with the same authoritative signature. Both work after rebinding.

A worker's premature automatic manifest-presence shortcut was removed. Existing lineage folds do not
yet carry fully relocated image metadata or the whole dimension registry, so probing them as loaded
images introduced twelve regressions. Automatic construction remains on the C++ path; only explicit
`rebindInstance` uses persisted metadata until the full loader iteration.

### Verification

The broader pre-change selection has 316 tests: 299 pass and 17 fail. Its additional failure is
`VortexStdLibTest.MapModuleBstAndOrdering` (evaluation byte budget exhausted); the other sixteen are
the recorded Vagent baseline. After integration and changed-range clang-format 19.1.7, 332 tests
run: 315 pass and exactly the same 17 fail. All sixteen new tests pass.

- Source snapshots: `/tmp/vortex-stdlib-i2-baseline/`.
- Baseline: `/tmp/vortex-stdlib-i2-baseline-test.log`.
- Final build/test: `/tmp/vortex-stdlib-i2-build-formatted.log`,
  `/tmp/vortex-stdlib-i2-test-formatted.log`.
- Luna prompts/configs/reports: `/tmp/vortex-stdlib-luna-i2-{manifest,hydration,guards}-*`.

No image file loader, installed artifact, consumer switch, or full host/adoption safety is claimed.

## Iterations 3-4 Complete

All five Luna worker patches integrated, caller-adapted, built and verified.

- **Export**: `VortexStdLib::exportImageToStore(Store&, int64_t)` returns
  `std::expected<MicroversionId, std::string>`; validates source/destination state, manifest fields,
  dimension registry, relocation references, promotion mapping and the final physical head. Legacy
  `exportStandardLibraryToStore` delegates to it.
- **Generator**: `tools/vortex_stdlib_gen.cpp` (`--stdlib`, repeatable `--module`, `--out`,
  `--verify`) builds in memory, verifies, and publishes only to an absent-or-empty directory.
  `--verify` delegates to `openStdlibImage`, so the artifact passes the exact consumer loader.
  `make stdlib-image` writes `build/vortex-stdlib-image` (override `VORTEX_STDLIB_OUT`).
- **Read-only storage**: `SpoolResult<T>`/`SpoolRefusal::ReadOnly`/`requireSpoolWrite` in
  `spool_write.hpp`; ops spool, primedia spool, `UserPermascroll` and `Store` refuse mutations with
  typed errors; read-only opens use `O_RDONLY`, never create files, and reject missing/empty ops.
- **Loader/policy**: `stdlib_image.{hpp,cpp}` — `StdlibMode`, atomic per-field `StdlibPolicy` (CAS
  setters), mode precedence explicit > `GLEDITOR_STDLIB_SOURCE` > `vortex.stdlib_source` >
  `ImageThenCpp`, image-dir discovery (`GLEDITOR_STDLIB_DIR` > `GLEDITOR_ASSET_DIR` > datadir >
  `./assets`), `localImage` default `mode != Cpp`, structured `StdlibRefusalCause` refusals, and
  `openStdlibImage` semantic validation (spans, manifest, epoch, opcode enumeration, signatures,
  relocation marks, read-only confirmation, unchanged files).

### Integration fixes the workers could not see

- `hasOnlyValueOwner` exempted no rank backlink, so it rejected every holder that heads the private
  `+d.vars` rank the design requires; it now allows the holder's own first-member negward edge on
  `d.vars` (applied to both the manifest holder and routine signature holders).
- Artifact layout corrected to `store/ops.nodes` + `store/store.tables` beside `permascroll/`.
- `is_directory(path, ec)` reports ENOENT through `ec`; discovery now gates on `exists()` so absent
  images name `ImageMissing`, not `ImageUnreadable`. `OpsSegmentUnreadable`/`StoreTablesUnreadable`
  are named in refusal details.
- `PrimediaSpool` facade keeps its total API and unwraps via `requireSpoolWrite`.
- Fallible callers adapted across store text writes, publication sealing, subscroll, mesh, evidence,
  foreign-op, UI session and tests.

### Verification

- `make stdlib-image` produces a 195K image: 10,422 ops, 5,012 cells, 21 modules; `--verify`
  reports `manual-rebind=ok`.
- Focused selection: 486 tests, 485 pass; the only failure is the recorded pre-existing
  `VortexStdLibTest.MapModuleBstAndOrdering` (byte budget). All new export/generator/loader/policy/
  read-only tests pass, including image determinism and populated-output refusal.
- Logs: `/tmp/vortex-stdlib-i3-gen2.log`, `/tmp/vortex-stdlib-i34-tests3.log`.

### Iteration 5 — graft / ensure / rebind

- `VortexStdLib::graftImage(const StdlibImage&)` clones the validated image subgraph into the
  destination arena under one `ArenaManifold` mark: dimension registry replay via
  `findOrMintDimension`, module merge by name, cell/value-bit clone, rank-edge replay with
  shared-anchor append rules, relocation-marked Int64 references and OpHandle target
  translation, and manifest/routine-record chain harvest. Every refusal path discards the mark,
  leaving the arena untouched; live marks, an already-ensured instance, a pre-existing
  `stdlib.image` record, missing schema, and out-of-subgraph references all refuse.
- `ensureStdlib()` resolves policy, probes the arena for an existing `stdlib.image` record
  *first* (rebind instead of re-graft or re-bootstrap — this also prevents the duplicate-manifest
  regression `SovereignLibraryPackaging` exposed), then grafts the image or falls back to C++
  bootstrap per `StdlibMode`.
- `rebindInstance()` rebuilds routine bindings and the ops enumeration from the arena's manifest
  for both grafted and C++-built libraries; a second `VortexStdLib` on the same arena rebinds
  without re-grafting (`SecondInstanceRebindsWithoutRegrafting`).
- `VagentP15Test.RegisterProgramImageRepopulatesAFreshContext` updated for owner-keyed write
  guards: the unclaimed "stray" opcode cell is now minted by a scoped VM whose guard dies with
  it, since `h.vm`'s live guard legitimately still protects cells it minted.

### Verification (iteration 5)

- Focused graft/loader/generator/host selection: 18/18 pass, including graft success, policy
  graft, `ImageThenCpp` fallback, second-instance rebind, live-mark refusal with untouched
  arena, same-instance regraft refusal, and `SovereignLibraryPackaging` export→import round-trip.
- Broad `xuzz_test` run in progress at time of writing; prior run's only migration-caused
  failures (the two above) are fixed. Remaining failures match the recorded pre-existing Vagent
  WIP set plus `VortexStdLibTest.MapModuleBstAndOrdering`.

### Iteration 6 — seam, packaging, acceptance

- `vortex.stdlib_source` setting key registered; `xuzz_app.cpp` calls
  `applyStdlibSettingsSource` with the resolved asset path before the first `WorkerContext`,
  so every worker context inherits the image policy.
- Checked-in artifact at `assets/vortex-stdlib/` (`store/{ops.nodes,store.tables}` +
  `permascroll/active.primedia`, ~780K), regenerated by
  `make stdlib-image VORTEX_STDLIB_OUT=assets/vortex-stdlib`. Makefile `install`/`uninstall`
  ship it as one unit under `$(INSTALL_DATADIR)/vortex-stdlib/`; `build-msys2.sh` stages it
  into the portable assets dir; `packaging/smoke-test.sh` verifies both halves resolve.
- Real-app acceptance: `xuzz` under offscreen SDL grafts the shipped image on both arena
  shapes (pristine vortex home and the document-backed base manifold) with zero fallback
  warnings; `GLEDITOR_STDLIB_SOURCE=image` (strict, throws on fallback) runs clean.

### Graft bugs found by real-app acceptance

- **Live-live edges**: replaying image registry adjacency between two already-live dimension
  cells (`d.contract -d.dims-> d.clone`) was refused as a foreign-rank attach. Relations
  wholly among pre-existing cells belong to the arena's own structure; the graft now skips
  them (merged modules are still reachable through home's `+d.stdlib` rank).
- **Split dimension registry**: on base-backed arenas the `d.dims` self-chain legitimately
  ends early, so `findOrMintDimension("d.vars")` minted a *duplicate* dimension and every
  graft edge landed under the wrong dim — `rebindInstance` then saw an empty vars rank.
  `graftDimension()` now resolves known system-dimension names directly through
  `SystemDimensions` and only falls back to `findOrMintDimension` for foreign names.
- **Dual scalar+content cells**: `promote()` routed `Int64`/`Double`/`Bool` cells through
  `makeScalarCell`, which restates the value's own rendering — opcode cells lost their
  `"#NAME"` labels and exported as `"27"`. Scalar-kind cells carrying exactly one content
  span now mint content first and take their bits via `setValue` in a second pass (the
  `cyclicHandles` two-phase shape); the image artifact was regenerated.
- **Export guard on grafted manifests**: `rebindInstance()` now marks
  `imageManifestEnded_` on success — a completed rebind *is* proof the arena carries a
  complete sealed manifest, so `SovereignLibraryPackaging`'s export→import round-trip works
  from a grafted library instead of refusing on the unset flag.

### Verification (iteration 6, in progress)

- Focused selection (loader/generator/host/graft/`VortexStdLibTest`/`VPLEngineExecution`):
  81/82 pass; the sole failure `VortexStdLibTest.MapModuleBstAndOrdering` fails identically
  under `GLEDITOR_STDLIB_SOURCE=cpp`, confirming it is the recorded pre-existing failure.
- `xuzz_test` full suite and `make test` (all three binaries + swarm) running at time of
  writing.

### Iteration 7 — peer review and hardening

Two independent read-only reviews (software engineer + system architect) audited the
migration surface against the design. Verdicts: APPROVE-WITH-FIXES / SHIP-WITH-FIXES.
Fixes landed:

- **Exception-safe graft mark**: `graftImage` mutations between `mark()` and `discard()`
  could throw (allocation, cell mint, link) and leak a live arena mark, wedging every
  later graft/bootstrap/compact. A local `MarkGuard` now releases the mark on any
  escape; `discard()`+disarm remains the single commit point.
- **Edge dimensions resolve by name**: the edge replay trusted `map[edge.dim]`, but a
  dimension cell outside the registry closure was already cloned as a plain cell —
  filing an edge under a non-dimension cell corrupts rank structure. A pre-pass now
  resolves every `d.*` edge dimension through `graftDimension()` *before* cloning, so
  `map[edge.dim]` is always a registered `DimRef` by construction.
- **Ops enumeration cardinality**: the manifest recorded only the compiled ABI kind
  count (`imageOpcodeCount` = 68 kinds), never the actual ops-rank member count — a
  truncated rank would rebind a partial opcode set silently (and `--module` subset
  images already did exactly that while verifying "ok"). New manifest field
  `imageOpcodeCellCount` records the real rank size at export; `rebindInstance`
  refuses when `enumeration.size()` disagrees. `--module` images stay self-consistent
  (they record their own subset count).
- **`promote()` OpHandle safety**: proxy op handles (whose targets live in foreign
  space and read as absent through `handleTarget()`) and unchecked `handleTarget`
  dereferences could reach mid-mint UB. Pre-mint validation now refuses proxies,
  absent targets, and ephemeral targets outside the promoted set; every op handle
  naming an unwritten target defers through the two-phase path regardless of cell
  ephemerality; the three dereference sites are checked.
- **Shared image cache**: every `VortexStdLib` construction paid a full disk load +
  store fold + manifold build — per worker context, per parallel query element, per
  command-path host rebuild. `openStdlibImage` now caches the `shared_ptr<const
  StdlibImage>` per directory keyed on the three artifact stamps (mtime+size), capped
  at 8 entries so throwaway test images cannot accumulate.
- **Smoke test**: `packaging/smoke-test.sh` now also requires `store/store.tables`,
  which `openStdlibImage` refuses without.

Descoped to v2 (recorded, not silently dropped):

- **§6.4 local-image graft**: `GLEDITOR_STDLIB_LOCAL{,_DIR}`/`setStdlibLocalImage`
  resolve policy but no second graft pass exists — local images "add, never replace"
  needs merge semantics beyond a second `graftImage` (which refuses once a manifest
  is present). The env surface stays for the v2 `--extend` work.
- **`stdlibStatus()`/`vortex_stdlib_status`/`ChoiceStackLive` gates**: the in-band
  status routine and outstanding-marks/choice-stack swap gates are unimplemented;
  `VortexHost::bindManifold` has zero in-tree callers, so the latent stale-binding
  hazard is documented rather than fixed here.
- **`:import-lib` boundary**: `importModuleFromStore` remains an explicitly
  unvalidated general import path; stdlib-shaped stores should go through the
  validated image/graft route.
- **Host recreation / `extraContexts_` growth**: `ZigzagVisualizer::
  ensureVortexHost()` rebuilds `VortexHost` on every VQL/VPL/logic command path,
  each paying a fresh graft+rebind (the image cache removes only the disk load)
  and wiping `macroRegistry_`/`customActionRoutines_` until the next keymap
  load; `VortexHost::extraContexts_` is cleared only by `bindManifold`, which
  has no in-tree callers. Host pooling/context reuse is v2 work.
- **Loader/rebind field symmetry**: `openStdlibImage` now requires and verifies
  `imageOpcodeCellCount` against the actual ops enumeration — an image missing
  or misstating it refuses at load, before graft can commit a manifest that
  rebind would reject (which would wedge the arena).

### Iteration 7 close-out — vortex-expert findings

A third reviewer (Vortex domain expert) audited semantic correctness end-to-end
and returned MIGRATION-RISKY. Disposition:

- **FIXED — `call()` divergence on unbound exports**: `rebindInstance`
  materialized an empty `RoutineBinding` for every exported symbol, including
  ops `persistRoutineBinding` marks with empty in/out ranks (the three
  `std:contract/require_*` templates, pipeline ops). Under bootstrap those ops
  have *no* `routineBindings_` entry, so `call()` falls through to the
  operand-wing path and writes arguments into the shared operand cells; the
  grafted empty binding wrote nothing — `call("std:contract/require_positive",
  {5})` left the operand `""` instead of `5`. Rebind now skips empty bindings,
  matching bootstrap membership exactly. Covered by the new
  `GraftedCallsMatchBootstrapCalls` test (12 scalar call pairs plus the
  contract operand side effect), verified to fail without the fix.
- **FIXED — manifest-record probes**: `ensureStdlib`'s `present()`,
  `graftImage`'s already-carried check, and `rebindInstance`'s record scan all
  matched bare `textOf == "stdlib.image"`. A text-only squatter could wedge an
  arena with no fallback. All three now require the `d.values` holder link —
  the invariant every real record satisfies — and `rebindInstance` refuses
  multiple linked records (the loader already requires exactly one).
- **Documented latent risks** (no code change; reachable only through
  unusual Manual-mode partial builds or corrupt arenas):
  - Name-merged modules can carry duplicate symbol names when an arena already
    has every canonical module but no record; a *mixed* merge makes graft bail
    into `bootstrap()`, whose Automatic path has no duplicate-module guard —
    pre-existing bootstrap non-atomicity, not introduced here.
  - Manifest validation proves structure + ABI epoch, not semantic
    equivalence to C++ bootstrap: a permuted same-arity `in`/`out` wiring
    passes every check. The trust boundary is the build pipeline; `contentVersion`
    remains informational metadata, compared only at export.
  - Shared-anchor rank appends replay in `manifold.cells()` order; safe today
    because appended sets are singleton or order-insensitive — an implicit
    invariant a future image shape could violate.
- **Test coverage added**: `RepeatedOpensShareOneCachedImage` (cache hit shares
  the `shared_ptr`; regeneration forces reload) and
  `GraftedCallsMatchBootstrapCalls` (call-result equivalence across sources).

Also fixed during review: `graftDimension()`'s `findOrMintDimension` fallback now
tries `findDimensionByName` first, so non-system registry dims (`d.params`,
`d.mesh.relocation`) cannot duplicate on split base/local registries; the xuzz seam
no longer lets `setStdlibImageDir` shadow `GLEDITOR_STDLIB_DIR`.

### Iteration 7 close-out — last recorded failure resolved

`VortexStdLibTest.MapModuleBstAndOrdering` was not a stdlib defect: the test (added
during this work, absent from HEAD~ history) set `scratchBytes=10000` on
`setExecutionLimits`, but `ArenaManifold::intern()` bounds the arena's *whole*
append-only scratch buffer — already ~13.8 KiB after stdlib bootstrap — so the
first post-limit `intern()` threw "byte budget exhausted" before any map logic
ran. The fuel assertions target `consumeWork`/`workRemaining`; the scratch budget
now gets 1 MiB headroom with a comment recording why. Test passes; the focused
selection is **117/117**.

The image cache's function-local statics are deliberately leaked (`new`), not
just for destruction order: `~Store → DimensionRegistry::unregisterStore` ran
after the registry's own function-local static was destroyed, segfaulting
`--module` generation and test teardown. Leaking keeps cached stores alive for
the process lifetime; `cache.clear()` eviction remains safe because it only runs
mid-execution while the registry is alive.

Verification re-run after all fixes: focused selection 117/117; `--module
std:math` generates and `--verify`s clean (`manual-rebind=ok`, self-consistent
`imageOpcodeCellCount`); strict `GLEDITOR_STDLIB_SOURCE=image` xuzz run clean.
