# Checker R3 safety review

**Scope:** read-only inspection of inheritance handling and the public checker API in
`apps/common/xanadu/vagent/checker.{hpp,cpp}`. No build or test command run.

## Inheritance: converged

`buildInheritance()` uses a queue-based topological walk rather than native recursion, diagnoses
cycles after the walk, and charges visits plus copied interface entries against
`CheckLimits::maxInheritanceWork`. `inherits()` also uses an explicit work vector and visited set.
The tests exercise deep event chains, a deep cycle, work-budget refusal, and copied-interface work.
The `check()` API reports budget exhaustion as a diagnostic through its `expected` result.

## API safety finding: unchecked AST recursion

`check(const Program&, CheckLimits)` accepts a public, already-built `Program`, but `CheckLimits`
bounds only inheritance work. Expression checking remains recursive: `derivePurity()` calls
recursive `pureBody()`, and `infer()` recurses through operands, blocks and metadata expressions.
`Context::depth` tracks lexical scope for local-name rules; it is not a recursion budget. The
parser's `ParseLimits::maxDepth` protects source parsed through `Parser`, but does not protect
callers that construct or modify the public AST and call `check()`.

A manually constructed method body with a very deep unary-expression chain is a concrete input shape
that can exhaust the native call stack during purity analysis or inference instead of returning a
diagnostic. Consider either adding an expression-work/depth limit to `CheckLimits` and applying it
to all recursive checker walks, or making these walks iterative. This is an API-boundary safety gap;
it does not affect source routed through the parser's default depth limit.
