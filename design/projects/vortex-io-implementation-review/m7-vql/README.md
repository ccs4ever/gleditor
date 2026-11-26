# M7-VQL gate: Vortex I/O review

The M7-VQL milestone (script-grammar `effect` statements over the Vortex I/O host) was gated on two
expert reviews, a VQL expert and a Vortex expert, repeated until both approved with zero BLOCKING
and zero MAJOR findings. **The gate passed in round 3.**

| round | target     | VQL expert                   | Vortex expert                | outcome             |
| ----- | ---------- | ---------------------------- | ---------------------------- | ------------------- |
| r1    | `8711aa1c` | 0 BLOCKING, 1 MAJOR          | 0 BLOCKING, 3 MAJOR          | NOT CONVERGED       |
| r2    | `be30c946` | 0 BLOCKING, 1 MAJOR          | 0 BLOCKING, 0 MAJOR          | VQL fixed; APPROVED |
| r3    | `183129f5` | 0 BLOCKING, 0 MAJOR, 2 MINOR | 0 BLOCKING, 0 MAJOR, 3 MINOR | **GATE PASSED**     |

## Reports and dispositions

- Round 1: [`r1-vql.md`](r1-vql.md), [`r1-vortex.md`](r1-vortex.md),
  [`r1-disposition.md`](r1-disposition.md)
- Round 2: [`r2-vql.md`](r2-vql.md), [`r2-vortex.md`](r2-vortex.md),
  [`r2-disposition.md`](r2-disposition.md)
- Round 3: [`r3-vql.md`](r3-vql.md), [`r3-vortex.md`](r3-vortex.md),
  [`r3-disposition.md`](r3-disposition.md)

## Proposals applied during the gate

- **P6, the `Submit` opcode** (`OpcodeKind::Submit`): a script action's admission is a VM
  instruction, executed once per reached action; the rollback notice (`setSubmitRollbackHandler`)
  tells the host before the cells go. See the M7-VQL notes in
  [`vortex-io-implementation-plan.md`](../../../vortex-io-implementation-plan.md).
- **P7, one cell-deletion algorithm**: [`p7-cell-deletion.md`](p7-cell-deletion.md).
  `ArenaManifold::deleteCell` and `detachCell` serve xuzz deletions and the engine's reclamation
  alike.
