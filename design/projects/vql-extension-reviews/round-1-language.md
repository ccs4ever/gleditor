# Round 1: language and ergonomics critique

Reviewed current V16.1 specification, conformance matrix, prerequisite repair record, full workflow
proposal, and relevant evaluator paths. The four findings below are implementation bugs, distinct
from the known reciprocal-link/nested-target repairs and performance/purity claims. Reproductions
ran against the current `build/vquery --engine direct --eval` headlessly, with isolated XDG paths;
none contacted external services.

## P1: empty path input invokes a UDF once

Reproduction:

```vql
def pure f($x) := "unexpected";
##/+d.definitely_absent/f()
```

Actual output: `unexpected`. Expected: empty. `evaluateStep` calls `callUserDef` once when
`currentCells.empty()`; its lambda-step branch does the same. This violates the specification's
per-candidate map-and-terminate contract. A failed selection can therefore execute a downstream
mutating function with no selected subject, a dangerous precedent for proposed workflow dispatch.
Separate top-level invocation from path application in the evaluator: an empty path stream must
remain empty. Add UDF and lambda regressions, including a body that would allocate, asserting both
empty output and unchanged arena size. A top-level zero-argument function should still run.

## P1: an empty final statement returns stale earlier results

Reproduction:

```vql
##/+d.example%"stale";
##/+d.missing
```

Actual output: `stale`. `executeScriptBody` updates `lastResult` only if the current result is
nonempty. A final query selecting nothing thus appears to have returned an earlier successful
answer. The conformance matrix says scripts are supported directly, but does not disclose this
fallback. Set the result from each value-producing statement even when empty; if definitions or
bindings are intentionally non-value statements, represent that distinction explicitly rather than
using emptiness as a sentinel. Acceptance: final empty traversal, failed typed filter, and empty
return erase earlier results; a block whose final expression succeeds still returns it.

## P1: binding an earlier parameter changes later argument evaluation

Reproduction:

```vql
def pure second($x, $y) := $y;
let $x := "caller" return second("callee", $x)
```

Actual output: `callee`; expected: `caller`. `callUserDef` evaluates arguments and binds parameters
in the same loop/environment. The first parameter shadows the caller variable before the second
argument reads it. Reusable bridge/query functions would silently receive the wrong subject,
audience, or evidence. Evaluate all explicit arguments in caller scope before installing callee
bindings. Then evaluate defaults under a documented callee policy, permitting earlier parameters if
intended. Preserve cell/functor identities in the argument snapshot. Add swapped-name,
shadowed-name, and functor-argument tests.

## P2: excess arguments are silently discarded

Reproduction:

```vql
def pure f($x) := $x;
f("one", "discarded")
```

Actual output: `one`, exit success. `callUserDef` stops at the parameter count without validating
remaining arguments. This conceals misspelled API calls and can discard an intended policy or
capability argument. Validate arity before evaluating any arguments/body; reject excess arguments
unless a final variadic parameter consumes them. Also validate required/default parameter ordering
at declaration time. Keep arity errors distinct from the deliberately chosen contract-match
filtering behavior. Acceptance: surplus arguments report function name/expected/actual arity and
mint no cells.

The workflow proposal already covers strict validation, exclusions/coverage lineage, durable
anchors, and explicit host submission well. These bugs are prerequisite execution fixes: library
contracts cannot compensate for discarded inputs, changed argument scope, or results resurrected
after an empty query.

## R1 implementation and verification

Implemented all four findings in `vql_engine.cpp` and added ten regressions to
`vql_semantics_repair_test.cpp`. Both `make -j$(nproc) xuzz_test vquery` and the headless `VQL*`
filter pass: **141 tests from 12 suites**. Clang-format19 dry-run also passes for the two files.

Decisions: empty results terminate subsequent ordinary paths, functor steps, and root selectors; an
initial Context-anchor invocation still supports bare calls such as `f()` and `count($empty)`. The
current parser represents explicit `./f()` like a bare contextual invocation, so this change does
not introduce an AST distinction between those spellings. Every script statement now replaces its
predecessor's result, including an empty result.

Explicit arguments capture cells and applicable untyped named functors in caller scope before any
parameter is installed. Defaults intentionally evaluate after earlier callee parameters are bound.
Excess arity and non-final variadic declarations refuse before explicit argument evaluation;
existing missing-argument/default/contract-match behavior remains. RAII scope restoration covers
lambda and UDF bindings/functors on success, rejected contracts, and exceptions from explicit
arguments, defaults, body evaluation, and return contracts. This restores execution scope, not arena
mutations already performed by a failing body. Regressions exercise cell/functor shadowing, mutating
empty paths, failed defaults/types/bodies/returns, and zero-argument/variadic compatibility.
