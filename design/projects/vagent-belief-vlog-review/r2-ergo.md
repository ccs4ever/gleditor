# Round 2 critique: PL & Ergonomics Expert (vagent-belief-vlog-unification.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Scanning API contract breaks `tab(find())`): RESOLVED — §3.2 and §3.3 cleanly decouple
  positional testing functions/generators (`find`, `upto`, `any`, `many`, `match`, `bal`), which
  produce 1-based integer position indices (`Int`) without advancing `&pos`, from movement functions
  (`tab`, `move`), which accept integer positions or relative offsets, advance `&pos`, and produce
  traversed substrings (`Str`). Composability `tab(find("foo"))` and non-destructive lookahead
  guards are fully restored.
- R1.2 (Grammar gap in `for believes` and `findall`): RESOLVED — §2.6 EBNF grammar explicitly
  incorporates `[ 'from' Postfix ]` across `BeliefQuery`, `ForBeliefStmt`, and `FindallExpr`,
  guaranteeing developers can filter queries, iterations, and collections over beliefs by source.
- R1.3 (Conflation of `tell` with local belief mutation): RESOLVED — §2.4, §4.2, and Proposal P18.3
  cleanly disentangle outgoing inter-agent communication performatives (`tell`/`untell` staged to
  `d.outbox`) from local belief maintenance (`believe`/`forget`/`revise` under `self` vs.
  `assert ... from S` / `retract ... from S` under explicit provenance).
- R1.4 (`from deduced` pseudo-source filter lexical status): RESOLVED — Clarified as a query filter
  matching intensional Horn-derived solutions, corresponding to the base rule provenance rank on
  `+d.source` specified in §2.1.
- R1.5 (Non-void effect method calls in event handlers): RESOLVED — §4.4 and §5.6 formalize that
  event handlers execute effects strictly under fire-and-forget semantics without intention
  suspension; return values are discarded silently, conforming to Rule V6.
- R1.6 (Range-restriction and dynamic floundering in negation): RESOLVED — §2.7 establishes
  compile-time safety requiring variables in `not believes` to be bound by earlier positive
  literals, and specifies that dynamic floundering triggers an immediate contract violation trap
  (`floundered_negation` / `cause = "floundered"`).
- R1.7 (Key ordering in `PredicateDecl`): RESOLVED — Proposal P18.3 formally schedules compiler
  typechecking and validation of `key` parameter placement during AST checking.
- House Rule Verification (Sentinel Ban): PASSED — Verified that zero sentinel values (`noCell`,
  `noIndex`, `noAxis`, `-1`) appear anywhere in runtime signatures or design prose. Unlinked slots,
  fallible operations, and optional fields are represented exclusively via `std::optional`,
  `std::nullopt`, or unlinked slots.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§2.6, line 475) Retain canonical `'do'` keyword in `ForBeliefStmt` grammar production

Full title: `ForBeliefStmt` EBNF grammar production omits `'do'`, risking lookahead ambiguity with
block braces

**Problem:** In §2.6 line 475, `ForBeliefStmt` is specified as:

```ebnf
ForBeliefStmt  = 'for' 'believes' GoalTerm [ 'from' Postfix ] Block ;
```

In `design/vagent-agent-language.md:1295` (`vagent §8.9`), `for` loops use `'do'`:

```ebnf
Primary ::= ... | 'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr
```

Omitting `'do'` creates an unnecessary parser lookahead ambiguity: if `Postfix` ends in an
expression terminated with a struct literal `{ ... }`, the lexer/parser must look ahead to
disambiguate the struct literal from the loop body `Block` `{ ... }`. Retaining `'do'` provides an
unambiguous delimiter separating the query filter from the loop body.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:475`:
  `ForBeliefStmt  = 'for' 'believes' GoalTerm [ 'from' Postfix ] Block ;`
- `design/vagent-agent-language.md:1295`: `| 'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr`

**Fix:** Update §2.6 line 475 to include `'do'`:

```ebnf
ForBeliefStmt  = 'for' 'believes' GoalTerm [ 'from' Postfix ] 'do' Block ;
```

______________________________________________________________________

### 2. (§2.6, line 485) Harmonize `from deduced` with `"rule"` provenance representation

Full title: Syntactic filter `from deduced` diverges from topological source identifier `"rule"`

**Problem:** §2.1 line 241 and the §2.1 topology diagram specify:
`Rule provenance on +d.source links to "rule".` However, §2.6 table line 485 introduces
`believes P(...) from deduced` without documenting how `deduced` maps to the `"rule"` source cell on
`+d.source`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:241`:
  `Rule provenance on +d.source links to "rule".`

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:485`:

  ```text
  | believes P(...) from deduced | Exclusively solutions derived via Horn clauses |
  ```

**Fix:** Add a parenthetical clarification in §2.6 stating that `deduced` is parsed as a contextual
keyword in `from` clauses that matches the internal `"rule"` provenance link on `+d.source`, or
permit `from "rule"` as an explicit alias.

______________________________________________________________________

### 3. (§4.4, line 859) Add static diagnostic when assigning non-void effect calls in event handlers

Full title: Add compile-time check forbidding assignment of effect return values in event handlers

**Problem:** §4.4 line 859 specifies that when an effect method is called inside an event handler,
execution does not suspend and results are discarded silently upon host completion. However, if a
developer attempts to capture the return value of a non-void effect method inside an event handler
(e.g., `let x := sample_composition()`), `x` cannot receive a valid result because the host effect
has not completed.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:858–860`:

  ```text
  The handler does not suspend; execution continues immediately to the next statement in the handler block. If the effect subsequently succeeds in the host, the result is discarded silently.
  ```

**Fix:** Specify in §4.4 and Proposal P18.6 that the compiler rejects expressions binding or
consuming the result of an `effect` method call within event handlers or lifecycle blocks
(`CompileError::CannotSuspend` or `CompileError::VoidEffectRequired`), enforcing that effect methods
in handlers can only be invoked as standalone statement expressions.

______________________________________________________________________

### 4. (§3.2, line 593) Classify index $i \le 0$ as non-positive rather than negative

Full title: Imprecise classification of scanning position 0 as negative

**Problem:** Line 593 states: "Negative positions count backward from the end: $0$ is after the last
character ($\text{len} + 1$), $-1$ is before the last character...". Mathematically, zero is
non-positive rather than negative.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:593`:
  `Negative positions count backward from the end: $0$ is after the last character`

**Fix:** Update the description in §3.2 line 593 to read: "Non-positive positions ($i \le 0$) count
backward from the end: $0$ is after the last character ($\text{len} + 1$), $-1$ is before the last
character...".

______________________________________________________________________

VERDICT: CONVERGED
