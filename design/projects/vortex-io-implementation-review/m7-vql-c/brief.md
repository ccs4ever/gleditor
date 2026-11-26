# M7-VQL-c gate brief

## Target and scope

- Repository: the current worktree, branch `vortex-io-m5-m6`.
- Target: the implementation of M7-VQL-c (VQL write and capture), diff `4d132ccf..<target>` under
  `apps/`, `tests/` and `design/`. The round's target commit is recorded in
  [`README.md`](README.md).
- Contract: [`design/vortex-io-m7-vql-c.md`](../../../vortex-io-m7-vql-c.md), including its owner
  decisions (§20, binding) and its "as built" sections (§18.1–§18.6, recorded scope, but a deviation
  that breaks an acceptance criterion or is described falsely is a finding);
  [`design/vortex-io-implementation-plan.md`](../../../vortex-io-implementation-plan.md) §1–§13;
  [`design/vql-query-language.md`](../../../vql-query-language.md) §4.13.
- Method: the code-review adaptation of the vortex-design-critique workflow in
  [`../m1-m6/brief-common.md`](../m1-m6/brief-common.md). Read it for severities, the report format
  and the round protocol. Primary question: correctness.

## Critics

| role       | lane                                                                                                                                                       |
| ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `vql`      | VQL expert: surface syntax and semantics, compile lowering, diagnostics, persist (coverage) rule, vquery CLI and REPL (`:open`, `:captures`, `:reconcile`) |
| `vortex`   | Vortex expert: owner/worker discipline, request/waiter/step lifecycle, same-request resume, abandon-and-pin, capture journal ordering, reapers             |
| `engineer` | Software engineer: concurrency, lifetimes, crash ordering and fsync, write grants and path safety, budgets and bounds, leaks, test validity                |
| `zigzag`   | Xanadu/ZigZag purist: cells and dimensions, owned records, P7 deletion reuse, permascroll policy, provenance, R6/R7/R8                                     |

Report files: `r<N>-<role>.md` in this directory, in the format of the common brief, heading
`# Round <N> critique: <Role Name> (M7-VQL-c code)`, ending `VERDICT: CONVERGED | NOT CONVERGED`.
Core files (`store.hpp`, `ops.hpp`, `compact_op.hpp`, `store_tables.hpp`, `segmented_ops_spool`,
`manifold`, `arena_manifold`, `vortex_core`, `vortex_vm`) need owner approval: a fix that needs one
is a numbered proposal `P10+`.
