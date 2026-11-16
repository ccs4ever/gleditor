# Round 5 disposition (performance-architecture-proposals.md)

Reviewed v5. Vortex, ergonomics and architecture converged with zero findings. Codebase verification
confirmed the corrected dense lookup and undo lifecycle, but found additional P5 vector consumers.
The target is revised to v6 solely to complete that migration scope.

| Finding                                                     | Ruling   | Resolution in v6                                                                                                                                                                                                                                                                                                                                                |
| ----------------------------------------------------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| codebase M1: cross-module explicit-vector consumers missing | Accepted | Add `apps/common/xanadu/vql/compiler.cpp:1392,1410` and `tests/xuzz/vortex_core_test.cpp:353,358` to P5's migration and register. A broader `rg` inventory also found `apps/common/xanadu/vpl/compiler.cpp:1341,1361`; include it in the same ruling. All three files use indexing/iteration and switch to `auto`/`OperandList` without allocating conversions. |

No runtime code changed. The correction adds concrete caller files to an existing proposal and does
not change P1/P2 ownership, lookup semantics, or the operand representation. Focused round 6
verifies this last codebase finding; other converged reviewers carry forward under the workflow's
focused round rule. Final writer and ZigZag reviews check the accumulated text and invariants.

## Final minor polish

Both writer findings are accepted in v6: P2 indexes at `opIndex - 1` after rejecting operation zero,
so no extra index-zero slot is priced. Memory is stated per allocated capacity, initially `opCount`.
The prose and U-P2 now use the same strict comparison and sparse overflow boundary.
