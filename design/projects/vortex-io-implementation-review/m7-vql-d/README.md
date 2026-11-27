# M7-VQL-d gate: Vortex I/O review

Brief: [`brief.md`](brief.md). Critics: VQL, Vortex and zigzag (per the roadmap).

| round | target     | VQL   | Vortex | zigzag  | outcome                                                                                                                                |
| ----- | ---------- | ----- | ------ | ------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| r1    | `301b9a8d` | 0/2/4 | 0/2/3  | 0/2/3   | NOT CONVERGED; fixed per [`r1-disposition.md`](r1-disposition.md)                                                                      |
| r2    | `09635a6c` | 0/1/0 | 0/1/1  | not run | NOT CONVERGED; fixed per [`r2-disposition.md`](r2-disposition.md)                                                                      |
| r2z   | `7dc93822` | -     | -      | 0/1/3   | NOT CONVERGED (zigzag round 2); fixed per [`r3-disposition.md`](r3-disposition.md)                                                     |
| r3    | `7dc93822` | 0/1/0 | -      | -       | NOT CONVERGED; fixed per [`r3-disposition.md`](r3-disposition.md)                                                                      |
| r3z   | `57be2657` | -     | -      | 0/2/2   | NOT CONVERGED (zigzag round 3); resolved per [`r4-disposition.md`](r4-disposition.md) (owner decision: reclamation moved to M7-VQL-d3) |
| r3v   | `57be2657` | -     | 0/2/2  | -       | NOT CONVERGED (Vortex round 3); resolved per [`r4-disposition.md`](r4-disposition.md) (owner decision: reclamation moved to M7-VQL-d3) |
| r4    | `2e9e4178` | 0/0/0 | 0/0/2  | 0/0/1   | **GATE PASSED**; MINORs fixed per [`r4-polish.md`](r4-polish.md)                                                                       |

**Outcome: GATE PASSED** in round 4. Every lane is CONVERGED with zero BLOCKING and zero MAJOR. The
owner decision of round 4 (engine memory reclamation moved to M7-VQL-d3) is what let the round
converge: slice d ships no reclaim of plain programs.

| role   | last round | tally | verdict   |
| ------ | ---------- | ----- | --------- |
| VQL    | r4         | 0/0/0 | CONVERGED |
| Vortex | r4         | 0/0/2 | CONVERGED |
| zigzag | r4         | 0/0/1 | CONVERGED |

Summary: four rounds, final target `2e9e4178`; the r4 MINORs were fixed as gate polish.
