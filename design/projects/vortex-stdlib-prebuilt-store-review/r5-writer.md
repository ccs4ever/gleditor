# Round 5 critique: Technical Writer (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/0M/9m. **VERDICT: CONVERGED.**

All R4 blocking-class items verified against the live tree: holder topology propagated
verbatim and consistently (§3.1/§4/§5.2/§6.4/P28/§11); edge-class ordering stated
("endpoint provenance before membership", ValueKind dispatch, src/dst residual);
environmental refusals unconditional with the no-fallback justification; Settings key
at call site pre-context with bad-spelling warn + per-field optionals; `VortexCore`
resync + `ChoiceStackLive` pre-move-assign; §6.4 local-image gating; process-state
shape; `status()` surfaces; consumer lists (`vicon` in / `vquery` out); `vortex_host.cpp
:113` named in P30; `registerProgramOps` re-registration verified genuinely idempotent
(keyed by cell). ~30 citations re-verified accurate.

## MINOR

m1. `"ops"` mint timing contradicts `varsRecord` filing (§4 step 2, P28) —
    `varsRecord` *unconditionally* `makeCell`s the key + links `+d.values → value`
    (`fragment.cpp:39–49` — no find-or-mint), so if the key existed at bootstrap start
    the end-time filing mints a duplicate. Only `opsHead` + the `+d.step` member chain
    need exist at start; the key record + `+d.values` edge are created by the end-time
    filing. Fix: "`opsHead` + chain minted at bootstrap start; `varsRecord(holder, vars,
    values, "ops", opsHead)` mints the key + links + files at bootstrap end."
m2. Process-side "holders" residual (§5.1:422–423, §6.4:747) — "covers both holders"
    should read "covers both folded-image states" (matching P31's correct :1025–1026).
m3. "the `home()` cell it bound against" ambiguous (§5.4:637, P30) — `arena().home()`
    returns `nullopt` for null-base arenas (`arena_manifold.cpp:30–32`); the token must
    be `arena().base()` + `core().home()`/`home_` — the bound home the genesis resync
    re-derives (own-home mints differ per scratch arena).
m4. "already in hand at `xuzz_app.cpp:1589–1593`" misstates — the Settings store is
    fetched at :1596–1597 *after* `bindXanaduStore` at :1590; what :1589–1593 holds is
    `session`/`bridgeStore`/`systemStore(Layout)`. Fix: "(the same
    `session->systemStore(SettingDoc::Settings)` call used at :1596, hoisted above
    `bindXanaduStore`)".
m5. `vicon` dropped from env-or-default list (§5.4:621–622) — same paragraph adds it to
    consumers :645–646; add `vicon` to "`vpl`/`vprolog`/`vagent`/`vicon`/`viconc` honor
    env-or-default only".
m6. U-X6 lists `vquery` — it carries no `VortexStdLib`; fix to
    `xuzz`/`vpl`/`vprolog`/`vagent`/`vicon`.
m7. P29 "sigCell is an *unscoped* vars record" — under the canonical terminology a vars
    record is an on-rank key cell that *cannot* own a private rank; `sigCell` is the
    `+d.params` target = an unranked holder. Fix: "an *unscoped* unranked holder with
    keyed `"in"`/`"out"` records on its private `+d.vars` rank".
m8. Sentinel notation `map[imageRecord]` (§5.4:640–641) — `map` is the graft's ref map,
    not retained; fix: "the grafted `stdlib.image` key ref recorded at graft time";
    `contains` alone can alias — the `textOf` guard covers it, keep explicit.
m9. `baseManifold_` is re-pointed by `bindManifold` itself (`vortex_host.cpp:154`), not
    the resync; `extraContexts_` workers (`vortex_host.cpp:112`) bound to the old base
    share the dangling exposure and are outside the re-fire — bound the claim, no
    mechanism change.

VERDICT: CONVERGED
