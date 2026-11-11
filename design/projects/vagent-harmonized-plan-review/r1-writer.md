# Round 1 Critique: Technical Writer (`writer`)

**Review Target**: `design/vagent-harmonization-and-missing-features-implementation-plan.md`\
**Companion Documents**:

- `design/vagent-metacircular-missing-features.md` (Proposals P26–P30, P36–P37)
- `design/vagent-language-harmonization.md` (Proposals P31–P35)
- `design/vagent-metacircular-compiler-implementation.md` (Metacircular Pipeline Architecture)
- `design/projects/vagent-harmonized-plan-review/brief-common.md`

______________________________________________________________________

## 1. Executive Summary & Specification Assessment

The *VAgent Harmonization & Missing Features Implementation Plan* provides an authoritative,
logically phased engineering roadmap for integrating all twelve accepted proposals (**P26 through
P37**) into the C++23 engine substrate and sovereign `.vagent` standard library. The document is
commendably structured around six discrete phases, adhering to core architectural invariants
(Nelsonian persistence via promotion, strict 2-degree manifold topology, Spool-Mesh actor
isolation, and universal 1-based indexing).

From a Technical Writer's perspective, the document excels in structural flow, execution gating, and
overall markdown hygiene (cleanly passing both `mdformat --wrap 100` and `mdl -i`). Codebase line
anchors across `vortex_stdlib.cpp`, `main.cpp`, `decls.cpp`, `executor.cpp`, and `meta.cpp`
demonstrate high mechanical sympathy and ground-truth precision.

However, a rigorous line-by-line verification against the accepted companion specifications
(`design/vagent-metacircular-missing-features.md` and `design/vagent-language-harmonization.md`)
reveals **0 BLOCKING**, **5 MAJOR**, and **6 MINOR** issues regarding specification fidelity,
EBNF completeness, type signatures, and stdlib taxonomy:

1. **Garbled Four-Tier Evaluation Hierarchy (P34) [MAJOR]**: Section 3.1 Item 5 misdefines the
   foundational 4-tier evaluation hierarchy as an arithmetic/relational precedence ladder,
   omitting procedural statement sequencing (`;`, `\n`), goal-directed conjunction/alternation
   (`&`, `|`, `!`), and relational belief queries (`believes`).
1. **Hallucinated Stdlib File Names & Omitted Modules (Phase 5B) [MAJOR]**: Section 6.2 lists
   non-existent modules `token.vagent` and `bdi.vagent`, while omitting the two actual standard
   library modules `fragment.vagent` and `arbitration.vagent`.
1. **Total Omission of Safe Navigation (`?.`) and Null Coalescing (`??`) (P32, P33) [MAJOR]**: While
   P32 and P33 are accepted, the plan's Phase 2, 3, and 4 deliverables completely omit token
   definitions, AST variants, type checking, and lowering for `?.` and `??`.
1. **Omission of Structured String Scanning Block Form (`scan ... with`) & Modal State Cleanup (P33)
   [MAJOR]**: The plan omits the structured block form `scan subject with { ... }`, lookahead
   resolution between infix `TokenKind::Scan` and postfix `TokenKind::Question`, and deletion of the
   `allowScan_` parser modal state.
1. **Missing `vortex_vm.cpp` VM Opcode Lowering for 1-Based Indexing (P31) [MAJOR]**: Track 3A and
   the Register omit `apps/common/xanadu/vortex/vortex_vm.cpp:1898–1937`, failing to specify
   the 1-based index migration for VM opcodes `OpcodeKind::ByteAt` and `OpcodeKind::StrSlice`.
1. **Discrepancy in Return Types for `make_cell` and `text_of` (P27) [MINOR]**: Section 2.2 line 139
   specifies bare `CellRef` and `Str` returns, violating the sentinel-free `Result<T, ZigzagError>`
   specification in companion document §3.2 and invariant §10.4.
1. **Omission of `std:env:get_var` in Deliverables (P26) [MINOR]**: Section 2.1 Track 1A omits
   `std:env:get_var(key: Str) : Opt<Str>`.
