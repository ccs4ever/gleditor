# Vagent result-use lowering re-review — R4

**Verdict: CONVERGED.** I re-read the exact current source after the prior report. The R3 finding is
resolved: `emit` now checks the discarded-expression set, evaluates into a scratch cell, and writes
Unit only on successful continuation. The wrapper does not turn a failing expression into success.

Reviewed `apps/common/xanadu/vagent/compiler.cpp`, SHA-256:

```text
14574c73703549a1caf96487e32999bc18c4c384cf12eaa65821cbc6a6a0da28
```

The exact test snapshot was `tests/xuzz/vagent_compiler_test.cpp`, SHA-256:

```text
fc0ac465a43aed8df75a7a20acbc3dc46fe6a312fd2b950926f95a15c348cafb
```

## R3 closure and semantic checks

- `emit` routes discarded nodes through
  `emitValue(expression, temporary(), copy(output, unit, continuation))` (`compiler.cpp:520-525`).
  Failure paths do not reach that continuation, preserving failure rather than manufacturing Unit.
- Preflight marks successful discarded expressions Unit but leaves Bottom as Bottom
  (`compiler.cpp:434-438`). This covers ordinary values and unlike conditional arms while retaining
  failure typing.
- New tests require successful discarded `if`, `false`, arithmetic, and final block values to
  produce an empty-string Unit cell (`tests/xuzz/vagent_compiler_test.cpp:556-589`). They also cover
  discarded failure and selected-arm behavior immediately after that range.
- Block result use is propagated correctly: non-final children are discarded and the final child's
  use follows its parent (`compiler.cpp:349-365`); conditional arms inherit result use, while
  conditions remain value-checked and pure (`compiler.cpp:405-428`). The emitter tests expression
  success rather than Bool truthiness and removes the conditional alternative before running the
  chosen arm (`compiler.cpp:630-645`).
- The clarified assignment contract in §8.3 matches the implementation: statement-local alternatives
  are cut, while the enclosing trailed mark retains rollback authority.

No actionable correctness or scope finding remains in the reviewed
block/local/conditional/result-use changes. I did not rerun the build or tests; the parent owns the
integration build.
