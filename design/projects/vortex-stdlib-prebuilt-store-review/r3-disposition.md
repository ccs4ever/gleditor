# Round 3 disposition — vortex-stdlib-prebuilt-store

Reviewer tally: writer 0B/1M/7m, vortex 0B/4M/6m, codebase 0B/2M/5m, architect 0B/5M/11m,
ergo 0B/1M/7m, zigzag 0B/2M/5m. Zero converged; zero BLOCKING.

## Cluster R3-1 — Registration scoping (ALL SIX critics)

**Disposition: ACCEPTED — redesigned as per-image holder records.** Verified:
`registerProgramImage` first-text-matches `"ops"` on `holder +d.vars`
(`vortex_vm.cpp:131–137`), so `map[home]` can never scope per graft — base shadows local,
foreign records first-match, absent records return `unexpected` instead of the specified
no-op. Fix adopted (the vortex critic's holder suggestion, which uses the existing API
verbatim): **`"ops"` moves under `"stdlib.image"` as the per-image holder** —
`stdlib.image +d.vars → "ops"` record → `+d.values → +d.step` chain. Registration is then
`registerProgramImage(map[stdlibImageRecord])` per grafted image — scoped by record
identity, using the vagent holder convention unchanged. `rebindInstance` (including
non-grafting second instances) enumerates **all** `"stdlib.image"` records on
`home +d.vars` and registers each — covers base + local images uniformly. `Cpp` mode
mints one `stdlib.image` record during bootstrap (dev fields: `contentVersion=0`,
`generatorHead="cpp"`, `imageEpoch=engineEpoch`) — same mechanism in both modes.
Cardinality pinned: **one `"ops"` record per image**; each `build*Module` tail-appends
its op members to the single chain (kills the 21-records reading). Absent-`"ops"` member
on a `stdlib.image` → skip (the specified no-op); malformed member → named refusal.
Residual threat stated: a foreign `"stdlib.image"` record still gets scanned — gated by
member decode (`isRegistrableOpcode` + `unexpected` on non-cells); accepted as equivalent
to today's foreign-`"ops"` exposure, not worse.

## Cluster R3-2 — Edge taxonomy not closed (zigzag M2)

**Disposition: ACCEPTED.** Three additions to §5.2: (a) new edge class — *non-anchor
endpoint maps to a pre-existing cell*: skip when the edge already holds in the arena,
otherwise named refusal (literal tail-append of an already-ranked member severs at its
mid-rank predecessor and rings the tail — verified `manifold.cpp:667–692`); (b) third
map-seed class: name-merged module cells join the map at seed time and are excluded from
cloning (§5.2.1); (c) residual rule — any remaining edge touching a pre-existing cell →
named refusal — taxonomy now closed; (d) dim classification decided **before** `map[]`
consultation.

## Cluster R3-3 — VM runtime tables on arena swap (vortex M2)

**Disposition: ACCEPTED.** Same dead-refs/aliasing argument as `opcodeMap_` — extended:
on arena swap, `cursorPC_` and `memoizedOps_` clear (all keys dead); `choiceStack_`
non-empty at `bindManifold` is a **named refusal** (`ChoiceStackLive`) — `Mark`s
snapshot old arena sizes and a `backtrack()`/`release()` would truncate the new arena to
dead counts (`arena_manifold.cpp:1671–1674`); produce cursors legitimately outlive
`call()`, so draining silently would corrupt — the precondition is "no live choice points
across `bindManifold`".

## Cluster R3-4 — `wholeSpace` semantics unstated (vortex M3)

**Disposition: ACCEPTED.** `promote`'s `d.dims`/`d.lineage` edge skip
(`arena_manifold.cpp:1778–1782`) excludes the registry rank's own edges — pinned:
`wholeSpace` **lifts the `d.dims` skip only** (`d.lineage` stays skipped — not stdlib
content); unused dims (`d.stack`, `d.cursors`, `d.cache`, `d.stores`, `d.role`,
`d.pinning-cursors`) then reach via the registry chain.

## Cluster R3-5 — `expectedScrollBytes` unreachable under tail truncation (vortex M4, zigzag m4)

**Disposition: ACCEPTED.** Manifest record/key cells mint **early in op order** (their
primedia lands early — names survive tail truncation); only the Int64 member *values*
(`expectedScrollBytes`, `generatorHead`, `contentVersion`, `imageEpoch`) write
store-side last via `SetValue` ops. Fallback diagnosis added: on manifest-missing, the
loader compares `perma->size()` against the ops' max referenced span-end → truncation
still names the scroll cause even when the manifest text is gone.

## Cluster R3-6 — `ownedStdlib_` has no rebind path (architect M2)

**Disposition: ACCEPTED.** Invariant: **every long-lived `VortexStdLib` re-runs
`ensureStdlib` when its arena is replaced** — implemented as a per-instance
arena-identity check (`VortexStdLib` records the arena it bound against; `ensureStdlib`
on a changed arena re-grafts + rebinds), plus the named call site: `initHostServices`
re-invokes `ensureStdlib` on `primary_.stdlib` **and** `vplEngine_.stdlib()`
(`vpl_engine.cpp:77–86`, `vortex_host.cpp:117–118`). Transient compilers safe by
construction; `ownedStdlib_` was the one unhandled instance.