1. **Incomplete Pattern Node Taxonomy in AST (P30) [MINOR]**: Section 3.2 item 3 lists
   `TuplePattern`, `ListPattern`, and `RecordPattern`, but omits `WildcardPattern` (`_`) and
   `CtorPattern` from `ExprKind`, and redundantly lists `DefaultArm`.
1. **Single-Arm Plan Compatibility in Parser Production (P36) [MINOR]**: Section 3.3 item 2 omits
   `SinglePlanBody ::= ("where" Expr)? Block` from `PlanBody`.
1. **Token Name Divergence: `TokenKind::Spread` vs `TokenKind::Ellipsis` (P35) [MINOR]**: Lexer
   definition uses `TokenKind::Spread` instead of companion spec's `TokenKind::Ellipsis`.
1. **Test Matrix Traceability Details (Phase 6) [MINOR]**: Minor pass criteria gaps for
   `vagent_ingestion_test.cpp`, `vagent_harmonization_test.cpp`, `vagent_pattern_match_test.cpp`,
   and `vagent_multi_arm_plan_test.cpp`.

Because five MAJOR issues exist, this review concludes with **VERDICT: REVISE**.

______________________________________________________________________

## 2. Proposal Register & Traceability Matrix (P26–P37)

Every numbered proposal from P26 through P37 is accounted for across the plan's architectural
breakdown, phase tracks, test suites, and register:

| Proposal ID | Title & Domain                             | Phase Tracks           | Section 7 Test Suite                 | Section 8 Register | Traceability Status                           |
| :---------- | :----------------------------------------- | :--------------------- | :----------------------------------- | :----------------- | :-------------------------------------------- |
| **P26**     | Epistemic Ingestion (`std:io`, `std:env`)  | Phase 1A (§2.1)        | `vagent_ingestion_test.cpp`          | Accounted          | **Full** (minor omission: `get_var`)          |
| **P27**     | First-Class ZigZag Primitives              | Phase 1B (§2.2)        | `vagent_ingestion_test.cpp`          | Accounted          | **Full** (type signature discrepancy)         |
| **P28**     | Metacircular Swarm Coordination            | Phase 5A (§6.1)        | `vagent_metacircular_quine_test.cpp` | Accounted          | **Full**                                      |
| **P29**     | Dynamic Image Spawning (`spawn image`)     | Phase 4D (§5.4)        | `vagent_image_spawn_test.cpp`        | Accounted          | **Full**                                      |
| **P30**     | Algebraic Pattern Matching                 | Phase 2B, 2C, 3C, 4A   | `vagent_pattern_match_test.cpp`      | Accounted          | **Full** (AST taxonomy minor gap)             |
| **P31**     | 1-Based Indexing Everywhere                | Phase 3A (§4.1)        | `vagent_harmonization_test.cpp`      | Accounted          | **Partial** (missing `vortex_vm.cpp`)         |
| **P32**     | Canonical Option (`Opt<T>`)                | Phase 3B (§4.2)        | `vagent_harmonization_test.cpp`      | Accounted          | **Partial** (omitted `?.` and `??`)           |
| **P33**     | Sigil Disambiguation & Scanning            | Phase 2A (§3.1)        | `vagent_harmonization_test.cpp`      | Accounted          | **Partial** (omitted `scan with`, `?.`, `??`) |
| **P34**     | Banishment of `;` & 4-Tier Operators       | Phase 2A (§3.1), 4B    | `vagent_harmonization_test.cpp`      | Accounted          | **Defective** (misdefined hierarchy)          |
| **P35**     | Delimiter Orthogonality (`(...)`, `[...]`) | Phase 2A, 2B, 2C       | `vagent_harmonization_test.cpp`      | Accounted          | **Full** (minor token name alias)             |
| **P36**     | Chained Multi-Arm Plans                    | Phase 2B, 2C, 3D, 4B   | `vagent_multi_arm_plan_test.cpp`     | Accounted          | **Full**                                      |
| **P37**     | Tree-Backed Map on `d.left`/`d.right`      | Phase 1C, 2A-C, 3E, 4C | `vagent_map_test.cpp`                | Accounted          | **Full**                                      |

