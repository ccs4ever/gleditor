# Round 2 critique: PL Design & Ergonomics Expert (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (Dynamic record construction & token representation): RESOLVED — Formalized in proposal
  `P22` (`compiler.cpp:756–768`) permitting dynamic field evaluation, integrated into Track 1.1.
- R1.B2 (`Opt<Cell>`, sentinel zero, and failure semantics): RESOLVED — Contradictory sentinel
  claims removed; proposal `P25` mandates `std::optional<CellRef>`, and goal-directed failure is
  respected.
- R1.B3 (Lexer string scanning quadratic allocation): RESOLVED — Track 1.1 explicitly specifies
  single-pass streaming scanning over source subjects without intermediate string slicing ($O(N)$).
- R1.B4 (Error propagation, diagnostic spans, and records): RESOLVED — Track 2.2 standardizes
  structured `Diagnostic{code, message, line, col, length}` records and multi-error collection.
- R1.M1 (Top-level compiler API and host integration interface): RESOLVED — Proposal `P23` defines
  C++ integration shims (`inlang_parser`, `inlang_checker`, `inlang_emitter`) using `RunCall`.
- R1.M2 (Grammar hallucination: unary `?` operator): RESOLVED — Erroneous unary `?` operator
  excised from Track 1.2; scanning remains binary `s ? e` and logic variables use `?name`.
- R1.M3 (Contradiction between R1 AST spec and implementation): RESOLVED — Section 3 canonical
  topology fully aligned with `r1_ast.cpp` (`makeCell(kindTag)`, `+d.grab`, `+d.step`, `+d.values`).
- R1.M4 (Absence of Proposal Register and cost/alternative pricing): RESOLVED — Formal Proposal
  Register `P18`–`P25` added in Section 6 with `file:line` citations, cost pricing, and fallbacks.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§3, line 124) Facade entry point module `stdlib/compiler.vagent`

**Problem:** Section 3 enumerates 12 modular files under `stdlib/` (`lexer`, `parser`, `ast`,
`expand`, `resolve`, `checker`, `emitter`, `fragment`, `decls`, `mind`, `arbitration`,
`plasticity`). While modular decomposition is clean and maps well to pipeline shims (`P23`), higher-
level consumers (such as REPLs, runtime meta-agents, or testing harnesses) benefit from an ergonomic
facade module (`std:compiler`) providing all-in-one compilation routines:

- `def compile_module(source: Str): Opt<Cell>`
- `def compile_expression(source: Str): Opt<Code<Cell>>`
- `def compile_fragment(label: Str, params: Cell, result_type: Str, source: Str): Opt<Code<Cell>>`
  Without a top-level facade, users and meta-programs must manually import and sequence six separate
  pipeline passes.

**Evidence:**

- `design/vagent-self-hosting-migration.md:120–136`: Package layout lists individual compiler passes
  without a unifying entry point facade.

**Fix:** Add `stdlib/compiler.vagent` (`std:compiler`) to the stdlib directory tree in Section 3 and
Wave 3 to serve as the unified facade orchestrating the lexing, parsing, expansion, resolution,
checking, and emission stages.

______________________________________________________________________

### 2. (§4, line 184) Parser error recovery and panic-mode synchronization tokens

**Problem:** Track 1.2 specifies recursive descent parsing and Track 2.2 specifies structured
`Diagnostic` records. To ensure that syntax errors during parsing do not immediately abort the
entire parse run or produce cascading nonsense diagnostics for subsequent valid tokens, the parser
grammar specification should define standard panic-mode synchronization boundaries.

**Evidence:**

- `design/vagent-self-hosting-migration.md:183–193`: Track 1.2 details grammar forms but does not
  mention synchronization tokens for multi-diagnostic reporting during parsing.

**Fix:** Add a brief specification note in Track 1.2 establishing that `stdlib/parser.vagent`
synchronizes on statement and declaration boundaries (newlines, semicolons, closing braces `}`, and
top-level keywords `def`, `plan`, `agent`, `event`, `let`) when recovering from syntax errors.

______________________________________________________________________

VERDICT: CONVERGED
