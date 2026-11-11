# Round 2: language and ergonomics critique

Reviewed all five peer R1 reports, the shared ledger, latest full workflow proposal (including
source/field/occurrence identity, late receipts, scan consistency, exact intent editions and
dispatch ownership), conformance and repaired evaluator. My R1 fixes built successfully; all 141 VQL
tests passed headlessly. This round is **not quiescent**: one distinct execution bug remains.

## L5 — P1: unknown nested functions fabricate empty text instead of refusing

Headless reproduction against current `build/vquery --engine direct --eval`:

```vql
contains("abc", definitely_missing_function())
```

Observed output: `true`, exit 0. A top-level invocation of the same missing function refuses.
`evaluateValueExpr` handles some builtins and UDFs, then falls through to `return ""` when a
`FunctionInvocation` name is unknown. `contains` treats that fabricated value as an empty needle,
which matches every string. Unknown calls nested inside other expressions can therefore produce
plausible success rather than a diagnostic; when used as a shape-assignment target, the same path
can fabricate an empty value cell and edit the subject. This is independent of resolved P2's shape
literal coercion.

**Fix:** make scalar function dispatch refuse unknown names and unsupported arities consistently
with path dispatch, before manufacturing a scalar cell. Prefer one shared validation/dispatch
contract for path/scalar contexts so refusal behavior cannot diverge. Preserve deliberate
missing-variable and optional-field policies separately. Regression: unknown top-level, nested,
UDF-body and assignment-RHS calls all name the missing routine; assignment refusal preserves source
links/content and does not mint the fabricated empty cell. No new language syntax is needed.

## Precise scope of the R1 decisions

Excess UDF arity and non-final variadic declarations are now validated before arguments/body
execute. This is not full declaration validation: required parameters after defaulted parameters,
duplicate parameter names, and missing untyped arguments retain existing behavior. Missing typed
arguments still follow contract matching, and defaults evaluate with earlier callee bindings. The
workflow proposal's future strict validation mode remains necessary; no all-arity safety guarantee
should be claimed from these checks.

Bare `f()` and explicit `./f()` currently share a Context anchor plus initial function-step AST. The
evaluator preserves that initial invocation, while explicit empty-variable/rank paths and later
steps terminate. A future distinction between “invoke without a subject” and “apply to an absent
context” requires parser/AST evidence; the current repair does not pretend to supply it.

The full workflow improvements are materially covered. I found no additional distinct conceptual
omission beyond the known deferred exact-value, strict-validation, effect and callable-contract
work. U4's dispatch owner/admission generation handles the concurrent-resume concern without
claiming that a local identifier creates remote exactly-once execution.

## L5 implementation and verification

Unknown scalar calls now refuse with name and arity instead of falling through to empty text.
`contains` requires exactly two arguments in both path and scalar contexts. Path `find` requires one
argument and `value` rejects more than its supported three, alongside existing count and
semantic-builtin checks. This deliberately leaves missing-variable/empty-path scalar rendering
unchanged. No dispatcher-wide rewrite was needed.

Three regression cases cover unknown top-level/nested/UDF calls, invalid builtin arities before
argument side effects, and direct/UDF/invalid-arity shape RHS refusal preserving subject content,
reciprocal links and cell count. `make -j$(nproc) xuzz_test vquery` succeeds; headless `VQL*` passes
**145 tests from 12 suites**, including the Vortex reviewer's new perspective restoration case.
Clang-format19 dry-run passes both owned files. Files released after verification.
