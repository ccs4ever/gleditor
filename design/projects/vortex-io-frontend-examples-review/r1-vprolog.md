# R1 review: VProlog/Vlog

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`.

Reference: `design/vortex-io-implementation-plan.md` §10, especially the VProlog/Vlog row; current
parser, compiler, logic lowering, and Vlog arena trail.

## Findings

### MAJOR — example depends on control syntax that the plan does not explicitly add

The query uses `->` and `;` to branch on `Submission = admitted(Req)`. The lexer/parser recognize
these tokens and parse them as compound terms (`parser.cpp` gives `->` precedence 1050 and `;`
1100), but query compilation has no lowering for if-then-else or disjunction. The current control
builtin set contains conjunction, negation, cut/fail/true, comparisons and type tests; it does not
contain `->` or `;` (`logic_compile_builtins.cpp`). Such a parsed goal therefore falls through to
ordinary predicate dispatch and is not the described branch. The implementation plan's VProlog row
explicitly plans a query-driver form, so this is implementable future syntax, but §10 should state
that the recognized admission sum branch is part of that lowering (and that its `;` is not a
speculative effect alternative). The example's “proposed query-driver form” label helps, but it
should not imply current parser/compiler support.

Evidence: `apps/common/xanadu/vprolog/parser.cpp` (`getInfixOp`, `parseClause`),
`apps/common/xanadu/vprolog/compiler.cpp` (`compileQuery`, `solve`), and
`apps/common/xanadu/vortex/logic_compile_builtins.cpp` (`isControlBuiltin`, `emitControlBuiltin`).

### MAJOR — a fallible `inspect_io/2` can still turn an outcome into proof failure

The text requires `inspect_io/2` to preserve transport statuses as values, but leaves it as an
ordinary user predicate and gives no clauses or totality contract. In Prolog, a missing status
clause or later goal failure makes the query fail/exhaust its alternatives. That conflicts with the
plan's requirement that Prolog unify a tagged outcome rather than translate it into logical failure.
Define the projection as a total built-in/driver operation returning a value, or show a complete
typed mapping whose result itself is returned independently of proof success. Ensure `Rejected`
refusal, `Complete`, `Partial`, EOF, failure, cancellation, timeout, overflow/stale where
applicable, and `Unknown` have explicit value cases.

Evidence: plan §10 says “Prolog unifies a tagged value instead of failing”; current
`Compiler::solve` returns only `std::vector<LogicSolution>` and `solveOnce` returns `bool`, so an
ordinary failed goal is represented as no solution, not as a distinct I/O outcome.

### MINOR — no concrete host binding/fixture contract

`ReadCap` is described as host-bound, but the example does not say how the capability enters the
query's variable environment or show its managed capability occurrence. The plan says capabilities
are owner-minted and authority cannot be reconstructed from text. Keep `ReadCap` as a symbolic
host-bound variable in the sketch, but explicitly state that it is injected as a managed cell
binding, never parsed from the atom `ReadCap`; an executable fixture should provide that binding.

## Semantics confirmed

- The existing AST stores query goals as terms and `compileQuery` lowers them to cells; it does not
  presently recognize I/O goals or reject their use in clauses. The planned frontend diagnostic
  `PROLOG_IO_EFFECT_CONTEXT` therefore requires new frontend analysis/lowering, not just service
  builtins.
- Current `Compiler::solve` is synchronous and materializes a vector of solutions; it exposes no
  suspended cursor/resume API. Pending continuation retention/re-await is a planned query-runner
  extension and should remain clearly future-facing.
- Vlog's logical variables are clone ranks, and arena mark/release supplies undo. This is a logical
  trail only: it cannot undo external I/O. The example correctly says cut/failure/disposal do not
  reverse work and that a resumed activation re-awaits the same request.
- Refusal as a tagged value and Pending as runner state are correctly distinct in the example. It
  should preserve that distinction while fixing the fallible `inspect_io` shape.

## Verdict

**Changes required before calling the VProlog sketch source-complete.** Its overall contract matches
the plan (top-level effect placement, explicit outcome sum, same activation on Pending, and
non-reversible external work), but the plan/example must explicitly establish the ITE sum-match
lowering and make outcome inspection total/value-returning. No target edits made.
