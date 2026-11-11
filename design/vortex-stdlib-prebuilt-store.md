# Vortex Standard Library as a Prebuilt Store

|           |                                                                                           |
| --------- | ----------------------------------------------------------------------------------------- |
| status    | revision 9 — review-converged at Round 10; final MINOR polish applied                     |
| companion | `design/vortex-hyperstructural-runtime.md` §7, `design/store-slice-convergence.md`,       |
|           | `design/osmic-microversioning-and-dag.md`, `design/vagent-self-hosting-migration.md`,     |
|           | `design/system-xanadocs-customization-and-metasystem.md`                                  |
| baseline  | worktree `metacircular_compiler_implementation_plan` @ `9d91bfd` + working tree, 2026-05  |
| scope     | the C++ `VortexStdLib` bootstrap (21 `build*Module` passes over `vortex_stdlib.cpp`)      |
|           | replaced at runtime by a shipped store image; update model; loader graft                  |
| citation  | `file:line` references name the enclosing function as the stable anchor (where the anchor |
|           | is a function); `§X.Y.N` cites numbered item N under section `§X.Y`; line numbers are     |
|           | exact against the stated baseline and WILL drift — `vortex_stdlib.*` is under concurrent  |
|           | modification (metacircular-compiler work). Re-resolve by symbol before implementing.      |

______________________________________________________________________

## 1. Executive Summary & Architectural Mandate

Today the Vortex standard library is **manufactured, not shipped**. `VortexStdLib::bootstrap()`
(`apps/common/xanadu/vortex/vortex_stdlib.cpp`, `bootstrap()` ~:1507) mints the whole library into
every `ArenaManifold` at context construction: 21 modules in mint order — `std:memoize`,
`std:contract`, `std:pipeline`, `std:functional`, `std:collections`, `std:math`, `std:string`,
`std:logic`, `std:goal`, `sys:array`, `std:zigzag`, `std:gc`, `std:ui`, `std:nav`, `std:bridge`,
`std:xuzz`, `std:xudu`, `std:env`, `std:io`, `std:map`, `std:hypertime`
(`vortex_stdlib.cpp:1508–1530`; `build*Module` calls run math-first at ~:1532) — wired as ~4,300
reachable zzstructure cells — opcode cells, input/output wings on `±d.grab`, contract chains on
`±d.contract`, pipelines on `+d.spin`, symbol tables on `+d.vars`/`+d.values`, predicate clauses on
`+d.clause` — chained off `home +d.stdlib`. The generator is the bulk of `vortex_stdlib.cpp`'s
`build*Module` tail (~4,000 lines interleaved with runtime helpers), it runs in every
`WorkerContext` (`worker_context.hpp:59–62`, `vortex_host.cpp` `makeWorkerContext` ~:113), and it
re-runs every time a document rebind rebuilds the host — the shipped path *replaces* `vortex_host_`
wholesale (`bindXanaduStore` → `ensureVortexHost` → `make_shared<VortexHost>`,
`zigzag_visualizer.cpp:362–394,2857–2866`); `VortexHost::bindManifold()` (`vortex_host.cpp:149–158`)
is the in-place swap primitive with no in-tree callers today, reserved for the multithreaded-host
direction (§5.2.5).

This document specifies the migration to a **prebuilt store distributed with the application**:

1. The stdlib stops being C++ source that executes at runtime and becomes **data**: a store image
   written once by a build-time generator, carried in the installed asset directory, and grafted
   into each execution arena at context start.
1. The image is an ordinary OSMIC operations spool — `OpKind::Structure` ops folded by the same
   `Manifold::applyStructure()` path every other slice uses — so its own history is non-destructive
   microversioning (`osmic-microversioning-and-dag.md` §5), diffable by `xudu-dump`, and auditable
   in-store.
1. The C++ builders do not disappear; they become the **generator's** front half. The binary no
   longer needs them at runtime: their output is what ships. That is the frontier
   `vagent-self-hosting-migration.md` draws for the compiler — the engine ends at a hardened
   substrate; everything above is manifold data — and it is the precondition for authoring stdlib
   modules in Vortex itself (per `GEMINI.md`'s Vortex-over-C++ rule).
1. Per the same baseline, no backwards-compatibility of format is owed; what *is* owed is that a
   malformed, stale, or newer-than-known image fails loudly with a named reason, per the R11/R14
   refusal discipline of `store-slice-convergence.md`.

Measured on this worktree (`exportStandardLibraryToStore` → `Store::save`, bound to a real
`UserPermascroll`): today's *reachable* library is **8,652 operations folding to 4,296 cells, 0
refused folds, 605 KiB of `ops.nodes`, ~48 KiB of permascroll** — but see §2.3: that export is
*incomplete* (orphan ref-target cells are missing), so the true image will be larger; §12 re-prices
it.

______________________________________________________________________

## 2. What `bootstrap()` Actually Produces Today

The runtime contract a loaded image must satisfy, inventoried with evidence:

### 2.1 The manifold payload

- **Module cells** on `home +d.stdlib` (`vortex_core.cpp:112–121`, `VortexCore::getOrCreateModule`):
  one cell per `std:*`/`sys:*` module.
- **Symbol cells** on `module +d.vars`, each `+d.values`-linked to its entry opcode
  (`vortex_core.cpp:123–131`, `exportSymbol`). `VortexStdLib::resolve("std:m/s")` walks this.
- **Opcode cells**: `VortexVM::mintOpcode` makes a scalar cell whose canonical bits are the
  `OpcodeKind` integer, labelled `#NAME` in content (`vortex_vm.cpp:157–166`). The kind survives any
  round-trip (`getOpcodeKind` falls back to content decode, `vortex_vm.cpp:186–196`); what does not
  survive is `opcodeMap_` membership — the map the write guard consults (`installWriteGuard`,
  `vortex_vm.cpp:84–95`).
- **Wings**: `bindInput`/`bindOutput` chain operand *slot cells* on `opcode ±d.grab` then `+d.step`;
  each slot's scalar is **the operand's `CellRef` as an `Int64`** (`vortex_core.cpp:418–452`).
  `wingOf` decodes `asInt64(slot) → CellRef` or falls back to `masterOf(slot)`
  (`vortex_core.cpp:383–399`).
- **Ref-valued operand scalars**: control-flow targets travel as operand *cells'* Int64 values —
  `opref(target)`/`contCell()` are `makeScalarCell(Int64(ref))` at ~40 sites in `buildGoalModule`
  (`vortex_stdlib.cpp:3497–3510`, binds at `:3564–4152`); dimension operands go the same way
  (`lit(Int64(dims().scan))`, `:4056–4059`). The decoders are the operand funnels
  `toInt`/`resolveCell`/`refArg` across `vortex_vm.cpp:53–70,768–829,1665–1711` and
  `vortex_stdlib.cpp:2437–2488`.
- **Contracts** on `±d.contract` chained `+d.step` (`vortex_core.cpp:481–516`), **pipelines** on
  `+d.spin` (`:476–479`), **clauses** on `+d.clause`, memo pins on `+d.cache`, cursors on
  `+d.cursors`.
- **Orphan cells**: parameter cells and opref'd internal op cells are linked from *nothing* —
  reachable in-memory only through Int64 refs. Anything the edge walk can't see is absent from any
  export today (§2.3).

### 2.2 The C++ residue bootstrap leaves behind (per-instance, not manifold)

- `routineBindings_`: `unordered_map<CellRef, RoutineBinding>` — declared routine signatures
  (`vortex_stdlib.hpp:578–582`). *Semantic*, not a cache: multi-op routines' signatures differ from
  entry-op wings — `clamp`'s entry `#CLAMP_MAX` binds `{in, lo}` while the routine signature is
  `{in, lo, hi}`, `hi` being `#CLAMP_MIN`'s input (`vortex_stdlib.cpp:1604–1631`). `call()` falls
  back to `inputsOf`/`outputsOf` when the map misses (`vortex_stdlib.cpp`, `call()` ~:2524–2540),
  binding the wrong arity.
- Cached refs (all `CellRef`, all mint-time only, `vortex_stdlib.hpp:585–607`):
  `predEqual_`…`predBridgeEdge_`, the `predVortex*` five, and **`goalHalt_`, `goalFail_`,
  `goalSentinel_`** — consumed unconditionally by `solve()`'s `candidatePreds` (~:4887–4909), by
  `goalProduce`/`goalBoundedFirst`/`goalResults` as `pushChoicePoint` floors (`:4198–4310`), and by
  `#GOAL_*` dispatch as the default halt (`:2460–2489`). Only `goalFail_` is exported as a symbol
  (`:4009`); the other two are linked from nothing — unreachable by `resolve()` and invisible to any
  edge walk.
- `opcodeMap_`: per-VM `CellRef → OpcodeKind` registration (§2.1).
- Runtime-only members that must *not* come from an image: `boundStore_`, `cliArgs_` (set app-side
  via `stdlib.setArgs`), `workerPool_`/`parallelThreshold_` (`vortex_host.cpp:162,185`).

Crucially, several `VortexStdLib` instances share one arena: `WorkerContext` constructs one per
context, and `VPLEngine::ownedStdlib_` (`vpl_engine.cpp:54–93`), `vprolog::Compiler`
(`vprolog/compiler.cpp:22`), `vicon::Compiler` (`vicon/compiler.cpp:65`) each construct their own
over an existing `VortexCore`. Presence on the manifold and readiness of an *instance* are different
questions — the loader design (§5) splits them.

### 2.3 The existing export/import path and its measured gaps

`exportStandardLibraryToStore` (`vortex_stdlib.cpp`, ~:5674–5691 current tree) already promotes the
`+d.stdlib` reachable subgraph through `zigzag::promote`, and `SovereignStoreLibraryRoundtrip`
(`tests/xuzz/vortex_stdlib_test.cpp:674`) proves the store writes and reloads — presence only, never
behavior. It does **not** yet produce a loadable stdlib image:

1. **Ref-valued scalars persist as garbage.** `promote` writes scalar values verbatim; only
   `ValueKind::OpHandle` cells get ref translation (`arena_manifold.cpp:1969–1971, 1982–1987`).
   Wing-slot Int64s, `opref`/`contCell` values, and dim literals all name source-arena refs —
   measured `value=2147484863` (`0x800004BF`) in the exported spool.
1. **Orphan cells are absent entirely.** `promote` discovers cells by edge walk
   (`arena_manifold.cpp:1778–1798`); param cells and opref'd internal ops — named only by Int64s —
   are **not in the measured 4,296-cell export**. The wing-slot fix (P26) is therefore a
   *reachability* fix first, an encoding fix second.
1. **The dimension registry is absent.** `promote` skips `d.dims`/`d.lineage` edges
   (`arena_manifold.cpp:1778–1782`): dimension cells arrive as unlinked orphans, the rank does not.
   Verified: on the reloaded fold, `home` and `dimsDimension` resolve but
   `dimensionNamed("d.stdlib")` returns nothing.
1. **No re-registration.** `opcodeMap_`, `routineBindings_`, and the cached refs are not rebuilt on
   import; multi-op routines bind wrong and the write guard sees no protected set.
1. **No permascroll.** `VortexHost::exportStandardLibrary` (`vortex_host.cpp:519–531`) exports into
   a default `Store()` whose `UserPermascroll` has empty `storageDir` — cell text is spooled to
   volatile memory and the saved spans name offsets in nothing. `SegmentedPrimediaSpool::read`
   silently clamps out-of-range spans to `""` (`segmented_primedia_spool.cpp:119–150`) — a missing
   permascroll is a *silent* empty stdlib, not a fold error.
1. **Base-bound exports write foreign refs.** On a document-bound arena, `initGenesis` adopts *base
   persistent* dims (`baseDimensionNamed`, `vortex_core.cpp:20–22,41–44`); `promote` then writes
   `SetLink` ops whose `linkId` is the base dim's op index — an index from the *document's* spool,
   meaningless in the destination. The cross-store guard only warns
   (`arena_manifold.cpp:1878–1889`). Whole-space export is defined only for
   `arena().base() == nullptr`.
1. **Imported dims land in a disconnected space.** `importModuleFromStore` maps edge dimensions to
   *copied* cells, so imported `+d.vars`/`+d.values` edges live on dim cells `core_.dims()` never
   sees — the whole imported structure is invisible to `resolve()`.
1. **`bootstrap()` is not idempotent.** It runs in the `VortexStdLib` ctor
   (`vortex_stdlib.cpp:1484–1487`) and again in `initHostServices` (`vortex_host.cpp:117–118`), plus
   once more per `VPLEngine::ownedStdlib_` — `exportSymbol` appends unconditionally, so each call
   duplicates every symbol. `resolve()`'s first-match hides it; `symbolsInModule` and `vortex_*`
   introspection counts expose it.
1. **`zzResolveDim` searched the wrong rank** (in-flight code, observed mid-review and since fixed):
   at critique time it walked `rank(arena, dims().dims, dims().step)` while
   `mintNamedDimension`/`findDimension` keep the registry on the `d.dims` chain
   (`vortex_core.cpp:24–27,80–97`); the current tree walks the correct `d.dims` rank. Recorded only
   because the baseline citation history matters.

______________________________________________________________________

## 3. The Shipped Artifact

### 3.1 Layout

```
assets/vortex-stdlib/
├── store/
│   ├── ops.nodes            # sealed OpsSegmentHeader + CompactOpNode run (formatVersion 2, R14)
│   └── store.tables         # documentId (pinned), localSegments, deployedScrolls, publishedLocalScroll
└── permascroll/
    ├── active.primedia      # cell text (module names, #OPCODE labels, term functors)
    └── segments/            # sealed primedia segments, if the image ever seals
```

This is the `tests/samples/xudu/` pairing — a store directory beside the one permascroll its spans
address — installed under `$(appdir)/vortex-stdlib/` by `make install`, beside `shaders/` and
`logo.png` (`Makefile:1786,1842–1848`). Installed-consumer scope: `make install` covers `xuzz` and
its assets; `vpl`/`vprolog`/`vagent`/`vicon` are tree-run tools that resolve the same image through
`./assets/` or the env overrides — the five-step chain in §5.1 serves both; wasm rides
`--preload-file` and android `assets.srcDirs` — the directory asset is free under both. **No
`publication-state/`**: the image is unsigned and carries no device keypair — the generator pins a
fixed, documented `masterIdentity`/`deviceKeys` (or strips the directory) so the installed asset
contains no secret material and is byte-deterministic (§4).

The image is a **whole-space slice**: genesis `home` and `d.dims` minted first (the fold assigns
`home_`/`dimsDimension()` to the first two cell births — the export's root order must preserve this
fiat), the full `d.*` registry rank (~21 cells: 19 named dims + `d.dims` + `d.mesh.relocation`, plus
`d.params` once minted), the `+d.stdlib` module rank and its reachable subgraph, and the manifest
record below on `home +d.vars`.

One manifest key record on `home +d.vars` — `"stdlib.image"` — whose `+d.values` names an *unranked*
holder cell owning a genuinely *private* `+d.vars` rank. This is the vagent two-cell holder
convention **verbatim** — an on-rank key record whose `+d.values` names an off-rank holder, exactly
`finalizeProgramImage`'s bare `makeCell()` holder + `varsRecord(image, vars, values, …)` fields
(`vagent/fragment.cpp:438–458`), attached to the program band by the `"imageRecord"` key's
`+d.values` (`decls.cpp:2245–2257`) — and it is forced by a flat-rank fact: a member of
`home +d.vars` cannot own a `+d.vars` rank of its own, because its `+d.vars` edge *is* its rank
position. **Every** manifest field — `imageEpoch`, `contentVersion`, `generatorHead`,
`expectedScrollBytes`, `imageOpcodeCount`, `imageVerbCount`, and `"ops"` — is a keyed record on the
holder's private rank, so `registerProgramImage`'s `rankAfter(holder, vars)` walk
(`vortex_vm.cpp:131–137`) enumerates *only* this image's records — true containment: foreign
`"ops"`-texted cells are structurally unreachable, and no posward-ordering invariant applies. The
image's `home` cell maps to the arena's `home` at graft. All manifest and signature records are
minted **unscoped** — no `+d.owner` arm — deliberately: `varsRecord`'s scope cell is an Int64
cell-ref (`vagent/records.hpp:113–118`) that would join the ref-relocation class for no benefit,
since these singleton keys match fine unscoped (`records.hpp:103–108`). `varsRecord` itself cannot
mint them — with `mint=true` it *always* files the `+d.owner` arm — so the mint path is a three-line
`keyRecord(owner, key, value)` helper: `makeCell(key)` + `link(record, values, POS, value)` +
`link(rankTail(owner, vars), vars, POS, record)` — the `varsRecord` pattern minus the scope arm and
its unconditional `d.owner` dim mint. The shape already exists file-local:
`vagent/fragment.cpp:39–49`'s 6-arg `varsRecord(arena, owner, varsDim, valuesDim, key, value)` is
the identical mint+link+`rankTail` sequence minus `d.owner` — the natural hoist into the shared
records header as `keyRecord`; the two same-named overloads are named distinctly at each mention.
`keyRecord` is scoped to **private or documented-sparse `+d.vars` ranks only** (the holder's rank;
`home +d.vars`, which only `stdlib.image` keys ride) — `owns()` treats an absent `+d.owner` as
match-any-owner (`records.hpp:103–108`), so a shared-rank keyed record that must be owner-scoped
still goes through `varsRecord`.

- **`"ops"`** — the `registerProgramImage` enumeration shape (`vortex_vm.cpp:121–155`): a
  `keyRecord` on the holder's private rank, `+d.values → opsHead`, `+d.step`-chained member cells
  each an Int64 naming an opcode-bearing cell **positionally** — record membership is itself the ref
  declaration (the `seedProgramImage` precedent), so members carry no per-cell marker; the generator
  restates them to store-space and the graft translates them by walking the record's chain (the ops
  members are the largest positional-ref population — ~1,200 cells). **One `"ops"` record per
  image** — each `build*Module` tail-appends its op members to the single chain.
