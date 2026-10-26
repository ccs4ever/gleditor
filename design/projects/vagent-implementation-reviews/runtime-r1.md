# Shared Vortex runtime review — R1

**Verdict: CONVERGED.** I found no actionable regression in the reviewed shared-runtime changes for
P1 ChoiceDepth, P12 Dewey opt-in, P13 trail cleanup, and the requested numeric correctness fixes.
Review scope was the shared Vortex VM, ArenaManifold, mesh artifact boundary, and their focused
tests; I did not review the in-progress Vagent compiler files or require implementation of unbuilt
language/runtime features.

## Checks

- **Choice depth and artifact boundary:** `ChoiceDepth` is appended to the shared opcode enum and
  reads the live `choiceStack_` size (`apps/common/xanadu/vortex/vortex_vm.hpp:78-88`,
  `apps/common/xanadu/vortex/vortex_vm.cpp:1045-1047`). The mesh pure-op validator rejects it, and
  the artifact decoder retains the previous `Cut` maximum, so neither newly minted nor forged
  artifacts can execute this stateful opcode
  (`apps/common/xanadu/vortex/mesh_evaluator.cpp:233-234,472-473`). This preserves the pure
  evaluator's boundary.
- **Dewey opt-in:** Branch path allocation and restoration now occur only for an active nonempty
  oracle path or a registered cut callback
  (`apps/common/xanadu/vortex/vortex_vm.cpp:184-189,202-210`). The OR-parallel worker setup installs
  its callback before execution, including the `setCurrentDeweyPath` shadow-delta path
  (`apps/common/xanadu/vortex/or_parallel.cpp:350-364`); victim tests likewise register before
  creating choice points (`tests/xuzz/transport_and_or_parallel_test.cpp:435-436,546-547`). Ordinary
  non-parallel choices no longer build unused Dewey vectors. The public header warns that installing
  a callback after choices exist cannot reconstruct branch paths
  (`apps/common/xanadu/vortex/vortex_vm.hpp:132-135`).
- **Arena trail lifetime:** Discarding an inner mark preserves trail entries while an enclosing mark
  remains; discarding the last mark clears records while retaining committed state
  (`apps/common/xanadu/zigzag/arena_manifold.cpp:1595-1608`). The tests cover nested undo,
  post-discard committed state, and repeated committed turns
  (`tests/xuzz/arena_manifold_test.cpp:308-386`).
- **Integer operations:** Integer add/subtract/multiply use checked operations; signed minimum
  negation/absolute value and `MIN / -1`, `MIN % -1`, and `DivMod(MIN,-1)` return precondition
  failures before appending outputs
  (`apps/common/xanadu/vortex/vortex_vm.cpp:366-373,375-489,823-831`). Integer/integer relational
  comparisons stay in `int64_t`, preserving distinctions around 2^53 and at the extrema
  (`apps/common/xanadu/vortex/vortex_vm.cpp:502-563`). The existing division-by-zero behavior
  remains 0/0.0, as covered in the new edge tests (`tests/xuzz/vortex_core_test.cpp:527-579`).
  `step()` checks execution failure before writing outputs (existing lifecycle at
  `apps/common/xanadu/vortex/vortex_vm.cpp:1128-1140`), and `evaluateCondition` now treats execution
  failure as false (`apps/common/xanadu/vortex/vortex_vm.cpp:352-358`). Focused tests cover exact
  comparisons, safe boundaries, overflow rejection with sentinels unchanged, mixed numeric/string
  comparisons, and overflow in contract evaluation (`tests/xuzz/vortex_core_test.cpp:428-621`).

The parent reported 91 focused runtime tests passing in `/tmp/vagent-numeric-runtime-test.log`; I
did not rerun builds or tests per scope.
