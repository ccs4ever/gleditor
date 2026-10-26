# Final checker AST-safety recheck

**Scope:** recheck of the API recursion finding in [`checker-r3-safety.md`](checker-r3-safety.md)
against the final checker guard and its direct-AST tests. Read-only; no build or tests run.

## Result: CONVERGED

The earlier finding is resolved. `Checker::run()` calls iterative `preflight()` before symbol
indexing, purity analysis or recursive inference. Its explicit stack covers declaration members;
declaration parameters/results and nested types; declaration contracts, goal clauses, bodies,
guards, initializers, triggers and deadlines; and every expression child plus `scope`, `recipients`,
`deadline`, `guard` and `recovery` metadata edge. It charges total AST nodes before queueing each
item, with separate expression-node, expression-depth, declaration-depth and type-depth limits. This
bounds the subsequent recursive checker traversals for inputs accepted by the preflight. The guard
also rejects an event declaration with more than one base before inheritance indexing.

The five direct-AST test cases exercise deep expression nesting, wide expression trees, all five
metadata edges, nested declarations and types, total node budget, and malformed event inheritance
shape. Their source inspection matches the guard behavior. No remaining counterexample to the
previous public API stack-exhaustion concern was found.
