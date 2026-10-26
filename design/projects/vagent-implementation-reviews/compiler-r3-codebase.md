# Vagent result-use lowering review — R3

**Verdict: MAJOR finding.** I reviewed the `ResultUse` extension against §§8.2–8.3 and its new
tests. The prior statement-position arm-type finding is resolved in preflight; the new result-use
mode does not yet affect emitted execution.

## MAJOR — `ResultUse::Discard` reports Unit but leaves the original value in the result cell

Preflight records discarded expressions and changes their inferred type to Unit
(`apps/common/xanadu/vagent/compiler.cpp:434-438`), and `emitStatement` returns that type to the
caller (`compiler.cpp:652-674`). But `discarded_` is never consulted during graph emission: `emit`
still copies literals and emits arithmetic/conditional values to the requested output
(`compiler.cpp:508-516,617-645`). Thus successful discarded expressions do not actually produce
Unit.

Counterexample: `compileStatement(Parser("3 + 4"), {}, ResultUse::Discard)` reports
`resultType == Unit`, runs to normal Halt with `produced == true`, but leaves integer `7` in
`result` instead of the Unit empty string. The new test requires Unit for this case, as well as for
`false` and unlike-arm conditionals (`tests/xuzz/vagent_compiler_test.cpp:556-567`). The same
discrepancy affects a discarded final block value (`tests/xuzz/vagent_compiler_test.cpp:570-589`).

Correction: make emission honor the discard set. Evaluate a discarded expression into a scratch
output, then write Unit to the public output only on its successful continuation; do not convert
failure into success. Add or retain the root arithmetic and final-block runtime assertions, which
expose this mismatch.

## Other reviewed behavior

`ResultUse::Discard` now permits unlike conditional arms in discarded contexts while preserving the
common-type rule in value contexts; the `if` condition and assignment checks remain independent from
result-use. The §8.3 clarification also resolves the previous `:=` question: caller/handler/turn
rollback must still undo bounded writes. No additional issue was found in the reviewed scope.
