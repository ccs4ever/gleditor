# I1 implementation gate: ML expert

Reviewed baseline: `80ebaef1f63472f7312f036b8476e06e01677639`. Frozen source manifest:
`/tmp/vagent-p19-i1/source-sha256.txt`, 21 entries, SHA-256
`de1a44708840bf6f3965b33669bfeab8c00a8665ae5e27df9a35d1ece8d16fa3`. All 21 file checksums match the
reviewed worktree.

Scope: strict `Exp`/`Ln`/`Sqrt`, executable `std:math` exports and host arity check, closed artifact
opcode allowlist, `d.trust`/`d.epistemic`, logical admission limits and settings defaults/schema,
program format bump. No scoring, learning, model state or priors are present.

Gate result: **PASS** — 0 BLOCKING, 0 MAJOR.

## Review results

- **Strict numeric contracts:** The enum values are appended after `Point`, keeping existing numeric
  opcode values stable (`apps/common/xanadu/vortex/vortex_vm.hpp:88–95`). Each operation requires
  exactly one Float represented as `double`, finite input, its specified domain (`Ln(x>0)`,
  `Sqrt(x>=0)`), and finite result; invalid input/domain/overflow returns a `Precondition` result
  without writing the output (`vortex_vm.cpp:404–409,881–905,1257–1264`). `run()` propagates failed
  steps (`vortex_vm.cpp:1330–1348`). Frozen math tests cover finite examples, allowed Exp underflow,
  domain/overflow and non-finite failures, invalid arity/types, output preservation, and opcode
  persistence/rebind/re-execution after store promotion/rebuild
  (`tests/xuzz/vagent_epistemic_math_test.cpp:60–156`). The corrected range-for uses a const
  reference at lines 108–119.
- **Export and host arity path:** `std:math/exp`, `/ln`, and `/sqrt` resolve to executable opcode
  entries with one input and one output (`apps/common/xanadu/vortex/vortex_stdlib.cpp:1500–1513`).
  The host `call` path refuses any non-unary argument list before writing input cells
  (`vortex_stdlib.cpp:2000–2009`). The dynamic actor test suite includes the ABI and export files
  via `XUZZ_TEST_SRCS` (`Makefile:639,1143–1148`). Their tests passed, including execution of each
  exported graph and both arity refusals (`tests.log`, `VagentEpistemicAbi` and
  `VagentEpistemicExports` suites).
- **Closed artifact allowlist:** The mesh artifact parser still limits accepted opcodes to `Cut`
  (`apps/common/xanadu/vortex/mesh_evaluator.cpp:483–489`); `pureOpcode` rejects the new math
  opcodes (`mesh_evaluator.cpp:237–245`). Tests reject them both during sealing and after tampering
  with an encoded artifact (`tests/xuzz/mesh_vortex_artifact_test.cpp:163–184`).
- **Dimensions and limits:** Runtime dimension registration includes `d.trust` and `d.epistemic` and
  remains idempotent; canonical promoted agent dimensions include both
  (`apps/common/xanadu/vagent/runtime/runtime_abi.hpp:47–91`;
  `apps/common/xanadu/zigzag/arena_manifold.cpp:2108–2115`). `EpistemicLimits` provides the shared
  positive logical bounds and checks signed values before conversion
  (`apps/common/xanadu/vagent/runtime/epistemic_limits.hpp:14–47`); AgentExecutor rejects zero
  limits when settings/runtime options are constructed
  (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:470–498`). System settings defaults and
  schema notes specify positive `max_sources=128` and `max_candidates=1024`
  (`apps/common/xanadu/system_docs.cpp:116–127,908–924`; names at `system_docs.hpp:284–290`). ABI
  tests for dimension idempotency, invalid host settings, and signed input rejection passed
  (`tests.log`, `VagentEpistemicAbi` suite).
- **Program version and frontend compatibility:** The actor program format is bumped to
  `vagent-program-2` (`apps/common/xanadu/vagent/runtime/program.hpp:35–38`; used in program
  reference material at `program.cpp:22–26`). VPL/VQL opcode name maps include the new enum members
  (`apps/common/xanadu/vpl/compiler.cpp:331–339`; `apps/common/xanadu/vql/compiler.cpp:125–133`).
  Existing compatibility tests passed in the supplied language/settings run.
- **I1 scope boundary:** No P19 scoring, learner update, feature/model implementation, or prior was
  added. Typed epistemic query refusal remains I2 work, consistent with the implementation sequence.

## Resolved review note

The initial frozen build failed because the math test copied a vector in a range-for under
`-Werror`; the implementation corrected it to `const auto&` before the final manifest. The final
runtime test suite passed. I also initially suspected the ABI/export tests were not registered; that
concern was withdrawn after verifying that Makefile dynamically discovers all `tests/xuzz/*.cpp` and
includes `vagent_*` tests in `vagent_actor_test`.

## Verification evidence

- Frozen manifest digest: `de1a44708840bf6f3965b33669bfeab8c00a8665ae5e27df9a35d1ece8d16fa3`
  (verified with `sha256sum -c`).
- Full I1 build: exit 0, `/tmp/vagent-p19-i1/build.log`.
- Actor integration suite: 596 tests passed, including ABI and export cases,
  `/tmp/vagent-p19-i1/tests.log`.
- Runtime suite: 112 tests passed, including all six math cases, `/tmp/vagent-p19-i1/tests.log`.
- Settings/stdlib/VQL/VPL compatibility suites: 204 tests passed,
  `/tmp/vagent-p19-i1/settings-stdlib-language-tests.log`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

VERDICT: CONVERGED