______________________________________________________________________

## 3. Detailed Review Findings

### 3.1 [MAJOR] Garbled Four-Tier Evaluation Hierarchy (Track 2A & Register P34)

#### Issue Description

In Section 3.1 Item 5 (lines 205–208) and Section 8 (Proposal Register P34, line 532), the plan
defines the 4-tier operator hierarchy as:

> *"Banish `;` as Disjunction: Strictly banish `;` in plan guards and boolean expressions. Implement
> the 4-tier operator hierarchy in the lexer and parser: Tier 1 (`not`, `and`, `or`), Tier 2
> (relational), Tier 3 (concatenation `++`), Tier 4 (arithmetic)."*

#### Companion Specification Contrast

In `design/vagent-language-harmonization.md` (§1 Invariant 4, §5.2 Table 5.2), the four-tier
hierarchy is not an expression precedence ladder, but the fundamental evaluation framework
governing the separation of procedural execution, Boolean logic, goal-directed search, and belief
queries:

- **Tier 1 (Procedural Sequencing)**: `\n`, `;` executing sequentially along `+d.spin`. Semicolon
  never denotes disjunction.
- **Tier 2 (Pure Boolean Logic)**: `not`, `and`, `or` operating strictly on `Bool` values with
  short-circuiting and emitting zero choice points.
- **Tier 3 (Goal-Directed Execution)**: `&` (goal conjunction chaining success), `|` (alternation
  introducing backtracking choice points `#Choice`), and `!` (cut cutting choice points `#Cut`).
- **Tier 4 (Relational Belief Queries)**: `believes (...)` evaluating Horn clause queries over
  `d.clause` ranks, composing via Tier 3 `&` and `|`.

Furthermore, statement-boundary choice cutting via `#CutChoiceDepth` (compiled via `bounded()`
sequence of `OpcodeKind::ChoiceDepth` and `OpcodeKind::Cut`) is a Tier 1 / Tier 3 interaction rule
governed by Griswold's Principle of Bounded Expressions.

#### Remediation

1. Rewrite Section 3.1 Item 5 to accurately define the four tiers (Procedural Sequencing, Pure
   Boolean Logic, Goal-Directed Execution, Relational Belief Queries).
1. Explicitly include parsing and token support for Tier 3 (`&`, `|`, `!`) and Tier 4 (`believes`)
   in Track 2A and Track 2C deliverables.
1. Update Proposal Register P34 (line 532) to match this definition.

______________________________________________________________________

### 3.2 [MAJOR] Hallucinated Stdlib File Names & Missing Modules (Phase 5B)

#### Issue Description

In Section 6.2 (Deliverables for Full Stdlib Migration, lines 460–473), the plan enumerates the
eleven target standard library modules as:

```text
- apps/common/xanadu/vagent/stdlib/token.vagent
- apps/common/xanadu/vagent/stdlib/lexer.vagent
- apps/common/xanadu/vagent/stdlib/ast.vagent
- apps/common/xanadu/vagent/stdlib/parser.vagent
- apps/common/xanadu/vagent/stdlib/expand.vagent
- apps/common/xanadu/vagent/stdlib/resolve.vagent
- apps/common/xanadu/vagent/stdlib/checker.vagent
- apps/common/xanadu/vagent/stdlib/emitter.vagent
- apps/common/xanadu/vagent/stdlib/decls.vagent
- apps/common/xanadu/vagent/stdlib/mind.vagent
- apps/common/xanadu/vagent/stdlib/bdi.vagent
```

#### Ground-Truth & Companion Specification Contrast

1. `token.vagent` and `bdi.vagent` **do not exist** in `apps/common/xanadu/vagent/stdlib/`.
1. The plan completely omits the two genuine stdlib modules:
   - `fragment.vagent` (Fragment descriptor assembly and link validation; mounted directly in
     `behavior StagedCompiler` §4.1).
   - `arbitration.vagent` (Utility scoring, intention state machine, deadline queue; companion
     spec §4, line 301, and §3.4, line 234).
