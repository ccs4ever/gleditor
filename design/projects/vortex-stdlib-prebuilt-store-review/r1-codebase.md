# Round 1 critique: Codebase Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 2 BLOCKING, 6 MAJOR, 14 MINOR.

Scope note: `vortex_stdlib.cpp` changed under review (~5,268–6,265 lines); the doc's
citation-drift disclaimer covers stale line numbers. Verified clean: 21-module bootstrap,
ctor bootstrap, `vortex_host.cpp:113,118,157`, Int64 wings, `wingOf` decode,
`mintOpcode`/`registerProgramImage`/`installWriteGuard`, promote's `d.dims`/`d.lineage`
skip + OpHandle deferral + relocation-marker requirement, `Makefile:1842–1848`,
`paths.hpp:47–50`, `CompactOpNode` invariants, `Store::load` refusal ordering,
`sealableOps`/`historyFromSeal`, `exportStandardLibraryToStore`/`importModuleFromStore`.

## BLOCKING

### §4 (lines 196–203) / P32 — `--extend` "applies only the module diff" specifies a mechanism that does not exist

`zigzag::promote` has no diff-vs-store mode; the builders are not diff-aware
(`exportSymbol` appends unconditionally, `vortex_core.cpp:123–131`). The specified sequence
— graft image *n*, run builders, promote — mints a full second copy of everything;
`adoptOpRecords`/`exportBinaryOps` merge *ops*, not *cells*. "~200 lines of driver" cannot
cover a structural differ plus idempotent builders.

**Fix:** (a) full re-mint appended under the parent head (still hits first-match shadowing
on `+d.vars` — see Vortex M4), or (b) a priced differ with builder-idempotence changes —
or defer `--extend` to v2 as the doc's own stated default suggests.

### P26 Cost (lines 417–419) — "(it does — `zzPromote` already resolves it)" misstates existing behavior

`initGenesis` mints exactly 19 named dims + `d.dims` (`vortex_core.cpp:52–73`) —
`d.mesh.relocation` is not among them; its only creator is `mesh_evaluator.cpp:743`
(`arena.makeCell`, unfiled). `zzResolveDim` walks `rank(arena, dims().dims, dims().step)`
(`vortex_stdlib.cpp:5787–5802`) — a *different* rank from the `d.dims`-chained registry
(`vortex_core.cpp:80–97`) — it cannot resolve a dim nothing registered and searches a rank
nothing populates.

**Fix:** P26 calls `findOrMintDimension("d.mesh.relocation")` during bootstrap; cost line
becomes "one dimension + marker cells + one edge per wing slot"; `zzResolveDim`'s rank
convention needs reconciling with `mintNamedDimension`'s (latent bug, in-flight code).

## MAJOR

### §5.2 step 4 / P28 — the ops manifest's Int64s land in the wrong ref space

Graft clones `valueBits` verbatim → `"ops"` members still name image-space refs;
`registerProgramImage` resolves `contains` on the runtime arena (`vortex_vm.cpp:121–155`).
On a document-bound arena, members can hit unrelated document cells → non-opcodes
registered as opcodes, real opcodes unregistered, write guard unprotected. On a baseless
arena, registration is empty.

**Fix:** OpHandle-valued members + `handleTarget` arm, or graft translates the enumeration
through `map[]` and calls `registerProgramOps` directly. State it in §5.2.4/P28.

### §3.1 / §4 / §11.2 — the artifact is not deterministic and its shipped layout is wrong

`StoreTables.documentId` is random per `Store` (`store_tables.hpp` ~107–118);
`UserPermascroll::Config{storageDir}` auto-mints a random device keypair and persists it —
**including the secret key** — under `permascroll/publication-state/`
(`user_permascroll.cpp:53–168`, secret at ~155–160, auto-mint ~361). §3.1 omits
`publication-state/`; the byte-identical gate fails on `store.tables` even when the spool
matches; shipping the directory **leaks private key material** in an installed asset. A
runtime opening with a non-default `masterIdentity` hits `PermascrollStateUnreadable`
(`:104–118`).

**Fix:** pin generator identity (fixed `documentId`/`masterIdentity`/`deviceKeys`, or strip
`publication-state/`); name the loader's `Config` in §5.1.

### §5 / P30 / P31 — `ensureStdlib()` is gated on config it cannot see at the point it runs

