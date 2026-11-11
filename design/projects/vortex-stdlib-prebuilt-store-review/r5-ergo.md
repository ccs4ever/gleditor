# Round 5 critique: Ergonomics Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 1B/5M/9m.

R4 landing check: MarksOutstanding-as-environmental landed correctly (§5 invariant,
§5.4 note, P30); `vortex.stdlib_source` moved to the Settings call site correctly
(verified `xuzz_app.cpp:1590`→`zigzag_visualizer.cpp:392`→`VortexHost` ctor precedes
every `loadConfigFromStore`); `vortex_stdlib_status` has a real dispatch mechanism
(`solveQueryHelper` `vortex_*` arms); §6.4 production path landed but with a flag
contradiction (M4).

## BLOCKING

### B1 — `ChoiceStackLive` is pinned to `bindManifold`, which has zero call sites; the named reachable driver replaces the host instead

`bindManifold` appears exactly twice in all C++ — declaration `vortex_host.hpp:79`,
definition `vortex_host.cpp:149–158`; repo-wide grep finds **no caller**. The doc's own
named reachable driver, `bindXanaduStore` document rebind, does not call it — it
*replaces the entire `VortexHost`*: `engine_.reset()` (`zigzag_visualizer.cpp:364`),
new `UnifiedTransclusionEngine` (:370), `ensureVortexHost()` (:392) →
`vortex_host_ = make_shared<VortexHost>(&engine_->manifold())` (:2857–2866). On the
path users actually take, the outgoing host's live `choiceStack_`/produce cursors are
**silently destroyed** — worse than the promised refusal and exactly the dishonesty the
refusal exists to prevent. The §5.2.5 machinery (resync, identity token) is likewise
anchored on host *reuse* nothing exercises — on replacement a fresh `VortexCore` ctor
handles `home_`/`dims_` already. §11.8 tests `bindManifold` directly — validates dead
code while the real path is untested. No `zigzag_visualizer.cpp` rewiring appears in
any proposal. Also: refusing-the-swap is the right boundary; the *placement* is wrong;
no recovery contract stated; `what()`'s dir+env-var payload is meaningless for
environmental refusals; §5.4 says `ChoiceStackLive` "refuses `ensureCells`" — it
refuses the *swap*, not `ensureCells`.

**Fix:** pick one and name it: (a) rewire `bindXanaduStore`/`ensureVortexHost` to reuse
the host through `bindManifold`/a new `rebindArena` — add `zigzag_visualizer.cpp` to
P30's Location; or (b) keep host replacement and check the *outgoing* host —
`vortex_host_->primary_.vm.choiceDepth()`/`choiceStack()` (`vortex_vm.hpp:245,265–267`)
— in `bindXanaduStore`/`ensureVortexHost`/`attachVortexHost` before `vortex_host_`
drops; a live stack refuses the rebind, old host+engine retained. Either way state the
recovery ("drain or close the produce cursor, then re-issue") and give
`StdlibImageRefused` an environmental payload (live choice-point count) — dir+env
remediation doesn't apply. Extend §11.8 with a `bindXanaduStore`-driven gate.

## MAJOR

### M1 — `localImage` has no user-facing channel and a self-contradicting default

`StdlibPolicy` gains `optional<bool> localImage` but §5.4's precedence bullets cover
only mode and dir — no env var, no Settings key, no CLI flag; the only writers are
programmatic setters. §6.4's "only on explicit opt-in" names no reachable mechanism
while "defaults true for app modes" contradicts "opt-in" unless the default is
per-mode (unstated). A `GLEDITOR_STDLIB_SOURCE=cpp` user has no way to opt in; an
image-mode user has no way to opt out short of deleting the dir. **Fix:** its own
precedence bullet — setter > `GLEDITOR_STDLIB_LOCAL=0|1` (or Settings key) > per-mode
default (true under `ImageThenCpp`/`Image`, false under `Cpp`) — plus a `local`
sub-record in `status()` (`{present, grafted|skipped|failed|suppressed, dirResolved}`),
or state plainly the opt-in is programmatic-only and scope "user patching" to image
modes.

### M2 — The status surface does not reach `vpl`/`vagent`/`vicon`

`VortexHost::stdlibStatus()` is xuzz-only (`zigzag_visualizer.cpp:2857–2866` is the
sole construction site); `vpl`/`vprolog`/`vagent`/`vicon` build `VortexStdLib`
directly. The builtin dispatches inside `solveQueryHelper` — reachable only where
`solve*` runs: vprolog and xuzz `solveLogic`. `vpl` never calls `solve*`; `vagent`
runs its own solver (`executor.cpp:3159–3176`); `vicon` compiles only. For two of the
three named consumers the only channel remains the stderr log the doc calls "not their
channel". **Fix:** put `stdlibStatus()` on `VortexStdLib` (the instance every consumer
owns) or a free function over the folded-image state; name each consumer's surface
(vpl REPL command, vagent directive, vicon flag) — or scope honestly.

