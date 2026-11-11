# r2 — codebase-completeness critic (inventory coverage and citation truth)

**Verdict: NOT CONVERGED** (all 7 r1 majors closed; 3 minors + nits remain — one revision
from converging)

**Counts:** 0 BLOCKING / 0 MAJOR / 3 MINOR / 4 NIT

Scope of this pass: did each r1 major close; do v2's *new* `file:line` cites land; did the
repartition drop any r1-covered item or misfile §10.1/§10.2 inventory.

## Disposition of r1 findings — all closed

| r1 | Fix verified in v2 |
| -- | ------------------ |
| M1 `staticFuel` | Phase 2: "`staticFuel` joins `ScalarCompilerLimits` (`compiler.hpp:23-26`, spec :3070-3072)" — struct is still `{maxNodes, maxDepth}` at :23-26; `vm_.run(cursor, staticFuel)` maps onto `run`'s `maxCycles` param (`vortex_vm.hpp:162`). ✓ |
| M2 frontend call site | Phase 1: "`apps/vagent/main.cpp:46-53` rewiring — `checkFile` drives the frontend pipeline". Verified: `checkFile` (:25) calls `parseProgram` (:47) then `check(*program)` (:53) inline. ✓ |
| M3 Makefile per-phase joins | Phase 0 names the three TU joins + "code_path.cpp and expand.cpp join the list in the phases that create them"; phase 1 states both joins explicitly. ✓ |
| M4 genesis order | Phase 5 letters now spec-order: (d) `d.patch` replay → "Replay hands (e) its install set" → (e) `+d.refs` rehydration "**last pass** ... runs after (d) precisely so name-record rebinds see post-installation symbols (the R7-B ordering, spec :2435-2439)". No other inversion echo found. ✓ |
| M5 learn_where/learn_body | Phase 4 enumerates all five twins; phase 5 (d) names "`learn_where`/`learn_body`'s descriptor re-point" (spec :1891-1892 verified); phase 7 names `learn_where` for §9.2 (spec :1184 verified). ✓ |
| M6 seam (iv) wake | Preconditions + phase 6 step 1 (iv): "`wake(pid)` plus `ActorExecution::message`→`std::optional` (`mesh_actor.hpp:81`, `vagent-agent-language.md:1670`) with mesh executor/test updates". Verified: `pending(pid)` at `mesh_actor.cpp:659`, non-optional `message` at `mesh_actor.hpp:81`, P4 row text matches. ✓ |
| M7 programRef/bind set/(c) | Phase 3 has the `programRef` bind-record emission bullet (spec :3057-3058, :2311-2326 verified — "`produces` `MicroversionId`s or canonical post-fold positions", riding the same record `E_ops` rides) and the twelve-dim bind set (count matches spec :3004-3006 exactly); phase 5 has (c) `programRef`→root bind and the filing list gains "the phase-3 bind set above". ✓ |
| m1 17-dims | Phase 5: "**17 system dims** ... (`d.grab`…`d.role`, `vortex_core.cpp:50-69`) plus the `home`/`d.dims` genesis pair beside (`:35`,`:44`) — nineteen cells all told". Verified :35/:44/:50-69 (17 mints, d.grab→d.role). ✓ |
| m2 test homes | Phase 1 test-infra bullet + cross-cutting "Test homes, honestly named" — `Makefile:649` glob, `:692` explicit list, `:698` filter-out, `:1348-1350` all verified (see n1 for a boundary nit); `test/vagent-*` targets exist (:1158/:1165/:1173/:1176). ✓ |
| m3 `run` harness | Per-phase **Requires** lines + kernel-simulable/adapter-gated split; `run`'s minimal call convention is a priced phase-3 item (`vortex_vm.cpp:854-893` Call/Return verified); ScalarHarness at `tests/xuzz/vagent_compiler_test.cpp:22-53` (cited :18-46). ✓ |
| m4 reflect/validate | Phase 3 names all 16 non-mutation members — the ten `reflect` functions + `validate`/`weight_of`/`value_of`/`next_rand`/`run`/`reify` (spec table :1152-1167 = 16 rows). ✓ |
| m5 v1.18 | Header and cross-cutting now say v1.19. ✓ |
| n1 marker bit | Now cites `compiler.hpp:15-18` in phase 3 — but see n2 below. |
| n2 fragment glob | Now states "Kernel-glob TU ... (Makefile:643)" — but see M-grade wording issue below. |

## New-cite audit — lands clean