## Cluster R3-7 — Generator never pins `Cpp` mode (architect M3)

**Disposition: ACCEPTED.** §4 states the tool forces `StdlibPolicy{mode=Cpp}` via the
explicit setter *before* constructing the scratch `WorkerContext` — otherwise the
ctor-time graft seeds the shipped image and `bootstrap()` doubles it or the gate
regenerates image→image vacuously.

## Cluster R3-8 — `vortex.stdlib_image_dir` dead-on-arrival (architect M1, ergo m2)

**Disposition: ACCEPTED — dropped.** The fold precedes any `loadConfigFromStore`
(`zigzag_visualizer.cpp:2859–2861`, `xuzz_app.cpp:1596`), and store-content steering the
loader's directory is a trust step worth refusing on principle. Kept:
`vortex.stdlib_source` **mode only**, honored **only from the Settings store** (a
downloaded document must not flip process mode; `bindStore` vs Settings last-writer-wins
resolved). Precedence settles conventionally: mode — explicit setter > env > Settings key
> default; dir — explicit setter > `$GLEDITOR_STDLIB_DIR` (taken-as-given: bad dir →
`ImageMissing` naming it) > `$GLEDITOR_ASSET_DIR` > `GLEDITOR_DATADIR` > `./assets`.
`status()` exposes the folded dir so the boundary is diagnosable.

## Cluster R3-9 — Portable-zip boundary ships no image (architect M5)

**Disposition: ACCEPTED.** The xuzz app seam **must** initialize the explicit setter from
`assetPath("vortex-stdlib")` before first context construction ("may" → "must", call site
named — `VortexHost` construction/`xuzz` init). `build-msys2.sh` staging gains
`assets/vortex-stdlib/`; `smoke-test.sh` gains an `ops.nodes` existence check (wasm
`--preload-file` + android `assets.srcDirs` ride free — noted).

## Minors — all accepted

Env spellings `image`/`cpp`/`imagethencpp` (lowercase, exact); `once_flag`+reset →
mutex-guarded `{once_flag; optional<Triple>}` swap; `StdlibPolicy` atomic snapshot +
failed fold **latches** (deterministic — a mid-process retry would diverge arenas); P34
residuals (`saveOsmicText` `store.cpp:4320–4340` included; permascroll mutators
refuse-or-document; request channel = `Config.readOnly` + an ops-spool open flag; errno
scope `{EACCES,EROFS,EPERM}`; refusals via `expected`/bool not throw — dtor `flush`;
primedia silent no-write vs the new named refuse — difference named); post-discard
registration → member decode happens *inside* the mark (the record-walk translation) so
post-`discard` registration is infallible by construction; `vortex_param` attribution →
`routineInputs`/`routineOutputs`/`call()` arity; `imageEpoch` gains
`kLastContentDecodableOpcode`/`kLastOpcodeKind` bounds + the authored `OpcodeKind`→label
table; `mesh_evaluator.cpp:743` migrates to `findOrMintDimension`; marks-check "(at
entry, before `graftImage` takes its mark)"; "invariant below" → points to §4;
`±d.grab` added to the single-owner list; record-member walk roots named; local-image
lifecycle (second process-static holder, once-per-process, unconditional-on-existence,
failure scoped to local graft — rollback+warn+keep-base; produced by a future
`vortex-stdlib-gen --out <local-dir>` or the `:export-lib` flow against a scratch
context); record surface varies by mode (`status()` = provenance probe); §12 restated
(~11–13K shared image cells; Cpp-mode arenas +~4K; per-instance `rebindInstance` cost);
P26 DRY commits to `common/xanadu/relocation_marks.hpp` (move `refCell` +
`markedRelocationSlots` + `harvestRelocationTargets` beside `foreign_op_handle.hpp`'s
constants); `SettingSpec` registration named (`system_docs.cpp:818–823`); checked-in
artifact stated + `./assets` CWD caveat; consumers corrected (`vpl`/`vprolog`/`vagent`/
`viconc` — `vquery`/`vqueryc`/`vplc` have no `VortexStdLib`); `vagent/main.cpp:136` added
to the site list; `§X.Y.N` convention declared; Stage-3 cited
(`vortex-multithreading-study.md`); critique-skill path named; §4 generator `Config` init
nit fixed; P27 `~:5502` → `~:5674`; `vortex_host_config.cpp` → `vortex_host.cpp:37–98` +
`system_docs.hpp:135/:261–265`.

## Refused / partial

- None refused. R3-1's adopted arm is the vortex critic's holder nesting (chosen over
  `registerProgramOps`-on-collected-members because it reuses `registerProgramImage`
  verbatim and gives `rebindInstance` a uniform enumerate-`stdlib.image` mechanism for
  non-grafting instances — the collection arm remains named as the fallback).
