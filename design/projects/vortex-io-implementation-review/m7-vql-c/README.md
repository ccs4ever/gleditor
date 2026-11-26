# M7-VQL-c gate: Vortex I/O review

Brief: [`brief.md`](brief.md).

| round | target     | VQL   | Vortex | engineer | zigzag | outcome                                                                                        |
| ----- | ---------- | ----- | ------ | -------- | ------ | ---------------------------------------------------------------------------------------------- |
| r1    | `dcce529b` | 0/1/4 | 0/1/4  | 0/2/7    | 0/2/7  | NOT CONVERGED; fixed per [`r1-disposition.md`](r1-disposition.md)                              |
| r2    | `cb194c92` | 0/0/1 | 0/1/2  | 0/0/3    | 0/1/3  | NOT CONVERGED (VQL and engineer CONVERGED); fixed per [`r2-disposition.md`](r2-disposition.md) |

| r3 | `e2b23837` | 0/0/1 | 0/0/2 | (r2) | 0/0/0 | **GATE PASSED**; MINORs fixed per
[`r3-disposition.md`](r3-disposition.md) |

**Outcome: GATE PASSED** in round 3. Every lane is CONVERGED with zero BLOCKING and zero MAJOR; the
engineer was not re-run, being CONVERGED in r2.

| role     | last round | tally | verdict   |
| -------- | ---------- | ----- | --------- |
| VQL      | r3         | 0/0/1 | CONVERGED |
| Vortex   | r3         | 0/0/2 | CONVERGED |
| zigzag   | r3         | 0/0/0 | CONVERGED |
| engineer | r2         | 0/0/3 | CONVERGED |

Summary: three rounds, final target `e2b23837`; the r3 MINORs were fixed as gate polish.