1. The verified 11 modules in the repository are: `lexer.vagent`, `parser.vagent`, `ast.vagent`,
   `expand.vagent`, `resolve.vagent`, `checker.vagent`, `emitter.vagent`, `fragment.vagent`,
   `decls.vagent`, `mind.vagent`, and `arbitration.vagent`.

#### Remediation

Replace `token.vagent` and `bdi.vagent` with `fragment.vagent` and `arbitration.vagent` in Section
6.2.

______________________________________________________________________

### 3.3 [MAJOR] Total Omission of Safe Navigation (`?.`) and Null Coalescing (`??`) (P32, P33)

#### Issue Description

In `design/vagent-language-harmonization.md` (§3.3, §4.1 Table 4.1, and Table 8 P32/P33), Safe
Navigation (`?.`, `TokenKind::QuestionDot`) and Null Coalescing (`??`, `TokenKind::DoubleQuestion`)
are core deliverables of the Option type and `?` disambiguation harmonization.

However, neither token, AST node, type-checking rule, nor lowering step appears anywhere in
`design/vagent-harmonization-and-missing-features-implementation-plan.md`.

#### Remediation

1. **Track 2A (§3.1)**: Add `TokenKind::QuestionDot` (`?.`) and `TokenKind::DoubleQuestion` (`??`)
   to the list of new punctuation tokens.
1. **Track 2B (§3.2)**: Add `SafeNav` and `Coalesce` to `ExprKind` in `ast.hpp`.
1. **Track 2C (§3.3)**: Add postfix `?.` member access and binary infix `??` parsing with
   Precedence 5 (between conjunction `&` and disjunction `or`).
1. **Track 3B (§4.2)**: Specify typing rules:
   - `base?.field`: If `typeof(base) == Opt<T>`, yields `Opt<typeof(T.field)>`.
   - `opt ?? fallback`: If `typeof(opt) == Opt<T>` and `typeof(fallback) == T`, yields `T`.
1. **Track 4**: Specify lowering for `?.` and `??` using conditional branching on presence.

______________________________________________________________________

### 3.4 [MAJOR] Omission of Structured String Scanning (`scan ... with`) & Modal State Cleanup (P33)

#### Issue Description

In `design/vagent-language-harmonization.md` (§4.1–4.2, Table 8 P33):

- String scanning provides Form A: Structured block form (`scan subject with { ... }`) using VM
  opcodes `#ScanPush` and `#ScanPop`.
- The parser modal state `allowScan_` in `parser.cpp:847` is deleted, resolved cleanly via
  predictive lookahead in `Parser::postfix()`.
- Postfix `TokenKind::Question` (`opt?`) and infix `TokenKind::Scan` (`subject ? scan_expr`) are
  disambiguated based on lookahead tokens.

In `design/vagent-harmonization-and-missing-features-implementation-plan.md`:

- Track 2A (§3.1 item 3) mentions only prefix `?ident` and postfix `expr?`, omitting infix `?`
  (`TokenKind::Scan`).
- Track 2C (§3.3) omits grammar production `scan subject with { ... }`.
- Deletion of `allowScan_` is not mentioned in deliverables.

#### Remediation

1. Add `scan subject with { ... }` parsing to Track 2C (§3.3) and lowering via `#ScanPush` and
   `#ScanPop` to Phase 4.
1. Explicitly mandate the removal of `allowScan_` from `apps/common/xanadu/vagent/parser.cpp:847`.
1. Document the predictive lookahead rule for `TokenKind::Scan` in Track 2A.

______________________________________________________________________

### 3.5 [MAJOR] Missing `vortex_vm.cpp` VM Opcode Lowering for 1-Based Indexing (P31)

#### Issue Description

In `design/vagent-language-harmonization.md` Table 8 (P31), the target files explicitly include:

```text
apps/common/xanadu/vortex/vortex_vm.cpp:1898–1937
```

and mandate: *"Update `ByteAt` and `StrSlice` opcodes to 1-based."*

Currently, in `vortex_vm.cpp:1898–1937`:

- `OpcodeKind::ByteAt` checks `idx < 0 || static_cast<size_t>(idx) >= s.size()`, indexing
  directly at `s[idx]` (0-based).
