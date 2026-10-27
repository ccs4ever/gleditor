# Round 9 critique: Codebase Expert (P19)

Reviewed immutable `r9-input-v1.5.txt` (SHA-256
`229e8434f752c9e2fa9e1394f6e79549d005279f180f7356325588b63ec90147`) against runtime baseline
`64b05125` in `/data/git/gleditor`.

Tally: **0 BLOCKING, 0 MAJOR, 0 MINOR.**

## Status of previous BLOCKING/MAJOR findings

- R8.1 (proposal register targets bounded frontend and omits migration hook): **RESOLVED** — §8 now
  targets `vagent/runtime/` and names `AgentExecutor::restoreState` plus a shared runtime translator
  composed with the host upgrade callback (snapshot lines 192–232, 923–937).
- R8.1 (source identity and per-observation ownership): **RESOLVED** — §2.1 preserves coalesced
  clauses, gives each source support its own observation history and identity, explicitly treats
  source names as aliases, and defines retry/current/retired semantics (lines 94–125).

## BLOCKING

None.

## MAJOR

None.

## MINOR

None. The corrected current-baseline table matches the implementation: the actor AST/compiler and
source-name ground-fact support exist, the actor query path is ground-fact matching rather than Horn
rule resolution, checkpoint restore is a fixed-dimension/name-matched copier, and `MeshActorRuntime`
provides a caller-supplied reconciliation callback. The candidate keeps the future epistemic
translator distinct from those existing capabilities (§1, lines 24–45).

The AST ownership is accurate: `EBelieves` and the belief form of `EFor` are in
`apps/common/xanadu/vagent/runtime/ast.hpp:184–195`; `PredicateDecl` is there at `:323–332`. The
proposed extensions are explicitly assigned to this runtime AST/checker/compiler, with bounded
`check` unchanged (snapshot lines 431–45, 489–500; proposal register lines 933–934).

Migration is adequately specified as future P19 work. Existing resume uses fixed term dimensions and
name-matches predicates
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3200–3203,3233–3247`); the generic upgrade
call invokes its supplied reconciler before swapping context
(`apps/common/xanadu/vortex/mesh_actor.cpp:718–731`). The candidate names those two distinct paths,
requires one runtime-owned translator with a complete ref map, descriptor/full-key compatibility,
owned-edge reconstruction, preserved source/observation history, epoch expiry and atomic refusal,
and does not claim the full P17 upgrade pipeline already exists (snapshot lines 192–250).

The source-observation migration resolves the coalesced-clause concern: existing `rtBSet` coalesces
same terms and keeps source support cells
(`apps/common/xanadu/vagent/runtime/compiler.cpp:4116– 4208`); the candidate gives each support an
independent observation history, distinguishes event retries from new same-value observations, and
retains removed support history under calibration state (snapshot lines 94–125). Legacy facts
lacking authenticated bindings or complete metadata are explicitly unscored rather than assigned
invented provenance (lines 105–110).

VERDICT: **CONVERGED**
