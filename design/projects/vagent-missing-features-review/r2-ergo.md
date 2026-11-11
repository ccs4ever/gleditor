# Round 2 critique: PL & Ergonomics Expert (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Cross-arena raw CellRef leak in Pattern B): RESOLVED — Repositioned to
  whole-compilation-unit granularity; worker actors promote locally to the store and exchange
  immutable `MicroversionId` strings, strictly preserving Ruling V1 and `MeshActorRuntime` arena
  isolation.
- R1.2 (Ambiguous EBNF grammar for MatchExpr): RESOLVED — Mandatory `with` keyword disambiguates
  scrutinees from record literals, and comma/newline arm separators eliminate shift/reduce
  ambiguity.
- R1.3 (VariablePattern identifier collision with constructors): RESOLVED — Binding patterns adopt
  explicit `?var` syntax consistent with VAgent belief patterns, leaving bare identifiers for
  constructor tags and literal `none`.
- R1.4 (DestructureLet assignment operator and syntax): RESOLVED — Standardized to
  `let Pattern := Expr` using VAgent's `:=` operator.
- R1.5 (Destructuring in for loops and missing do keyword): RESOLVED — Scoping example rewritten
  using canonical `for item in stack do` with an explicit destructuring `let` statement in the body.
- R1.6 (Cognitive mismatch and invalid BDI syntax in CompilerCoordinator): RESOLVED — Coordinator
  uses named plans (`plan HandleResultPlan for ...`), deadline clauses (`within 30s`), and standard
  reactive event handlers (`on CompileRequest as req`).
- R1.7 (Pervasive type and signature divergence from VAgent syntax): RESOLVED — All signatures
  standardized to colon return types (`: RetType`), canonical types (`Str`, `Bool`, `Int`, `Float`,
  `()`, `[T]`, `Opt<T>`, `Result<T, E>`), and valid declaration keywords.
- R1.8 (Proposal P30 file inventory scope): RESOLVED — P30 expanded to comprehensively include
  `token.hpp/.cpp`, `lexer.cpp`, `stdlib/lexer.vagent`, `ast.hpp`, `r1_ast.hpp/.cpp`,
  `stdlib/ast.vagent`, `checker.cpp`, `stdlib/checker.vagent`, and `stdlib/emitter.vagent`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§7, line 519) Include `stdlib/parser.vagent` in Proposal P30 file enumeration

Full title: Proposal P30 omits `apps/common/xanadu/vagent/stdlib/parser.vagent` from the explicit
file list

**Problem:** In §7 (line 519), Proposal P30 enumerates:

```text
apps/common/xanadu/vagent/token.hpp:15–53, token.cpp, lexer.cpp, stdlib/lexer.vagent,
parser.cpp, ast.hpp, r1_ast.hpp/.cpp, stdlib/ast.vagent, checker.cpp,
stdlib/checker.vagent, compiler.cpp, stdlib/emitter.vagent
```

While C++ `parser.cpp` is included, the metacircular VAgent parser `stdlib/parser.vagent` was
omitted from this explicit list (it was present in the Round 1 proposal register). Because
metacircular self-hosting requires `stdlib/parser.vagent` to parse `match ... with` expressions and
destructuring statements, it should be explicitly listed alongside the other stdlib modules.

**Evidence:** From `design/vagent-metacircular-missing-features.md:519`:

```text
apps/common/xanadu/vagent/token.hpp:15–53, token.cpp, lexer.cpp, stdlib/lexer.vagent,
parser.cpp, ast.hpp, r1_ast.hpp/.cpp, stdlib/ast.vagent, checker.cpp,
stdlib/checker.vagent, compiler.cpp, stdlib/emitter.vagent
```

**Fix:** Add `stdlib/parser.vagent` to P30's file list in Table 7.

### 2. (§6.1, line 448) Clarify refutable pattern static diagnostics for `DestructureLet`

Full title: Specify static check and diagnostic for refutable patterns in `let Pattern := Expr`

**Problem:** Section 6.1 defines `DestructureLet ::= "let" (TuplePattern | CtorPattern) ":=" Expr`.
For tuple patterns `[a, b, c]`, destructuring is irrefutable provided the expression's tuple length
is statically known. However, if a user writes a refutable pattern (e.g. `let Some(?x) := opt_val`),
the statement could fail at runtime. VAgent statements support fallback via `else Expr`
(`Stmt ::= ... | 'let' Ident [ ':' Type ] ':=' CallExpr 'else' Expr`). The document should clarify
whether refutable destructuring requires an `else` fallback clause or emits a static diagnostic.

**Evidence:** From `design/vagent-metacircular-missing-features.md:448`:
`DestructureLet ::= "let" (TuplePattern | CtorPattern) ":=" Expr` From
`design/vagent-agent-language.md:1344–1347`: `Stmt ::= ... | EffectRecovery`

**Fix:** Add a brief note in §6.1 or §6.3 stating that `let Pattern := Expr` is restricted to
statically irrefutable patterns (tuples and single-variant records); refutable patterns used without
`else` emit `E313: Refutable pattern in let binding requires else clause`.

### 3. (§6.2, line 466) Replace pseudo-call `str(members)` with VAgent string conversion

Full title: Minor string conversion helper in `stringify_ast` example

**Problem:** In §6.2 (line 466), the worked example uses
`"agent " ++ name ++ " with " ++ str(members)`. In VAgent, stringification functions are named
`int_to_str` or `to_str`.

**Evidence:** From `design/vagent-metacircular-missing-features.md:466`:
`"agent " ++ name ++ " with " ++ str(members),`

**Fix:** Change `str(members)` to `int_to_str(members.len())` or `to_str(members)`.

VERDICT: CONVERGED