### M3 — The `status()` record cannot describe the states users will hit

`source∈{image,cpp}` has no value for "nothing loaded" (Image-mode or environmental
refusal); no fold-failed-latched vs grafted distinction; no local-image state though
§6.4 calls it the provenance probe; `lastRefusal`'s term shape + builtin arity
unpinned. Worse: the builtin sits behind `solve()`'s use-point throw — unreachable in
exactly the state it exists to diagnose (`solveLogic` swallows the throw to
`std::cerr`+`{}`, `vortex_host.cpp:942–945`). **Fix:** `source` → `std::optional` (no
sentinels); add the `local` sub-record; pin the term shape (`vortex_stdlib_status
(-Status)` or positional per `vortex_param/4`); dispatch arm answers *before* the
use-point throw — the same diagnostic exemption `resolve()`/`modules()` already get.

### M4 — `--module` and `--out` are both named "the local-image producer" and contradict §5.1

P32 says `--module` = per-module dev image = §6.4 producer; §6.4 says per-module output
fails `FormatRefused`. If `--module` wraps `exportModuleToStore` it produces a refused
image; if it emits whole-space-restricted-to-module (genesis+registry+manifest intact),
the doc never says so. "After dev additions" names no input channel — the only v1 way
is editing `build*Module` C++ and re-running the generator; unstated, a dev gets a base
copy whose same-text symbols all shadow → silent no-op patch. **Fix:** pin
"`--module <name>` emits a whole-space image (genesis, registry, manifest) whose
`+d.stdlib` rank carries only the named module(s) — the §6.4 producer; container-less
per-module exports stay `FormatRefused`"; and "dev additions enter by editing the
builders and re-running the generator — interactive authoring is U-X8".

### M5 — A valid store that isn't a stdlib image falls through every named validation arm

`$GLEDITOR_STDLIB_DIR` pointed at an ordinary document store folds fine: genesis
present → not `FormatRefused`; complete permascroll → `expectedScrollBytes` check can't
run (no manifest); `stdlib.image` lookup misses → `imageEpoch` has no operand. §5.1's
fallback covers only *truncation* — the well-formed-non-stdlib case has no refusal
class. **Fix:** a successful fold carrying no `stdlib.image` key record → `FormatRefused`
naming "store carries no stdlib.image manifest — not a stdlib image"; the span-end
fallback applies only when the manifest's names may themselves be truncated.

## MINOR

m1. `vortex_stdlib_status` arity/return shape unpinned — positional
    (`?Mode,?Source,?Dir,?Refusal,?Version`, `vortex_param/4` precedent :709–781) or
    record term; and whether it takes the `createPredicate`+`exportSymbol`+
    `candidatePreds` route (:3420–3444) — which adds it to `symbolsInModule`.
m2. Local-image dir has no override — pin `GLEDITOR_STDLIB_LOCAL_DIR` (taken-as-given,
    `paths.hpp:28–30`) and/or `vortex-stdlib-gen --local` writing the resolved path, so
    devs can A/B images without clobbering the canonical dir.
m3. §6.5 honesty extension: shadowed same-text local symbols **double-enumerate** in
    `symbolsInModule`/`vortex_function`/`findall` (the triple-bootstrap precedent);
    `"stdlib.image"` records visible on `home +d.vars` browsing;
    `vortex_stdlib_status` joins `symbolsInModule` if exported.
m4. Pin `vortex.stdlib_source`'s `SettingSpec` (string schema `image|cpp|imagethencpp`;
    notes "xuzz-only; read once pre-context — mid-session edit applies next launch");
    `vortex.worker_threads` et al. re-apply live (`xuzz_app.cpp:4225–4227`), this key
    never can.
m5. `Image`-mode refusal in GUI xuzz: `resolve()`s → nullopt → zero action routines →
    dead keybindings, no in-app signal until a `call()`/`solve()` throws (swallowed to
    `std::cerr`, `vortex_host.cpp:942–945`). Pin: recorded refusal surfaces once into
    the app's error surface at init.
m6. Pin the v1 dev loop end-to-end: edit builder → `vortex-stdlib-gen --module
    std:mymod --out <local-dir>` → **restart the app** (once-per-process fold; reset
    hook is test-only) — so no reader assumes hot-reload or that bare `--out` makes a
    patch.
m7. "installed as a developer tool" names no install rule — name the
    `$(OBJDIR)/vortex-stdlib-gen` + `install -m 755` addition beside
    `Makefile:1798–1802`.
m8. §5.4 parenthetical: `ChoiceStackLive` refuses the *swap*, not `ensureCells` —
    correct while fixing B1.
m9. `status()` under a bad explicit dir: `dirResolved` should report the *attempted*
    dir (what the user typed) in `lastRefusal`, not the fallback resolution.

VERDICT: NOT CONVERGED
