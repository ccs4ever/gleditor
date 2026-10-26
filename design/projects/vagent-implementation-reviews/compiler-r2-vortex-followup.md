# Scalar vagent compiler review — Vortex expert follow-up

## Verdict: CONVERGED — prior finding withdrawn; no remaining VM-specific issue

This follow-up supersedes finding 1 in `compiler-r2-vortex.md`.

### Withdrawal: no-`else` conditional typing

The earlier report treated the statement-position rule for `if c then a else b` as applying to an
`if` without an `else`. That was incorrect. Section 8.2 explicitly says an `if c then e` without
`else` is total and requires `e` to have type `()` (`design/vagent-agent-language.md:1091–1092`).
The later rule allowing any arm types in statement position explicitly names the two-arm form
`if c then a else b` (`:1105–1107`). The compiler's special case in
`apps/common/xanadu/vagent/compiler.cpp:412–422` correctly preserves the stricter no-`else` rule,
even when the overall result is discarded. The existing rejection test is correct. I withdraw the
prior finding; there is no contradiction in the specification.

### VM and arena correctness recheck

- Generated graph execution records the caller's choice depth before opening its statement choice,
  and uses `Cut(depth)` on success. `VortexVM::cut` discards only marks above that barrier; it does
  not remove the caller's earlier choice. `ArenaManifold::discard` preserves writes while an
  enclosing mark remains, and later `release` of that outer mark undoes them. The `:=`
  outer-rollback test confirms this (`tests/xuzz/vagent_compiler_test.cpp:538–555`).
- The statement failure alternative is a real `Fail`/backtrack path, not a value sentinel. Tests
  cover a failed statement, trying alternatives, failed blocks, and preservation of the caller's
  choice barrier.
- `Branch` jumps on truthy comparison output and falls through to its encoded false continuation,
  matching the compiler's comparison and division guards
  (`apps/common/xanadu/vortex/vortex_vm.cpp:716–724`). `Value` copies through `VortexCore::value`;
  generated temporaries and lexical locals are initialized through the graph, and external binding
  cells are read again on graph rerun. Tests exercise fresh bindings, local reinitialization and
  lexical shadowing.
- Integer minimum handling and division/remainder zero and min/-1 guards avoid undefined signed
  operations before invoking the VM arithmetic opcodes. Core numeric contract faults remain failures
  requiring adapter backtrack/recovery; the compiler header documents this boundary and a real-VM
  test exercises recovery (`tests/xuzz/vagent_compiler_test.cpp:299–315`).
- AST node/depth budgets are checked in preflight before graph minting. The code treats graph
  allocation and runtime fuel/contract exhaustion as resource faults rather than pretending they are
  successful statement results. No finding remains within the scalar subset; actor scheduling, calls
  and loops remain out of scope as instructed.

The current source has 39 real-VM compiler tests. Per the parent update, all 39 compiler tests, 97
frontend tests and the full build now pass. I did not rerun them.

## Reviewed snapshot hashes

`git hash-object` values at review time:

| File                                           | Hash                                       |
| ---------------------------------------------- | ------------------------------------------ |
| `apps/common/xanadu/vagent/compiler.hpp`       | `82c18a44a519e843e666cd9ce3db0b51e1738d18` |
| `apps/common/xanadu/vagent/compiler.cpp`       | `4192e55033cb8c11dbfdcd27204ded6e58bba9e1` |
| `tests/xuzz/vagent_compiler_test.cpp`          | `7aa97f814e82c5c0bed17fa23417b0e4d8ceb97a` |
| `apps/common/xanadu/vortex/vortex_vm.cpp`      | `5598232a0d5b93ff11ea20eca1c76332fcc25c8e` |
| `apps/common/xanadu/zigzag/arena_manifold.cpp` | `bb12cbf5418b25b961107c7d41582d8b6b29c403` |
