# Round 2 critique: Technical Writer (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (VAgent Code Examples Fail to Parse Under Live Grammar): RESOLVED — All code listings have
  been rewritten to strictly conform to live grammar (no module braces, `: RetType`, `:=`
  assignments, `within` deadlines, `let w := spawn`, named plans, `for..do` loops, and no default
  parameter initializers).
- R1.B2 (Ambiguous Match Grammar and Missing Lexer Accounting in P30): RESOLVED — The
  `match ... with` header keyword, explicit comma/newline arm delimiters, `"none"` literal pattern,
  `?var` binding, and Proposal P30 token/lexer inventory additions are fully and accurately
  specified.
- R1.M1 (Type Syntax, Names, and Fictitious Library APIs): RESOLVED — Type annotations are
  standardized across all sections to valid VAgent types (`Str`, `[Str]`, `Opt<T>`, `Result<T, E>`,
  `()`), and fictitious standard library APIs were replaced with valid operations.
- R1.M2 (Promotion Mechanism Missing Store Context and Violates Rule 3): RESOLVED —
  `std:zigzag:promote` specifies mutable `Store*`, parent microversion derivation from
  `boundStore_->primaryCurrentVersion()`, `extraRoots` dimension handling, and a sentinel-free
  `Result<Str, ZigzagError>` return.
- R1.M3 (Divergence Between Sequence Diagram Performatives and Coordinator Code): RESOLVED — Swarm
  architecture is cleanly bifurcated into synchronous in-turn `StagedCompiler` (Pattern A) and
  whole-compilation-unit `MicroversionId` exchange (Pattern B), eliminating cross-arena cell leaks.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§4.2, line 361) BDI Suspension Idiom in `CompilerCoordinator` Example

Full title: Clarify BDI Plan Suspension vs. Event Handler Context in Coordinator Example

**Problem:** In `agent CompilerCoordinator` (§4.2, line 361),
`achieve CompileUnit(...) by worker within 30s` is written directly inside
`on CompileRequest as req`. Under strict checking in
`apps/common/xanadu/vagent/checker.cpp:2258–2261`, `achieve` is a plan suspension statement
requiring `context.body == Body::Plan`. In an idiomatic VAgent agent, the `on` handler adopts a goal
(`adopt CoordinateCompilation(req.path)`), and a corresponding plan
(`plan CoordinateCompilationPlan for CoordinateCompilation(path)`) contains the `achieve` suspension
statement.

**Evidence:** From `apps/common/xanadu/vagent/checker.cpp:2258–2261`:

```cpp
require(context.body == Body::Plan && context.suspend && context.bounded &&
            !context.beliefLoop,
        DiagnosticCode::InvalidPosition,
        "achieve is a plan suspension statement", expression.loc);
```

**Fix:** In the illustrative `CompilerCoordinator` code sketch, show
`on CompileRequest as req { adopt CoordinateCompilation(req.path) }` delegating to
`plan CoordinateCompilationPlan for CoordinateCompilation(path) { ... achieve ... }`.

______________________________________________________________________

### 2. (§4.2, lines 344–367) Event and Plan Naming Parity Between Diagram and Coordinator Code

Full title: Align Performative Names Between Sequence Diagram and Coordinator Code Listing

**Problem:** In the sequence diagram (§4.2, line 344), the worker informs the coordinator via
`tell UnitCompiled("main.vagent", ver)`, and the coordinator notifies the client via
`emit CompilationComplete(path: "main.vagent", version: ver)`. In the code listing below it (lines
364–366), the coordinator declares `plan HandleResultPlan for HandleCompiledUnit(path, version)`,
omitting an explicit belief or event trigger and omitting `emit CompilationComplete`.

**Evidence:** Lines 344–345 vs. lines 364–366 of `design/vagent-metacircular-missing-features.md`.

**Fix:** Update `CompilerCoordinator` to include `emit CompilationComplete(...)` upon completion,
aligning the performative names identically between the diagram and code.

______________________________________________________________________

### 3. (§1 & §7) Internal Section Cross-Reference Links

Full title: Add Explicit Section Cross-References in Executive Summary and Proposal Register

**Problem:** The 5 capability gaps listed in §1 (lines 26–48) and Proposals P26–P30 in §7 (lines
515–519) describe features specified in §§2–6 without explicit `§X.Y` cross-reference citations.

**Evidence:** Capability gap 1 describes features in §2, gap 2 describes §3, gap 3 describes §4, gap
4 describes §5, and gap 5 describes §6.

**Fix:** Add `(§2)`, `(§3)`, `(§4)`, `(§5)`, and `(§6)` cross-references to the numbered list in §1
and the ID column of the table in §7.

______________________________________________________________________

VERDICT: CONVERGED
