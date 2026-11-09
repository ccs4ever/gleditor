# Remaining VQL §4.12 completion gates

Iteration 50 committed the closure VM foundation; iterations 51–53 add lambda values, persistent
bindings and typed structural closure bodies. Earlier bounded milestones are recorded in
[the VM iteration log](vortex-vm-only-iterations.md). Old summary lists must not be read as if
native frames, lazy contract defaults or supported mutation groups were wholly unimplemented.

The audit groups the remaining acceptance work into nine packages, with C1 completed in iteration 52
and C2 partly implemented in iteration 53. These package identifiers are acceptance gates, not
reserved iteration numbers. At the owner's request, subsequent iterations group related
implementation and route work into larger deliverables. The earlier eight to eleven iteration
estimate is superseded by that grouping; it is not a completion promise.

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

| Package | Remaining work                                                                                                                                               | Acceptance evidence                                                                                                                                                                                                                                                                               |
| ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| C1      | Implemented in iteration 52: runtime closure bindings, native initialization and successful session publication.                                             | A closure created inside a native frame is returned, stored and applied after cursor inactivity and fresh-VM reload; absent/open and bound-empty remain distinct; metadata refusals write no Store operations. The real REPL retains values across queries.                                       |
| C2      | Iteration 53 adds zero/unary typed/dot/destructured formals, compatible blocks and nested creation; whole-formal defaults and broader callable forms remain. | Types, defaults, dot/context, destructuring and nested creation preserve pre-match scope, shadowing, filtering, return promises, capture lifetime and rollback. Unsupported forms refuse until their slice lands.                                                                                 |
| C3      | Iteration 54 implements runtime handle application and recursive forwarding in reusable bodies; decorated static callable boundaries remain in C4.           | Direct, forwarded and mutually recursive applications preserve capture timing, argument grouping, explicit empty values and result identity/order. Existing static variants retain their semantics.                                                                                               |
| C4      | Remaining decorated recursive functors.                                                                                                                      | Captured/computed predicates, shapes and deferred pins/yield forms preserve body-then-decoration ordering, full reference widths and portable graphs. Limits and malformed inputs refuse explicitly.                                                                                              |
| C5      | Iteration 54 implements final named rest defaults, context and destructuring; value-lambda variadics remain in C2.                                           | Defaults, dot and destructuring preserve omission versus explicit empty, each actual's contract before concatenation, duplicates/order, forwarding and native recursion.                                                                                                                          |
| C6      | Remaining stream lifecycle and parallel/mesh behavior.                                                                                                       | Overlapping cursors, nested streams, choice, cancellation, inactivity and reopening preserve ownership and do not leave stale maps/proxies. CLI/host/worker routes have matching results and errors.                                                                                              |
| C7      | Remaining contract/effect examples and validation boundaries.                                                                                                | Execute the four documented examples' actual bodies. Parameter mismatches filter; return mismatches name the function. Aliases and nested validation failures restore transactions and append no source Store operations. Existing supported mutation groups are not reimplemented.               |
| C8      | Final applicable route and lifecycle matrix.                                                                                                                 | CLI, host/palette, selectors, scripts/batches, mesh and fresh-VM exports agree on values, errors, budgets and side effects. Classify the existing broader audit failures by specification scope rather than calling them passing.                                                                 |
| C9      | §4.12.4 measurement and lowering decision.                                                                                                                   | Separately measure creation/capture, binding, compilation, repeated execution and allocations across path lengths and complexity, with full-width references and an arbitrary-length fallback. Adopt specialization only if evidence supports it; retaining ordinary lowering is a valid outcome. |

Each implementation stage runs its relevant routes immediately; C8 is the final combined gate, not a
reason to defer route checks. Passing a bounded stage does not close §4.12. The remaining VQL
specification and VPL, Prolog and VLace VM restoration follow this priority work and are outside the
planning band above.
