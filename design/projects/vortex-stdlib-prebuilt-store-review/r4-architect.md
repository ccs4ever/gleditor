# Round 4 critique: System Architect (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/3M/12m.
Every R3 disposition's text verified as landed; spot-checks all land
(`registerProgramImage` scoping, `isCellRelocationHandle` gating, `refCell`/marks,
`promote` skips, `seedProgramImage`, `outstandingMarks`, P34/P31/packaging cites,
`kLastContentDecodableOpcode` bounds).

## MAJOR

### M1 — R3-6's invariant is not implementable as specified: no identity token exists, and the `VortexCore` resync is absent

(a) `primary_.arena` move-assigns **in place** (`vortex_host.cpp:149–155`, comment
:150–154 says refs stay valid *because* the object survives) — so "record `&arena()`"
never fires. `ArenaManifold` has no epoch/generation/id (`arena_manifold.hpp:835–869`);
`base()` misses same-base and `nullptr→nullptr` rebinds; `slots_.data()` unspecified.
Workable: record a sentinel the bind minted (`map[imageRecord]` re-verified via
`contains()` + `textOf()`), an `ArenaManifold` generation counter, or explicit
`bindManifold` notification — the doc names none.

(b) `VortexCore::home_`/`dims_` (19 `DimRef`s) are ctor-only (`vortex_core.cpp:16,
31–74`). Post-swap `resolve()` (`:104–110`), `findOrMintDimension` (`:91–102`),
`getOrCreateModule` (`:112–121`) all consume dead refs — the graft aliases arbitrary
new-base cells or misses; test 8 cannot pass; §5.2.5's clear-list omits the state
`resolve()` itself needs. P30 needs a `VortexCore` genesis resync (initGenesis
adopt-or-mint re-run, with an own-home guard — `arena().home()` returns only the
*base*'s home, `arena_manifold.cpp:30–32`) and resync scope pinned
(`defaultReader_`/`store_` revert to move-source defaults). Latent today —
`bindManifold` has zero callers/tests — but the doc builds §5.2.5/P30 on it.

Adjacent: `bindXanaduStore` (`zigzag_visualizer.cpp:362–394`) does `engine_.reset()` +
rebuild while `vortex_host_` (shared_ptr, built `if (!vortex_host_)`) survives — the
host's `baseManifold_`/`base_` dangles on document rebind today, uncatchable by any
identity check (`base()` still returns the stale pointer). Name the driver set.

### M2 — "Settings store only" unenforceable via the named plumbing + dead-on-arrival for the primary context

(a) `loadConfigFromStore`/`fromStore` runs on the **document** store too —
`bindStore(store_)`/`bindStore(&engine_->store())` → `loadConfigFromStore`
(`zigzag_visualizer.cpp:2861,2863`, `vortex_host.cpp:160–166`). A document carrying
`vortex.stdlib_source` writes process policy first — the exact trust step refused.
Fix: read the key only at the Settings call site, or a kind-tagged load.

(b) The first `ensureCells` runs inside the `VortexHost` ctor — reached via
`bindXanaduStore`→`ensureVortexHost` (`xuzz_app.cpp:1590` → `zigzag_visualizer.cpp:392`)
before *every* `loadConfigFromStore` — the mode key can never steer the primary
context. Reachable fix: read the Settings store + call the mode setter before
`vortexHost()` (systemStore in hand at :1589–1593). Also `xuzz_app.cpp:4226`
live-reloads mid-session.

### M3 — §6.4 local-image graft is not mode-gated; under `Cpp` it silently contradicts two stated invariants

Under `Cpp`: (i) the generator forces `StdlibPolicy{Cpp}` — its ctor-time `ensureCells`
would fold `paths::dataPath("xuzz","vortex-stdlib-local/")`, baking the build host's
local image into the shipped artifact (determinism + trust boundary; `make
stdlib-image` is only incidentally protected by the Makefile XDG redirect — a hand-run
`build/vortex-stdlib-gen` is not; the doc never says the generator suppresses the local
graft). (ii) It contradicts P30's "`Cpp` = today's behavior" and §5.4's all-em-dash
`Cpp` row. If `Cpp` skips it, "under every mode" misreads. State the mode-gating +
generator suppression.

## MINOR

m1. `StdlibPolicy` write-merge unstated — two writers (app seam dir; Settings mode) on
    one `atomic<shared_ptr<const>>`; whole-struct replace clobbers the other field.
    Pin per-field optionals/setters or read-modify-write.
m2. `{once_flag; optional<Triple>}` can't carry the latched refusal (`nullopt` post-
    failure has nowhere to record it → `optional<expected<Triple,Refusal>>` or a
    sibling); whether `resetStdlibImageForTesting` also resets the local holder
    unstated.
m3. `ChoiceStackLive` check placement: `bindManifold` move-assigns at :155 *before*
    `initHostServices` at :157 — a post-swap refusal can't un-swap. Pin the check at
    `bindManifold` entry or state the post-swap contract.
m4. Invalid spellings unstated (`=bogus`, malformed key): warn+default, refuse, or
    treat as unset?
m5. Middle-chain dir steps ambiguous — only `GLEDITOR_STDLIB_DIR` pinned taken-as-given;
    bad `GLEDITOR_ASSET_DIR`/`GLEDITOR_DATADIR` fall-through vs refuse unstated
    (`assetDir()`'s own convention is taken-as-given, `paths.cpp:41–48`).
m6. `vicon` missed from consumers (`apps/vicon/main.cpp:121` → `vicon::Driver`
    →`Compiler::stdlib_`); `vlace`/`vlacec` correctly excluded; `vquery`/`vqueryc`/
    `vplc` verified absent.
m7. `makeWorkerContext`'s explicit `stdlib.bootstrap()` (`vortex_host.cpp:113`) is
    outside the ctor — under the split it double-mints over a ctor graft (the surviving
    §6.5 bug). Must become `ensureStdlib()` or be dropped — not named in P30.
m8. App-seam call site is `bindXanaduStore`→`ensureVortexHost` (`xuzz_app.cpp:1590` →
    `zigzag_visualizer.cpp:392`), not the visible `vortexHost()` at :1595;
    `attachVortexHost` (:2845) accepts a pre-built host bypassing the seam.
m9. wasm `--preload-file` + android `assets.srcDirs` note claimed in disposition —
    neither word in the doc.
m10. §12 "~+4K cells" omits P26 per-ref markers (~1.5–3K, minted in every mode) →
     ~5.3–6.8K; and "over today's 4,296" compares arena cells to the *export* count.
m11. `d.params` shares `sql_lens.cpp:703`'s filed qattr under a lens-document base —
     `findOrMintDimension` adopts the same dim cell; structurally harmless but "unrelated
     namespace" undersells the literal share.
m12. `paths::dataPath("xuzz",…)` hardcodes xuzz's data dir for every consumer —
     intentional one-image sharing unstated; `Store::load`'s `ops.export` arm →
     `adoptOpRecords` writes through the spool → mid-load named refuse under read-only
     — consistent but unnoted.

VERDICT: NOT CONVERGED
