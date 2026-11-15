# Remaining VQL §4.12 completion gates

Iteration 50 committed the closure VM foundation; iterations 51–53 add lambda values, persistent
bindings and typed structural closure bodies. Earlier bounded milestones are recorded in
[the VM iteration log](vortex-vm-only-iterations.md). Old summary lists must not be read as if
native frames, lazy contract defaults or supported mutation groups were wholly unimplemented.

The audit groups the remaining acceptance work into nine packages, with C1 completed in iteration 52
and C2 extended beyond iteration 53 by explicit calls and lazy fixed-formal defaults. These package
identifiers are acceptance gates, not reserved iteration numbers. At the owner's request, subsequent
iterations group related implementation and route work into larger deliverables. The earlier eight
to eleven iteration estimate is superseded by that grouping; it is not a completion promise.

Iteration 52 implements C1 as an end-to-end deliverable: graph-resident runtime binding snapshots
and native initialization, portable bound closure application after reload, and successful query
binding publication into a VQL session. Independent implementation tracks share one writer freeze,
full build and combined validation cycle. Live continuation transport remains a distinct C6
acceptance gate. Further iterations should combine adjacent packages where their implementation and
tests can share a meaningful end-to-end result.

Iteration 54 combines C3 runtime closure application inside reusable bodies with C5 named rest
defaults, context and destructuring. The combined graphs, portable routes, 1,133-test affected
matrix and independent reviews passed. General decorated static callable work remains in C4;
value-lambda signature extensions remain in C2. Static decorated functors retain their existing
application-time lexical rules and remain a separate C4 deliverable.

Iteration 55 extends C4 with symbolic decorated named variants, per-variant alias ports and bounded
capture preparation. All 1,157 affected tests passed, including native recursive forwarding and
portable graphs. C4 remains open for the excluded forms; C2 and C6–C9 remain acceptance work rather
than reserved iteration numbers.

The subsequent [VM integrity restoration](vql-vm-integrity-restoration.md) validates all four C7
example bodies through the CLI, including runtime UTC timestamps and atomic refusal of the original
exclusive archive body. It also repairs runtime quantifiers, morphisms and cursor routes. The full
engine gate has 38 existing failures and no new failures against its pre-stage baseline; this is not
a full-suite pass. C2, C4, C6 and the final C8/C9 acceptance work remain open. The broader C7
boundary audit must remain tied to that final route matrix.

The [explicit closure calls/defaults package](vql-closure-calls-defaults-plan.md) extends C2 with
multiple fixed formals, lazy whole-formal defaults and positional runtime calls. Its 143 focused
tests passed; the full engine run retains exactly the 38 baseline failures, with no new failing
names. Native export/reload, host, source mesh, CLI and persistent REPL routes passed. C2 remains
open for variadic value lambdas, named closure actuals and static AST-functor defaults.

The [stream-call semantics package](vql-stream-call-semantics-plan.md) then repairs empty-path
function execution and Cartesian comparisons while preserving bare nested invocation roots and
native recursion. All 19 new tests passed. Its final full engine run has 32 baseline failures, six
fewer than `f0f74ad0`, with no new failing names. This supports function semantics across the
existing packages; it does not close the remaining signature, lifecycle or route gates.

The [runtime closure signatures package](vql-closure-signatures-plan.md) adds named actuals and
final value-lambda rest formals, with private VM Groups ports for original-group contracts and one
reusable body graph across arities. Static AST-functor defaults/captures remain open; this closes
neither C4 nor the lifecycle/route gates. Its route tests also identify an existing C8 defect:
repeated mesh result identities evict earlier value links. Native reload and host preserve those
identities; distinct-output SOURCE mesh calls are covered separately.

The [portable mesh result package](vql-mesh-result-identity-plan.md) resolves that C8 duplicate-link
defect for the common producer, including nested shared data, signed publication/import and fresh
Store reload. A shared counted v2 reader rejects malformed metadata. Publication preflight also
refuses ambiguous raw references from another Store before effects. Its final full engine run has
exactly the same 32 baseline failures and no new failing names. This closes the identified packaging
slice, not the final C8 route/lifecycle matrix or §4.12.

| Package | Remaining work                                                                                                                                                                                     | Acceptance evidence                                                                                                                                                                                                                                                                               |
| ------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| C1      | Implemented in iteration 52: runtime closure bindings, native initialization and successful session publication.                                                                                   | A closure created inside a native frame is returned, stored and applied after cursor inactivity and fresh-VM reload; absent/open and bound-empty remain distinct; metadata refusals write no Store operations. The real REPL retains values across queries.                                       |
| C2      | Explicit calls, multiple fixed typed/dot/destructured formals, lazy defaults, nested creation, final variadics and named closure arguments are implemented; static AST-functor defaults remain.    | Types, defaults, dot/context, destructuring and nested creation preserve pre-match scope, shadowing, filtering, return promises, capture lifetime and rollback. Unsupported forms refuse until their slice lands.                                                                                 |
| C3      | Iteration 54 implements runtime handle application and recursive forwarding in reusable bodies; decorated static callable boundaries remain in C4.                                                 | Direct, forwarded and mutually recursive applications preserve capture timing, argument grouping, explicit empty values and result identity/order. Existing static variants retain their semantics.                                                                                               |
| C4      | Iteration 55 adds computed scalar/Boolean predicates, existing literal/nested shapes and frozen static bodies; non-closed defaults, computed shape targets and historical/projection forms remain. | Captured/computed predicates, shapes and deferred pins/yield forms preserve body-then-decoration ordering, full reference widths and portable graphs. Limits and malformed inputs refuse explicitly.                                                                                              |
| C5      | Iteration 54 implements final named rest defaults, context and destructuring; runtime value-lambda rest is implemented in the signatures package.                                                  | Defaults, dot and destructuring preserve omission versus explicit empty, each actual's contract before concatenation, duplicates/order, forwarding and native recursion.                                                                                                                          |
| C6      | Remaining stream lifecycle and parallel/mesh behavior.                                                                                                                                             | Overlapping cursors, nested streams, choice, cancellation, inactivity and reopening preserve ownership and do not leave stale maps/proxies. CLI/host/worker routes have matching results and errors.                                                                                              |
| C7      | Documented bodies verified; final mutation/route boundary audit remains.                                                                                                                           | Execute the four documented examples' actual bodies. Parameter mismatches filter; return mismatches name the function. Aliases and nested validation failures restore transactions and append no source Store operations. Existing supported mutation groups are not reimplemented.               |
| C8      | Final applicable route and lifecycle matrix.                                                                                                                                                       | CLI, host/palette, selectors, scripts/batches, mesh and fresh-VM exports agree on values, errors, budgets and side effects. Classify the existing broader audit failures by specification scope rather than calling them passing.                                                                 |
| C9      | §4.12.4 measurement and lowering decision.                                                                                                                                                         | Separately measure creation/capture, binding, compilation, repeated execution and allocations across path lengths and complexity, with full-width references and an arbitrary-length fallback. Adopt specialization only if evidence supports it; retaining ordinary lowering is a valid outcome. |

Each implementation stage runs its relevant routes immediately; C8 is the final combined gate, not a
reason to defer route checks. Passing a bounded stage does not close §4.12. The remaining VQL
specification and VPL, Prolog and VLace VM restoration follow this priority work and are outside the
planning band above.