The `VortexStdLib` ctor runs inside `WorkerContext` member-init before `bindStore`/
`loadConfigFromStore` and before host config exists. `makeWorkerContext`'s post-ctor call
site (`vortex_host.cpp:113`) *can* see `config_`, but the doc's flowchart puts
`ensureStdlib` at the ctor, and `WorkerContext` is also constructed outside `VortexHost`
(`mesh_actor.cpp:191,325`, `spool_mesh.cpp:113`, `mesh_evaluator.cpp:936`).

**Fix:** internal lazy resolution at graft time (env/explicit default chain) or plumbed
ctor args; `VortexHostConfig` keys are dead at ctor time as specified.

### §11.4 / §5.1 — "missing permascroll → spans resolve empty *loudly*" asserts detection that does not exist

`SegmentedPrimediaSpool::read`/`readView` silently clamp out-of-range spans to `""`
(`segmented_primedia_spool.cpp:119–150`); `Store::load` never cross-checks span extents;
`applyStructure` folds without span readability checks — the measured 0-refused fold proves
it. A missing/short permascroll yields a *silent empty stdlib*.

**Fix:** spec the detector — record expected scroll extent in `stdlib.image` and refuse on
mismatch, or walk folded spans vs `perma->size()` at open; price it in P30/P31.

### §2.3 — missing sixth gap: base-bound export writes foreign dim refs into the destination spool

On a document-bound arena, `initGenesis` adopts base persistent dims
(`baseDimensionNamed`, `vortex_core.cpp:20–22,41–44`); `promote` maps non-ephemeral cells
to themselves and writes `SetLink` ops whose `linkId` is the base dim's op index — an index
from the *document's* spool, meaningless in the destination (~`:2097–2110`); the
base-store guard only warns (`:1878–1889`). `:export-lib` on a live document produces
corrupt dim links *today*.

**Fix:** add §2.3 gap 6; state the whole-space export is only defined for `arena().base()
== nullptr` and gate export entry points accordingly.

### P33 — `rebindCaches()` cannot restore `goalHalt_`/`goalSentinel_`; the stale-ref class is wider than wing slots

Only `goalFail_` is exported (`:4010`); `goalHalt_`/`goalSentinel_` have no symbol path;
refs are also baked as plain Int64 scalars (`contCell` `~:3509`) — P26's wing-slot-only
conversion misses them.

**Fix:** export both (or locate structurally on `std:goal`), add to P33's inventory; widen
P26 to all ref-by-Int64-convention scalars.

## MINOR

1. Hex slip: `2147484863` is `0x800004BF`, not `0x800000BF`. [Verified correct by orchestrator.]
2. (P27) Registry rank = 20–21 cells (19 dims + `d.dims` + relocation), not 18.
3. (§3.1 vs P28) Manifest anchor inconsistency — pick one.
4. (§5.3) `cliArgs_` is set via `stdlib.setArgs()` app-side, not `vortex_host.cpp:162,185`.
5. (P32) `:export-lib` drives `exportLibrary`, not `exportStandardLibrary`.
6. `SovereignStoreLibraryRoundtrip` TEST at `:667`; asserts `.has_value()` only — supports
   "presence, not behavior."
7. (§7.2) Federation storage ~`arena_manifold.hpp:872–885`.
8. (§2.3.2) Understated: `importModuleFromStore` maps edge dims to *copies* — imported
   `+d.vars`/`+d.values` edges live in a disconnected dimension space, invisible to `resolve()`.
9. (P26) Marker is load-bearing twice: reachability (`:1772–1777`) and the refusal
   (`:1812–1824`) — cite both.
10. (P27) Unstated fold-order fiat: `Manifold` assigns `home_`/`dimsDimension()` to the
    first two cell births; `extraRoots` ordering satisfies it — say so.
11. (§6.1) `hyperLca`/`hyperDepth`/`hyperChildren` consult `boundStore_`; only
    `hyperPath`/`hyperIsAncestorOf` are pure-string — "on the image fold" requires binding.
12. (§3.1) `store.tables` fields are `documentId`, `localSegments`, `deployedScrolls`,
    `publishedLocalScroll` (`store_tables.hpp` ~107–118).
13. (§4) `UserPermascroll::Config` also carries `masterIdentity`/`deviceKeys`/`deviceId` —
    name the fields; the auto-minted keys feed the secret-leak finding.
14. In-flight-code caveat: `zzResolveDim` searches `rank(d.dims-cell, d.step)` while
    `mintNamedDimension`/`findDimension` keep the registry on the `d.dims` chain — real
    disagreement in code landing mid-review.

VERDICT: NOT CONVERGED
