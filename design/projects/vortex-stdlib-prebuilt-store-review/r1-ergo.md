# Round 1 critique: Programming Language Design & Ergonomics Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 4 BLOCKING, 7 MAJOR, 5 MINOR.

## BLOCKING

### 1. (§3.1 line 83, §5.2 line 218, P28) The "ops" enumeration is ref-space-incoherent across the graft

P28 promises replay "verbatim", but `registerProgramImage` interprets each member's Int64
as a `CellRef` *in the arena being registered* (`vortex_vm.cpp:121–155`). The grafted
enumeration contains store-space refs — they either name nothing (`contains` fails →
refusal) or name *unrelated arena cells* whose Int64 decodes a registrable kind — silently
registering the wrong cells as opcodes. The codebase's own precedent is vagent's
`seedProgramImage`, which rewrites enumeration positions through the promote twin map
(`apps/common/xanadu/vagent/lineage.cpp:398–411`).

**Fix:** (a) translate each member's Int64 through the graft map in step 3 (the
`seedProgramImage` precedent — no VM change); or (b) OpHandle-valued members +
`handleTarget` arm in `registerProgramImage`. Option (a) matches P26's twin-map story.

### 2. (§5.2 line 213, §6.4 line 289) Plain `link()` rewire severs pre-existing rank heads — vagent's own-module contract and local-image grafts both break

`ArenaManifold::link` *overwrites* the edge and clears the displaced neighbor's reciprocal
edge (`arena_manifold.cpp:1349–1377`); insert semantics live in `insertIntoRank`
(`:1500–1527`). Replaying `home +d.stdlib` or a merged module's `+d.vars` head edge onto a
populated arena detaches the entire pre-existing rank: vagent's `binds.ownModule` requires
the agent's own module to be *first* on `+d.stdlib` (`adapter.cpp:449–456`,
`executor.cpp:239–245`) — the graft orphans it and `ownModule` silently binds `std:memoize`;
§6.4's local image similarly orphans the base rank head and *replaces* the module's symbol
rank rather than appending.

**Fix:** distinguish internal edges (raw-replay) from rank-membership edges onto shared
anchors (`insertIntoRank`/`rankTail` append, matching `exportSymbol`/`getOrCreateModule`);
state which so the choice's observables (shadowing, order) are specified.

### 3. (§4.1 line 144, §5 line 195) `stdlibPresent → done` leaves every second `VortexStdLib` instance with dead per-instance state

`pred*` caches, `goalHalt_/goalFail_/goalSentinel_`, `routineBindings_`, and `opcodeMap_`
registration are per-`VortexStdLib`/per-VM state, not manifold state. Non-host consumers
build their own `VortexStdLib` over a shared `VortexCore`: `vprolog::Compiler`
(`vprolog/compiler.cpp:22`), `vicon::Compiler` (`vicon/compiler.cpp:65`), `VPLEngine`'s
`ownedStdlib_` (`vpl_engine.cpp:54–55,67–68,79–80,92–93`), `makeWorkerContext` extras. The
first grafts; each subsequent instance sees `stdlibPresent → done` with all caches `noCell`:
`solve()` pushes 17 `noCell` predicates (`vortex_stdlib.cpp:4584–4602`), `goalProduce`/
`goalResults` push `goalSentinel_ = noCell` (`:4078,4115,4140`), `#GOAL_*` dispatch reads
`goalHalt_` (`:2462,2467,2489`) — silently wrong results, no refusal.

**Fix:** split `ensureStdlib` into `ensureCells()` (presence-checked graft, once per core)
and `rebindInstance()` (always: rebuild `routineBindings_`, rebind caches, register ops
into *this* VM). Ctor calls both; `bindManifold` calls `ensureCells` + `rebindInstance`.

### 4. (§2.2 line 48, P33 line 379) `rebindCaches()` cannot re-resolve `goalHalt_`/`goalSentinel_` — no symbol path — and the goal trio is missing from the inventory

`goalHalt_`/`goalSentinel_` are minted (`vortex_stdlib.cpp:3355–3357`) and **never
exported** — only `goalFail_` (`:3876–3878`). `resolve()` walks `d.vars`/`d.values` only,
so they are unreachable by path and stay `noCell` on the graft path: `goalProduce`/
`goalBoundedFirst`/`goalResults` push `goalSentinel_` (`:4078,4115,4140`), dispatch reads
`goalHalt_` as default halt (`:2462,2489`) — every `every`/`while`/`produce` runs with a
dead sentinel.

**Fix:** export `halt`/`sentinel` as `std:goal` symbols (note the `symbolsInModule`
change), rebind by `#GOAL_*` label scan, or record them in `stdlib.image`. Add the goal
trio to §2.2's inventory.

## MAJOR

### 1. (P30, §4.1) `VortexHostConfig::stdlibSource`/`stdlibImageDir` cannot reach the decision point