Every v2 cite I checked is accurate: `Makefile:643` (recursive
`find apps/common/xanadu` glob feeding `XUDU_CORE_OBJS`/`xuzz_test` :1000), `:649`
(`tests/xuzz` glob), `:688-690` (explicit `VAGENT_FRONTEND_SRCS`), `:692` (explicit test
list), `:698` (`filter-out` of all `vagent/` objects), `:1147` (vagent links no kernel),
`main.cpp:46-53`, `mesh_actor.cpp:659`, `mesh_actor.hpp:81`, `mesh_actor.cpp:473-477`
(the `executor_` callback inside `pumpOne`, which starts :460), `compiler.hpp:15-18`/`:23-26`,
`checker.cpp:2061-2067` (import allowlist), `:2069` (`builtinTypes`), `:765-779` (builtin
member-signature rows), `checker.hpp:12-27`/`:30`, `compiler.cpp:500-502` (preflight
"construct is not supported"), `:108-110` (baseline-check precedent), `:160-181`
(`binding()` render check + `externalReferences_`), `:769-772` (`dimConstant`/
`findOrMintDimension`), `:853-855` (`localCells_`), `:884-891` (`const Expr*`-keyed
tables), `vortex_vm.cpp:854-893` (Call+Return frame mechanism), `mesh_evaluator.cpp:735`
(one fresh `"cell"` marker per ref), `arena_manifold.cpp:78-105` (`ensureDimension`,
`arenaDims_` write at :89), `:289-290` (`attach()`'s `d.stores`/`d.store-refs` at :274+),
`dimension_registry.cpp:194-217` (`getOrCreate` rebuilds and move-assigns the manifold),
`vortex_core.cpp:14`/`:29-70`/`:35`/`:44`/`:50-69`, `vagent-agent-language.md:1670` (P4),
spec cites :2235-2243, :2335-2343, :2344-2357, :2397-2402, :2905-2907, :3031-3039,
:3057-3058, :3070-3072, :3109, :1050-1060, :1141-1144, :1166-1177, :1850-1893,
:2500-2504, :2311-2326, :2163-2185 — all land. `tests/samples/vagent/mini_*.vagent` = 23
files. No line exceeds 100 chars (mdformat-clean).

## Completeness re-audit — nothing dropped

§10.1 rows: parser/expand/resolve/frontend/ast.hpp/ast_clone/fragment/code_path/stdmeta/
checker.cpp/compiler.cpp/apps-vagent/adapter/Makefile — all mapped; the five
`VAGENT_FRONTEND_SRCS` joins all stated; checker items distributed across phases 1/2/3/4
with the run/value_of special form and allowlist+signature rows in the phase-3 `std:meta`
bullet. §10.2: P15 (all five call sites + four carve-outs + spawn-*and*-resume
registration pass per spec :2335-2337), P16 (marker mint, flag windows, no-interleave
:2171-2179, three-state flag, ref-record arm), seams (i)–(v) all present with (iii)'s
conditional phrased as the register has it (:3098-3099), P17 out. Disposition's M-A
unowned list: programRef record, both `setOpcodeKind` arms, (c)+bind set, genesis driver
hook, dead-`let`, reserved names (phase-1 `splice`/`static` + phase-3 `run`/`value_of` +
`src:`/`reify:`/`patch:` — matches §5.1's split), `closed`-flag chain
(compute→emit→re-verify across phases 2→3→4), no-marks minting — all scheduled.
U-M1–U-M11 out-of-scope list matches §15's eleven rows.

## MINOR

### m1 — "`d.op-label`/`or_parallel` the known members" rides a cite that doesn't name them

- **Plan ref:** Phase 5 filing bullet (line 322): "an unfiled session-minted dim accretes
  one `dimensionNamed`-invisible orphan per checkpoint — the honest residual
  (spec :2397-2402, `d.op-label`/`or_parallel` the known members)".
- **Code/spec:** spec :2397-2402 states the accretion mechanism but names no members.
  The membership claim is code-true — `mesh_value.cpp:473` mints `d.op-label` via
  `ensureDimension`; `or_parallel.cpp:181-184` mints `d.or`/`d.branch`/`d.frontier`/
  `d.submission` — but the plan attaches the naming to a cite that doesn't carry it.
- **Fix:** append the code cites — `(spec :2397-2402; today's unfiled session mints:
  `d.op-label`, `mesh_value.cpp:473`, and `or_parallel.cpp:181-184`'s four)`.

### m2 — `fragment.{hpp,cpp}`'s directory stated loosely; the placement is load-bearing

- **Plan ref:** Phase 3 bullet (lines 196-197): "Kernel-glob TU under
  `apps/common/xanadu/` — no Makefile edit (Makefile:643)"; exit (line 244):
  "`vagent_runtime_test` filter-outs all `vagent/` objects (Makefile:698) and cannot link
  this code."
- **Code/spec:** §10.1's tree (spec :2920, :2944) puts `fragment` under
  `apps/common/xanadu/vagent/` — where the sibling `cell_path.cpp`/`compiler.cpp` live.
  The plan's wording permits that reading, but also permits `apps/common/xanadu/` root,
  where the file would *escape* the `:698` filter-out (which removes only `vagent/`
  objects) and silently join `VAGENT_RUNTIME_CORE_OBJS` — likely breaking
  `vagent_runtime_test`'s link if `fragment.cpp` consumes `vagent/` symbols, and
  contradicting the plan's own "cannot link this code" rationale.
- **Fix:** name the directory exactly — "Kernel-glob TU under
  `apps/common/xanadu/vagent/` (the §10.1 tree) — no Makefile edit; the `:698` filter-out
  is why `vagent_runtime_test` can't link it".

### m3 — `reify:<src>:<serial>`'s shared-counter draw at replay is not stated

- **Plan ref:** Phase 5 (d) replay bullet (lines 336-344) and "Durable semantics"
  (lines 351-354) enumerate `patch:<n>`'s monotone serial/no-ABA but not the reify
  label's counter consumption.
- **Code/spec:** spec :2346-2348 — replay mints **no** `patch:<n>` serial, yet "the
  install copy's own `reify:<src>:<serial>` label still mints off the same counter" —
  and :2505-2507 — `reify`'s `<serial>` comes from the *durable per-agent* `d.vars`
  counter "so reified labels never collide with a promoted registration's after
  `resume`". A replay that re-derives `<serial>` per-incarnation collides after resume —
  the same class of durable invariant the bullet exists to pin down.
- **Fix:** add to (d) or the durable bullet: "replayed installs draw `reify:<src>:<serial>`
  off the same durable `d.vars` counter (`patch:<n>` consumes none — spec :2346-2348,
  :2505-2507)".

## NIT

### n1 — `Makefile:1348-1350` off by one

`zigzag_test` runs at `:1351`, outside the cited range (`test:` :1348, gleditor :1349,
xuzz :1350). Claim's substance is verified; cite should read `:1348-1351`. (The swarm
script at :1352-1355 is a shell test, not a gtest binary, so "runs ... only" holds for
the stated point.)

### n2 — `CompilerBinding` marker bit still referenced, never scheduled (r1 n1 residue)

Phase 3 cites the bit as the `closed` flag's "provenance" (`compiler.hpp:15-18`, a
two-field struct today); phase 2's `cap$N` `CompilerBindings` emission implies its
addition but no bullet says so. Spec :2958-2959 lists it as a `compiler.cpp` addition.
One clause in phase 2 would close it.

