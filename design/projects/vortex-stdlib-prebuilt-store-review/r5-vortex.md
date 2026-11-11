# Round 5 critique: Vortex/Spool-Mesh Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/2M/12m. **VERDICT: NOT CONVERGED.**

All accepted R4 mechanisms re-verified against code — two-cell holder +
`registerProgramImage` walk, edge ordering vs `link()`/`rankContains`/`insertIntoRank`,
`outstandingMarks()` mark gate, ctor-cached `home_`/`dims_`, late `SetValue` writes,
`localImage` plumbing, `wholeSpace`+`extraRoots`, `isCellRelocationHandle` gating,
non-atomic registration disclosure, P34 read-only surface — all implementable as
written. (`wingOf` has no OpHandle arm today — P26's paired change required in every
mode, which the design says.)

## MAJOR

### M1 — The swap mark-gate cannot see `vplEngine_.ownedVm_`'s choice stack or any non-VM mark

Two checks as specified: `ChoiceStackLive` at `bindManifold` entry and
`outstandingMarks() == 0` at `ensureCells`/`graftImage` entry. On the swap path
`ensureCells` runs *after* the move-assign — it reads the fresh arena where
`outstandingMarks()` trivially returns 0 — so the only effective pre-swap check is
`primary_.vm.choiceStack_`. Two classes slip both gates: (a) `vplEngine_.ownedVm_` —
a second surviving `VortexVM` over `primary_.arena`; `VPLEngine` has no `vm()`
accessor (`vpl_engine.hpp:69–89` vs `:131`) so `bindManifold` cannot reach its stack;
post-swap `backtrack()`/`release()` truncates the *new* arena to dead counts (the same
`ArenaManifold` object survives move-assign, so recorded `Mark` sizes apply to the
replacement). (b) any stray non-VM `mark()` caller — `mark` is public. The
per-instance rebind *does* cover `ownedVm_`'s maps via `vplEngine_.stdlib()`'s
`ensureStdlib` → `rebindInstance`; only the mark-bearing stack is missed.

**Fix:** specify the `bindManifold` entry gate as `arena().outstandingMarks() == 0`
checked **pre-move-assign** — counts every outstanding mark on the old arena (both
VMs' stacks + stray `mark()` callers), needs no `VPLEngine` accessor, mechanically
subsumes `ChoiceStackLive`; retain `ChoiceStackLive` as the named cause when a VM's
stack is the holder. `ensureCells`-entry check stays for non-swap paths.

### M2 — The swap resync leaves engine-level arena-keyed caches dangling

§5.2.5's coverage list (`primary_.core` resync, `primary_.vm` clears, both stdlib
`rebindInstance`s, `gcCursor_` respawn) omits every other host-owned structure holding
`CellRef`s into `primary_.arena`: `vplEngine_`'s `defaultDim_` (`vpl_engine.hpp:140`,
cached `d.1` resolved once at construction), `env_`/`dependencyStack_`/
`cellDependencies_` (:141–144); `vqlEngine_`'s `env_`/`perspectiveClones_`/
`functors_`/`sessionMounts_`/`userDefs_` (`vql_engine.hpp:225–238`); `vqlCompiler_`'s
`varBindings_` (`vql/compiler.hpp:116`); `customActionRoutines_`/`macroRegistry_`
user entries (`initHostServices` re-registers only built-ins). Post-swap these refs
alias fresh cells — the doc's own "stale ints *alias*" argument applies equally.
Pre-existing bugs, but the design sells a complete resync and §11's swap gate asserts
"rebind correctly" for stdlib only — an implementer ships a host whose VPL/VQL
environment silently targets dead cells after every document rebind, with no named
boundary.

**Fix:** either enumerate per-engine reset semantics (`defaultDim_` resync + env/deps
clear, `perspectiveClones_`/`sessionMounts_` policy, `varBindings_.clear()`,
non-builtin rule for custom routines), or draw the boundary explicitly — "arena-keyed
caches outside `VortexCore`/`VortexVM`/`VortexStdLib` are caller-managed; document
rebind rebuilds `vqlEngine_`/`vplEngine_`/action registrations" — and extend the §11
swap gate to assert the chosen semantics (e.g., `vplEngine_.defaultDim()` post-swap
resolves the new arena's `d.1`, VQL env lookups miss rather than alias).

## MINOR

m1. "primedia … stays as harmless dead spans" — rollback is total: `release()`
    truncates `slots_`/`links_`/`content_`/**`scratch_`** (`arena_manifold.cpp:
    1671–1674`; `intern()` appends into `scratch_` :1158–1167). Interned primedia does
    not survive rollback — which is what §11's bit-identical gate requires. Fix the
    mechanism wording (same as codebase m5).
m2. `linked(member, dim, NEG)` is not equivalent to `rankContains` — "non-head on
    some rank" vs "on this rank"; a member on a *different* same-dim rank has the NEG
    link but fails `rankContains`. Drop the equivalence (same as zigzag m1).
m3. "its only other caller" — `xuzz_app.cpp:4226` is a second direct call site and
    `tests/xuzz/vortex_host_test.cpp:181` a third. Fix: "invoked only inside xuzz
    (`xuzz_app.cpp:1596,4226`, `vortex_host.cpp:165`)".
m4. `localImage` unset-default under `Cpp` unpinned — pin the resolution:
    `localImage.value_or(mode != Mode::Cpp)` (same as codebase m4).
m5. `"ops"` record mint timing — `varsRecord` *mints* the record cell
    (`fragment.cpp:36–48`); it cannot file a pre-minted record. Reword: opsHead +
    `+d.step` chain mints at bootstrap start; the keyed `"ops"` record is
    minted-and-filed at bootstrap end (same as writer m1).
m6. `expectedScrollBytes` direction + dev placeholder unpinned — pin `size() !=
    expected → ScrollTruncated` or justify `>`-acceptance; add the dev placeholder to
    the dev-field list.
m7. `vortex_core.cpp` missing from P30 Location — `initGenesis` resync lives there
    (same as zigzag m3).
m8. "per-module/dev records" is not a reachable ops-precheck case — per-module images
    fail `FormatRefused` upstream; the reachable case is foreign/malformed
    `stdlib.image` records.
m9. Name-merged cells' outgoing non-membership edges refuse under the residual rule
    (a dev module carrying `+d.name` annotations → src maps to pre-existing → whole
    local graft refuses; base survives). Defensible — name it as intended.
m10. OpHandle slot-scans (`findOpHandle`/`versionAnnotation`/`aliases`,
     `manifold.cpp:1332–1437,1452–1468`) conflate OpHandle wing slots with real
     handles on the folded image — unreachable today (image manifold serves
     load→graft only); one-line boundary note in P26.
m11. `generatorHead` self-naming — state the arithmetic: `MicroversionId` is
     positional, so `generatorHead` = promote head + planned trailing op count,
     written never read back.
m12. Unmarked-OpHandle refusal is narrower than stated — the mark requirement fires
     only when the *target* is also ephemeral (`arena_manifold.cpp:1816–1819`;
     persistent-target handles fall through to `getCompactOp` at :1823 with no mark).
     Moot on null-base scratch; fix the phrasing.

**Observation (not a finding):** `installWriteGuard` is last-wins
(`vortex_vm.cpp:84–95`) and `vplEngine_` constructs after `primary_`, so the core's
write funnel consults `ownedVm_`'s `opcodeMap_` — a one-line note naming which VM's
map the guard reads would make rebind-ordering unambiguous.

VERDICT: NOT CONVERGED
