# Performance architecture proposals review

Target: [performance-architecture-proposals.md](../../performance-architecture-proposals.md), v8.
Status: **CONVERGED after the C++ performance follow-up**. Round 8 has zero BLOCKING, MAJOR or MINOR
findings and closes all six round-7 issues. The original six-role workflow converged through round
6; the additional expert reviewed the v7 corrections. See the
[latest expert critique](r8-cpp-performance.md) and [final disposition](r8-disposition.md).

V8 adds the owner's requested release ThinLTO+PGO pipeline, including training/profile-use stages
and held-out validation. The round-8 expert verdict below applies to v7; this owner amendment has
not been through an additional expert round.

## Round history

| Round | Review scope                                              | Converged in round               | Disposition                  |
| ----- | --------------------------------------------------------- | -------------------------------- | ---------------------------- |
| 1     | All six reviewers, initial proposal                       | 0/6                              | [Round 1](r1-disposition.md) |
| 2     | All six, revised proposal                                 | 4/6                              | [Round 2](r2-disposition.md) |
| 3     | Vortex and ergonomics, v3                                 | 0/2                              | [Round 3](r3-disposition.md) |
| 4     | Vortex, ergonomics and affected architecture, v4          | 1/3                              | [Round 4](r4-disposition.md) |
| 5     | Affected reviewers plus final writer/ZigZag checks, v5/v6 | 5/6                              | [Round 5](r5-disposition.md) |
| 6     | Focused codebase verification of final P5 scope, v6       | 1/1; all six latest converge     | [Round 6](r6-disposition.md) |
| 7     | Independent C++ performance expert, v6                    | 0/1; design reopened             | [Round 7](r7-disposition.md) |
| 8     | C++ performance follow-up, v7                             | 1/1; all round-7 findings closed | [Round 8](r8-disposition.md) |

Follow-up rounds rerun outstanding reviewers and reviewers whose domains materially changed. A
converged verdict carries forward where the subsequent edit does not alter that domain.

## Latest recorded verdicts

| Role                           | Latest critique                  | BLOCKING | MAJOR | MINOR      | Verdict   |
| ------------------------------ | -------------------------------- | -------- | ----- | ---------- | --------- |
| Technical Writer               | [Round 5](r5-writer.md)          | 0        | 0     | 2, applied | CONVERGED |
| Vortex and Spool-Mesh          | [Round 5](r5-vortex.md)          | 0        | 0     | 0          | CONVERGED |
| Codebase                       | [Round 6](r6-codebase.md)        | 0        | 0     | 0          | CONVERGED |
| System Architect               | [Round 5](r5-architect.md)       | 0        | 0     | 0          | CONVERGED |
| Language Design and Ergonomics | [Round 5](r5-ergo.md)            | 0        | 0     | 0          | CONVERGED |
| Xanadu/ZigZag                  | [Round 5](r5-zigzag.md)          | 0        | 0     | 0          | CONVERGED |
| C++ Performance Expert         | [Round 8](r8-cpp-performance.md) | 0        | 0     | 0          | CONVERGED |

## Independent performance pass

[Round 7](r7-cpp-performance.md) reopened v6 with one blocking, two major and three minor findings.
The [round-7 disposition](r7-disposition.md) records source verification and the applied v7 fixes.
[Round 8](r8-cpp-performance.md) independently verified every fix and converged without findings.

P3 preserves local-only quote routing even when values change to ExternRef. P1 now specifies
first-write payload snapshots per rollback boundary, including nested commit/rollback and ownership
costs. P4 no longer predicts release savings from instrumented self time; v8 also requires release
ThinLTO and PGO with separate training and validation workloads. Allocation, compaction,
mapping-density and inline/LTO measurements are explicit future implementation gates.

Original-role verdicts above are historical; those six reviewers were not rerun on v7. The
owner-requested follow-up reran the C++ performance expert until its findings were closed.

## Result and remaining decisions

The continued review resolved undo ownership and allocation-free rollback, owning-table reset,
root-eviction retention, dense lookup presence, complete operand consumer migration, and opcode
classification precedence. The direct instruction-only lookup is deferred as U-P4 until its
precondition and measured benefit are established.

The owner has accepted P1-P5, including release ThinLTO+PGO. The
[implementation plan](../../performance-architecture-implementation-plan.md) records execution order
and its
[seven-role Luna critique](../performance-architecture-implementation-plan-review/README.md), which
converged on implementation-plan v2. Section 6 contains future implementation acceptance gates. This
review changed documentation only; it did not run runtime tests or produce new benchmark
measurements. The review files and proposal pass the repository's Markdown formatter, Markdown lint
and whitespace checks.