- `OpcodeKind::StrSlice` treats `start` and `end` as 0-based clamped offsets `[start, end)`.

In the Implementation Plan (§4.1 Track 3A and Section 8 Table P31), `vortex_vm.cpp` is completely
omitted from Target Files, and Deliverable 6 only discusses subscript offset translation in
`compiler.cpp`. Without updating `vortex_vm.cpp`, string primitives in the VM engine remain 0-based.

#### Remediation

1. Add `apps/common/xanadu/vortex/vortex_vm.cpp:1898–1937` to Target Files of Track 3A (§4.1) and
   Proposal Register P31 (§8).
1. Add Deliverable 7 to Track 3A: Update `OpcodeKind::ByteAt` and `OpcodeKind::StrSlice` in
   `vortex_vm.cpp` to execute 1-based indexing ($[1, N]$) with closed interval $[start, end]$ and
   symmetric negative indexing ($N + 1 + k$).

______________________________________________________________________

### 3.6 [MINOR] Discrepancy in Return Types for `make_cell` and `text_of` (P27)

#### Issue Description

Section 2.2 line 139 states:

> *"Implement missing primitives in `std:zigzag`: `make_cell(text)` returning `CellRef`,
> `text_of(cell)` returning `Str`... and `promote(root)` returning `Result<Str, ZigzagError>`..."*

This contradicts companion specification `design/vagent-metacircular-missing-features.md` §3.2,
Table 9 (line 850), and Invariant §10.4:

- `make_cell(text: Str) : Result<CellRef, ZigzagError>`
- `text_of(cell: CellRef) : Result<Str, ZigzagError>`
- `promote(root: CellRef, budget: Opt<Int>) : Result<Str, ZigzagError>`

