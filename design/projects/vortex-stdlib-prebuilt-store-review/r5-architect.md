# Round 5 critique: System Architect (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/2M/5m. **VERDICT: NOT CONVERGED.**

**Verified clean:** two-cell holder compatible with `registerProgramImage`
(`vortex_vm.cpp:121–155` vs `fragment.cpp:438–458`); §5.2.3 precedence + marked-value
dispatch match `link()`/`rankContains`/`handleTarget`; refusal matrix + local-image
gating complete across all mode×outcome combinations; consumer list accurate
(`vpl`/`vprolog`/`vicon`/`viconc`/`vagent` in, `vquery`/`vqueryc`/`vplc` out);
packaging claims hold; layering sound (`paths::dataPath`/`dataDir` header-inline
env-only; `getSetting`/`SettingSpec` engine-side; only SDL `assetPath()` app-confined);
P30 worker fix correct (`vortex_host.cpp:113` orphan `bootstrap()` → `ensureStdlib()`);
P34 verified (`O_RDWR|O_CREAT` only, no `readOnly` in `UserPermascroll::Config`).

## MAJOR

### M1 — The xuzz seam window is one statement too late: the first `ensureCells` fires inside the `ZigzagVisualizer` ctor, not `bindXanaduStore`

The doc places the seam at `xuzz_app.cpp:1589–1596` and names the first-context driver
`bindXanaduStore`→`ensureVortexHost` (`xuzz_app.cpp:1590` → `zigzag_visualizer.cpp:392`).
The true first context is built one statement earlier, inside
`make_shared<ZigzagVisualizer>("")` at `xuzz_app.cpp:1588`: ctor →
`populateFallbackStructure()` (`zigzag_visualizer.cpp:74`→:157–213) → `adoptDocument
(...,"fallback")` (:212→:215–308) → `engine_ = make_unique<UnifiedTransclusionEngine>`
(:267) → `ensureVortexHost()` (:306) → `vortex_host_ = make_shared<VortexHost>` +
`bindStore` (:2857–2866) → `VortexHost` ctor → `WorkerContext` → `VortexStdLib` ctor →
`ensureStdlib`/`ensureCells`. A seam in the cited window can never precede the first
context — and the fold latches process-wide (`optional<expected<Triple,Refusal>>`, §5.1),
so the consequences are permanent: (a) the ctor-path host consumes the *unset* policy;
(b) the dir setter (`assetPath("vortex-stdlib")`, the mandatory portable-zip leg) can
never precede the once-fold — a `ImageMissing`/`Refusal` latches inside :1588; under
`Image` mode it then refuses every subsequent context (xuzz cannot boot against an
installed/portable image), under `ImageThenCpp` it silently mints C++ forever;
(c) "before *any* `loadConfigFromStore`" is also false — `adoptDocument`→
`bindStore(ownedStore_)`→`loadConfigFromStore(*store_)` (:244–247,:2861;
`vortex_host.cpp:160–166`) runs inside the ctor on the fallback document store.

**Fix:** move the seam before `xuzz_app.cpp:1588` — `session`/`systemStore(Settings)`
are in hand well before; the `vortex.stdlib_source` read + mode setter and the
`assetPath("vortex-stdlib")` dir-setter init run immediately before
`make_shared<ZigzagVisualizer>("")`. Re-cite the first-context driver chain as
`xuzz_app.cpp:1588` → `ZigzagVisualizer()` → `populateFallbackStructure` (:74) →
`adoptDocument` (:212,:267,:306) → `ensureVortexHost` (:2857–2866). Optionally note the
ctor-path host is discarded by `bindXanaduStore`'s `engine_` replacement (:370) —
bounding the wasted graft — but the fold latch is process-level, which makes the
misplacement fatal rather than merely wasteful.

### M2 — `ChoiceStackLive` is checked on an unnamed single stack, but two `VortexVM`s survive the swap

The entry-check timing is right, but the doc never says *whose* `choiceStack_`. Two
`VortexVM`s outlive the move-assign over the same replaced arena: `primary_.vm`
(`worker_context.hpp:72`) and `vplEngine_`'s `ownedVm_` (`vpl_engine.cpp:78`,
`vpl_engine.hpp:131` — the doc's own "one long-lived non-primary instance"). A live
choice stack on `ownedVm_` — reachable through VPL `produce`/`every` routines via
`vplEngine_.stdlib()` — holds `Mark`s snapshotting the old arena's sizes
(`ChoicePoint{.mark = core_.arena().mark()}`, `vortex_vm.cpp:317–326`); a post-swap
`backtrack()`/`release()` truncates the *new* arena to dead counts
(`arena_manifold.cpp:1657–1674`) — the precise corruption the check prevents, through a
VM the named check doesn't cover. Accessor gap: `VPLEngine` exposes `core()`/`arena()`/
`stdlib()` but no `vm()` (`vpl_engine.hpp:68–82`), and `VortexStdLib::vm_` is private.

**Fix:** name the coverage set — "every `VortexVM` over the swapped arena" — check
`primary_.vm` plus `vplEngine_`'s `ownedVm_` (add `VPLEngine::vm()` or a
`VortexStdLib::choiceStackLive()` probe) at `bindManifold` entry before
`extraContexts_.clear()` and the move-assign; `extraContexts_` VMs die with their
contexts and need no check. One clause + one accessor.

## MINOR

m1. The Settings-update path isn't named — `xuzz_app.cpp:4219–4228` re-runs
    `loadConfigFromStore` when `system://settings` changes; sibling `vortex.*` keys
    re-apply there via `applyParallelConfig` (`vortex_host.cpp:75–95,174–186`). Since
    `vortex.stdlib_source` is absent from `fromStore` and the seam never re-fires, a
    mid-session edit silently no-ops while adjacent keys visibly take effect. State
    startup-only explicitly, or read it in the Settings-update arm through the named
    helper.
m2. Per-field setters pin the merge hazard but not the write primitive —
    `load→copy→store` loses a concurrent field update; pin `compare_exchange_weak`
    retry loop (or a write mutex). Latent today (writers are main-thread).
m3. `resetStdlibImageForTesting` can orphan an in-flight graft's read of the folded
    `State` — a worker-thread `VortexStdLib` ctor (`vprolog::Compiler` inside
    `workerPool_->run`, `vortex_host.cpp:1001–1031`) mid-graft loses the `State` the
    graft reads. Hold `shared_ptr<State>` acquired per `ensureCells` under the mutex,
    or document the hook as requiring quiescent contexts.
m4. `base()`+`home()` cannot detect the same-base swap it is named for —
    `bindManifold` move-assigns over the *same* `baseManifold_` (`vortex_host.cpp:
    149–156`), so `base()` is the same pointer and `arena().home()` delegates to the
    base's home, unchanged. What detects it is the unconditional `ensureStdlib`
    re-fire + the `stdlibPresent` probe + the sentinel check. Reword: the token
    distinguishes cross-base moves only; same-base swaps are detected by re-fire plus
    sentinel — or adopt the generation counter the doc already lists.
m5. `attachVortexHost` bypass note omits: zero in-tree callers, and `ensureVortexHost`
    unconditionally replaces `vortex_host_` at every `adoptDocument`/`bindXanaduStore`
    — the bypass is unreachable and bounded to the next document bind.

VERDICT: NOT CONVERGED
