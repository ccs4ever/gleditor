# Vortex I/O M1–M6 code review

Target: the implementation of M1–M6 of
[`design/vortex-io-implementation-plan.md`](../../../vortex-io-implementation-plan.md), diff
`389d21ce..<target>`.

Workflow brief: [`brief-common.md`](brief-common.md).

## Round summary

| Round | Target commit | Critics | BLOCKING | MAJOR | MINOR | Status                                                       |
| ----- | ------------- | ------- | -------- | ----- | ----- | ------------------------------------------------------------ |
| 1     | `757cd55b`    | 4       | 2        | 15    | 25    | Fixed (A, B)                                                 |
| 2     | `dbe8e40c`    | 4       | 0        | 4     | 13    | Fixed ([disposition](r2-disposition.md)); vortex CONVERGED   |
| 3     | `e16ec377`    | 4       | 1        | 4     | 8     | Fixed ([disposition](r3-disposition.md))                     |
| 4     | `720fef61`    | 4       | 0        | 0     | 4     | CONVERGED; MINORs applied ([disposition](r4-disposition.md)) |

## Latest role convergence

| Role      | Latest report                        | Verdict   | BLOCKING / MAJOR |
| --------- | ------------------------------------ | --------- | ---------------- |
| vortex    | [`r4-vortex.md`](r4-vortex.md)       | CONVERGED | 0 / 0            |
| engineer  | [`r4-engineer.md`](r4-engineer.md)   | CONVERGED | 0 / 0            |
| architect | [`r4-architect.md`](r4-architect.md) | CONVERGED | 0 / 0            |
| zigzag    | [`r4-zigzag.md`](r4-zigzag.md)       | CONVERGED | 0 / 0            |

## Open proposals

Applied: P1, P3, P4, P5, P6 and P7. Not needed: P2. Nothing is open.

- P7: one ZigZag cell-deletion algorithm (`zigzag/cell_deletion`, `ArenaManifold::deleteCell`):
  break connections, remove, leave broken or repair; shared by xuzz delete, the I/O reapers and the
  VQL script program reclamation; cursor move-off rule. Applied with owner approval; see
  [`../m7-vql/p7-cell-deletion.md`](../m7-vql/p7-cell-deletion.md).
- P6: a language-neutral `Submit` opcode in `VortexVM` (`vortex_vm.{hpp,cpp}` only), with a submit
  hook beside the await hook. Applied with owner approval: two inputs (capability, request
  description), three outputs (submission sum, arm ordinal, arm payload), once per executed
  instruction, refusal a value, host exceptions contained. VQL effect clauses lower to it
  (`MakeTerm`, `Submit`, `BranchFalseCell`, `Await`); see the plan's "P6: the Submit opcode".
- P4: per-cell `ArenaManifold` release (`freeCell`) and the request reaper that uses it. Applied
  with owner approval; see [`p4-p5-implementation.md`](p4-p5-implementation.md).
- P5: the `Append` intent fsync moved off the owner into the append's worker turn, with
  `capture.journal` version 4. Applied with owner approval; same document.
