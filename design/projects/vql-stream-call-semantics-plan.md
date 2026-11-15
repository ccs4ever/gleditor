# Compiled stream call and comparison semantics

The baseline is `f0f74ad0`: 4,408 active engine tests, 4,342 passed, 38 failed and 28 skipped. This
package repairs empty-path calls, stream comparisons and unresolved-call diagnostics. The bounded
implementation is validated below; §4.12 remains incomplete.

## Invocation roots and empty paths

A bare initial invocation differs from a function mapped after a traversal. `f()` must still run
inside a native frame whose inherited context Stream is empty. `##/d.missing/f()` and `./f()` have
no candidate and skip the function, including actual-argument/default/body effects. Captured
relative functors and macro groups retain that empty-flow rule. Their intentional `?`/`*` identity
result remains separate.

`PathExpression.isInvocationRoot` records the parser's syntax distinction. The parser marks only an
implicit-context initial named or explicit runtime closure invocation, including calls followed by
traversal. Explicit dot, leading slash, anchors, lambdas and macros remain unmarked. The compiler
rejects inconsistent markers before anchor lowering; static path keys and AST dumps retain the bit.
ASTs remain compilation intermediates. Exported execution uses only graph instructions.

Normal nonempty input retains existing per-candidate mapping and UFCS rules. Only the first marked
named/runtime invocation tests input presence and admits one null-context invocation when that input
is empty. It tests incoming membership rather than the call result, so a call that filters to empty
is not replayed. The fallback result passes through the existing dereference, predicates, shapes,
clamps and projections. Historical step pins preserve this first-step eligibility through their
unpinned lowering; later steps never inherit it. Explicit `$f(args)` stays Explicit in both arms;
the implicit `/$f` alias stays Contextual and gets no fallback.

A contextless initial control item does not become a formal actual. An omitted typed required formal
still filters empty; actual `##/f()` uses its real candidate under UFCS. All supported unshadowed
argument-free whole-walk quantifiers retain their empty-input semantics, including `count()` = 0.
User shadowing and argument-taking `count(expr)` remain ordinary per-candidate calls.

## Cartesian scalar comparison

Comparison prepares its left operand then right operand exactly once in the optional caller or
predicate context. Both prepare even when the left stream is empty. Operands retain complete
streams, identity, order and duplicates. A constant-size nested StreamBegin/IteratorNext graph
compares pairs with the existing scalar VqlCompare and exits on the first satisfying pair. All six
operators are existential, including `!=`; either empty side yields false.

The scalar leaf preserves distinct Int64/Double, exact integer and lossless mixed comparisons, IEEE
floating-point rules and canonical text fallback. Known singleton versions retain causal ancestry
comparison; uncertain runtime/multivalued version operands refuse with the existing by-form version
diagnostic. They do not acquire display-string ordering. No runtime AST or second matcher is
introduced.

Loop instructions are independent of product cardinality; static input wings and stream snapshots
remain O(L+R), and pair work is O(L×R). There is no Cartesian product buffer and no speedup claim.
VM cycles and arena work bound execution. StreamBegin and StreamReverseBegin charge each declared
input before preparing its chunk and before publishing a replacement iterator. An exhausted setup
preserves the prior iterator and cursor and permits retry. Input-wing collection retains its
existing representation; this is not a bound on all allocation or CPU cost.

## Diagnostics and regression observations

Shared unresolved-function refusals include the function name and actual count before argument
effects. Unknown names retain Unknown; known unlowered functions retain Unsupported. Bodies are
compiled even when runtime input may be empty, so unreachable unknown calls still refuse.

Successful compilation allocates ephemeral graph/register cells. No-effects checks inspect document
links/content rather than total arena cell count. Rejected compilation rollback retains its stronger
allocation checks. Older parity tests that explicitly expected a downstream empty call now require
empty results and pair them with positive bare-root and nonempty-path assertions. An obsolete typed
variadic graph assertion no longer requires a descriptor variant emitted only for that removed
empty-path fallback; its actual group/value checks remain.

## Acceptance and independent gates

The dedicated tests cover six-operator truth tables, unequal cardinalities, duplicates, empty sides,
exact scalar types and causal versions. Two default mutations produce an observable left-then-right
marker, including an empty left operand. A match–miss–match probe reuses one compiled graph and its
registers; low-budget failure is followed by retry on the same VM. Forward/reverse stream tests
prove failed setup preserves an existing iterator.

Nested native-frame calls with empty inherited context retain named recursion and explicit runtime
closure invocation. Explicit dot, relative functors, later calls, macro paths and unknown names have
separate regressions. Forged invocation markers refuse as Malformed before effects. A portable
fixture compares the rank `b,c` with scalar `b`, requiring the second pair for `!=`, after source
AST/compiler/VM destruction and Store reload, and through host and Vquery SOURCE mesh. Real CLI
probes return `different` and a forbidden-link count of `0`.

Luna architecture, senior and resource reviews accepted the bounded design and implementation.
Independent follow-up review accepted the invocation-root validation and historical-pin propagation.
Every source writer froze before full `make -j$(nproc)`; C++ uses exact clang-format 19. Every
executable runs headlessly with separate XDG data, config and cache directories. The final engine
run was isolated from other test/format processes.

## Validation results

Full `make -j$(nproc)` passed. The broad VQL/native-frame matrix ran 1,001 active tests: 980 passed,
20 baseline failures remain, and one opt-in profile was skipped; seven probes were disabled. All 19
added tests passed. Native self/mutual recursion, captured and static functors, typed rest,
structural frames, return promises and closure routes retained their behavior.

The final standalone engine run has 4,427 active tests: 4,367 passed, 32 failed and 28 skipped, with
12 disabled profiling/benchmark probes. No failing name is new against `f0f74ad0`. The six targeted
failures are fixed: Cartesian existential comparisons; empty ordinary, captured-functor and macro
walks; empty count shadowing; and unresolved-function diagnostics. This is not a full-suite pass or
§4.12 completion.

The final slice run retains its two baseline failures (266/268 passed). The earlier library run
retains its one baseline failure (841/842 passed); the generic library was unchanged by the root
repair. Repository formatting passed with the required clang-format-19 path. Changed Markdown passed
mdformat and mdl. Lint still reports the unrelated UI typography violations at
`apps/common/ui/view/slice/acme_presenter.hpp:28` and
`apps/common/ui/view/slice/slice_view_presenter.hpp:46`.

Logs are `/tmp/vql-stream-semantics-build.log`, `/tmp/vql-stream-semantics-broad.{log,xml}`,
`/tmp/vql-stream-semantics-full-final.{log,xml}`,
`/tmp/vql-stream-semantics-zigzag-final.{log,xml}`, `/tmp/vql-stream-semantics-gleditor.{log,xml}`,
`/tmp/vql-stream-semantics-cli.log`, `/tmp/vql-stream-semantics-format.log` and
`/tmp/vql-stream-semantics-lint.log`.