- **`"stdlib.image"`** — the version record; each field a keyed `+d.vars` member of the holder
  (`fieldKey +d.values → scalar`): `imageEpoch` (text digest over the ordered `OpcodeKind`→label and
  `StructureVerb`→label tables **plus the decode-boundary constants** `kLastContentDecodableOpcode`/
  `kLastOpcodeKind` (`vortex_vm.hpp:145–158` — a moved boundary with identical labels must not pass;
  the `OpcodeKind`→label table itself is authored by this work — none exists today),
  `imageOpcodeCount`/`imageVerbCount` (Int64s), `contentVersion` (Int64, monotonic per release),
  `generatorHead` (text `ver.str()` — `MicroversionId::parse`-round-trippable),
  `expectedScrollBytes` (Int64 — the permascroll size the image was generated against, §5.1's
  loud-missing-permascroll check).

### 3.2 Why a store and not a serialized arena snapshot

`ArenaManifold::PrefixSnapshot`/`encodeArenaPrefix` (`arena_manifold.hpp:781–799,947–950`)
serializes raw slot arrays — dense arena refs, ephemeral-bit ids, trail state. As a *shipped* format
it fails every standing rule: opaque to `xudu-dump`, invisible to `std:hypertime`, no OSMIC lineage,
epoch-locked, uninspectable as a xanadoc. The store image keeps all of those properties; the
snapshot machinery stays as the in-process optimization §7.3 keeps for seeding extra contexts.
Refused for distribution; kept for memory.

______________________________________________________________________

## 4. The Generator

A build-time tool — `build/vortex-stdlib-gen`, from `tools/vortex_stdlib_gen.cpp` (P32) — runs the
existing machinery in the right order, on a **virgin scratch `WorkerContext` with no base**
(required: exports are only defined for `arena().base() == nullptr`, §2.3.6; and a virgin context
keeps runtime cursors/pins off `home`'s other ranks out of the reachable set):

1. `Store` bound to a `UserPermascroll` configured with `storageDir = out/permascroll`,
   `masterIdentity = <pinned>`, and `deviceKeys = <pinned-zero-or-fixed>` (P34's new `readOnly`
   field omitted — the generator writes) — fields named in `user_permascroll.hpp:103–114`; a fixed
   `documentId` on the `Store` so `store.tables` is deterministic. The tool first forces
   `StdlibPolicy{mode=Cpp}` via the explicit setter — otherwise the scratch context's ctor-time
   `ensureCells` grafts the *shipped* image and `bootstrap()` doubles it (or a presence-check
   produces a vacuous image→image regeneration that can never catch a builder regression).
1. Scratch `WorkerContext` → its **ctor-time `ensureStdlib` under the forced `Cpp` policy** builds
   the arena image — **exactly once**, and via `ensureStdlib` not a bare `bootstrap()` call (the
   image is one-bootstrap canonical; today's triple-run duplicates are a bug the image does not
   inherit, §6.5 — and an explicit post-ctor `bootstrap()` would double-mint over the ctor build,
   P30). During the build, the builders emit the relocatable encodings and self-describing records
   **in-arena** (P26, P28, P29): wing slots OpHandle+marker; `opref`/`contCell`/`lit(dim)` scalars
   marked-Int64, one fresh `"cell"`-texted marker per marked cell (the `refCell` convention — a
   *shared* marker is impossible under `link()`'s 2-degree splice: the next mark would evict the
   previous slot's forward edge, `arena_manifold.cpp:1344–1378`); `d.mesh.relocation` minted via
   `findOrMintDimension` (and `mesh_evaluator.cpp:743`'s bare `makeCell("d.mesh.relocation")`
   migrates to it — no non-registry twin dims); the `+d.params` signature records mint at the end of
   each `build*Module`; **one** `"ops"` enumeration (`opsHead` + `+d.step` member chain) is minted
   at bootstrap start and each `build*Module` tail-appends its op members; at bootstrap end the
   `stdlib.image` key record + holder mint, and `keyRecord(holder, "ops", opsHead)` mints the key,
   links `+d.values → opsHead`, and files it on the holder's private rank alongside the field
   records (§3.1 — the record helper *mints* the key cell; it cannot file a pre-minted one). The dev
   fields — `contentVersion=0`, `generatorHead="cpp"`, `imageEpoch=engineEpoch`,
   `expectedScrollBytes=0` — are the bootstrap-time placeholder *every* arena carries under `Cpp`;
   the shipped image overwrites them via step 5's trailing `SetValue` ops. `expectedScrollBytes=0`
   is an **unexported placeholder** — meaningful only on an artifact that passed through the
   generator's trailing `SetValue`; a `Cpp`-built arena is never validated or shipped without
   restating it (a verbatim U-X8 interactive export carrying `0` would fail §5.1's `ScrollTruncated`
   by design). Every arena — `Cpp`-mode included — is self-describing, so `rebindInstance` is
   uniform across modes.
1. Whole-space export (P27): `promote` with `extraRoots{home, dims_.dims}` under the `wholeSpace`
   flag — which **lifts the `d.dims` edge-skip** (`d.lineage` stays skipped — not stdlib content) so
   the registry rank's own `+d.dims` edges and all 19 dim cells — including unused ones reachable
   *only* through it — enter the image — plus a harvest pass enumerating every marked cell's
   referent **and** every `"ops"`/`"in"`/`"out"` record member as extra roots — this is what pulls
   params, internal ops, `goalHalt_`/`goalSentinel_`, and record referents into the image.
1. **Restate Int64 refs** (the `seedProgramImage` precedent, `vagent/lineage.cpp:398–411`): promote
   writes Int64 bits verbatim — a ref-valued Int64 names a *source-arena* ref — so after promotion
   the generator appends `SetValue` ops restating each marked cell's and each record member's Int64
   to its referent's **store-space position** via the promote twin map. The image's ref Int64s then
   name image-space cells, which is what §5.2.3's `map[imageRef]` translation consumes. (OpHandle
   wing slots need no restate — promote's deferred-handle path writes `store.makeOpHandle`
   natively.)
1. The manifest mints **names early, values late**: the `stdlib.image` key record, its holder, and
   its field-key cells are listed in `extraRoots` — which seeds `order`/`frontier` ahead of BFS
   discovery (`arena_manifold.cpp:1745–1759`), so they are emitted early in op order (their primedia
   lands early in the scroll — the names must survive tail truncation, §5.1). Late writing is
   reserved for the fields whose values *cannot exist* until the spool settles —
   `expectedScrollBytes` (final scroll size — written as **bits-only `SetValue`s** with a
   `PrimediaSpan{}` restate, the `vagent/lineage.cpp:407–410` precedent, so the manifest's own tail
   writes carry no span and the arithmetic closes; `generatorHead`'s text span is counted into the
   size it reports) and `generatorHead` (final head — computed **arithmetically**: `MicroversionId`
   is positional, `microversion.hpp:63–156`, so the final head is the promote head + the planned
   trailing op count, written never read back) — plus `imageEpoch`/`contentVersion`, which are
   early-computable but written late anyway so all manifest scalars travel one mechanism;
   `imageOpcodeCount`/`imageVerbCount` likewise write last, counted over the finalized tables. The
   `SetValue` op retargets a cell's content span (`ops.hpp:108–109`), so the trailing writes replace
   the dev placeholders. Emission everywhere walks manifold ranks (`Manifold::cells()` birth order,
   `+d.vars` ranks), consulting `routineBindings_`/`opcodeMap_` by key only — `unordered_map`
   iteration order would break byte-determinism.
1. `store.save(dir)` + `perma->flush()` produce §3.1's layout; `--verify <dir>` folds and re-emits
   the isomorphism report used by tests.

**Generator discipline (single-owner invariant):** every `+d.values`, `+d.params`, `±d.contract`,
`+d.spin`, `+d.step`, `+d.clause`, `±d.grab` (wing heads — the largest structure class), and
`d.mesh.relocation` mark edge targets a cell owned by exactly that edge — an aliased target silently
evicts the other owner's edge at mint *and* at graft replay (`link()`'s splice clears both displaced
adjacencies). The generator asserts it; every OpHandle+ephemeral cell in the bootstrap arena carries
the relocation mark (one unmarked OpHandle targeting an *ephemeral* cell hard-refuses the whole
export, `arena_manifold.cpp:1816–1819` — the mark requirement fires only under
`isEphemeral(*target)`; persistent-target handles fall through to the `getCompactOp` check at :1823
needing no mark — moot on the null-base scratch where every target is ephemeral).

**Determinism gate:** given the same source, `ops.nodes` must be byte-identical — that is what makes
a checked-in artifact reviewable as a diff and keys "what changed" queries. The two entropy sources
are named and pinned: `documentId` (random per `DocumentId` — the `generate()` default,
`document_id.hpp:27,57–62`) set to a reserved fixed constant for the stdlib image, and the
auto-minted device keypair. The `store.tables` scroll fields (`localSegments`, `deployedScrolls`,
`publishedLocalScroll`) ship inert — a read-only image never seals or deploys. No
`publication-state/` ships; the unsigned asset is the *preprint* — authorship/signing for swarm
distribution is the goal, recorded as U-X5.

**Update mode (`--extend`) is deferred to v2** — see §6.2: nothing computes a structural diff today,
and append-only growth keeps changed symbols shadowed behind `resolve()`'s first-match. v1 ships a
complete sealed image per release.

______________________________________________________________________

## 5. The Load Path

`bootstrap()`'s unconditional constructor call becomes **`ensureStdlib()`** (P30), split by whose
state is being ensured:

```
VortexStdLib ctor (and ensureStdlib again on bindManifold)
        │
        ▼
ensureCells()     ── once per arena ──
        reject real live marks before presence probing (even on a present arena)
        │
        ▼
stdlibPresent()?  (resolve("std:math/abs") — a symbol, not just the module cell)
        │ yes ──▶ cells already ready
        │ no
        ▼
image available & acceptable under the policy mode?  (§5.4 matrix)
        │ yes ──▶ graftImage(image)  (§5.2, mark-scoped)
        │ no  ──▶ bootstrap() (Cpp builders) or named refusal, per mode
        ▼
rebindInstance()  ── always, per VortexStdLib, after ensureCells ──
        rebuild routineBindings_ from records, rebind every cached ref by symbol path,
        registerProgramImage into this VM
```

Per-instance vs per-arena is the load-bearing split: `vprolog`/`vicon`/`vpl` construct their own
`VortexStdLib` over a shared `VortexCore` (`vprolog/compiler.hpp:78`, `vicon/compiler.hpp:160`,
`vpl_engine.hpp:132–136`; `vagent/lineage.cpp:564` builds a whole `WorkerContext` —
outside-`VortexHost` class, §5.4) — a second instance must not re-graft but **must** rebind its own
caches/registration, or `solve()`/`#GOAL_*`/`call()` run on `noCell` refs. `bindManifold` calls
`ensureCells()` then `rebindInstance()`; the ctor calls `ensureStdlib()` = both in that order.

**Image-initialization invariant:** `ensureCells`/`graftImage`/`rebindInstance` and their image
registration run only with `ArenaManifold::liveMarks() == 0` — real marks, excluding a still-present
synthetic hydration floor.

**Hydration accounting:** P30 adds this accessor and explicit `hasHydrationFloor_` state.
`hydratePrefix` first requires **raw `outstandingMarks_ == 0`**, before replacing any state; its
signature becomes `ArenaResult`, refusing with `ArenaRefusal::MarksOutstanding` otherwise. The sole
production caller, `or_parallel.cpp:349`, checks this expected result before using its fresh worker
arena. On unexpected it fails the spark with the named arena refusal; it never executes an
unhydrated worker as fallback. Successful hydration clears/reseeds the floors and sets the flag, so
the seed is always the oldest floor. `evictOldestMark` clears the flag when removing that seed.
Ordinary real-mark release/discard cannot pop the seed: if the flag is set and only that floor
remains, release/discard is an invalid/stale-Mark use and asserts before mutation. The arena has no
clear/reset API. Whole-state move assignment replaces its count/flag/floors together; moved-from
arenas may only be destroyed or assigned a fresh arena before reuse (no `liveMarks()` call on
moved-from state). The accessor returns `outstandingMarks_ - (hasHydrationFloor_ ? 1 : 0)`,
asserting that a flagged seed implies a nonempty floor stack and positive count. Ordinary `mark()`
adds a real floor without changing the flag. The seed has no corresponding releasable `Mark`, but is
**evictable**, not permanent (`arena_manifold.cpp:2186–2203`). Today's host arenas are not hydrated;
explicit tracking keeps the public accessor correct for OR-parallel workers too. The invariant is
**checked at `ensureCells`/`graftImage`/`rebindInstance` entry, before the presence probe or graft's
own mark**. A second stdlib constructed over a marked arena records `MarksOutstanding` even if cells
already exist; it does not register under that mark. A graft under a live mark is rollback-able
(`release()` truncates `slots_`/`links_`, `arena_manifold.cpp:1657–1674`) and `opcodeMap_` is not
trailed, so it would hold refs to truncated slots. This is an **environmental refusal —
`MarksOutstanding` refuses `ensureCells` outright in *every* mode, including `ImageThenCpp`**: a C++
fallback under a live mark runs `bootstrap()` minting trailed cells while writing the same untrailed
`opcodeMap_` — the identical corruption the gate exists to prevent, silently. The assert stays as
the debug tripwire; today's call sites are all mark-free on normal initialization (ctor,
`initHostServices`, `bindManifold` pre-cursor).

**Direct compilation boundary:** This is not a new system-wide prohibition of runtime cell
allocation: `mintOpcode` still has the pre-existing untrailed-map exposure for direct callers that
register under marks (`vortex_vm.cpp:157–159`). P30 also gates the known UI bypass,
`ZigzagVisualizer::compileVQL`, before compiler construction/minting even when a host is already
present (`zigzag_visualizer.cpp:3393–3410`). This non-replacing operation checks
`vortex_host_->primaryContext().arena.liveMarks() != 0`, not the host aggregate: worker-only marks
cannot roll back the primary compiler's cells and do not block it. It returns
`CompilationResult{success=false, errorMessage=…}` and the same visible refusal feedback. Its
existing `const_cast` UI path handles feedback writing. General direct-VM registration/trailing
redesign is outside this image migration; no claim that arbitrary compiler/VM clients are made
mark-safe is implied.

### 5.1 Opening the image (once per process)

- The image dir resolves through a **headless chain** (engine code cannot pull `paths.o`+SDL,
  §9/P31): explicit path (early static setter / test hook) > `$GLEDITOR_STDLIB_DIR` (**taken as
  given** — a bad explicit dir fails `ImageMissing` naming it, never silently falls through) >
  `$GLEDITOR_ASSET_DIR`/`vortex-stdlib` > compiled `GLEDITOR_DATADIR`/`vortex-stdlib` >
  `./assets/vortex-stdlib` (CWD-relative — note a `xuzz_test` run outside the repo root silently
  exercises the C++ path). The xuzz app seam **must** initialize the setter from
  `assetPath("vortex-stdlib")` before first context construction (`assetPath` is SDL-backed,
  `paths.hpp:47–50` — the portable-zip layout is otherwise unreachable headless, §9/P31).
- `UserPermascroll` opened on `permascroll/` under P34's **new** `Config.readOnly` field (skips
  `permascrollState`/`create_directories`/write-back — no LMDB, no key mint, no asset-dir mutation —
  and opens the primedia spool `O_RDONLY` *by request*, not just by EACCES detection) and `Store` on
  `store/` under P34's `O_RDONLY` fallback in `SegmentedOpsSpool::openActiveSegment`: today the
  spool opens `O_RDWR|O_CREAT` with no fallback (`segmented_ops_spool.cpp:603–611`), so a root-owned
  `make install` asset cannot be loaded by a non-root process *at all* — `Store::load` throws
  "cannot read the operations" (`store.cpp:4406–4412`). Physical read-only turns any accidental
  runtime append into a named refusal rather than silent mutation of a writable dev tree.
- Validation order, pinned: `permascroll/` dir-exists **precheck** (a missing dir names the
  *packaging* cause, `ImageMissing` — distinct from a present-but-short scroll) → open (read-only) →
  `rebuildManifold` fold → read `stdlib.image.expectedScrollBytes` (the record's *names* are minted
  early, §4 — they survive tail truncation; the Int64 member value bits survive regardless) and
  compare `perma->size()` (`user_permascroll.hpp:180`) — **`size() != expectedScrollBytes` is
  `ScrollTruncated`**: the image is a sealed artifact, so a *grown* tail is a refused deviation, not
  dead bytes (a legitimately extended dev-tree scroll regenerates its own manifest) → then
  `imageEpoch`. When the manifest itself can't be located (truncation severe enough to eat early
  names), the loader falls back to comparing `perma->size()` against the ops' max referenced
  span-end — the cause is still `ScrollTruncated`, not a generic "missing manifest". A successful
  fold carrying **no `stdlib.image` key record** — e.g. `$GLEDITOR_STDLIB_DIR` pointed at an
  ordinary document store — is `FormatRefused` naming "store carries no `stdlib.image` manifest —
  not a stdlib image" (a valid non-stdlib store must not fall through the arms causelessly); the
  span-end fallback applies only when the manifest's names may themselves be truncated. A legacy
  per-module `:export-lib` image lacking the genesis `home`/`d.dims` prefix is a named
  `FormatRefused`.