If `text_of` returns bare `Str`, extracting text from an invalid cell cannot report failure without
sentinel strings (e.g. `""`), which is strictly forbidden by `AGENTS.md` ("No sentinels outside the
wire and on-disk formats"). Furthermore, `promote` supports an optional cell allocation `budget`
parameter.

#### Remediation

Update Section 2.2 line 139 to specify `make_cell(text: Str) : Result<CellRef, ZigzagError>`,
`text_of(cell: CellRef) : Result<Str, ZigzagError>`, and
`promote(root: CellRef, budget: Opt<Int>) : Result<Str, ZigzagError>`.

______________________________________________________________________

### 3.7 [MINOR] Omission of `std:env:get_var` in Deliverables (P26)

#### Issue Description

Section 2.1 Track 1A Deliverables list `setArgs`, `args`, `read_file`, `read_stdin`, `print`, and
`eprint`. Companion document `design/vagent-metacircular-missing-features.md` §2.2 also specifies:

```vagent
module std:env
def get_var(key: Str) : Opt<Str>
```

#### Remediation

Add `std:env:get_var(key: Str) -> Opt<Str>` to Deliverables in Section 2.1.

______________________________________________________________________

### 3.8 [MINOR] Incomplete Pattern Node Taxonomy in AST (P30)

#### Issue Description

Section 3.2 item 3 lists new `ExprKind` variants as:

> *"Add new `ExprKind` variants in `ast.hpp`: `Match`, `MatchArm`, `Map`, `Tuple`, `TuplePattern`,
> `ListPattern`, `RecordPattern`, and `DefaultArm`."*

1. It omits `WildcardPattern` (or `Wildcard`) for `_`, and `CtorPattern` for `Ctor(...)`. In
   `ast.hpp`, `ExprKind` currently lacks wildcard representations.
1. `DefaultArm` is listed as an `ExprKind`. However, in plans, default arms are represented by
   `struct PlanArm` (with `pattern == nullptr && guard == nullptr`), and in match expressions,
   default arms are represented by `WildcardPattern` (`_ => ...`).

#### Remediation

Refine the AST list in Section 3.2 to include `WildcardPattern` and `CtorPattern`, and remove
`DefaultArm` as an `ExprKind`.

______________________________________________________________________

### 3.9 [MINOR] Single-Arm Plan Compatibility in Parser Production (P36)

#### Issue Description

Section 3.3 item 2 states:

> Parse `PlanDecl ::= "plan" Ident "for" GoalPattern PlanBody`. Parse
> `GoalPattern ::= Ident ("(" (TermPattern ("," TermPattern)*)? ")")?`.
> Parse `PlanArmList` containing `where Expr => Block`,
> `case Pattern ("where" Expr)? => Block`, and `default => Block`.

This omits `SinglePlanBody ::= ("where" Expr)? Block` from
`PlanBody ::= SinglePlanBody | MultiArmPlanBody` (companion spec §7.2). Preserving
single-arm plan parsing is required for backward compatibility with existing plans.

#### Remediation

Explicitly state in Section 3.3 item 2 that `PlanBody` parses both `SinglePlanBody`
(`("where" Expr)? Block`) and `MultiArmPlanBody` (`"{" PlanArmList "}"`).

______________________________________________________________________

### 3.10 [MINOR] Token Name Divergence: `TokenKind::Spread` vs `TokenKind::Ellipsis` (P35)

#### Issue Description

In Section 3.1 item 2, the plan defines `TokenKind::Spread` (`...`). Companion specification
`design/vagent-language-harmonization.md` Table 4.1 line 259 defines `TokenKind::Ellipsis` (`...`).

#### Remediation

Standardize on `TokenKind::Ellipsis` (or explicitly note `TokenKind::Spread` as an intentional
token rename).

______________________________________________________________________

### 3.11 [MINOR] Test Matrix Traceability Enhancements (Phase 6)

#### Issue Description

In Section 7.1 Test Matrix:

- `vagent_ingestion_test.cpp`: Pass criteria should explicitly list `std:env:args` and CLI argument
  forwarding.
- `vagent_harmonization_test.cpp`: Pass criteria should mention `?.` (safe navigation), `??` (null
  coalescing), and `scan subject with { ... }`.
- `vagent_pattern_match_test.cpp`: Pass criteria should mention refutable let bindings with `else`
  blocks and diagnostic `E313`.
- `vagent_multi_arm_plan_test.cpp`: Pass criteria should mention nullary goal check `E315`.

#### Remediation

Incorporate these specific diagnostic and semantic checks into the Section 7.1 pass criteria.

______________________________________________________________________

## 4. Diagnostics & Line Anchor Verification Audit

### 4.1 Diagnostic Code Consistency

All four numbered diagnostics are verified to match companion specifications:

- `E101`: 1-based indexing violation (index 0 is invalid; literal `[0]` rejected).
- `E310`: Non-exhaustive pattern match.
- `E313`: Refutable pattern in let binding requires diverging `else` clause.
- `E315`: Cannot match pattern on nullary goal (use `where` or `default`).

### 4.2 Codebase Anchor Audit

The codebase anchors cited in the implementation plan were verified against the live repository:

- `apps/vagent/main.cpp:56–76`: `readSource` implementation (exact match).
- `apps/vagent/main.cpp:337–426`: `main()` execution and print loop (exact match).
- `apps/common/xanadu/vortex/vortex_stdlib.hpp:317–330`: `std:zigzag` declarations (exact match).
- `apps/common/xanadu/vortex/vortex_stdlib.hpp:505`: `boundStore_` member pointer (exact match).
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:4634–4760`: `buildZigzagModule` (exact match).
- `apps/common/xanadu/vortex/vortex_core.hpp:45–57`: `struct Dims` directory (exact match).
- `apps/common/xanadu/vagent/ast.hpp:38–92`: `enum class ExprKind` (exact match).
- `apps/common/xanadu/vagent/ast.hpp:208–231`: `struct Decl` (exact match).
- `apps/common/xanadu/vagent/decls.cpp:1268–1301`: `lowerClauseBody` (exact match).
- `apps/common/xanadu/vagent/decls.cpp:1979–2000`: `case DeclKind::Plan:` lowering (exact match).
- `apps/common/xanadu/vagent/executor.cpp:206–273`: `VagentExecutor::operator()` (exact match).
- `apps/common/xanadu/vagent/executor.cpp:3030–3090`: BDI intention scheduler fault ladder and
  `GoalFailed` handling (exact match).
- `apps/common/xanadu/vagent/meta.cpp:506`: `MetaRuntime::validate` (exact match).
- `apps/common/xanadu/vagent/meta.cpp:1270–1360`: `MetaRuntime::selectClause` (exact match).
- `apps/common/xanadu/vagent/parser.cpp:560–575`: `Parser::plan()` (exact match).
- `apps/common/xanadu/vagent/parser.cpp:840–865`: `allowScan_` in `Parser::postfix()` (exact match).

______________________________________________________________________

## 5. Summary of Findings & Actionable Remediations

| ID       | Severity  | Target Location            | Description                                                                                         | Actionable Remediation                                                                                                         |
| :------- | :-------- | :------------------------- | :-------------------------------------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------- |
| **W-01** | **MAJOR** | §3.1 Item 5, §8 (P34)      | Mislabeled 4-tier evaluation hierarchy as operator precedence ladder                                | Rewrite to define procedural sequencing, Boolean logic, goal-directed search (`&`, `\|`, `!`), and belief queries (`believes`) |
| **W-02** | **MAJOR** | §6.2 (Phase 5B)            | Listed non-existent `token.vagent` & `bdi.vagent`; omitted `fragment.vagent` & `arbitration.vagent` | Replace with verified list of 11 stdlib modules from repository and companion spec §4                                          |
| **W-03** | **MAJOR** | §3.1, §3.2, §3.3, §4.2, §8 | Complete omission of Safe Navigation (`?.`) and Null Coalescing (`??`)                              | Add `TokenKind::QuestionDot`/`DoubleQuestion`, AST kinds, type checking, and lowering                                          |
| **W-04** | **MAJOR** | §3.1, §3.3, §5             | Omission of `scan subject with { ... }`, lookahead resolution, and removal of `allowScan_`          | Specify structured scanning grammar, opcode lowering (`#ScanPush`/`#ScanPop`), and delete `allowScan_`                         |
| **W-05** | **MAJOR** | §4.1 (Track 3A), §8 (P31)  | Omission of `vortex_vm.cpp:1898–1937` (`ByteAt`, `StrSlice`) from 1-based indexing                  | Add `vortex_vm.cpp` to target files and specify VM opcode lowering for 1-based indexing                                        |
| **W-06** | **MINOR** | §2.2 (Track 1B)            | Return types for `make_cell` and `text_of` specified as bare `CellRef` and `Str`                    | Update signatures to `Result<CellRef, ZigzagError>`, `Result<Str, ZigzagError>`, and `promote(root, budget)`                   |
| **W-07** | **MINOR** | §2.1 (Track 1A)            | Omission of `std:env:get_var`                                                                       | Add `std:env:get_var(key: Str) -> Opt<Str>` to deliverables                                                                    |
| **W-08** | **MINOR** | §3.2 (Track 2B)            | Incomplete pattern node taxonomy; `DefaultArm` listed as `ExprKind`                                 | Add `WildcardPattern`, `CtorPattern` to `ExprKind`; remove `DefaultArm`                                                        |
| **W-09** | **MINOR** | §3.3 (Track 2C)            | Omission of `SinglePlanBody` compatibility in plan grammar                                          | Clarify `PlanBody ::= SinglePlanBody \| MultiArmPlanBody`                                                                      |
| **W-10** | **MINOR** | §3.1 (Track 2A)            | `TokenKind::Spread` vs `TokenKind::Ellipsis`                                                        | Standardize token naming on `TokenKind::Ellipsis` or document alias                                                            |
| **W-11** | **MINOR** | §7.1 (Test Matrix)         | Minor omissions in pass criteria details                                                            | Add `std:env:args`, `?.`, `??`, `scan with`, `E313`, and `E315` to pass criteria                                               |

______________________________________________________________________

## 6. Verdict

```text
VERDICT: REVISE
```

The implementation plan is structurally sound and architecturally well-conceived, but requires
updating to resolve the 5 MAJOR specification gaps (garbled 4-tier evaluation hierarchy, stdlib
module naming, omission of `?.`/`??` and `scan with`, and missing VM opcode index updates) before
engineering commencement.
