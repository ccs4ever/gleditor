# Round 3 critique: System Architect (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/5M/11m.

Every proposed mechanism verified implementable: headless chain belongs in `paths.hpp`'s
inline `paths::` namespace; `fromStore`/`getSetting` plumbing exists
(`vortex_host.cpp:37–98`, `system_docs.hpp:135–745`, `system_docs.cpp:2935`);
`openActiveSegment` `O_RDWR|O_CREAT`-only confirmed (`segmented_ops_spool.cpp:608`);
`UserPermascroll` ctor writes before any fallback (`permascrollState` →
`create_directories(publication-state)` `:58–59` + `mdb_env_open`/`mdb_put` `:166`, all
before `create_directories(segments)` `:371`, `spool_.openActiveSegment` `:384`);
`Store::load` writes nothing; `~UserPermascroll` flush early-outs on `readOnly`
(`segmented_primedia_spool.cpp:352`).

## MAJOR

### M1 — `vortex.stdlib_image_dir` is dead-on-arrival in every shipped flow

Fold happens inside the first `VortexStdLib` ctor — `primary_` (`zigzag_visualizer.cpp:
2859`), vagent `compile` (`apps/vagent/main.cpp:136`), tool mains — all strictly before
`loadConfigFromStore` (earliest `bindStore` `:2861`, then `xuzz_app.cpp:1596`). Image
folds once; `resetStdlibImageForTesting` is test-only — the dir key can never influence
the fold. (The *mode* key works — consulted per `ensureCells`.) Compounds: a settings
cell that silently does nothing violates fails-loudly; store-content steering the
loader's directory is a trust step worth refusing. **Fix:** drop `vortex.stdlib_image_dir`,
keep `vortex.stdlib_source` (mode only — dir stays setter/env, operator-controlled).

### M2 — Arena-swap rebind covers only `primary_.stdlib`; `vplEngine_.ownedStdlib_` has no invocation path

`ownedStdlib_` is long-lived over the shared core (`vpl_engine.cpp:77–86`,
`vpl_engine.hpp:132`); `bindManifold` move-assigns `primary_.arena` (`vortex_host.cpp:
155`) and `initHostServices` runs `ensureStdlib` on `primary_.stdlib` only (`:117–118`).
`ownedStdlib_`'s bindings/caches name dead cells — and stale ints *alias* fresh cells, so
`call()`/`solve()` silently bind wrong (`vortex_vm.cpp:303–306`'s throw is the lucky
case). Transient compilers are safe by construction; `ownedStdlib_` is the one unhandled
instance. **Fix:** state the invariant — every long-lived `VortexStdLib` re-runs
`ensureStdlib` when its arena is replaced — and name the call site, or add a per-instance
arena-generation check.

### M3 — Generator never pins `Cpp` mode — the ctor-time graft precedes `bootstrap()` and the pipeline degrades silently

Under default `ImageThenCpp` the scratch `WorkerContext`'s `stdlib` ctor grafts the
*shipped image* before `bootstrap()` is invoked → doubled stdlib, or (presence-aware) a
vacuous image→image regeneration that can never catch a builder regression. **Fix:** §4
states the tool forces `StdlibPolicy{mode=Cpp}` via the explicit setter before
constructing the scratch context.

### M4 — `registerProgramImage(map[home])` contradicts the scoping claim (sixth independent finding)

Holder + first-text-match on `home +d.vars` (`vortex_vm.cpp:130–141`); grafted records
tail-append → foreign `"ops"` first-matches on any bound arena — the R2-6 hazard
unclosed. **Fix:** target the graft's mapped record — `map[imageOpsRecord]` consumed by
an arm reading `record +d.values → +d.step` directly (or `registerProgramOps` on the
already-walked enumeration).

### M5 — Portable-zip boundary silently ships no image

`build-msys2.sh:52–54` stages assets by *enumerating files* (`shaders/*.glsl`, `vulkan/
*.spv`, `logo.png`) — `assets/vortex-stdlib/` silently omitted; `smoke-test.sh:48–55`
checks shaders only. Headless chain can't reach `<exe dir>/assets` (needs
`SDL_GetBasePath`, `paths.cpp:26–63`) — the zip's own layout; "may consult `assetPath`"
must be "**must** … before first context construction" + named call site; add the dir to
staging + an `ops.nodes` check (or `GLEDITOR_STDLIB_SOURCE=image` probe) to smoke-test.
(wasm `--preload-file` + android `assets.srcDirs` copy the whole dir free.)

## MINOR

1. Site list one short: `apps/vagent/main.cpp:136` (vagent runner compile context).
2. `StdlibPolicy` synchronization unstated — worker-thread compilers construct
   `VortexStdLib` inside `workerPool_->run` (`vortex_host.cpp:1001–1031`) while
   `loadConfigFromStore` writes policy on main — torn read once Stage-3 exists. Specify
   atomic snapshot (`atomic<shared_ptr<const StdlibPolicy>>` or holder mutex), single
   snapshot per `ensureCells`, and whether a failed fold latches or retries.
3. `once_flag` + `resetStdlibImageForTesting` don't compose — `once_flag` can't re-fire;
   reset must swap `mutex`-guarded `{once_flag; optional<Triple>}` state. Name it.
4. P34 residual write paths: `saveOsmicText` (`store.cpp:4320–4340`); `UserPermascroll`
   mutators ungated (`clear()` `:456`, `sealIncremental` `:528–530`, deploy `:578–616`);
   the read-only *request* channel unnamed (`Store::load` has no options param); empty
   `ops.nodes` under `O_RDONLY` mislabeled "cannot read".
5. §6.4 local-image fold lifecycle unspecified (second holder + reset? per-arena?);
   local graft unconditional-on-existence (not `stdlibPresent`-gated) unstated.
6. §12 "~5–6 K-cell" shared image inconsistent with the doc's own additions (~1,200 ops
   members + ~2,600 sig + ~1.5–3K markers + ~1.3K orphans → ~11–13K); Cpp-mode arenas
   grow ~4K; per-instance `rebindInstance` cost per transient compiler worth a line.
7. P26 DRY note understates sharing: `refCell` (`records.hpp:63–72`) is byte-identical to
   the needed write half; `markedRelocationSlots`/`harvestRelocationTargets` are
   anonymous-namespace in `lineage.cpp:207–249` — a file move, not a refactor. Commit to
   `common/xanadu/relocation_marks.hpp` beside `foreign_op_handle.hpp` (constants at
   `:24–25` already shared).
8. Store-key wrinkles: two `loadConfigFromStore` sources (document `bindStore` vs
   Settings store `xuzz_app.cpp:1596/:4226`) — last-writer-wins; honor stdlib keys only
   from the Settings store. Keys also need `SettingSpec` registration
   (`system_docs.cpp:818–823`) — name it in P31.
9. Checked-in vs generated implied not stated; `./assets` is CWD-relative — `xuzz_test`
   run outside repo root silently exercises the C++ path.
10. `vquery`/`vqueryc`/`vplc` don't consume the image (`apps/vquery/main.cpp:487–488,532`,
    `vqueryc/main.cpp:118–121`, `vplc/main.cpp:121–124`); the tree-run consumers are
    `vpl`, `vprolog` (`main.cpp:559–561`), `vagent`, `viconc` (`main.cpp:75–77`).
11. `$GLEDITOR_STDLIB_DIR` semantics: pin taken-as-given (bad dir fails `ImageMissing`
    naming the dir, not silent fall-through).

VERDICT: NOT CONVERGED
