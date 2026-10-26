# Vagent block/local/conditional lowering review — R2

**Verdict: MINOR finding.** I reviewed the current block/`let`/`if` compiler snapshot and its
focused tests against §§4.2, 5.8, 6.7, 8.2–8.3, and the reviewed transaction-boundary clarification
in §8.3. No issue was found with the prior `:=` caller rollback case: that rollback is required for
an enclosing transaction.

## MINOR — statement-position `if` rejects different arm types

`Compiler::preflight` unconditionally rejects an `if` whose successful arms have different types at
`apps/common/xanadu/vagent/compiler.cpp:414-418`. This also applies when the conditional itself is a
bounded statement, where the spec explicitly permits differing arm types because their values are
discarded (`design/vagent-agent-language.md:1104-1107`).

Counterexample: `if 1 then 2 else "x"` as a standalone statement (or
`{ if 1 then 2 else "x"; () }`). The condition is a successful `Int` expression, so the then arm
runs; at statement position the arm value is discarded and the statement should succeed with `()`.
The current compiler refuses the program with “discarded differing arm types are not lowered yet.”
Existing tests reject conditions such as `if false then 1 else 2` for the separate, correct reason
that a Bool literal is not a success/failure condition, but do not cover a successful non-Bool
condition with unlike arms (`tests/xuzz/vagent_compiler_test.cpp:517-535`).

Correction: when the `if` is in bounded statement position, permit unlike arm types and lower the
selected arm's result as discarded/Unit. Retain the common-type requirement in value positions. Add
the counterexample as a compile-and-run test that verifies success, Unit result, and that only the
selected arm executes.

## Reviewed behavior

- The `:=` clarification in §8.3 correctly scopes irreversibility to alternatives within the bounded
  statement. The outer caller/handler/turn mark must remain able to undo it, as tested in
  `BoundedAssignmentStillParticipatesInOuterRollback`
  (`tests/xuzz/vagent_compiler_test.cpp:537-553`).
- Block statements are individually bounded, so their own alternatives do not leak into later
  statements; a block failure still reaches the enclosing statement choice and rolls back the
  block's earlier writes (`compiler.cpp:496-505,597-605`; tests
  `BlocksTakeEachStatementsFirstResult`, `FailedNestedBlockRollsBackItsWholeEnclosingStatement`, and
  `BlockFailureAllowsSurroundingAlternative`).
- Locals resolve lexically, initializers bind before introducing a shadow, and each declaration is
  reinitialized on graph rerun (`compiler.cpp:122-155,346-395,489-494,607-609`; tests at
  `vagent_compiler_test.cpp:410-437`).
- Conditions are preflighted as pure; Bool values are accepted only when the expression's outer
  operator is a comparison. The emitted conditional branches on expression success, not Bool
  truthiness, cuts condition alternatives before entering the selected arm, and a failing then arm
  cannot fall through to else (`compiler.cpp:167-172,281-289,397-420,610-624`; tests at
  `vagent_compiler_test.cpp:464-506`).

The parent reported the extended test suite/build running; I did not duplicate it.