- `rebuildManifold(head)` folds once. The folded `Manifold` is held process-wide — the *folded-image
  state* is a `std::shared_ptr<State>` under a mutex where `State` carries a `std::once_flag` plus a
  `std::optional<std::expected<Triple, StdlibImageRefused>>` (`once_flag` is immobile, so the state
  is held by indirection; `ensureCells` acquires a `shared_ptr` snapshot under the mutex before
  folding or grafting — an in-flight graft keeps the old `State` alive to completion, so a test
  reset can never orphan a reader): the `{UserPermascroll, Store, Manifold}` triple folds lazily on
  first `ensureCells` (Stage-3 mesh contexts — `design/vortex-multithreading-study.md`'s Stage 3
  parallel contexts — can construct concurrently), and a **failed fold latches**: the recorded
  refusal replays per instance (a mid-process retry would diverge arenas).
  `resetStdlibImageForTesting()` swaps a fresh `State` under the mutex — covering *both* the base
  and §6.4 folded-image states — re-fold after `GLEDITOR_ASSET_DIR`/`resetAssetDirForTesting` or
  policy changes; it is a test hook, not a runtime repoint path — §5.4.
- Header validation is free: `OpsSegmentUnreadable` names both node sizes (R14),
  `StoreTablesUnreadable` refuses legacy side tables — every store-level refusal
  (`OpsSegmentUnreadable`, `StoreTablesUnreadable`, `PermascrollStateUnreadable`, an empty or absent
  `ops.nodes` under `O_RDONLY`) maps onto `ImageUnreadable` in the cause enum; `ImageMissing` is
  reserved for the dir/scroll prechecks. Note `Store::load`'s `ops.export` arm → `adoptOpRecords`
  (`store.cpp:3836`ff) *writes* through the spool — under a requested read-only it must refuse by
  name mid-load (P34's rule applies; the stdlib image itself never carries `ops.export`).
- **Semantic validation** (P31): read `stdlib.image` — the key record on `home +d.vars`, then
  `linked(key, values, POS)` → the holder, then each field by key on its private `+d.vars` rank
  (§3.1); refuse when `imageEpoch` differs from `engineEpoch` — the same digest over this build's
  `OpcodeKind`/`StructureVerb` tables (`engineEpoch` is defined symmetrically in the same record
  shape). An image naming kinds or verbs this build lacks — or ordering them differently — must say
  so, never silently misdecode Int64 kinds.

### 5.2 The graft (per arena)

`graftImage` supersedes `importModuleFromStore` (the `VortexHost::importLibrary` command handler
delegates to it):

The graft runs **inside a taken `mark()`** — `discard` on success, `release` on any mid-graft
refusal (rolls back the mark-covered vectors — `release()` truncates `slots_`/`links_`/`content_`/
`scratch_` to the mark's recorded sizes, `arena_manifold.cpp:1671–1699`, so primedia interned by
`makeCell(text)` dies with the cells that owned it; nothing persists past the mark *in those
vectors* — which is what §11's rollback gate asserts). The claim is scoped honestly: `release()`
does **not** restore the untrailed side-maps — `arenaDims_` (`arena_manifold.hpp:847`),
`boundDimensions_` (:888), `storesRankTail_` (:899). `overlay_` entries tracked by `shadowOrder_`
are restored by `release()` (`arena_manifold.cpp:1666–1669`); they are not wholly untrailed. The
bit-identical property holds only under the pinned invariant that **all graft dim mints are
rank-filed `findOrMintDimension`** (`vortex_core.cpp:99–102`) and the graft never calls
`ensureDimension`/`bindDimension`/`bindForeignDim`/`attach`/`installWriteGuard` inside the mark (the
`meta.cpp:1499–1516` comment names the hazard — "`arenaDims_` is untrailed" — and in-tree code
already refuses a dim mint under live marks for this desync class); §11's rollback gate asserts the
side-maps are unchanged too, not just cell/link counts. Registration and rebind run after `discard`
(untrailed state cannot ride the mark) — steps 4–5 below sit *outside* the mark. The *intended
subgraph* is defined operationally: **every image cell** minus the name-mapped dimension cells, the
seeded name-merged content cells, and `home` itself (a whole-space image grafts everything else).

1. **Seed the ref map**: image `home → arena home`; every cell on the image's `d.dims` rank → the
   arena dimension of the same name (`findOrMintDimension`, `vortex_core.cpp:99–102` — which
   performs the rank append internally); and **name-merged content cells** — pinned to *image
   `home +d.stdlib` rank members whose full text `findModule()`s to a live arena module cell* (the
   §6.4 module merge: `map[localModule] = existingModule`) — seed the map and are *excluded from
   cloning*. Dimension identity is by *name*; the image's `d.dims` **link edges are never replayed**
   — replaying a divergent registry edge onto a populated rank severs it (strict 2-degree: `link()`
   clears both displaced adjacencies, `arena_manifold.cpp:1344–1378`), minting a duplicate
   same-named dim.

1. **Clone cells in `image.cells()` fold-birth order** (`manifold.hpp:407`), skipping seeded
   name-merge cells — `makeCell(text)` + `setValueBits(kind, bits)`, deferring all ref-valued
   members to step 3. Mark edges (`+d.mesh.relocation → "cell"`) replay into the arena like any
   internal edge — deliberately: they are what mark-aware *exporters* — the `relocation_marks.hpp`
   harvest machinery, P26 — re-read as ref declarations when a grafted stdlib cell is re-exported
   (`promote` alone reads marks only through the OpHandle arm, `arena_manifold.cpp:1772–1777`).

1. **Rewire refs, honoring edge class** (each edge replayed once, pos-side; a pos-side edge and its
   neg twin are one logical edge; dim classification is decided **before** `map[]` is consulted — an
   unfiled non-`"d."`-texted edge-dim is a cloned cell, not a name-mergeable dim. Classes below
   apply **in order** — evaluate *endpoint provenance* before *membership*, because a name-merged
   member edge satisfies both):

   - *Pre-existing member endpoints*: an edge whose non-anchor endpoint maps to an already-live
     arena cell (a seeded name-merge): **skip when the member is already on that rank** — the query
     is *rank membership on that anchor's rank*, `rankContains(anchor, member, dim)`
     (`arena_manifold.cpp:1381–1421` — `linked(member, dim, NEG)` answers a different question,
     "non-head on *some* same-dim chain", and would silently skip a member on a *different* rank),
     not the literal `src→dst` edge, which holds only when the member is rank-first — otherwise a
     named refusal. (Tail-appending an already-ranked member would splice it out mid-rank and sever
     at its old predecessor — the R3 hazard this class exists to refuse.)
   - *Membership edges onto shared anchors* (`home`'s `d.stdlib`/`d.vars`, a name-merged module's
     `+d.vars`) **whose member endpoint is graft-minted**: **tail-append** —
     `link(rankTail(anchor, dim), dim, POS, member)`, the `exportSymbol`/ `getOrCreateModule`
     pattern verbatim (`vortex_core.cpp:117–119,128–130`; equivalently
     `insertIntoRank(member, rankTail(anchor, dim), dim, POS)` — `arena_manifold.cpp:1423–1439`,
     which yields `anchor→cell→displaced`, order preserved). `link()` onto an *occupied* mid-rank
     slot is the banned operation — it severs the incumbent adjacency; and per-member head-anchoring
     reverses accumulation.
   - *Internal edges* (both endpoints graft-minted): verbatim
     `link(map[src], map[dim], dir, map[dst])` — legal only because the §4 single-owner invariant
     holds (every listed edge's target is owned by exactly that edge; an alias would silently
     evict).
   - *Dimension edges*: an edge whose dim names a `"d."`-texted cell not on the image registry rank
     is name-merged through `findOrMintDimension` (never cloned as an ordinary cell — that is the
     §2.3.7 defect in miniature); the refusal arm covers only a malformed `"d."` name or a policy
     refusal to mint unfiled dims — `findOrMintDimension` mints any well-formed name.
   - *Marked ref-valued cells* — detected by **edge-existence** ("has an outgoing
     `d.mesh.relocation` edge") which identifies *ref-bearing*; `ValueKind` then dispatches the
     decode: **Int64** → `setValueBits(Int64, map[asInt64(cell)])`; **OpHandle** →
     `setValueBits(OpHandle, map[handleTarget(cell)])` — the OpHandle-marked wing slots take the
     same path as unmarked OpHandle values. (`isCellRelocationHandle` detects nothing *among the
     Int64-marked cells* — it is OpHandle-gated, `foreign_op_handle.cpp:100,111`; the wing slots it
     does see are already covered by the dispatch.)
   - *Record members*: Int64 members of each grafted `"ops"`/`"in"`/`"out"` chain are translated by
     walking the chains — `setValueBits(Int64, map[imageRef])`. Walk roots: the grafted `"ops"`
     record on the `stdlib.image` holder's private `+d.vars` rank, and each grafted `sigCell`'s
     `"in"`/`"out"` records reached via each grafted sym's `+d.params` edge — record identity by
     minted-set membership, never by text.
   - *OpHandle values* (unmarked): `setValueBits(OpHandle, map[target])`.
   - *Residual rule*: any remaining edge whose **src or dst endpoint** maps to a pre-existing cell →
     named refusal — the taxonomy is now closed (dims are already classified before `map[]`, so the
     dim cell's own provenance never triggers it). An intended consequence, named: a name-merged
     module cell's own *outgoing* non-membership edges (e.g. a dev module carrying `+d.name`
     annotations) refuse the local graft — the base survives per §6.4 — rather than merging or
     silently dropping.
   - *Map-miss discipline*: for a marked cell or record member, a miss is **always** a named refusal
     (it can only mean a harvest/restate gap — verbatim would write the stale image-space ref); for
     an ordinary edge, a miss inside the intended subgraph is a named refusal, outside it is skipped
     by design. — **`discard` boundary: steps 4–5 run after `discard` on the grafted, now-permanent
     cells.**

1. **Register**: the graft knows `map[imageRecord]` by identity (minted-set membership) — but
   registration needs the **holder**: `holder = linked(map[imageRecord], values, POS)`, the unranked
   cell whose `+d.vars` rank is private (§3.1). An **ops-presence precheck precedes the call** —
   scan `rankAfter(holder, vars)` for an `"ops"`-texted record; absent → skip (foreign/malformed
   `stdlib.image` records; per-module images are `FormatRefused` upstream at §5.1 and never reach
   the graft), because `registerProgramImage` verbatim returns `unexpected` rather than a no-op
   (`vortex_vm.cpp:138–141`). `vm.registerProgramImage(holder)` then walks that private rank →
   `"ops"` → `+d.values → +d.step` chain, consuming the (already translated) members into
   `opcodeMap_` — **scoped per image by construction**: the base image's holder registers its ops; a
   §6.4 local image's holder registers its ops; foreign `"ops"`-texted records anywhere are
   structurally unreachable, not merely unscanned. Member decode ran *inside* the mark during step
   3's chain walk, so post-`discard` registration is infallible (a member failing
   `isRegistrableOpcode` refused earlier at translation). Residual: a foreign `"stdlib.image"`
   record is still enumerated like a grafted one — member decode gates it (`isRegistrableOpcode`,
   `vortex_vm.hpp:155–157`, called by `registerProgramOps`, `vortex_vm.cpp:97–119`); same exposure
   as today's foreign-`"ops"` record, no worse.

1. **Rebind runtime tables** (`rebindInstance`): `routineBindings_.clear()` then rebuild from the
   `+d.params` signature records (present in *both* modes — builders emit them, §4); every cached
   ref — `predEqual_`…`predBridgeEdge_`, the `predVortex*` five,
   `goalHalt_`/`goalFail_`/`goalSentinel_` — re-resolves by symbol path, total and `nullopt`-safe
   (this cached-ref half is `rebindCaches()` in code, P33); then registers ops by enumerating
   **all** `"stdlib.image"` key records on `home +d.vars` — for each: `linked(key, values, POS)` →
   holder, ops-presence precheck, `registerProgramImage(holder)` (grafted or Cpp-built — uniform;
   this intentionally re-registers the grafted holder — `registerProgramOps` is idempotent — since
   the enumerate-all exists for `Cpp`-mode and non-grafting instances; a malformed foreign
   `stdlib.image` aborts *that record's* `registerProgramOps` mid-list non-atomically — accepted
   exposure — and the loop **continues** to remaining records). **On arena swap** there are two
   distinct paths, and the doc names both honestly:

   (a) **Host replacement — the shipped path.** `vortex_host_` is destroyed by `ensureVortexHost`'s
   **unconditional** replace arm — `vortex_host_ = make_shared<VortexHost>(&engine_->manifold())`
   whenever `engine_` is non-null (`zigzag_visualizer.cpp:2857–2866`). Most replacement paths reach
   it: `adoptDocument` (:306 — the document funnel behind `adoptXanaduStore` :354–360,
   `adoptXanaduDocs` :424–428, `populateFallbackStructure` :212, and `xuzz_app.cpp:629,:836,:3008`),
   `bindXanaduStore` (:392), and six unguarded command paths (:3345 `translateVQLAndAttachToFocus`,
   :3594 `navigateVQL`, :3618 `executeVQLScript`, :3640 `executeVPL`, :3661 `executeLogicQuery`,
   :3690 `defineMacro`). `compileVQL` (:3395–3397), `vortexHost()` (:2850–2855), and
   `dispatchAction` (:2869–2872) ensure only when the host is null. `attachVortexHost` (:2844–2848)
   assigns directly, bypassing this funnel. `bindManifold` has **zero in-tree callers** — it is the
   in-place-swap primitive the multithreaded-host direction
   (`design/vortex-multithreading-study.md`) generalizes, exercised today only by the §11 swap gate.
   On host replacement the ctor path covers everything the resync machinery covers in-place — a
   fresh `VortexCore` re-derives `home_`/`dims_` at construction — *except* the outgoing host's
   liveness: `bindXanaduStore` calls `engine_.reset()` (:364) *before* the host is replaced, and
   every other `ensureVortexHost` caller drops the host with no teardown at all, so an outgoing
   host's live `choiceStack_`/produce cursors would be silently destroyed. **Two check positions are
   required, not a single post-mutation funnel gate:** `bindXanaduStore` and `adoptDocument` check
   at their first statement, before any presentation mutation, store write, `ownedStore_`
   replacement, or engine reset (`:362,:215`, respectively). Their API wrappers `adoptXanaduStore`,
   `adoptXanaduDocs`, and `reloadStoreVersion` propagate the refusal before doing their own work.
   `attachVortexHost` checks the **outgoing** host before direct assignment. The incoming host must
   use the current engine manifold/store when `engine_` exists. An independently owned host is
   permitted only in the no-engine case, with base/store lifetimes covering installation; it cannot
   use unrelated cell space against a displayed engine document. Coherence is a caller precondition
   of this embedding-only API, not a new mismatch-refusal branch. Incoming marks remain valid
   because installation preserves that arena; they block later replacement until the owning runner
   drains them. `ensureVortexHost` retains the check immediately before its replace arm for command
   callers, whose engine and store have not changed. Each uses a new public
   **`VortexHost::liveMarks() const -> std::size_t`** (P30), which sums `ArenaManifold::liveMarks()`
   (an adjusted `std::uint32_t` arena count) over the primary arena and every private
   `extraContexts_` arena into a `std::size_t` total internally. Existing primary-only accessors
   cannot do this; no context iterator is exposed to UI code. A null outgoing host means count zero.
   Check and mutation run under the host's existing serialized ownership; background jobs must be
   joined before replacement, not raced with this snapshot. Nonzero count refuses with
   `ChoiceStackLive` carrying that count. Hydration-floor accounting is the explicit arena state
   specified in §5's invariant, including eviction; it is not inferred from `floors_.size()`.
   Today's `extraContexts_` are unhydrated (`vortex_host.cpp:107–115`), and OR-parallel hydration
   uses transient local arenas (`or_parallel.cpp:345–349`). Arena marks are the right mechanism —
   every choice point takes a `core_.arena().mark()` (`vortex_vm.cpp:317–326`) and every produce
   cursor's *enclosing choice point* holds one (`spawnCursor` itself takes no mark, :230–244 —
   `goalProduce`/`goalBoundedFirst`/`goalResults` spawn-then-push,
   `vortex_stdlib.cpp:4210,4247,4272`, so a live cursor implies a live mark) — so the count covers
   the primary VM's stack, `vplEngine_`'s `ownedVm_` stack (a second surviving `VortexVM` over the
   same arena with no `vm()` accessor, `vpl_engine.hpp:129–136`), and any stray non-VM `mark()`
   holder, with no per-VM enumeration. **The refusal has a channel:** `ensureVortexHost`,
   `bindXanaduStore`, `adoptDocument`, `adoptXanaduStore`/`adoptXanaduDocs`, `reloadStoreVersion`,
   `attachVortexHost`, and `bindManifold` all return expected of their actual object pointer for
   chainable visualizer/host mutators (the house rule — a fallible op returns `expected`, never a
   sentinel or a `void` swallow). On the unexpected arm the caller honors it: `bindZigzag` skips
   **all** post-bind bookkeeping
   (`zigzagStoreIndex`/`sliceHeads`/`map`/`linkContext`/`bridgeCoordinator`/ `anchorPresentation`
   unchanged, `xuzz_app.cpp:1829–1841`). The lambda itself returns expected; its lens-surface branch
   has no visualizer bind and succeeds without that call. Every caller checks before its own
   post-bind work: **reorder** `views.documentActivated` to bind before
   `keyboardPane.leaveZigzag(false)`, and adoption callers skip minted-map focus restoration on
   refusal (`xuzz_app.cpp:632–637,836–844`). The old host, engine, bound store, focus, and document
   remain unchanged because the check precedes mutation. **The refusing visualizer member**, not the
   external lambda, writes its private `commandBarFeedback_` and error flag and calls
   `setCommandBarVisible(true)`: the existing renderer is gated by that visibility flag
   (`zigzag_visualizer.cpp:2393–2455`), so setting text alone is not observable. Startup image
   refusal uses this same explicit open-and-write path. Boot binding (`xuzz_app.cpp:1590`), adoption
   sinks (:629,:836,:3008), all `bindZigzag` call sites, reload wrappers, and visualizer tests
   consume expected results. Constructor fallback/lazy/const-compile null-host callers explicitly
   check the result; liveness refusal is impossible there. Tail ensures after the serialized entry
   check also check and propagate any unexpected error. Recorded image refusal is handled by the
   post-construction status reader (§5.4), not confused with a replacement unexpected result.
   `reloadStoreVersion` prechecks before its own `store_->setCurrentVersions`
   (`zigzag_visualizer.cpp:415`); adoption wrappers precheck before `projectStoreWithProvenance`,
   not after projection.

   | Converted operation                                 | Result                                                  |
   | :-------------------------------------------------- | :------------------------------------------------------ |
   | Visualizer bind/adopt/reload/attach/ensure mutators | `std::expected<ZigzagVisualizer *, StdlibImageRefused>` |
   | `VortexHost::bindManifold`                          | `std::expected<VortexHost *, StdlibImageRefused>`       |
   | External `bindZigzag` lambda                        | `std::expected<void, StdlibImageRefused>`               |

   **Command-path policy:** a command-entry `ensureVortexHost` that would replace a marks-live host
   refuses to *replace* and **skips command execution**. It retains the old host, engine, and store.
   Commands keep their existing bool, `ScriptResult`, vector, and `CompilationResult` signatures:
   bool returns false, vector returns empty, message-bearing results report failure plus cause, and
   all write feedback **and open the command bar**, including non-bar invocation. No success result
   implies that the refused command ran. This intentionally replaces revision 6's
   proceed-on-retained policy: transient compiler/stdlib construction can register or mint under the
   suspended flow's marks, conflicting with §5's invariant. Cursor-preserving command coexistence
   requires a separate execution isolation design and is not promised in v1. The alternative
   ("cursors can't outlive a command") is refused because `goalProduce`-class flows suspend marks
   across commands deliberately — the doc's own model says they can and do. The recovery contract's
   audience is the in-flight flow, not a person at a keyboard — "resume the suspended producing flow
   to completion through its owning runner, then retry"; embedders use `VortexStdLib::goalResume` or
   re-drive `goalResults` (`vortex_stdlib.cpp:4222–4236,4282–4287`), not a nonexistent
   `std:goal/resume` export. No cursor-enumeration or cancellation surface exists in v1. The owning
   runner must retain its cursor token; losing it is an embedding error. Unbounded flows are not
   promised bounded-time recovery. The operation's expected error and feedback carry the aggregate
   count; stdlib status carries only its own arena count. Separately, an *external* shared_ptr copy
   retained after a successful, mark-free replacement still has the pre-existing non-owning-base
   lifetime restriction. This does not excuse a dangling installed host on refusal: that state is
   prohibited by the entry checks.

   (b) **`bindManifold` — the in-place primitive** (`vortex_host.cpp:149–158`, move-assigns
   `primary_.arena` in place): the same `liveMarks() != 0` gate runs **at entry, pre-move-assign** —
   a post-swap refusal cannot un-swap, and `ensureCells`'s own entry check reads the *fresh* arena
   where the count is trivially 0, so entry is the only place it can fire. It returns
   `std::expected<VortexHost *, StdlibImageRefused>` — the pre-swap refusal travels only in that
   operation's error and caller feedback. It does not write any stdlib loader-status field or
   `lastRefusal`. Its `Mark`s snapshot the old arena's sizes; a `backtrack()`/`release()` would
   truncate the *new* arena to dead counts (`arena_manifold.cpp:1671–1674`). After the swap:
   `opcodeMap_`, `cursorPC_`, `memoizedOps_`, `choiceStack_`, `oraclePath_`, `oracleCursor_`,
   `cutCallback_`, `currentDeweyPath_`, and `totalCycles_` are cleared — the whole arena is replaced
   so every entry is dead, and `contains()` cannot filter because stale ints *alias* fresh cells
   (the write guard consults `hasOpcodeKind` map-only, `vortex_vm.hpp:194–196`; a stale
   `cutCallback_` would fire a dead coordinator binding, a stale `oraclePath_` mis-steers
   backtracking — the four beyond the R5 list are cleared for the same aliasing reason, not left as
   "probably harmless") — **and `VortexCore` runs a genesis resync**: `home_`/`dims_` are
   ctor-cached (`vortex_core.cpp:16,31–74`), and
   `resolve()`/`findOrMintDimension`/`getOrCreateModule` all consume them, so they are re-derived
   against the new arena (adopt-or-mint, with an own-home guard for null-base arenas:
   `arena().home()` returns only the *base*'s home, `arena_manifold.cpp:30–32`; fresh dim mints
   append at `rankTail`, not `mintNamedDimension`'s adopted-walk position — `vortex_core.cpp:18–28`
   — and a base missing a mid-list genesis dim leaves the base rank's tail orphaned in the overlay:
   adopted dims stay reachable via `baseDimensionNamed`, invisible to rank walkers; pre-existing
   sever class, named as a known limitation) before `ensureStdlib` re-fires — otherwise the graft's
   own seed/probe reads dead refs. Runtime mints re-register via `mintOpcode`; a `VortexStdLib`
   sharing a `VortexVM` just re-registers the same arena's manifest.

   **Boundary named:** arena-keyed caches *outside* `VortexCore`/`VortexVM`/`VortexStdLib` are
   caller-managed — `vplEngine_`'s `defaultDim_` (`vpl_engine.hpp:140`, a cached `d.1`),
   `env_`/`dependencyStack_`/`cellDependencies_` (:141–144), `vqlEngine_`'s
   `env_`/`perspectiveClones_`/`functors_`/`sessionMounts_`/`userDefs_` (`vql_engine.hpp:225–238`),
   `vqlCompiler_`'s `varBindings_` (`vql/compiler.hpp:116`), and user entries of
   `customActionRoutines_`/`macroRegistry_` (`initHostServices` re-registers only built-ins,
   `vortex_host.cpp:125–142`). These are pre-existing cells-over-side-tables debt — the same defect
   class P29 eliminates for `routineBindings_` — tolerated in v1 as a caller-managed boundary; the
   v2 shared-base direction (§7.2) either migrates them to cells or accepts them as a permanent
   named exception. Post-swap these refs alias fresh cells — the same dead-ref argument — so
   document rebind *rebuilds engine state*: `env_` is session state and is dropped, not remapped
   (VQL env entries are computed cell sets with no name-addressable manifold path to re-resolve —
   dropping is the only honest semantic); the host replacement path gets this for free (fresh
   engines). For `bindManifold`, P30 adds public chainable `VPLEngine::onArenaSwapped()`,
   `VQLEngine::onArenaSwapped()`, and `VQLCompiler::onArenaSwapped()` methods implemented inside
   their own classes. Inside `initHostServices`, order the swap path explicitly: core genesis
   resync, VM clearing, ensure/rebind of both long-lived stdlibs, these cache-reset methods, then
   action/worker-service reinstallation. VPL resets `defaultDim_` and clears env/dependency state;
   the next `defaultDim()` lazily resolves/mints the new `d.1` as today
   (`vpl/vpl_engine.cpp:104–109`). VQL clears all listed maps and resets `activePerspective_` to
   empty. Session mounts are caller-managed: reissue MOUNT declarations after a swap rather than
   expecting cleared mounts to survive. Its owned `MultiStoreCoordinator` is reset **in place**,
   through a new chainable `MultiStoreCoordinator::onArenaSwapped()`: clear vector `stores_` and
   reset optional `storesTail_`, then call existing `initDimensions()` against the resynced core to
   replace `coordinatorHome_` and cached store/clone/name/role dimensions (`multi_store.cpp:40–46`,
   `multi_store.hpp:175–184`). No mounted-store refs survive; saved documents themselves are not
   changed. The object stays at the same address because VQL's `coordinator_` is a reference and
   cannot be reseated by replacing `ownedCoordinator_`. Host VQL owns its coordinator
   (`vql/vql_engine.cpp:121–123`), so its reset calls this method. A VQL engine with a borrowed
   coordinator leaves coordinator reset to that external owner; the owner must resync it before
   reuse after replacing its arena. The compiler clears `entryOp_`, `currentOp_`, `allOps_`,
   `lastResultCells_`, and `varBindings_` (`vql/compiler.hpp:111–116`). No private-member access by
   the host is implied. §11's swap gate asserts it (`vplEngine_.defaultDim()` post-swap resolves the
   new arena's `d.1`; VQL env lookups miss rather than alias). **Write-guard lifetime is part of
   P30, not an unresolved either/or.** Today installation is last-wins (`vortex_vm.cpp:84–95`) and
   destruction clears under `guardInstalled_` (`:76–82`), meaning "installed previously", not "owns
   the current slot". A transient compiler can overwrite the persistent VM's guard and later clear
   it. Reinstalling only at arena swaps leaves the rest of a session unprotected. P30 replaces the
   single slot in `vortex_core.hpp` with owner-keyed live predicates:
   `setWriteGuard(const VortexVM &owner, predicate)` inserts or replaces that owner's entry;
   `clearWriteGuard(const VortexVM &owner)` removes only that entry. `writeValue` refuses if **any**
   live predicate recognizes the clone master. `installWriteGuard` registers by `*this`; `~VortexVM`
   unregisters only itself. Remove obsolete `guardInstalled_` and its historical single-slot
   comment. Repeated installs are idempotent. The existing lifetime contract remains that the
   referenced core outlives its VMs; mutation is serialized with VM execution. No predicate can
   survive its VM destructor. This also protects primary-VM ops when VPL's second VM has a different
   map. `rebindInstance` repopulates each map **in place**, so all predicates see their owner's
   current map. Transient `vprolog::Compiler` VMs (`vortex_host.cpp:935,1001`) and vquery's `vm`
   (`vquery/main.cpp:487`) cannot remove a surviving VM's protection. Cost: one owner/predicate
   entry per live VM and O(live VMs) predicate checks per guarded write (normally one or two
   persistent VMs per core plus transient compilers), with no arbitrary ceiling. This runtime
   lifetime registry cannot be manifold data: it contains process-local callbacks. A single
   installer token is insufficient because removing the last installer still loses a surviving
   predecessor; swap-only reinstall is refused for its mid-session unguarded interval. Tests cover
   both destruction orders and disjoint opcode maps, not just immediate post-swap writes.

The graft is O(cells) — same order of work the C++ builders do — but ~150 lines of generic walk, not
~4,000 lines of module knowledge.

**The named compromise (transclusion honesty):** the v1 graft *copies*, it does not transclude. A
grafted cell's text is a byte copy with no `d.clone` arm, no provenance edge, no span into the
shipped permascroll — none is expressible across index spaces in v1. Three consequences are accepted
and named: grafted text carries no attribution to the shipped span; a stdlib cell later promoted
into a user document spools *fresh primedia* (quotation/transcopyright provenance severed,
`arena_manifold.cpp:1993–1995`); and `std:hypertime` answers hold only on the image fold —
in-context stdlib cells are history-less by construction. The v2 shared base (§7.2) is what restores
single-instance, attributable stdlib cells.

### 5.3 What does not move

- `call()` fast path: `routineBindings_` repopulated by step 5; `inputsOf`/`outputsOf` fallback
  stays for ad-hoc routines. `routineBindings_` becomes a *derived index* — the authoritative
  signatures live in `+d.params` records.
- `#NAME` dispatch: C++-backed routines keyed by opcode label (`#GOAL_*`, `#ENV_*`, `#IO_*`,
  `#MAP_*`, `#HYPER_*`, `#NAV_*`, `#UI_*`, `#BRIDGE_*`, `#ARRAY_*`, `#ZZ_*` — `vortex_stdlib.cpp`
  `call()` ~:2047–2503) are unaffected — labels are cell content. `std:xuzz`/`std:xudu` actions are
  `Nop`-minted and dispatched app-side via `registerActionRoutine`/`dispatchAction` — likewise
  content-driven.
- The builtin-predicate dispatch in `solveQueryHelper` (`vortex_stdlib.cpp` ~:357; `is/2`,
  arithmetic, `vortex_*` introspection, scanning) is engine code and stays.
- `boundStore_`, `workerPool_`, `cliArgs_`: runtime properties, set post-graft as now.

### 5.4 Policy modes and the refusal matrix

The load decision is taken **at graft time** inside `VortexStdLib` (the only form that covers
`WorkerContext`s built outside `VortexHost` — `mesh_actor.cpp:191,325`, `spool_mesh.cpp:113`,
`mesh_evaluator.cpp:936`, `vagent/lineage.cpp:564`, `apps/vagent/main.cpp:136`), under a
process-level `StdlibPolicy` —
`{optional<Mode> mode, optional<Path> imageDir, optional<bool> localImage}` — read as one atomic
snapshot per `ensureCells` (`atomic<shared_ptr<const StdlibPolicy>>` — worker-thread compilers
construct `VortexStdLib` inside `workerPool_->run`, `vortex_host.cpp:1001–1031`, while the app seam
writes on the main thread). Fields are **per-field `optional`s with per-field setters** — the dir
writer (app seam) and the mode writer (Settings read) must not clobber each other with a
whole-struct replace — and each setter updates through a `compare_exchange_weak` retry loop on the
shared-pointer atomic (load, copy-modify, retry — a plain `load→copy→store` would lose a concurrent
field update; writers are main-thread today so the race is latent, pinned anyway). Precedence,
settled conventionally:

- **mode**: explicit setter > `$GLEDITOR_STDLIB_SOURCE` > `vortex.stdlib_source` > default
  `ImageThenCpp`. Accepted spellings: `image` / `cpp` / `imagethencpp`, lowercase exact-match; an
  unrecognized spelling warns on `"vortex.stdlib"` naming the value and is treated as unset (silent
  fall-through could mask the very `ImageMissing` the user meant to exercise). **Settings-store
  only, and enforced by read site, not by the key:** `vortex.stdlib_source` is read by a named
  helper at the *Settings call site only* (`SettingSpec`-registered, `system_docs.cpp:818–823` —
  notes carry "xuzz-only; read once pre-context — a mid-session edit applies next launch": the
  Settings-change arm at `xuzz_app.cpp:4219–4228` re-applies sibling `vortex.*` keys live via
  `loadConfigFromStore`, but this key is deliberately **startup-only** — the seam never re-fires and
  the first fold has already latched) — **never** inside
  `VortexHostConfig::fromStore`/`loadConfigFromStore`, which `bindStore` feeds with *document*
  stores (`vortex_host.cpp:160–166`, `zigzag_visualizer.cpp:2861–2863`): a document carrying the key
  would otherwise flip process mode — the trust step refused. And the read must happen **before the
  first context exists** — which is *earlier than `bindXanaduStore`*: the first `ensureCells` fires
  inside the **`ZigzagVisualizer` constructor** at `xuzz_app.cpp:1588`
  (`make_shared<ZigzagVisualizer>("")` → `populateFallbackStructure`
  `zigzag_visualizer.cpp:74→:157–213` → `adoptDocument` :212→:215–308 → `ensureVortexHost`
  :306→:2857–2866 → `VortexHost` ctor → `WorkerContext`); the ctor's own
  `adoptDocument`→`bindStore`→`loadConfigFromStore` (:244–247,:2861) runs even earlier on the
  fallback document store. So the seam — `systemStore(Settings)` + mode setter + the
  `assetPath("vortex-stdlib")` dir setter — sits **above `xuzz_app.cpp:1588`**, before
  `make_shared<ZigzagVisualizer>`; one line later and the process fold latches with an unset policy
  — `ImageMissing` before the dir setter can run under `Image`, or a silent permanent C++ under
  `ImageThenCpp`. (The ctor-path host is discarded by `bindXanaduStore`'s `engine_.reset()` at :364
  — the wasted graft is bounded, but the latch is process-level, which is what makes the ordering
  fatal.) `attachVortexHost` (:2845) accepts a pre-built host and bypasses the seam — it has zero
  in-tree callers. Today it is replaced by the next engine-bearing `ensureVortexHost` call,
  including commands; under P30 a live-mark refusal retains it. Scope: the key is **xuzz-only** —
  `loadConfigFromStore` is invoked only inside xuzz (`xuzz_app.cpp:1596,4226`,
  `vortex_host.cpp:165`), so `vpl`/`vprolog`/`vagent`/`vicon`/`viconc` honor env-or-default only;
  `status()` exposes the folded dir + effective mode so the boundary is diagnosable. The key is
  **absent from the shipped Settings document** (absent = env-or-default); a user-added value
  validates against the registered `SettingSpec` and applies next launch — the notes say so, so
  nobody expects a mid-session edit to do anything. `loadConfigFromStore`'s shipped callers are
  `xuzz_app.cpp:1596,4226` and `vortex_host.cpp:165` (plus the test site
  `vortex_host_test.cpp:181`).
- **dir**: explicit setter > `$GLEDITOR_STDLIB_DIR` (taken-as-given; names the leaf image directory
  itself — `store/ops.nodes`+`permascroll/` live directly inside it) > `$GLEDITOR_ASSET_DIR` >
  `GLEDITOR_DATADIR` > `./assets` — these last three are asset roots with `/vortex-stdlib` appended,
  not leaf image directories. Every level is **taken-as-given** (stricter than `assetDir()`, which
  takes only `GLEDITOR_ASSET_DIR` literally, `paths.cpp:45–48`, and `isDirectory`-guards the rest,
  `:50–75`; `GLEDITOR_DATADIR` is a *build-time* define compiled into `assetDir()`, not a
  launch-time override): a bad `$GLEDITOR_ASSET_DIR`/configured `DATADIR` resolves to a nonexistent
  dir → `ImageMissing` naming that path, never a silent skip down-chain. **No store key**: the fold
  precedes `loadConfigFromStore` in every shipped flow, so a stored dir could never take effect, and
  store-content steering the loader's directory is a trust step refused on principle.
- **localImage**: explicit setter > `$GLEDITOR_STDLIB_LOCAL=0|1` (taken-as-given) >
  `localImage.value_or(mode != Mode::Cpp)` — unset resolves `true` under `ImageThenCpp`/`Image`,
  `false` under `Cpp` (parity/debug stays clean — a `GLEDITOR_STDLIB_SOURCE=cpp` run or the §11
  Cpp-parity gate must not silently pick up a local patch, the exact thing the generator's
  `localImage=false` pin prevents). The local dir itself takes the same chain shape:
  `paths::dataPath("xuzz", "vortex-stdlib-local/")` default < `$GLEDITOR_STDLIB_LOCAL_DIR` override
  (taken-as-given, leaf-level — the generator's `--out` must name the same path the app resolves, or
  `GLEDITOR_STDLIB_LOCAL_DIR` is set for the app to match; a mismatch is silent-by-default, so an
  explicit `GLEDITOR_STDLIB_LOCAL=1` with an absent/unreadable resolved dir **warns** on
  `"vortex.stdlib"` naming `local.dirResolved` — expressed dev intent, not the default — while
  unset-and-absent stays silent), so devs can A/B images without clobbering the canonical dir.

The mode is consulted per `ensureCells`; the image itself folds once and a failed fold latches
(§5.1). Long-lived instances re-run `ensureStdlib` when their arena is replaced — `initHostServices`
invokes it on `primary_.stdlib` **and** `vplEngine_.stdlib()` (`vpl_engine.cpp:77–86` —
`ownedStdlib_` is the one long-lived non-primary instance; transient compilers are safe by
construction). The identity token is pinned: each `VortexStdLib` records **`arena().base()` plus
`core().home()` (`home_` — the cell the genesis bind resolved)** — `primary_.arena` move-assigns *in
place* (`vortex_host.cpp:149–155`), so the arena's address can never change, and `arena().home()`
returns `nullopt` for null-base arenas (`arena_manifold.cpp:30–32`) — `core().home()` is the member
that discriminates scratch arenas. The token distinguishes **cross-base moves only**; a same-base
swap (`bindManifold` over the same `baseManifold_`) leaves both halves unchanged and is detected by
the unconditional `ensureStdlib` re-fire through `initHostServices` plus the `stdlibPresent` probe,
with the sentinel-check — re-verify `contains` + `textOf` on the grafted `stdlib.image` key ref
recorded at graft time (`contains` alone can alias fresh cells) — covering callers that can't resync
(a `Manifold` generation counter remains the named alternative). The re-fire happens through
`bindManifold`'s own post-swap genesis resync (§5.2.5), so `home_`/`dims_` are already live when
`ensureStdlib`'s probe runs. Per-app image subsetting is U-X6. Tree-run consumers: `xuzz`, `vpl`
(`VPLEngine` → `ownedStdlib_`), `vprolog` (`main.cpp:559–561`), `vagent`, `vicon`/`viconc`
(`apps/vicon/main.cpp:121` → `vicon::Driver` (arena/core/compiler by value,
`vicon/driver.hpp:66–68`) → `vicon::Compiler::stdlib_`, `compiler.hpp:160`;
`viconc/main.cpp:75–77`), `vlace`/`vlacec` (`vlace` drives `runMeshCommand` → per-job
`WorkerContext`s, `vlace/main.cpp:108` → `mesh_actor.cpp:191,325` — they fold like every consumer;
`vlace::Driver`/`vlacec` run `VortexCore`+`VortexVM` stdlib-free, `driver.cpp:50–53,92–95` — the
in-tree precedent for the §6.4 dev-scratch) — `vquery`/`vqueryc`/`vplc` have no `VortexStdLib`
(`apps/vquery/main.cpp:487–488,532`, `apps/vqueryc/main.cpp:118–121`, `vplc/main.cpp:121–124`).
Bare-`VortexCore` helpers (`xuzz_app.cpp:542,889,907`, `lens_file_picker.hpp:28,197`) never fold —
"once per process" is per-`VortexStdLib`, not per-`VortexCore`.

| mode           | image absent    | stale/foreign format | epoch ≠ engine  | scroll missing/short | mid-graft refusal          | image OK |
| :------------- | :-------------- | :------------------- | :-------------- | :------------------- | :------------------------- | :------- |
| `ImageThenCpp` | fallback + warn | C++ fallback + warn  | fallback + warn | fallback + warn      | rollback → fallback + warn | graft    |
| `Image`        | refuse          | refuse               | refuse          | refuse               | rollback → refuse          | graft    |
| `Cpp`          | —               | —                    | —               | —                    | —                          | C++      |

("Stale" here means *storage-format* staleness — `OpsSegmentUnreadable`-class — not a version-old
image, which §6.3 covers. `image absent` warns even under `ImageThenCpp`: absence is *the* packaging
failure an installed app most needs flagged — dev trees rely on `status()`/the `"vortex.stdlib"` log
rather than silence. **Environmental refusals are outside this matrix**: `MarksOutstanding` refuses
`ensureCells` outright in *every* mode, and `ChoiceStackLive` refuses document
adoption/binding/direct attachment before mutation, command-driven replacement at
`ensureVortexHost`'s replace arm, and the *in-place swap* at `bindManifold` entry (§5.2.5) — a C++
fallback mints under the same live mark and hits the identical untrailed-state corruption (§5
invariant).)

**Error channel & observability:** `ensureCells` returns `std::expected<void, StdlibImageRefused>`;
this shared refusal type includes environmental causes despite its historical name. The instance
records image-initialization refusals (ctor is `void`). Host/visualizer `ChoiceStackLive` refusals
instead return through the operation's expected error and feedback; they do not overwrite a retained
stdlib's loader status or make its valid image unusable. `StdlibImageRefused::what()` carries the
cause (`ImageMissing`, `ImageUnreadable`, `ScrollTruncated`, `EpochMismatch{image,engine}`,
`FormatRefused`, `MarksOutstanding`, `ChoiceStackLive`, `GraftRefused{detail}`), both epochs when
relevant, and a payload by cause class — image refusals carry the resolved dir + the remedial env
var; environmental refusals carry the live mark count (audience: the in-flight flow — "resume the
suspended producing flow to completion through its owning runner, then retry"; embedding APIs are
`goalResume`/`goalResults`, not a `std:goal/resume` routine; no cursor-enumeration surface exists in
v1). The recorded refusal's throw placement is pinned, not left ambiguous:
`solveQuery`/`solve`/`solveOnce` do **not** throw at entry — the refusal throws inside
`solveQueryHelper`'s per-goal dispatch, checked *after* the `vortex_stdlib_status` arm (functor-text
match, the `vortex_param` precedent at `vortex_stdlib.cpp:709–710` — no exported symbol is needed
for the arm to fire), so a query consisting solely of `vortex_stdlib_status` goals answers in-band
under a latched refusal while `std:math/abs` goals refuse at their own dispatch — the same
diagnostic exemption `resolve()`/`modules()`/`symbolsInModule()` get at the public layer
(`nullopt`/empty), which keeps the `stdlibPresent` probe and internal rebinding from wedging on the
recorded refusal. `call(CellRef, args)` keeps its entry throw; `call(path, args)` first resolves and
delegates (`vortex_stdlib.cpp:2032–2039`), returning its existing empty result if resolution fails.
Thus `call("std:logic/vortex_stdlib_status")` never reaches the cell-ref throw under a missing-image
refusal (`resolve()` → `nullopt`): the builtin is solver-goal-only, stated honestly. For
introspection parity it is registered `createPredicate`+`exportSymbol` in `std:logic` beside
`vortex_param` (`vortex_stdlib.cpp:3434–3435`) and `predVortexStdlibStatus_` joins the P33
cached-ref rebind list — so `symbolsInModule` sees it and it survives `rebindCaches` post-graft.
**`status()` is pinned**: `VortexStdLib::stdlibStatus()` — on the instance every consumer owns, not
`VortexHost`, which is xuzz-only — returns this record for hosts/tools:

