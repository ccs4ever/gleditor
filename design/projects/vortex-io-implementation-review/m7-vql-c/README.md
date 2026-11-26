# M7-VQL-c gate: Vortex I/O review

Brief: [`brief.md`](brief.md).

| round | target     | VQL   | Vortex | engineer | zigzag | outcome                                                                                        |
| ----- | ---------- | ----- | ------ | -------- | ------ | ---------------------------------------------------------------------------------------------- |
| r1    | `dcce529b` | 0/1/4 | 0/1/4  | 0/2/7    | 0/2/7  | NOT CONVERGED; fixed per [`r1-disposition.md`](r1-disposition.md)                              |
| r2    | `cb194c92` | 0/0/1 | 0/1/2  | 0/0/3    | 0/1/3  | NOT CONVERGED (VQL and engineer CONVERGED); fixed per [`r2-disposition.md`](r2-disposition.md) |

Role convergence after r2: VQL and engineer CONVERGED; Vortex and zigzag each held one MAJOR (both
fixed in r2), so round 3 should re-check those two lanes.
