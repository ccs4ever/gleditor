# M7-VQL-d2 gate brief

## Target and scope

- Repository: the current worktree, branch `vortex-io-m5-m6`.
- Target: M7-VQL-d2, core proposal P11 (a quote that admits scratch separator spans, so a multi-row
  `join` of a loaded store's rows keeps each row's transclusion), diff `7ac9ed6f..<target>` under
  `apps/`, `tests/` and `design/`. The round's target commit is in [`README.md`](README.md).
- Contract: [`design/vortex-io-m7-vql-d2.md`](../../../vortex-io-m7-vql-d2.md) (design and
  as-built), P11 and §6.1b of
  [`design/vortex-io-m7-vql-roadmap.md`](../../../vortex-io-m7-vql-roadmap.md), and
  [`design/store-slice-convergence.md`](../../../store-slice-convergence.md) R6–R8, R12.
- Core file: `arena_manifold.{hpp,cpp}` is changed under the owner's approval of P11. Any other core
  file change (`store.hpp`, `ops.hpp`, `compact_op.hpp`, `store_tables.hpp`, `segmented_ops_spool`,
  `manifold`, `vortex_core`, `vortex_vm`) is a finding; a fix that needs one is a numbered proposal
  `P13+`.
- Method: the code-review adaptation of the vortex-design-critique workflow in
  [`../m1-m6/brief-common.md`](../m1-m6/brief-common.md): severities, report format and round
  protocol. Primary question: correctness.

## Critics

| role     | lane                                                                                                                                   |
| -------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `zigzag` | Xanadu/ZigZag purist: transclusion identity, namespaces, scratch scroll policy, `promote()` as the one road, permascroll growth, R6–R8 |
| `vortex` | Vortex expert: arena invariants (mark/release, ephemeralBit, refs mapping), join host relations, owner discipline, bounds and leaks    |

Report files: `r<N>-<role>.md` in this directory, heading
`# Round <N> critique: <Role Name> (M7-VQL-d2 code)`, ending `VERDICT: CONVERGED | NOT CONVERGED`.