```text
{mode, dirResolved, source: optional<source>, lastRefusal?,
 contentVersion?, arenaLiveMarks, local:{state, dirResolved}}
```

`source` is `optional` because "nothing loaded" is a real state (an `Image`-mode or environmental
refusal during initialization leaves no stdlib and no `image`/`cpp` enum value may claim otherwise).
`arenaLiveMarks` is an instantaneous `core_.arena().liveMarks()` count for this instance's arena,
not the host aggregate. `lastRefusal` records only this instance's initialization failure. A refused
host replacement can therefore have nonzero aggregate count and an unchanged healthy stdlib status;
no host back-pointer or status-provider callback is implied. The aggregate is reported in the
operation's expected error/HUD payload only. Successful `ImageThenCpp` fallback leaves `lastRefusal`
empty and `source=cpp`; the rejected image's cause is a warning log, not a failed initialization or
automatic error-bar opening. Only failure of initialization itself sets `lastRefusal`. `dirResolved`
reports the *attempted* dir under an explicit-setter refusal (what the user typed, so a typo is
visible in-band). `local.state ∈ {absent, skipped, grafted, failed, suppressed}` is pinned by
trigger — `absent` (no image at the resolved dir), `skipped` (`localImage` resolved false — `Cpp`
mode or explicit `0`), `grafted`, `failed` (malformed/stale — rolled back, base intact),
`suppressed` (`Image`-mode base refusal, §6.4) — with `dirResolved` always populated whenever the
local chain ran, so "wrong dir" is always diagnosable; the `"vortex.stdlib"` startup log is emitted
at `GLEDITOR_LOG_INFO` from `ensureStdlib` after policy resolution in every mode, reporting the
local decision and, **when its chain ran**, the resolved local dir. A skipped chain has no attempted
local dir. All stdlib-bearing consumers can see where the loader looked; bare-core consumers do not
emit this line. Reachability is stated honestly: the builtin is reachable only where `solve*` runs —
vprolog and xuzz `solveLogic`; `vpl` never calls `solve*`, `vagent` runs its own solver, and `vicon`
compiles only — so those three get the C++ accessor plus the `"vortex.stdlib"` startup log line; no
fake REPL command is promised. A recorded refusal under `Image` mode surfaces into xuzz at
**`ensureVortexHost`, immediately after successful host construction**: the member reads
`newHost->primaryContext().stdlib.stdlibStatus().lastRefusal`. It compares the structured diagnostic
`{effectiveMode, dirResolved, cause, epochPair?, detail}` with visualizer-owned
`lastSurfacedImageRefusal_` (optional, initialized empty). A changed diagnostic is copied into that
latch, written to feedback, and opens the command bar. An identical diagnostic on a per-command host
rebuild does not reopen it. A successful initialization with no recorded refusal clears the latch,
so a later new failure episode is visible. This transient UI latch is not per-host and is not
persisted. `resetStdlibImageForTesting` resets engine state only; tests use a fresh visualizer when
asserting a fresh startup notification. This is the concrete reader and once-per-diagnostic-episode
rule — not a constructor expected channel or an invented banner. Errors do not wait for use-points
where `solveLogic` would swallow them to `std::cerr` (`vortex_host.cpp:942–945`); mid-session
`ChoiceStackLive` refusals surface through `commandBarFeedback_` on the refusing bind path
(§5.2.5a). Fallbacks and refusals log on the `"vortex.stdlib"` spdlog category (the `vortex.host`
convention) with the decision inputs — mode, dir tried, cause — so "why did my stdlib load from C++"
is always answerable.

