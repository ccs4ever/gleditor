# Scalar vagent compiler review — Vortex expert, round 2

## Verdict: NOT CONVERGED — one minor statement-position refusal

The generated scalar graphs use the expected `ChoiceDepth`/`Choice`/`Cut` structure. Success cuts
only choices created since the captured barrier; statement failure reaches the failure continuation
and releases the root mark; `Fail` and branch retries preserve the enclosing alternative; and
reversible writes remain under the caller's outer mark. The code checks input ownership, scalar
types, AST shape, limits and compile-time absence of active marks before graph emission. Lexical
local binding, shadow-initializer lookup and rerunning a graph with fresh external values are
covered. I found one remaining mismatch in the latest `ResultUse` handling.

## Finding

1. **[MINOR] Discarded no-`else` conditionals reject a valid statement-position then arm.** In
   `apps/common/xanadu/vagent/compiler.cpp:412–422`, the preflight forces `ResultUse::Value` for the
   then arm whenever an `if` has no `else`, and then rejects any then-arm type except Unit or
   Bottom. That rejection remains even when the whole conditional is explicitly
   `ResultUse::Discard`, or is a nonfinal block statement (where the block passes `Discard`). The
   language spec says an `if` in statement position may have arms of any type because their values
   are discarded (`design/vagent-agent-language.md:1103–1107`). For example,
   `compileStatement("if 1 then 2", {}, ResultUse::Discard)` and `{ if 1 then 2; 9 }` should
   succeed: when the condition succeeds, discard the Int result; when it fails, the implicit else
   path produces Unit. The compiler already emits Unit for the absent else branch. **Correction:**
   in a discarded position, preflight the then arm with `ResultUse::Discard` and reconcile it with
   the implicit Unit else; preserve the current Unit-arm requirement when the conditional's value is
   used. Add both root-discard and nonfinal-block tests. The existing rejection in
   `tests/xuzz/vagent_compiler_test.cpp:604–614` should be narrowed to cases the spec actually
   forbids, rather than asserting rejection of `if 1 then 2` under discard.

The newly added explicit result-use path otherwise handles differing explicit `if/else` result types
as intended (`compiler.cpp:405–438`, `521–530`, `tests/xuzz/vagent_compiler_test.cpp:556–589`). `:=`
remains reversible under an outer handler/turn mark: the statement cut captures and preserves that
caller barrier, and the test backtracks it successfully
(`tests/xuzz/vagent_compiler_test.cpp:538–555`; the turn/handler boundary is specified in §5.8 and
§6.7). Existing tests also cover minimum integer literal handling, divide/remainder domain guards,
numeric contract-fault recovery, choice barriers, block failure, local shadowing/reinitialization,
and failed conditional arms.

## Review boundary and verification state

This review is limited to scalar graph generation and the Vortex/Arena behavior it directly uses.
Actor dispatch, calls and loops remain outside this kernel and are not requested as approval
conditions. I did not run a build or tests. The source now contains 39 real-VM compiler tests; per
the parent review update, the original 34 passed and the five-test extension's focused build was
still pending at review time. I treat the new test results as unverified.
