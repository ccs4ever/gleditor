# Final vagent checker safety review — R2

**Verdict: MAJOR finding.** The new `maxPurityWork` debit closes the prior fixed-point-pass finding.
A separate uncharged linear member lookup still permits quadratic checker work within the declared
AST and purity budgets.

Reviewed snapshot SHA-256:

```text
apps/common/xanadu/vagent/checker.hpp
b977a247046e33423ecb75d943ee31729197df724e874d4b5a5841240e7a603b

apps/common/xanadu/vagent/checker.cpp
f0bc94f0d37d509387570b124f05707ea60607fc8cd2206575003299f5e749d7

tests/xuzz/vagent_checker_test.cpp
fe1bc5e68f874f7fe97509546363412d6c81bad82495bf66f5ff6192612829ff
```

## MAJOR — repeated uncharged local-method scans bypass the work budgets

`function()` resolves an unqualified method by calling `member(owner, name, Method)`
(`apps/common/xanadu/vagent/checker.cpp:611-616`). `member()` scans the owner's entire `members`
vector (`checker.cpp:414-420`). Neither `maxPurityWork` nor the AST budget charges for those
comparisons; purity charges one expression node before calling `function()` (`checker.cpp:722-731`),
while ordinary type inference also resolves calls through the same path (`checker.cpp:793-805`).

Counterexample: an `agent A` declares N ordinary pure methods, each with body `m_last()`, where
`m_last` is the final method in the agent's member list and returns an integer literal. Each call
lookup scans nearly N members, so purity derivation and later inference each perform Θ(N²) member
comparisons. For N around 80,000, the declarations, types and two-expression method bodies are still
below the default `maxAstNodes = 1,000,000`; purity's initialization, pass and AST-node debits are
also well below `maxPurityWork = 1,000,000` because all methods remain pure after one pass. Yet the
uncharged lookups require billions of comparisons. This is a flat, parser-valid method list and does
not rely on deep nesting or malformed AST.

Correction: index each owner's methods by name once during preflight/global indexing, or charge the
comparisons performed by `member()` against an explicit checker work budget. The budget must also
cover the later `infer()` calls, not only purity derivation.

## Closure of the previous finding and remaining scope

- The specific fixed-point purity cost from R1 is now charged: method initialization, each method
  entry of every pass, and every visited body expression debit `maxPurityWork` before the
  corresponding work (`checker.cpp:715-784`). The regression exercises a reverse impure dependency
  chain, refusal at 2,000 units, acceptance at 100,000, and transitive impurity in a condition
  (`tests/xuzz/vagent_checker_test.cpp:815-837`).
- Iterative preflight still bounds declaration/expression/type depth and nodes before global
  indexing, purity derivation or inference, including metadata edges and contract/goal expressions.
  The manual-event-base shape guard is correctly placed in that preflight.
- The Kahn inheritance work counter and iterative memoized ancestry/cache paths remain bounded. The
  earlier event multiple-inheritance source example was invalid; parser syntax permits one event
  base, and the checker now refuses malformed ASTs with more than one.

I did not rerun tests or the integration build.
