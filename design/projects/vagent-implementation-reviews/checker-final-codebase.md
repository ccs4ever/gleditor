# Final vagent checker safety review

**Verdict: MAJOR finding.** The iterative preflight and inheritance safety changes close the prior
stack-depth and event-shape issues, but a checker-wide work bound can still be bypassed by the
fixed-point purity pass.

Reviewed snapshot SHA-256:

```text
apps/common/xanadu/vagent/checker.hpp
9acbd523495f15ae9bde452e0b02e4ba87d44d43b191a8ea16041c0a83d7dc86

apps/common/xanadu/vagent/checker.cpp
838fb07d0dd8d1532bf6f70984211f5e643882ee890bbb619495d3e4d41463be

tests/xuzz/vagent_checker_test.cpp
46187183c0d83d85e254fc6c5984a7a436092dd996ce1b8a33c93fb041da3824
```

## MAJOR — purity derivation has quadratic work despite the AST budget

`derivePurity` repeatedly scans every method until no purity value changes
(`apps/common/xanadu/vagent/checker.cpp:676-690`). Each scan calls `pureBody` on methods still
marked pure; each method call resolves the target and reads its current purity. A
reverse-propagating chain therefore takes one full pass per method.

Counterexample: build an agent with a long flat chain of pure methods where `m00000()` calls
`m00001()`, `m00001()` calls `m00002()`, and so on in declaration order, while the last method calls
an impure builtin such as `Logging.info("x")`. The last method is initially impure; each iteration
then discovers one preceding method is impure. With N methods, purity derivation does Θ(N²)
method-body checks. The new default permits up to one million AST nodes
(`apps/common/xanadu/vagent/checker.hpp:17-22`), while the preflight only bounds shape and the
inheritance graph has its own independent budget; neither limits this purity work. A large but valid
flat source can consequently consume excessive CPU despite passing every declared guard.

Correction: derive purity with a reverse-call worklist/SCC pass so each call edge is processed a
bounded number of times, or charge a finite purity-work budget and return a typed refusal before the
remaining scans. Add an adversarial reverse-order method-chain test that asserts the bounded-work
behavior, not just stack safety.

## Guard and inheritance closure

- The new iterative preflight charges before queue insertion and bounds total AST, declaration,
  expression and type nodes/depths; it covers declaration expression fields, contracts, goal
  clauses, all expression metadata, and nested type arguments (`checker.cpp:207-283`). `run()` calls
  it before global indexing, purity derivation or inference (`checker.cpp:1900-1934`).
- The event-base guard now rejects malformed hand-built ASTs before inheritance-cache indexing;
  parser source still permits only one event base (`checker.cpp:252-257`, `parser.cpp:548-559`).
- The topological inheritance pass, iterative memoized ancestry, cache copy debits and finite
  inheritance-work refusal remain bounded as reviewed. Capacity diamonds/conflicts are validated;
  event source syntax cannot construct multiple-base conflicts.
- No other actionable issue was found in the reviewed guard/inheritance paths. The parent owns the
  integration build; I did not rerun it.
