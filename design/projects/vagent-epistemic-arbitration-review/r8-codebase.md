# Round 8 critique: Codebase Expert (P19)

Reviewed immutable `r8-input-v1.4.txt` against repository HEAD `64b05125` in `/data/git/gleditor`.
P19.1–P19.8 approval is treated as design approval, not implementation evidence.

Tally: **0 BLOCKING, 1 MAJOR, 1 MINOR.**

## Status of previous BLOCKING/MAJOR findings

- R7 findings: **RESOLVED** — R7 recorded zero BLOCKING and zero MAJOR findings; the R6 issues it
  tracked were resolved in the candidate.

## BLOCKING

None.

## MAJOR

### 1. (§8) Frontend targets and the missing migration hook

Full title: Proposal register points at the retired frontend and omits the concrete migration hook

**Problem:** The integrated actor frontend and belief implementation live under `vagent/runtime/`,
but P19.1, P19.4 and P19.5 name `vagent/compiler.cpp` and P19.4 names the bounded frontend's
lexer/parser/checker files. P19.1 also calls for runtime/reconciler integration without naming the
existing reconciliation path whose behavior P19 must extend. This leaves the approved implementation
register pointing implementers at the preserved bounded frontend and does not identify the resume
migration that must carry the new per-source epistemic records. This is a material register
correction, not a request to re-open the approved P19 scope.

**Evidence:** The brief distinguishes the actor frontend at `apps/common/xanadu/vagent/runtime/`
from the preserved bounded frontend. In this tree, runtime predicate declarations are minted by
`apps/common/xanadu/vagent/runtime/compiler.cpp:3963–3990`, and belief changes are emitted at
`:4545–4576`. The proposal register instead lists `vagent/compiler.cpp` at snapshot lines 724,
727–728. `AgentExecutor::restoreState` in `apps/common/xanadu/vagent/runtime/agent_executor.cpp`
rebuilds beliefs at `:3233–3247`: it copies each old predicate's clause under a same-named new
predicate using `cloneGraph` with `termFrom`/`termInto` defined at `:3200–3203`; that copy set
contains term topology and `d.source`, but not `d.epistemic`. The generic live-upgrade path in
`apps/common/xanadu/vortex/mesh_actor.cpp:683–731` calls a caller-provided `StateReconciler` at
`:718–722`; it does not provide an epistemic-field migration policy itself.

The runtime belief base is already source-aware: compiler comments specify clause facts on
`d.clause` and source-name cells on `d.source` (`runtime/compiler.cpp:3947–3951`); `rtBSet` removes
only the replacing source for keyed assertions (`:4116–4121`, `:4137–4167`), and tests cover
same-key replacement and preserving another source (`tests/xuzz/vagent_belief_test.cpp:412–430`,
`:470–486`). But the represented source is a string name: `rtBSet` receives a source name
(`compiler.cpp:4116–4121`), while tells derive it from `envelope.from`
(`runtime/agent_executor.cpp:1107–1115`) and store it as the source of the fact. That is not the
authenticated source-incarnation and per-observation identity required by P19's record schema
(snapshot lines 81–84, 107–111). The resume migration therefore needs to match/transform the
existing source-aware clauses and add/migrate P19 metadata explicitly; copying only current source
name strings is insufficient.

**Fix:** Amend P19.1, P19.4 and P19.5's file lists to target
`vagent/runtime/{runtime_abi,compiler,agent_executor}.{hpp,cpp}` as applicable, and explicitly name
`AgentExecutor::restoreState` as the resume migration path. Keep `MeshActorRuntime::upgrade`'s
caller-supplied `StateReconciler` as the separate live-upgrade seam; state which P19 adapter owns
that reconciliation and list any `mesh_actor` changes only if generic runtime behavior must change.
Specify migration from a shared fact clause/source-name rank to keyed per-source observations and
authenticated source-incarnation records, including how multiple observations coalesced on one
clause retain independent identity. P19.4's new syntax must be lowered by the runtime compiler; do
not direct it to the bounded `vagent/` frontend.

## MINOR

- Snapshot §1's baseline table at lines 23–25 says the actor executor/runtime ABI/reconciler are
  absent and does not distinguish the already-integrated callback-based live-upgrade API from a
  future P19 epistemic reconciler. The following paragraph partly repairs this by noting the runtime
  is integrated (lines 28–37), but the table remains confusing. Label the actor runtime and generic
  upgrade callback as built, and the epistemic migration/reconciler as future P19 work.
- The table row at line 23 groups “Belief database and isolated SLD solver” as one unbuilt
  prerequisite. The runtime has a compiled ground-fact belief base with source coexistence
  (`runtime/compiler.cpp:3947–3951`, `:4116–4208`; `tests/xuzz/vagent_belief_test.cpp:274–350`),
  while rule resolution is a separate capability. Split the row so the baseline does not imply
  ground beliefs themselves are absent.

VERDICT: **NOT CONVERGED**