______________________________________________________________________

## 6. Store Updates — the OSMIC Model (v1 honest scope)

### 6.1 What ships each release

v1 delivers **a complete sealed image per release**, swapped atomically as a unit (the whole
`assets/vortex-stdlib/` directory is one artifact; `make install` installs it as one).
`contentVersion` orders releases; `imageEpoch`/`engineEpoch` comparison gates acceptance (§5.4).
Regenerating without history is also the compaction story — the spool does not grow unboundedly,
each release starts a fresh lineage.

Within one image, OSMIC microversioning is already doing its job: the generation run's ops are an
immutable ancestral trail — `xudu-dump --section=ops` renders it, and `std:hypertime` answers "what
does this image contain" on the fold (with the image's `Store` bound —
`hyperLca`/`hyperDepth`/`hyperChildren` consult `boundStore_`).

### 6.2 What is deferred (U-X7): shared-lineage releases and delta transport

The attractive version — release *n*+1's image containing release *n*'s ops as its ancestry,
upgrades transported as appended microversions via `exportBinaryOps`/`adoptOpRecords` — is
**deferred**, for a verified reason: nothing today produces the diff honestly. `exportSymbol`
appends and `resolve()` takes the *first* text match (`vortex_stdlib.cpp` ~:2005–2012), so a
re-minted same-name symbol lands *after* the old one on `+d.vars` and the update silently never
takes effect; a purely additive diff can never express retraction (clears exist only under
`ShadowCommit::Clears`, `arena_manifold.cpp:2066–2095`); and re-emitted manifest records shadow the
same way. A real `--extend` needs: a differ over fold-state, repoint semantics for changed
symbols/records (or versioned module names), and retraction emission — priced as its own proposal in
v2, not half-built here. The user-visible v1 contract: each release is self-contained and complete;
upgrades are replace-in-place package events.

### 6.3 Version compatibility

| skew                       | mechanism                                           | behavior                                               |
| :------------------------- | :-------------------------------------------------- | :----------------------------------------------------- |
| storage format             | `OpsSegmentHeader.nodeSize`/`formatVersion`, tables | `OpsSegmentUnreadable`/`StoreTablesUnreadable`, named  |
| permascroll missing/short  | dir precheck + `expectedScrollBytes`                | named refusal identifying the packaging cause          |
| image epoch ≠ engine epoch | `stdlib.image.imageEpoch` vs `engineEpoch` digests  | mode-dependent: refuse (`Image`) / fallback+warn       |
| image older, same epoch    | `contentVersion` comparison                         | loads; absent modules are ordinary `resolve()` misses  |
| image newer, same epoch    | `contentVersion` > known                            | loads; informational — epoch is the compatibility gate |

### 6.4 User patching