`ensureStdlib` runs in `VortexStdLib`'s ctor inside `WorkerContext`'s init list — before
`VortexHost`'s body, `config_`, or `bindStore` exist. `VPLEngine`/`vprolog::Compiler`/
`vicon::Compiler` never see a `VortexHostConfig`. As specified, mode and custom dir are
unselectable for the first context and every non-host consumer.

**Fix:** `VortexStdLib` gets its own load-default (`assetPath` lookup + env override, or a
static setter the process installs early); `VortexHostConfig` overrides for later contexts.

### 2. (§5, §6.3) The refusal-vs-fallback matrix and the error channel are unspecified

(a) Under `ImageThenCpp`, does a *stale* or *newer* image refuse or fall back?
(b) Under `Image`, does a missing image refuse, and through what channel — ctor returns
`void` today; `throw`, `std::expected`, or `WorkerContext`-level error? (c) What do
`vquery`/`vpl`/`vprolog` users *see*?

**Fix:** write the mode×condition matrix, name the error type (`std::expected<_,
StdlibImageRefused>` naming both epochs), and name the surfacing point per host.

### 3. (§11, §9) Image bootstrap multiplicity unspecified → observable introspection counts change

Production runs `bootstrap()` 3× on the primary arena (ctor + `ownedStdlib_` ctor +
`initHostServices`), triplicating symbols on `+d.vars`; `resolve()` first-match still works
but `symbolsInModule`/`vortex_function`/`vortex_param`/`findall` enumerate all three. A
single-bootstrap image silently *de*-multiplicates solution counts.

**Fix:** state the image is exactly-one-bootstrap canonical; document the observable
count change as a fix, or specify symbol-dedup in validation.

### 4. (§5.3) `imageEpoch` count-equality misses the same-count reorder case → silent opcode misdecode

A reordered `OpcodeKind` enum with unchanged count passes; grafted Int64 kinds decode to
*different* kinds and cannot self-heal (`getOpcodeKind` is Int64-based,
`vortex_vm.cpp:190–200`).

**Fix:** `imageEpoch` = hash over the ordered kind→label table (`#NAME` labels are the
stable external name) or a canonical enum-map digest. Counts are not an epoch.

### 5. (§11) The isomorphism gate cannot literally be "identical" — `call()` outputs carry CellRef ints

`vortex_param`/`vortex_instruction` return `makeScalarCell(int64(ref))`
(`vortex_stdlib.cpp:733–734`) — values differ across arenas by construction. As written the
test is unfailable-by-construction and an implementer will weaken it ad hoc.

**Fix:** name the equivalence — compare text, rank shape, and scalar values modulo the
graft bijection; CellRef-carrying outputs compare mapped, not equal.

### 6. (P29) The signature record's encoding is underspecified and `d.params` collides with an existing name

The in/out split encoding is open (two ranks? count cell? pivot?), and `"d.params"` is
already a qattr name in `sql_lens.cpp:703` — a new global dimension name needs justification.

**Fix:** pin the record shape (e.g. `sym +d.params → inHead` + `sym +d.sig-outputs →
outHead`, or one rank + count cell) and justify the dimension or rename it.

### 7. (§5.2 line 209) The graft scope names only the "+d.stdlib reachable subgraph" — the manifest records on `home +d.vars` are never grafted

Step 2 clones the `+d.stdlib` subgraph; the `"ops"`/`"stdlib.image"` records live outside
it. A literal implementation never grafts the manifest → P28/P31 dead on arrival.

**Fix:** "the whole image fold: genesis dims, the `d.dims` registry rank, the `+d.stdlib`
module subgraph, and the manifest records on `home +d.vars`."

## MINOR

1. (§3.1) "16 named dims" — `initGenesis` mints 19 (`vortex_core.cpp:35–74`).
2. (P31) `assetDir()`'s installed-Unix steps need SDL (`src/paths.cpp:26–32`) — headless
   tools fall through to `GLEDITOR_DATADIR`/`./assets`; pin the headless lookup contract.
3. (§8) Module list order doesn't match bootstrap order (`std:memoize` first, `:1508`) —
   the image's `+d.stdlib` rank order is observable via `modules()`.
4. (§11.4) Missing-permascroll refusal misdiagnoses without a dir-presence precheck.
5. (§6.4) Clarify the name-merge key is full module text, prefix-agnostic (`sys:array`
   included — `importModuleFromStore`'s `std:` prefix filter at `:5434` would drop it).

## Verified OK

- `resolve()`/`call()`/`modules()` semantics survive the graft for clean arenas.
- `isRegistrableOpcode`/`isContentDecodableOpcode` cover all emitted kinds.
- The failure modes are in per-instance state and rank-head handling, not the resolve walk.

VERDICT: NOT CONVERGED