### n3 — Phase 5 (c) overlabels the letter

Spec's lettered (c) is "`programRef → the program root` bind" only (:2344-2345); the
agent cell/apparatus dims/`validate` constants are the adapter's V10 bind sweep
(:1053-1060), not lettered genesis steps. The plan's "(c) `programRef` → program root
bind — the phase-3 bind set's real arm: root, agent cell, ..." folds the whole set into
the letter — the work is covered (per the disposition's grouping) but a strictly-lettered
acceptance test would find (c) narrower.

### n4 — "kernel tables" misnomer for the Emitter's per-compile tables

Phase 2 (lines 151-153): "kernel tables key on `const Expr *` (`compiler.cpp:884-891`)" —
those are `Emitter`'s per-`compileStatement` tables in `compiler.cpp`'s anonymous
namespace, not kernel/VM state. The pointer-keyed claim lands; the name is loose.

## Coverage matrix (§10.1/§10.2 → v2) — all rows closed or noted

| Row | Phase | Status |
| --- | --- | --- |
| parser.cpp | 1 | ✓ (:848-855 verified) |
| expand / resolve / frontend / ast_clone / code_path | 1/0/0/0/1 | ✓ all five Makefile joins stated |
| ast.hpp | 1 (+0 ordering note) | ✓ |
| fragment | 3 | ✓ modulo m2's directory wording |
| checker.cpp | 1/2/3/4 | ✓ all items mapped |
| compiler.cpp | 2/3 | ✓ modulo n2's unscheduled bit |
| stdmeta prelude | 3/4 | ✓ read/write split per I2 |
| apps/vagent | 1 | ✓ rewiring named |
| adapter | 3/5/6 | ✓ incl. programRef record, bind set, (a)–(e) driver, all five seams |
| Makefile row | 0/1 | ✓ |
| P15/P16/seams | 4/6/6 | ✓ spawn+resume arms stated; (iv) scheduled |
| P17 | out | ✓ |