The shipped image is read-only. A writable **local image** at
`paths::dataPath("xuzz", "vortex-stdlib-local/")` (the shared xuzz data dir — every consumer folds
the same local image; intentional one-image-per-user, stated for `vpl`/`vprolog`/`vagent` users)
grafts *after stdlib presence exists* — after the base graft under `ImageThenCpp`/`Image`, and under
`Cpp` only on explicit opt-in — the `localImage` resolution is `value_or(mode != Mode::Cpp)` (§5.4):
unset is true under image modes, false under `Cpp`, and the generator's `StdlibPolicy{Cpp}` pin sets
`localImage=false` — otherwise a hand-run `build/vortex-stdlib-gen` would fold the build host's
local image into the shipped artifact, breaking byte-determinism and the trust boundary (the
Makefile's `XDG_DATA_HOME` redirect is only incidental protection). `Image` mode + base refusal
suppresses the local graft (nothing to graft onto). Failure semantics: absence skips silently
(absence is the default); a malformed/stale local image is **scoped to the local graft** — rollback
the local graft, warn on `"vortex.stdlib"`, keep the base — never a base re-bootstrap.

**Production path, named:** `vortex-stdlib-gen --module <name> --out <local-dir>` — installed as a
developer tool in v1 (P32). The flag selects a **bootstrap-suppressed `VortexStdLib`** — the tool
constructs the scratch `ArenaManifold` + `VortexCore` + `VortexVM` + `VortexStdLib` under a
ctor-tag/`Bootstrap::Manual` arm that skips `ensureStdlib` entirely (`status().source` is `nullopt`
because no source was loaded; this is not a third source enumerator). `Manual` construction is
unrelated to `local.state=suppressed`, which means a base refusal skipped a local graft. The tool
then drives the manifest-emission skeleton factored out of `bootstrap()`: `beginImageOps()` mints
`opsHead` + the `+d.step` chain at start; the public `buildModule(std::string_view)` facade first
obtains the module cell through `getOrCreateModule(name)`, runs its named private
`build*Module(CellRef)`, and tail-appends their op members; `endImageManifest()` mints the
`stdlib.image` key record + holder + `keyRecord(holder,"ops",opsHead)` + field records at end —
`bootstrap()`'s driver becomes those three calls over the 21 base modules, `--module` over the named
subset only. `--module` is repeatable, one module name per occurrence; the selected set is built in
canonical bootstrap order, not flag order. A single invocation emits one complete image. `--out`
must be absent or empty; a nonempty destination is refused as `OutputExists`, never merged,
appended, or overwritten. Regeneration uses a fresh directory, followed by updating
`GLEDITOR_STDLIB_LOCAL_DIR` and restarting the app. That shape is forced twice over: a `Cpp`-policy
scratch bootstraps the *whole* base stdlib, so a whole-space export of it re-imports ~4,300 base
symbol cells that all tail-append as shadowed duplicates at graft (module cells name-merge; symbol
cells never do — §5.2.3); `--module` on a full scratch cannot bound it, since `promote`'s
reachability floods `edge.pos` **and** `edge.neg` (`arena_manifold.cpp:1791–1797`) — a module-rooted
export walks `module -d.stdlib→ home` into the entire library; and a stdlib-*less* scratch emits no
manifest at all — the `stdlib.image` holder/`"ops"` record live inside `bootstrap()`'s tail, so the
exported artifact would fail §5.1's own `FormatRefused` and carry no `"ops"` to register. The
suppressed construction is the only shape that mints the named modules *and* the manifest skeleton
*without* the base. The v1 limit is stated: **the selected module set must be self-contained** — no
dependency is satisfied from the shipped library. There is no automatic dependency closure. Builders
must use a checked generator lookup for every required `std:*` resolution; a missing required module
or symbol is an error, not an optional feature or an unchecked `.value()`/dereference. Selection
runs in bootstrap order: e.g. `--module std:logic --module std:goal` can satisfy goal's dependency,
whereas `--module std:goal` alone must fail before output publication. Unknown module names also
refuse. Generated callable ref targets and marked relocation referents must all close within the
selected scratch before verification can pass; dynamic runtime lookups are not a promise that
arbitrary external modules will be available. The manifest's `home +d.stdlib` rank records exactly
the included module set. Cross-module references into the shipped library remain U-X1/whole-scratch
work. The failure is observable: a dev builder resolving an absent `std:*` symbol gets `nullopt` and
the generator **exits non-zero naming the unresolvable symbol** ("builder for `std:mymod` resolved
`std:math/abs` — scratch contexts carry no stdlib; self-contained modules only") — not a silent skip
or a `nullopt` deref. The dev loop end-to-end: edit the `build*Module` C++ →
`vortex-stdlib-gen --module std:mymod --out <local-dir>` (`--out` names the image directory itself —
`ops.nodes`+`permascroll/` live directly inside it — and it must equal the directory the *app*
resolves: set `GLEDITOR_STDLIB_LOCAL_DIR` for the app to the same path, or write into the default
`paths::dataPath("xuzz", "vortex-stdlib-local/")` data dir, which is **not** on the asset-dir chain
— `GLEDITOR_ASSET_DIR` does not relocate it) → **restart the app** (the fold is once-per-process;
`resetStdlibImageForTesting` is test-only — no hot-reload). The interactive `:export-lib` flow is
*not* a source: it drives per-module `exportLibrary` (`zigzag_visualizer.cpp:3820–3831` →
`vortex_host.cpp:505`), whose output lacks the genesis prefix and fails §5.1's `FormatRefused` — v1
local-image authoring is a developer flow; an interactive `:export-lib-image` command is U-X8. Its
fold lifecycle mirrors the base's: a second process-static folded-image state, once-per-process — a
mid-session local regeneration is invisible until restart (right for v1), and
`resetStdlibImageForTesting()` covers both folded-image states.

- **New modules/symbols** graft by name-merge keyed on **full module-cell text**
  (`"std:math"`/`"sys:array"` — prefix-agnostic, the same key `getOrCreateModule` uses): a matching
  local module cell *maps to the existing arena cell*, and its `+d.vars` members **tail-append** per
  §5.2.3. Collision rule: a same-text local symbol lands posward of the base's and is **shadowed by
  `resolve()` first-match** — local images add, never replace; overriding a shipped symbol is
  precisely the repoint semantics deferred to U-X1.
- The local image passes the same §5.1 validation (epoch + scroll); its own `"ops"` record registers
  via its own grafted `stdlib.image` holder's private rank (§5.2 step 4). Its `"stdlib.image"` key
  record coexists with the base's on `home +d.vars` — harmless (validation reads records pre-graft,
  per image; registration enumerates all holders). `home +d.vars` itself stays sparse — only
  `"stdlib.image"` keys ride it; `"ops"` is unreachable from the shared rank by construction — and
  the record surface varies by mode and graft count (`"stdlib.image"` exists in both modes, dev
  fields under `Cpp`) — `status()` is the sanctioned provenance probe.
- **Cell-level patching** (repointing a symbol's entry, dropping a link) is *deferred* — U-X1 — for
  the same reason §6.2 is: repoint/retract needs `SetLink`-clears or a shadow-and-override
  convention that must be specified, not hand-waved.

A local image survives base upgrades because it is its own spool grafted by name — no shared op
indices to invalidate; the merge rules re-derive cleanly against any same-epoch base.

### 6.5 Observable change vs today: symbol multiplicity

The image is **exactly-one-bootstrap canonical**. Today production runs `bootstrap()` 3× on the
primary arena (ctor + `ownedStdlib_` ctor + `initHostServices`), triplicating every symbol on
`+d.vars`; `resolve()` first-match hides it but `symbolsInModule`/`vortex_function`/
`vortex_param`/`findall` enumerate all three. The image removes the duplication — an observable
introspection-count change, recorded here as the bugfix it is. Three more introspection deltas ship
with it, named honestly: `d.params` and `d.mesh.relocation` join the visible dimension registry (dim
listings gain entries); every wing slot carries a permanent `+d.mesh.relocation` marker edge
in-arena after graft — `bundle-stdlib` and dim-listing surfaces see them; the `stdlib.image` key
record itself is visible on `home +d.vars` browsing (one per grafted image — the manifest is data by
design); and the two newly-exported goal refs `std:goal/halt` + `std:goal/sentinel` join
`symbolsInModule` (P33 — today only `fail` is exported, `vortex_stdlib.cpp:4009`). Two
*re-introducing* cases are named too: shadowed same-text local symbols **double-enumerate** in
`symbolsInModule`/`vortex_function`/`vortex_param`/`findall` (the §6.4 tail-append lands them
posward — `resolve()` first-match hides them for lookup but not for introspection); and a grafted
ref-valued scalar carries its *image-space* text rendering (the content span is cloned verbatim
while `setValueBits` rewrites only the bits) — functionally inert, since
`asInt64`/`registerProgramImage`/`wingOf` all decode bits, but `textOf`/`xudu-dump`-class readers
see the image-space position, not the arena twin's.

______________________________________________________________________

## 7. Integration Depth

### 7.1 Chosen: per-arena graft (v1)

The stdlib materializes into each arena as today — but from data, not C++. There is no new
multi-base overlay model. P30 does add explicit synthetic-floor bookkeeping and
`ArenaManifold::liveMarks()` (§5), plus a host aggregate and the §5.2.5 pre-mutation
adoption/direct-attach and command replace-arm checks. Converted mutators return expected;
`bindManifold` checks before assignment too. These are lifetime/accounting changes, not new overlay
lookup semantics. Memory cost is status quo: every context already carries a private stdlib copy;
the process adds one shared, read-only image `Manifold`.

### 7.2 Deferred: the shared persistent base (v2 direction, U-X2)

One image shared by all contexts — stdlib cells as persistent refs, shadow-on-write — is the right
end-state (it is also what restores transclusion/attribution, §5.2) but not a small step: a stdlib
base and a document base are two `Store` spools sharing one index space — `CellRef` 42 means a cell
in each. A two-base arena needs ref-space partitioning or the federation `Space`/proxy machinery
(`arena_manifold.hpp:872–886`), which exists for cross-store actor references but has no transparent
read-through of whole subgraphs today. Prefixing stdlib ops into every document store is refused: op
indices are positional, so an updated prefix renumbers every document op and rewrites history — the
one thing OSMIC forbids.

### 7.3 Optimization kept in reserve: prefix hydration (U-X3)

`exportPrefix`/`hydratePrefix` (`arena_manifold.cpp:2126–2194`; `or_parallel.cpp:210` calls
`exportPrefix`, `:349` calls `hydratePrefix`) could clone a template-arena stdlib into each context
as a memcpy — viable only when template and target dimension refs coincide, which depends on the
*base* (`vortex_core.cpp:41–49`). An optimization, not a substrate — and one the liveness gate
interacts with honestly: `hydratePrefix` seeds `outstandingMarks_ = 1` (`arena_manifold.cpp:2192`,
no corresponding `Mark`, but evictable through `evictOldestMark`, :2196–2203). P30 explicitly tracks
the seed's lifetime so the gate counts `liveMarks()` — real marks, excluding a still-present seed —
not the raw count (§5.2.5a/§5's invariant). Today's hydration users are `or_parallel`'s transient
`ShadowDeltaPrefix` worker arenas (`or_parallel.cpp:345–349` — per-job local arenas, not
`extraContexts_`, which share `baseManifold_` via `WorkerContext{baseManifold_}` and are never
hydrated, `vortex_host.cpp:107–115`); were a `VortexStdLib` ever grafted onto a hydrated arena the
floor-aware count keeps the gate working — and §7.3's mechanism stays in reserve precisely because
it targets arenas that never join the gated set.

______________________________________________________________________

## 8. Refused Alternatives

1. **Keep runtime C++ generation.** Keeps the stdlib outside the docuverse — uninspectable,
   unversioned, duplicated per context, permanently C++-bound — against the self-hosting direction
   and `GEMINI.md`'s Vortex-over-C++ rule.
1. **Ship a `PrefixSnapshot`/`encodeArenaPrefix` blob.** Refused in §3.2.
1. **Prefix stdlib ops into every document store.** Refused in §7.2: positional indices.
1. **A manifest format (JSON/YAML/bencode) describing modules.** A second encoding of structure the
   store already expresses natively; violates the cells-and-dimensions rule
   (`.agents/skills/vortex-design-critique/SKILL.md` §1, rule 2); cannot carry opcode graphs,
   contracts, or clauses without reinventing zzstructure.
1. **`importModuleFromStore` unchanged.** Measured broken: dead Int64 refs, missing orphan cells, no
   registry rank, no re-registration, dims copied into a disconnected space (§2.3).
1. **In-place shared-lineage updates in v1 (`--extend`).** Refused for now, deferred to U-X7:
   append-only growth keeps changed symbols shadowed behind `resolve()` first-match and can never
   express retraction — the update would silently not take effect (§6.2).
1. **`hydratePrefix` as the primary path.** Depends on template/target dimension identity (§7.3); an
   optimization.
1. **OpHandle-valued ref scalars everywhere.** A competing uniform fix for the ref class — but every
   `toInt`/`resolveCell`/`refArg` decode site (~40) would need a `handleTarget` arm, and `render()`
   returns label text for OpHandle cells. Marked Int64 keeps the values Int64 and leaves all decode
   sites untouched; relocation happens once at graft.

______________________________________________________________________

## 9. The Numbered Proposal Register (`P26`–`P34`)

Continuing the global register (latest assigned: `P25`, `vagent-self-hosting-migration.md` §6).

### `P26`: Relocatable refs — OpHandle wing slots + marked-Int64 ref scalars

- **Location**: `vortex_core.cpp:418–452` (`bindInput`, `bindOutput`), `wingOf` (`:383–399`);
  `vortex_stdlib.cpp` `buildGoalModule` `opref`/`contCell` (~:3497–3510) and `lit(dim)` sites
  (~:4056–4059); `vortex_vm.cpp` decoders unchanged.
- **Problem**: every scalar whose value names a cell/dim is epoch-local and, worse, names cells the
  promote edge-walk cannot reach — wing slots, ~40 control-flow operand constants, param cells,
  internal ops. Measured `0x800004BF` dead refs; orphan targets absent from the export entirely
  (§2.3.1–2).
- **Change**: (a) wing slots mint `ValueKind::OpHandle` + `+d.mesh.relocation → <fresh "cell">` —
  **one marker cell per marked cell** (the `refCell` convention, `vagent/records.hpp:63–72`): a
  shared marker is impossible — `link()`'s 2-degree splice evicts the *previous* slot's forward edge
  (`manifold.cpp:679–688`), leaving only the last mark standing. Promote follows `handleTarget` into
  the deferred set and translates natively (`arena_manifold.cpp:1772–1777,1812–1824,1982–1987`);
  `wingOf` gains an `OpHandle → handleTarget` arm *with* the same `arena_.contains` check the Int64
  arm performs (`handleTarget` returns bits unchecked, `arena_manifold.cpp:1090–1098`). (b) every
  ref-valued scalar keeps Int64 bits and gains `+d.mesh.relocation → <fresh "cell">`; record members
  (`"ops"`, `"in"`, `"out"` chains) carry Int64 refs **positionally** — membership is the ref
  declaration (the `seedProgramImage` precedent), no marker needed. The generator harvests every
  marked referent **and every record member** into `extraRoots`, then **restates** each ref Int64 to
  its referent's store-space position via the promote twin map (`lineage.cpp:189–249,398–411`) —
  promote writes Int64 bits verbatim. The graft detects marked cells by **edge-existence** ("has an
  outgoing `d.mesh.relocation` edge" identifies ref-bearing) then dispatches on `ValueKind` — Int64
  → `asInt64`→`map[]`, OpHandle → `handleTarget`→`map[]` — never `isCellRelocationHandle`, which is
  OpHandle-gated and detects nothing *among the Int64-marked cells*
  (`foreign_op_handle.cpp:100,111`) — and translates marked values and record members through
  `map[]` (§5.2.3); a map miss is always a named refusal. (c) `d.mesh.relocation` minted via
  `findOrMintDimension` in bootstrap — the dim joins the registry rank and exports under P27;
  `mesh_evaluator.cpp:743`'s bare `makeCell("d.mesh.relocation")` migrates to `findOrMintDimension`
  (non-registry twin dims not tolerated). DRY commit: `refCell` (`records.hpp:63–72`) is
  byte-identical to the write half needed here, and
  `markedRelocationSlots`/`harvestRelocationTargets` are anonymous-namespace in
  `lineage.cpp:207–249` — move all three to **`common/xanadu/relocation_marks.hpp`** beside
  `foreign_op_handle.hpp` (whose `cellRelocationDimension`/`cellRelocationMarker` constants,
  `:24–25`, are already the shared home); only the restate step genuinely differs (vagent restates
  lineage refs; this design restates stdlib refs). **One clause is forced by the scratch arena:**
  every dim lookup in that code resolves through `arena.dimensions()` — the *filed name table*
  (`base_ ∪ arenaDims_`, `arena_manifold.cpp:34–46`) — but `findOrMintDimension` files dims
  rank-only and a null-base scratch `WorkerContext` never calls `ensureDimension`, so
  `arena.dimensions()` is empty there and a verbatim move returns nothing (it works in vagent only
  because `dimensionVocabulary`+`fileNameSet`, `lineage.cpp:426–461`, pre-file the vocabulary into a
  *base*). The moved helpers resolve the mark `DimRef` via `VortexCore::findDimension` (the `d.dims`
  rank) — or detect marks by `dimensionsOf`-text match like `isCellRelocationHandle` — never
  `arena.dimensions()`; `arena.dimensions()` is not a legal mark lookup in the generator scratch.
  (The same latent blindness already exists in-tree — `executor.cpp:564,634,2924` probe
  `arena.dimensionNamed("d.mesh.relocation")` and miss rank-filed dims; this fix un-breaks them.)
  The rule extends to `d.params` and every graft-minted dim: they are rank-filed too
  (`vortex_core.cpp:92–102` files on the `d.dims` rank only, never `arenaDims_`), so on a base-less
  grafted arena a `dimensionNamed("d.params")` reader misses — all dim readers go through
  `core.findDimension`/`dimensionsOf`-text, the same rule for scratch/graft-sensitive readers;
  existing based-arena readers with a filed vocabulary are not prohibited. One boundary line: the
  folded image's `findOpHandle`/ `versionAnnotation`/`aliases` slot-scans
  (`manifold.cpp:1332–1437,1452–1468`) conflate OpHandle wing slots with real handles.
  `ArenaManifold` has no equivalent slot-scanning APIs, so the risk reappears only after later
  promotion into a document and folding. Marker edges replay deliberately (§5.2) and `findOpHandle`
  scans `slots` for `valueKind==OpHandle && valueBits==targetOp` (`manifold.cpp:1332–1342`,
  `handleTarget` returns `valueBits` unchecked, `arena_manifold.cpp:1090–1098`). The design accepts
  this folded-scan ambiguity; the graft's registration path uses `handleTarget`, not those scans. No
  arena-scan implementation or downstream attribution correction is claimed.
- **Cost**: one new dimension + OpHandle+marker per wing slot + a per-ref marker (~2 ops each) on
  ~40 ref scalars; a harvest pass in the generator; **side effect disclosed**: every `bindInput`
  caller's promotes enlarge (mesh_evaluator artifact outputs `mesh_evaluator.cpp:702`, vagent
  meta/executor) — correct refs, bigger exports.
- **Fallback**: clone-cell wings (`masterOf`) — refused, `OpHandle` is the persisted-ref kind
  (`ops.hpp:159`); OpHandle scalars — refused in §8.8 (decode-site churn).

### `P27`: Whole-space export — include the registry rank

- **Location**: `arena_manifold.cpp:1778–1782` (`promote` walk), `vortex_stdlib.cpp`
  `exportStandardLibraryToStore` (~:5674).
- **Problem**: `promote` skips `d.dims`/`d.lineage` edges → no registry rank in the image;
  `dimensionNamed` fails on the fold (measured §2.3.3). Also: export is only defined for
  `arena().base() == nullptr` — base-bound exports write foreign `linkId`s (§2.3.6).
- **Change**: `PromotionBudget` gains `wholeSpace`, defined precisely: **lifts the `d.dims`
  edge-skip only** (`d.lineage` stays skipped — not stdlib content) — otherwise the registry rank's
  own `+d.dims` edges and the unused dim cells reachable only through them never enter the image.
  `exportStandardLibraryToStore` promotes `home` under `extraRoots{home, dims_.dims}` — ordered
  first two positions preserve the home/dimsDimension first-two-births fold fiat
  (`manifold.cpp:447–454`) — plus the harvested marked/member referents, under that flag; both
  export entry points assert the base-null precondition.
- **Cost**: +1 bool; ~21-cell registry rank + links added to the export.
- **Fallback**: `sliceGenesis`+`makeDimension` before promote — two sources of dim identity;
  rejected.

### `P28`: Ops-manifest record and registration

- **Location**: `vortex_vm.cpp:121–155` (`registerProgramImage`), the generator, graft §5.2.4.
- **Problem**: `opcodeMap_` cannot be recovered by value scan; and verbatim Int64 members graft into
  the wrong index space — refusal in a virgin arena, **silent misregistration of document cells** in
  a bound one (`registerProgramOps` guards only `contains` + `isRegistrableOpcode`,
  `vortex_vm.hpp:155–157`, called by `registerProgramOps`, `vortex_vm.cpp:97–119`; registration is
  not atomic).
- **Change**: **one** `"ops"` enumeration (`opsHead` + `+d.step` member chain) mints at bootstrap
  start; each `build*Module` tail-appends its op members. It is filed as a keyed record —
  minted-and-filed at bootstrap *end* by the §3.1 `keyRecord` helper (the record helper *mints* the
  key cell; it cannot file a pre-minted one) — on a **private `+d.vars` rank of the `stdlib.image`
  holder** — the unranked `+d.values` target of the on-`home +d.vars` key record, alongside the
  manifest field records — the vagent two-cell holder convention verbatim (`fragment.cpp:438–458`,
  `decls.cpp:2245–2257`). The private rank is forced by flat-rank reality: a member of
  `home +d.vars` cannot own a `+d.vars` rank (its `+d.vars` edge *is* its rank position), so
  `registerProgramImage`'s `rankAfter(holder, vars)` first-match on an on-rank cell can only ever
  scan the shared tail — the holder's private rank is what makes the scoping *structural*: foreign
  `"ops"`-texted cells are unreachable, no posward-ordering invariant applies, and an ops-less
  holder yields the genuine `unexpected` (prechecked, §5.2.4). `rebindInstance` (and non-grafting
  second instances) enumerate **all** `"stdlib.image"` key records on `home +d.vars` →
  `linked(key, values, POS)` → ops-presence precheck → `registerProgramImage(holder)` — uniform
  across modes and graft counts. Members are positional Int64 refs (P26), translated by walking the
  chain *inside* the graft mark (post-`discard` registration is then infallible). Residual: a
  foreign `"stdlib.image"` record is enumerated — member decode gates it (`isRegistrableOpcode`).
- **Cost**: one record + ~1,200 member cells; one small decode arm.
- **Fallback**: image-side enumeration read + `registerProgramOps` on the translated span — accepted
  as equivalent; the holder path is chosen for API verbatimness (`registerProgramImage` unchanged)
  and a uniform enumerate-`stdlib.image` mechanism for non-grafting instances.

### `P29`: Routine signatures as manifold records

- **Location**: `routineBindings_` (`vortex_stdlib.hpp:582`), `call()` (~:2504 fast path, ~:2524
  fallback), generator + loader.
- **Problem**: declared signatures are C++-only state; multi-op routines (`clamp`) bind wrong
  through the fallback (§2.2).
- **Change**: the builders emit `sym +d.params → sigCell` **at the end of each `build*Module`**
  (`d.params` minted via `findOrMintDimension` — a new dim, justified: no existing dim carries
  signature arity; note `zigzag/sql_lens.cpp:703` already files a `"d.params"` qattr — under a
  lens-document base `findOrMintDimension` adopts *that same dim cell*: structurally harmless since
  edges are per-cell, but a literal share, not a separate namespace); `sigCell` is an *unscoped*
  **unranked holder** — the same two-cell shape as the manifest holder — whose private `+d.vars`
  rank carries keyed `"in"`/`"out"` records, each record's `+d.values` head a `+d.step` chain of
  positional-Int64 member cells naming param cells (harvested + restated per P26/P27, translated by
  §5.2.3). Emitted in **every** arena — `Cpp` mode included — so arenas are self-describing and
  `rebindInstance` is uniform; the goal trio's exports carry `{in:{},out:{}}` records like `fail`
  for `routineInputs`/ `routineOutputs`/`call()`-arity symmetry (`vortex_stdlib.cpp:4293–4307` —
  `vortex_param` enumerates wings, not records, `:709–781`). Loader rebuilds `routineBindings_`
  keyed by entry op (the symbol's `+d.values` target); `routineBindings_` remains a derived index —
  the authoritative copy is manifold data.
- **Cost**: ~1 sig cell + 2 record members + ~1 member cell per param (~2,600 cells total); one new
  dimension.
- **Fallback**: derive signatures by walking `+d.spin` — `Call`/`RunCall` frames make the boundary
  ambiguous; rejected.

### `P30`: `ensureStdlib()` split + `graftImage()`

Scope: image initialization and its allocator/VM/host lifetime boundaries, including dependent
engine caches and refusal delivery. This does not redesign general runtime registration,
persistence, or user-session state.

- **Location**: `vortex_stdlib.cpp:1484–1507` (ctor/`bootstrap`), `vortex_stdlib.cpp`
  `importModuleFromStore` (~:5694, superseded), `vortex_stdlib.{hpp,cpp}`
  (`ensureStdlib`/`ensureCells`/`rebindInstance`/`rebindCaches`/`keyRecord`/ `buildModule` decls),
  `vortex_vm.{hpp,cpp}` (swap clear-list + `rebindInstance`-facing API + owner-keyed guard
  registration/destruction), `vortex_host.{hpp,cpp}:113,118,149–158` (including new aggregate
  `liveMarks()`), `vortex_core.{hpp,cpp}` (genesis resync and owner-keyed write predicates),
  `zigzag/arena_manifold.{hpp,cpp}` (`liveMarks()` and synthetic hydration-floor flag maintained by
  hydrate/evict/discard/release/moves and the raw-no-marks hydration precondition),
  `vortex/or_parallel.cpp:349` (consume hydration's new `ArenaResult`), `vpl/vpl_engine.{hpp,cpp}`,
  `vql/vql_engine.{hpp,cpp}`, `vql/compiler.{hpp,cpp}` (new `onArenaSwapped()` cache-reset methods),
  `multi_store.{hpp,cpp}` (in-place coordinator cache/mount reset; no reseating of references),
  `worker_context.hpp:59–62`, `zigzag_visualizer.{hpp,cpp}` (pre-mutation checks, direct attachment,
  ensure arm, reload/adopt wrappers, command/compile result handling, feedback writing/visibility,
  new `lastSurfacedImageRefusal_` latch and post-construction status reader; current relevant ranges
  :215–428,:2844–2875, :3339–3706), `apps/xuzz/xuzz_app.cpp` (boot/adoption sinks, `bindZigzag` and
  all callers, pane/focus/bookkeeping ordering), `tests/zigzag/test_visualizer.cpp`,
  `tests/zigzag/paired_edit_heads_test.cpp`, `tests/xuzz/vortex_host_test.cpp`,
  `tests/xuzz/vortex_stdlib_test.cpp`, `tests/xuzz/vortex_core_test.cpp`,
  `tests/xuzz/arena_manifold_test.cpp`, and existing VM tests (liveness accounting and guard
  lifetimes).
- **Problem**: no image-aware bootstrap; the ctor+host+`ownedStdlib_` triple-call is not idempotent;
  and per-instance caches/registration die if a second `VortexStdLib` sees `stdlibPresent → done`.
- **Change**: `ensureStdlib() = ensureCells() + rebindInstance()` — `ensureCells` is
  presence-checked (`resolve("std:math/abs")`) and image-first per §5's flow and §5.4's matrix;
  `graftImage` runs inside a taken `mark()` (`discard` on success, `release` on refusal — atomic
  graft); `rebindInstance` always rebuilds `routineBindings_` (`clear()` first — records exist in
  both modes since builders emit them), rebinds all cached refs, registers ops by enumerating
  `stdlib.image` holders via `linked(key, values, POS)` (§5.2 step 4), and on arena swap clears
  `opcodeMap_`, `cursorPC_`, `memoizedOps_`, `choiceStack_`, `oraclePath_`, `oracleCursor_`,
  `cutCallback_`, `currentDeweyPath_`, `totalCycles_` (stale ints/callbacks alias fresh cells —
  §5.2.5) — each instance's `rebindInstance` clears its *own* `vm_`'s tables, and `initHostServices`
  firing `ensureStdlib` on `primary_.stdlib` + `vplEngine_.stdlib()` covers both VMs. The liveness
  gate is `VortexHost::liveMarks() != 0` — a named `ChoiceStackLive` refusal — checked before
  mutation at `bindXanaduStore`/`adoptDocument` entry and before direct `attachVortexHost`
  assignment, inside `ensureVortexHost`'s **replace arm** for command callers (§5.2.5a), and at
  `bindManifold` entry pre-move-assign (the in-place primitive, §5.2.5b); arena marks aggregated
  over `primary_` + `extraContexts_` cover every surviving VM's stack, worker-context cursors, and
  stray `mark()` holders. Each arena count excludes only its explicitly tracked, still-present
  hydration seed. Image cell creation/registration through ensure/graft/rebind run only with
  `liveMarks() == 0` — an environmental refusal in *every* mode (`MarksOutstanding`), checked at
  entry — no C++ fallback, since `bootstrap()` under a live mark is the identical corruption.
  **Long-lived-instance invariant:** every `VortexStdLib` re-runs `ensureStdlib` when its arena is
  replaced — `initHostServices` invokes it on `primary_.stdlib` *and* `vplEngine_.stdlib()`
  (`ownedStdlib_` is the one long-lived non-primary instance, `vpl_engine.cpp:77–86`); the identity
  token is `arena().base()` + `core().home()` (the arena object survives move-assign in place — its
  address can never be the token; the token covers cross-base moves, same-base swaps ride the
  re-fire + sentinel, §5.4), and the `VortexCore` genesis resync re-derives `home_`/`dims_` against
  the new arena (ctor-cached only today, `vortex_core.cpp:16,31–74`) *before* `ensureStdlib`
  re-fires. The honest driver set: document rebind **replaces the host** (`bindXanaduStore` →
  `ensureVortexHost` → `make_shared<VortexHost>` — the ctor path covers it); `bindManifold` has zero
  in-tree callers today — it is the explicit/future in-place swap the resync machinery exists for
  (the multithreaded-host direction). A retained external `shared_ptr` copy still has a
  non-owning-base lifetime restriction after a successful replacement; an installed host dangling
  after refusal is prohibited; `baseManifold_` itself is repointed by `bindManifold` at :154, and
  `extraContexts_` workers bound to an old base share the same pre-existing exposure outside the
  re-fire. Arena-keyed caches outside `VortexCore`/`VortexVM`/`VortexStdLib` are caller-managed
  (§5.2.5 boundary — `vplEngine_`/`vqlEngine_`/`vqlCompiler_` envs, custom action registries).
  `makeWorkerContext`'s explicit `stdlib.bootstrap()` (`vortex_host.cpp:113`) is **removed** — the
  ctor-time call already covers it, and a bare `bootstrap()` would double-mint over a ctor graft.
  `importModuleFromStore` reimplemented over the graft. The `WorkerContext` ctor calls
  `ensureStdlib` as today (the policy is process-level, §5.4 — no ctor signature change needed).
- **Cost**: ~150 lines of graft + the split + ~60 lines rebind, plus API/caller handling, one
  hydration-seed flag per arena and O(1) mark accounting, an O(contexts) host liveness check at
  replacement, and one predicate per live VM with O(live VMs) checks per guarded write (§5.2.5b). No
  per-frame polling; entries disappear on VM destruction. The additional C++ is required for
  allocator/callback lifetimes, which bytecode cannot own. Removes duplicated bootstraps.
- **Fallback**: `Cpp` retains builder output/parity, not unsafe historical lifetime behavior. If the
  lifetime/accounting changes are refused, safe in-place swaps and command-time host replacement
  with suspended flows cannot be promised.

### `P31`: Image location, headless resolution, semantic validation

- **Location**: `VortexStdLib` (policy + loader), `include/gleditor/paths.hpp` (inline headless
  chain), the xuzz app seam — **above `xuzz_app.cpp:1588`**, before
  `make_shared<ZigzagVisualizer>("")`, whose ctor builds the *first* context
  (`populateFallbackStructure`→`adoptDocument`→`ensureVortexHost`,
  `zigzag_visualizer.cpp:74,:215–308,:2857–2866`) — which initializes the dir setter from
  `assetPath("vortex-stdlib")` *and* reads `vortex.stdlib_source` + calls the mode setter (the key
  is deliberately **not** read by `VortexHostConfig::fromStore`/`loadConfigFromStore`,
  `vortex_host.cpp:37–98,169–172` — `bindStore` feeds those document stores, and the first
  `ensureCells` precedes every config load anyway); `settings::` constants (`system_docs.hpp:135`,
  `kVortex*` `:261–266`) + `SettingSpec` registration (`system_docs.cpp:818–823`), packaging
  (`build-msys2.sh:52–54` staging + `smoke-test.sh:48–55` check), Makefile install rules
  (`Makefile:1842–1848`).
- **Problem**: the image needs a distribution path resolvable without SDL (engine binaries link
  `$(XUDU_LIBS)` — `src/paths.cpp` pulls `SDL_GetBasePath`), plus the §5.4 refusal discipline and
  the missing-permascroll detector.
- **Change**: §5.1's lookup chain (explicit setter > `$GLEDITOR_STDLIB_DIR` taken-as-given
  > `$GLEDITOR_ASSET_DIR` > `GLEDITOR_DATADIR` > `./assets`), headless-inline; mode-only store key
  > `vortex.stdlib_source` (`SettingSpec`-registered, read **only at the Settings call site**
  > pre-context — never through `fromStore`, which document stores also drive — and no `image_dir`
  > key: the fold precedes config load, and store-content steering the loader is refused on
  > principle); `stdlib.image` validation incl. `expectedScrollBytes` + the span-end fallback and
  > dir precheck; mutex-guarded `shared_ptr<State>` folded-image state carrying
  > `optional<expected<Triple, Refusal>>` (the latched refusal has a home; `ensureCells` takes a
  > shared snapshot so a test reset can't orphan an in-flight graft) +
  > `resetStdlibImageForTesting()` swapping both base and local states; `make install` rules for
  > `assets/vortex-stdlib/` + msys2 staging + smoke-test check. The image is a **checked-in
  > artifact** (the `tests/samples` precedent) — `install` copies it; no build-time dependency.
- **Cost**: ~80 lines + install/packaging rules.
- **Fallback**: `assetPath()` at the app seam only — insufficient alone (headless tools); a
  `vortex.stdlib_image_dir` store key — refused: dead-on-arrival in every shipped flow (fold
  precedes `loadConfigFromStore`) and a store-content→loader trust step.

### `P32`: The generator tool

- **Location**: `tools/vortex_stdlib_gen.cpp` (new), `vortex_stdlib.{hpp,cpp}` (the
  suppressed-construction arm + `buildModule`/`beginImageOps`/`endImageManifest` factored skeleton,
  below), Makefile `stdlib-image` target.
- **Problem**: nothing binds a real permascroll, pins identity, writes manifests, or validates
  output — and `--module` as specified has no way to mint a module: `build*Module` are private
  `VortexStdLib` members (`vortex_stdlib.hpp:527,547–567`), the sole ctor unconditionally
  `bootstrap()`s (:1484–1487), and the `stdlib.image`/`"ops"` records live inside `bootstrap()`'s
  tail (:1507–1553) — so neither "full `Cpp` scratch" (base floods) nor "no `VortexStdLib`" (no
  modules, no manifest) can produce the §6.4 artifact.
- **Change**: §4's pipeline — including forcing `StdlibPolicy{mode=Cpp, localImage=false}` via the
  explicit setter before the scratch `WorkerContext` exists (otherwise the ctor-time graft seeds the
  shipped image, and a `localImage` fold would bake the build host's §6.4 patch into the artifact —
  determinism cannot depend on the Makefile's XDG redirect); `--verify`; `--stdlib` (whole-space
  image, the shipped default — produced on a full `Cpp`-policy scratch) / `--module <name>`
  (whole-space image on a **bootstrap-suppressed `VortexStdLib`** — the enabling clause: a ctor tag
  / `Bootstrap::Manual` arm under the `Cpp` policy that skips `ensureStdlib` entirely
  (`status().source` is `nullopt`), plus a public `buildModule(std::string_view)` facade dispatching
  to the named private `build*Module(CellRef)` after `getOrCreateModule(name)`, plus the
  manifest-emission skeleton factored out of `bootstrap()` — `beginImageOps()` (opsHead + `+d.step`
  chain), `buildModule` per named module (tail-appends op members), `endImageManifest()` (holder +
  `keyRecord(holder,"ops", opsHead)` + field records); `bootstrap()`'s driver becomes those three
  calls over the 21 base modules, `--module` calls them over the named subset — so the local image
  carries modules *and* the manifest its own §5.1 `FormatRefused` requires; `--module` cannot mean
  "reachability-bounded per-module" — `promote` floods both directions and `exportLibrary`-class
  container-less exports stay `FormatRefused`) output selection. `--module` is repeatable; build the
  selected set in bootstrap order, require checked required-symbol lookups and a self-contained
  selected set, and fail non-zero on unknown modules/unresolved dependencies before output
  publication (§6.4). No automatic dependency closure is promised. `--out` accepts an absent/empty
  directory only (`OutputExists` otherwise); a single invocation emits one image, never an
  append/merge/overwrite. determinism pins (fixed `documentId`, fixed or stripped
  `deviceKeys`/`masterIdentity` — no `publication-state/` in the artifact); installed as a developer
  tool — `$(OBJDIR)/vortex-stdlib-gen` + `install -m 755` beside `Makefile:1798–1802` — so §6.4's
  production path is reachable on an installed tree; `--extend` is *not* in v1 (U-X7).
- **Cost**: ~200 lines of driver over existing APIs.
- **Fallback**: `:export-lib` — refused: it drives `exportLibrary` (per-module,
  `zigzag_visualizer.cpp:3821` → `vortex_host.cpp:505`) on a live document arena — wrong base
  precondition (§2.3.6) — and depends on a UI session's transient permascroll (§2.3.5).

### `P33`: Exported goal refs + `rebindCaches()`

- **Location**: `vortex_stdlib.cpp` `buildGoalModule` (~:3487–3489 mints, ~:4010 exports),
  `vortex_stdlib.hpp:585–607` cache members.
- **Problem**: `goalHalt_`/`goalSentinel_` are mint-time refs linked from nothing — unreachable by
  `resolve()`, invisible to the edge walk, dead after graft (`pushChoicePoint` throws
  `std::invalid_argument` on `!contains`, `vortex_vm.cpp:303–306`; `getCursorOpcode == haltOp` never
  matches → produces drain).
- **Change**: export `std:goal/halt` and `std:goal/sentinel` via `exportSymbol` (visible
  `symbolsInModule` addition — documented); `rebindCaches()` — the cached-ref half of
  `rebindInstance` (P30) — resolves every cached ref — `predEqual_`…`predBridgeEdge_`, `predVortex*`
  ×5, `goalHalt_`/`goalFail_`/`goalSentinel_` — by explicit symbol path, enumerated in code, so the
  isomorphism gate can assert each.
- **Cost**: 2 symbols + ~60 lines; fixes the latent double-bootstrap as part of P30's split.
- **Fallback**: manifest-record members — refused: symbols are the one mechanism that also fixes the
  user-visible "can't name the halt op" gap.

### `P34`: Read-only ops spool + read-only image open

- **Location**: `segmented_ops_spool.cpp:603–611` (`openActiveSegment`), `segmented_ops_spool.hpp`;
  `UserPermascroll` (`user_permascroll.hpp:103–114` Config, `user_permascroll.cpp` ctor
  `permascrollState`/`create_directories`/write-back path); primedia `O_RDONLY` pattern
  `segmented_primedia_spool.cpp:220–241,352–353`, `hpp:167`; `Store::load`/`save`
  (`store.cpp:4406–4412`).
- **Problem**: `Store::load` cannot open `ops.nodes` on a root-owned install — the spool requires
  `O_RDWR` and throws on EACCES; the shipped asset is dead to non-root users and silently mutable
  whenever writable. Symmetrically, `UserPermascroll::Config` has **no** `readOnly` today: the ctor
  touches `permascrollState` (LMDB) and `create_directories` + write-back *before* the primedia
  spool's EACCES fallback could ever matter — a read-only asset dir fails during initialization, not
  open.
- **Change**: (a) `openActiveSegment` retries `O_RDONLY` on `errno ∈ {EACCES, EROFS, EPERM}` (a
  transient `O_RDWR` failure must not silently downgrade) and records `readOnly`; append/flush/seal
  paths **refuse** `readOnly` via `std::expected`/bool returns — not exceptions (`~UserPermascroll`
  calls `spool_.flush()` unconditionally, `user_permascroll.cpp:401–403` — a throw terminates; note
  the primedia spool's existing readOnly is a *silent* no-write at `flush()`,
  `segmented_primedia_spool.cpp:352` — the named refuse is new behavior for both spools,
  intentionally louder). (b) `UserPermascroll::Config` **gains** `readOnly` (new field, wired to
  `spool_`): under it the ctor skips `permascrollState`, `create_directories`, and write-back
  entirely — no LMDB, no key mint, no asset-dir mutation — the primedia spool honors a *requested*
  `O_RDONLY` (not just EACCES-detected), and the remaining mutators (`clear()` `:456`,
  `sealIncremental`/write-back `:528–530`, `registerSubscroll`/`ingestSubscroll` `:561–619` and
  their file writes at `:584–586,:612–616`) refuse or are documented unreachable on the image path.
  (c) `Store::save`/`writeSegmentFile` (`store.cpp:4093–4125`, impl `segmented_ops_spool.cpp:726`)
  and `saveOsmicText` (`store.cpp:4320–4340`) refuse early with a named error under a read-only
  spool; an empty `ops.nodes` under `O_RDONLY` is a named loader refusal, not a bare "cannot read".
  (d) The §5.1 open path requests `readOnly` always — the request channel is `Config.readOnly` for
  the permascroll and an explicit open flag on the ops spool (`Store::load` has no options param
  today, `store.hpp:1340` — the added flag defaults to writable for existing callers; only image
  loads explicitly request read-only).
- **Cost**: ~60–70 lines across the two spools + `UserPermascroll` ctor branch.
- **Fallback**: ship `ops.export` text instead — slower parse, and a later `save()` still tries
  `O_CREAT` in the asset dir; refused.

______________________________________________________________________

## 10. Open Questions

| id   | question                                                                                     | default / boundary                                                                                                                     |
| :--- | :------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------- |
| U-X1 | Local-image cell-level patching: repoint-on-name-match vs v2 shared-base shadowing?          | deferred; name-merge covers new modules/symbols only (§6.4)                                                                            |
| U-X2 | Shared persistent stdlib base — proxy read-through vs ref-space partitioning?                | deferred to a dedicated v2 study (§7.2)                                                                                                |
| U-X3 | `hydratePrefix` seeding: is template/target dimension identity provable under same-base?     | optimization only; v1 grafts per arena (§7.3)                                                                                          |
| U-X4 | Read-only `Store`/`UserPermascroll` surface for *all* shipped assets, not just stdlib?       | P34 lands the ops-spool + permascroll halves; widening to every shipped asset is follow-on (U-X4 → P34's pattern, not a new mechanism) |
| U-X5 | Shipped images signed/`btpk:`-published for swarm distribution?                              | unsigned file asset — the preprint; authorship is the *goal*, revisited with swarm distribution                                        |
| U-X6 | Do `xuzz`/`vpl`/`vprolog`/`vagent`/`vicon` share one image or per-app subsets?               | one image (§5.4 `imageDir` already resolves per-process); per-app subsets are a packaging refinement                                   |
| U-X7 | Shared-lineage releases + delta transport (`--extend`, `adoptOpRecords` deltas, retraction)? | deferred — needs the differ + symbol repoint/shadow spec (§6.2)                                                                        |
| U-X8 | Interactive local-image authoring (`:export-lib-image` whole-space command in xuzz)?         | deferred; v1 local images come from the installed `vortex-stdlib-gen` (§6.4)                                                           |

______________________________________________________________________

## 11. Validation & Test Plan

1. **Isomorphism gate** (`tests/xuzz/vortex_stdlib_image_test.cpp`): build the arena image by C++,
   export, graft into a fresh context, then compare `modules()`, `symbolsInModule(m)`,
   `vortex_param`/`vortex_instruction`/`vortex_contract` tables, and `call()` outputs on a fixed
   argument battery — **modulo the graft bijection**: text, rank shape, and scalar values must
   match; CellRef-carrying outputs (e.g. `vortex_param`'s `int64(ref)` results,
   `vortex_stdlib.cpp:733–734`) compare *mapped*, not equal. Assert each P33 rebind target.
1. **Determinism gate**: `vortex-stdlib-gen` twice → byte-identical `ops.nodes` *and* `store.tables`
   (pinned `documentId`) and identical `xudu-dump --section=ops` output; no `publication-state/` in
   the artifact.
1. **Skew gates**: forged `imageEpoch` → named refusal (or fallback+warn per mode); format-mangled
   `ops.nodes` → `OpsSegmentUnreadable`; missing `permascroll/` → refusal naming the packaging
   cause; truncated scroll (`< expectedScrollBytes`) → refusal naming the scroll;
   `perma->size() > expectedScrollBytes` → `ScrollTruncated` too (grown is a deviation, §5.1); a
   well-formed store carrying no `stdlib.image` record → `FormatRefused`; severe truncation eating
   the early names → `ScrollTruncated` via the span-end fallback, not a generic missing-manifest
   error — no silent empty stdlib.
1. **Multi-context gate**: two `WorkerContext`s + one `vprolog::Compiler`-style second
   `VortexStdLib` over the same core — graft once, `call()`/`solve()` independent and correct in
   both instances (the per-instance rebind test, §5's load-bearing split).
1. **Local-image gate**: a local image (produced by `vortex-stdlib-gen --module`, §6.4's named
   production path — the dev-only scratch) adding a symbol to `std:math` grafts — the new symbol
   resolves, the pre-existing rank is intact (no severed head), **each image's own `stdlib.image`
   holder registers its `"ops"`** (base and local both land in `opcodeMap_`), a same-text local
   symbol resolves to the *base* entry (shadowed, §6.4), a same-text *module* name-merge leaves the
   rank unsevered (the pre-existing-member edge class, §5.2.3 — exercised via `rankContains` skip).
1. **Isolation gate**: a foreign `"ops"`-texted record minted on `home +d.vars` must not register —
   the private-rank holder makes it structurally unreachable from the `stdlib.image` walk (§5.2 step
   4); its position on the shared rank is irrelevant by construction.
1. **Rollback gate**: a mid-graft refusal (forged map miss) leaves the arena bit-identical to
   pre-graft *over the mark-covered vectors* — `release()` undid every minted cell and appended
   edge, **and** the untrailed side-maps are unchanged (`arenaDims_`/
   `boundDimensions_`/`storesRankTail_` — asserted explicitly, since `release()` does not restore
   them, §5.2); `ImageThenCpp` then bootstraps cleanly over the pristine arena, not over debris.
1. **Swap gate**: two drivers, all gated paths — (a) document replacement refuses `ChoiceStackLive`
   **before any mutation** in `bindXanaduStore`, `adoptDocument`, their wrappers/reload paths, and
   direct `attachVortexHost`. Assert `EXPECT_FALSE(result.has_value())` on these expected-returning
   mutators, pointer identity of host/engine/store/bound store, unchanged focus/minted maps,
   unchanged pane state, and untouched `bindZigzag` bookkeeping
   (`zigzagStoreIndex`/`sliceHead()`/map/link context). With the command bar initially closed,
   refusal must open it and draw cause plus recovery. Separately, a suspended produce cursor →
   `executeLogicQuery` refuses **replacement** at the ensure arm and skips execution; its existing
   empty-vector result is not expected. Assert visible refusal feedback, no transient compiler
   construction or command writes, and unchanged suspended flow choice points. Aggregate
   `extraContexts_` marks too (a worker-context cursor also refuses); (b) `bindManifold` onto a
   document arena — `ownedStdlib_` and a second `VortexStdLib` rebind correctly (`call()`/`solve()`
   hit the new arena's cells — which requires the `VortexCore` genesis resync, §5.2.5: `resolve()`
   itself consumes `home_`/`dims_`); `cursorPC_`/`memoizedOps_`/`opcodeMap_`/`choiceStack_`/
   `oraclePath_`/`oracleCursor_`/ `cutCallback_`/`currentDeweyPath_`/`totalCycles_` hold no stale
   entries; a live mark at swap refuses `ChoiceStackLive` **at `bindManifold` entry** — the swap
   aborts and the old arena is retained, not half-replaced. Engine-cache boundary asserted:
   `vplEngine_.defaultDim()` post-swap resolves the new arena's `d.1`, and VQL `env_` lookups *miss*
   rather than alias (caller-managed, §5.2.5). Also assert the owned VQL coordinator's home anchor
   and dimension refs use the new core, old named-store lookup misses after mount reset, and clone
   dereference uses the new clone dimension. The orphaned-base-dimension limitation remains the
   acceptance boundary named in §5.2.5b; this migration does not repair it.
1. **Hydration accounting gate**: hydrate an arena, take a real mark, then evict the oldest
   synthetic floor. `liveMarks()` must remain one, not underflow or become zero. Release/discard
   real marks and assert zero. Hydration into an arena with any raw outstanding mark refuses before
   changing cells, floors, or counts. Also cover seed-only eviction, ordinary nesting, and move
   assignment into a mark-free destination; do not inspect a moved-from arena until assigning a
   fresh arena. A stale-Mark release/discard against the sole seed triggers its specified debug
   assertion before mutation.
1. **Status/notification gate**: a worker-only mark yields a nonzero aggregate replacement error
   while primary `stdlibStatus().arenaLiveMarks` is zero and its healthy loader `lastRefusal` is
   unchanged. A fresh visualizer under a missing-image refusal opens the command bar after host
   construction; identical per-command rebuilds do not reopen it. Recovery clears the visualizer
   latch. A later distinct refusal is surfaced. Direct `compileVQL` with a primary arena mark
   refuses before minting/registering any op, including when no ensure runs; a worker-only mark does
   not prevent primary compilation.
1. **Guard lifetime gate**: two VMs over one core register disjoint opcode cells; writes to either
   are refused. Destroy either VM first and assert the surviving VM's cells remain protected. A
   third transient compiler's creation/destruction, repeated guard installation, and arena-resync
   map rebuild must neither remove surviving entries nor leave callbacks into destroyed VMs.
1. **Cpp-parity gate**: `GLEDITOR_STDLIB_SOURCE=cpp` mode emits the same `d.params`/`"ops"` records
   the image carries (builders emit them) — `rebindInstance` rebuilds identical `routineBindings_`
   from a C++-built arena; absent `"ops"` records remain a no-op.
1. **End-to-end**: `xuzz` boots on the image with `GLEDITOR_STDLIB_SOURCE=image` under the
   Makefile's `SDL_VIDEODRIVER=offscreen` windowed path — **`--headless` does not count**:
   `opts.headless` saves the session and `return 0`s at `xuzz_app.cpp:1055–1064`, before the
   renderer (:1067) and `ZigzagVisualizer` (:1588) exist, so no `VortexHost`, no `ensureCells` fold,
   and no `bundle-stdlib` dispatch ever run — the gate boots windowed (or a scripted command path
   below :1588 is added and named), asserts `bundle-stdlib` shows the module rank, keymap actions
   dispatch, and `vagent findall`/`symbolsInModule` counts are single-bootstrap (the §6.5 change).
1. **Read-only gate**: image dir `chmod 0555` (simulating root-owned install) loads under `Image`
   mode (P34); a write attempt refuses.

______________________________________________________________________

## 12. Resource Accounting

- **Image size**: today's *incomplete* export measures 8,652 ops / 4,296 cells / 605 KiB `ops.nodes`
  \+ ~48 KiB permascroll. P26's reachability fix pulls in the orphan cells (params, ~40 opref'd
  internal ops, goal refs) plus per-ref marker cells+edges, the manifest (~1,200 op members),
  signature records (~2,600 cells), and the restate pass — ~3.8K marked/positional `SetValue`
  restates and ~4K marker/link ops — **expected ≈ 18–22K ops, ≈ 1.2–1.4 MiB**; re-measure after P26
  lands. Release updates ship a complete fresh image (no growth by appending; regeneration is the
  compaction, §6.1).
- **Startup cost**: one `Store::load` + one cold fold (linear in ops, milliseconds at this size) —
  once per process — plus one mark-scoped O(cells) graft per `WorkerContext`/bound arena (the mark
  adds only bookkeeping; rollback is a truncation), the same order as today's bootstrap mint —
  footnote: in xuzz today a `WorkerContext` is built *per command* (`ensureVortexHost` replaces the
  host on every `execute*` call, `zigzag_visualizer.cpp:2857–2866`), so the graft runs per query —
  the same cadence and order as the bootstrap it replaces; the *fold* is what is once-per-process.
- **Memory**: the shared image `Manifold` holds ~11–13K cells (base 4,296 + ~2,600 sig + ~1,200 ops
  members + ~1.5–3K marker cells + ~1.3K orphan/param referents — bottom of range ≈10.9K). Per
  arena: image mode grafts that same ~11–13K-cell copy; `Cpp` mode now mints the self-describing
  records too (~**5.3–6.8K** cells/arena of records+markers over today's live-arena mint — sig
  records ~2,600 + ops members ~1,200 + per-ref markers ~1.5–3K minted by `bindInput`/`bindOutput`
  in every mode — the cost of mode-uniform `rebindInstance`). Per-instance `rebindInstance` walks
  ~2,600 sig cells + ~1,200 ops members + ~30 symbol resolves — runs per transient
  `vprolog::Compiler` (per query in `solveLogicBatchParallel`), comparable to today's per-query
  bootstrap.
- **Build cost**: the generator runs in milliseconds; `stdlib-image` is opt-in, so default `make` is
  unchanged.
- **Latent ceilings** (pre-existing, unchanged by graft, listed for honesty): `call()` runs bodies
  under `vm_.run(cursor, 100)` (`vortex_stdlib.cpp` ~:2511,2529 — a routine over 100 steps returns
  `{}`), and `runPipeline` defaults `limit = 1000` (`vortex_vm.cpp:431`).
