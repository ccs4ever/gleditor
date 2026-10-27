# Round 8 critique: Technical Writer (r8-input-v1.4.txt)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 1 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R7 writer: no BLOCKING or MAJOR findings; the R6 tie, adjudication, and delivery wording items
  remain resolved in v1.4 (§4 and §6.3, snapshot lines 330–334 and 617–659).

## BLOCKING

None.

## MAJOR

### 1. (§1, §9) Baseline statuses contradict the integrated runtime

Full title: Baseline table and lifecycle status contradict integrated baseline

**Problem:** The top-level status correctly calls this an approved but unimplemented P19 design, and
lines 30–32 explain that runtime commit `46402f26` is integrated. However, the baseline table still
says the actor executor/runtime ABI/reconciler are absent and P17/P18 are only historical
prerequisites (lines 24–25). The open-boundaries table then says full actor operation is not
implemented and lists the executor as a prerequisite (line 798). These statuses describe the
original `5ee5f6ff` review input, not the R8 baseline `64b05125`. This is a material internal
contradiction for implementers, even though the surrounding paragraph partly corrects it.

**Evidence:** R8 brief names `64b05125` as the runtime baseline.
`design/vagent-agent-language.md:60–67` states that `46402f26` is integrated, names the distinct
runtime frontend, and says shared-program bases, live upgrade, frames, turns, beliefs, effects, and
checkpoint/resume have runtime tests, while P18/P19 remain unimplemented.
`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3145–3156` documents the implemented
checkpoint/resume migration boundary. `tests/xuzz/vagent_upgrade_test.cpp:3–10` identifies tests for
shared program store, upgrade/reconciler and rollback; `tests/xuzz/vagent_resume_test.cpp:3–4`
covers checkpoint, resume and migration.

**Fix:** Replace the stale rows with a baseline table distinguishing: integrated runtime mechanisms
(cite runtime files/tests); P18 design absent but runtime ground-belief features present (cite
`runtime_abi.hpp` and belief tests); and P19 features still owed. Rewrite §9's full-actor row to say
the actor runtime is integrated while P19 arbitration remains unimplemented and requires
reconciliation against the integrated runtime. Keep the current approval-versus-implementation
distinction.

### 2. (§8, lines 724, 727–728, 731) Proposal paths name the wrong vagent frontend

**Problem:** The proposal register points P19 surface syntax and parts of verification/query
lowering to `vagent/{token,lexer,ast,parser,checker,compiler}` or `vagent/compiler.cpp` without the
`runtime/` directory. On the R8 baseline these are the preserved bounded frontend and scalar
compiler. The integrated actor runtime has its own AST, parser, checker, compiler and runtime ABI.
The brief explicitly says `check` uses the former and `check-runtime`/`run` use the latter. As
written, an implementer can add P19 syntax to the frontend that does not run agents, or miss the
runtime graph lowering boundary. P19.8's generic `vagent/compiler.cpp` reference has the same
ambiguity.

**Evidence:** `design/vagent-agent-language.md:60–65` identifies the two frontends and their
distinct commands. `apps/common/xanadu/vagent/runtime/compiler.cpp:1–4` is the actor compiler
implementation, while `apps/common/xanadu/vagent/compiler.cpp:1` includes the separate
`common/xanadu/vagent/compiler.hpp`. Runtime ABI dimensions and existing belief fields are in
`apps/common/xanadu/vagent/runtime/runtime_abi.hpp:48–69`; P19's proposed dimensions must
extend/reconcile that ABI. The proposal register's ambiguous paths are at snapshot lines 724,
727–728, and 731.

**Fix:** Name exact runtime paths
(`vagent/runtime/{token,lexer,ast,parser,checker,compiler}.{hpp,cpp}`, `runtime/runtime_abi.hpp`,
and `runtime/agent_executor.*` where applicable), and separately list any legacy-front-end
compatibility work only if intended. Update P19.1/P19.5/P19.8 entries and identify which parts are
compiler lowering versus host/executor authorization. Use repository-root-relative paths or
explicitly qualify the paths consistently.

## MINOR

- (§1, line 30) “The companion and its review history are imported” is ambiguous because the
  previous sentence refers to both the companion design and its review history, while R8 brief names
  a specific P17 companion. Prefer naming `design/vagent-shared-program-and-live-upgrade.md` and its
  review record explicitly; retain the separately stated historical P18 limitation.

VERDICT: NOT CONVERGED
