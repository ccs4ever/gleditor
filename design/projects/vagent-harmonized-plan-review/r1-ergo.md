# Round 1 Critique: Programming Language Design & Ergonomics Expert (`ergo`)

**Target Document**: `design/vagent-harmonization-and-missing-features-implementation-plan.md`\
**Companion Documents**:

- `design/vagent-language-harmonization.md` (Proposals P31–P35)
- `design/vagent-metacircular-missing-features.md` (Proposals P26–P30, P36–P37)
- `design/projects/vagent-harmonized-plan-review/brief-common.md`

______________________________________________________________________

## 1. Executive Summary & Ergonomic Assessment

The implementation plan establishes a comprehensive, phased engineering roadmap for bringing all
twelve accepted proposals (P26–P37) into the C++23 codebase and sovereign `.vagent` standard
library. From a programming language design and ergonomics perspective, the plan successfully adopts
the core Nelsonian and BDI invariants: universal 1-based indexing ($[1, N]$), nominal `Opt<T>` with
goal-directed soft unwrap (`opt?`) and assertive force unwrap (`opt!`), delimiter orthogonality
(`(...)` tuples, `[...]` lists with `...` spread, `{...}` records), multi-arm plan declarations, and
first-class tree-backed maps (`Map<K, V>`).

However, rigorous cross-examination against the accepted specification documents
(`vagent-language-harmonization.md` and `vagent-metacircular-missing-features.md`) reveals three
**MAJOR** ergonomic gaps and four **MINOR** specification ambiguities in the implementation plan:

1. **Garbled Four-Tier Evaluation Hierarchy (P34) [MAJOR]**: The plan mislabels the foundational
   four-tier evaluation hierarchy as an expression operator precedence ladder (`not/and/or`,
   relational, `++`, arithmetic), completely omitting procedural statement sequencing (`;`, `\n`),
   goal-directed conjunction/alternation (`&`, `|`, `!`), and relational belief queries
   (`believes`).
1. **Omission of Safe Navigation (`?.`) and Null Coalescing (`??`) (P33) [MAJOR]**: While P33 is
   marked accepted, the plan's Phase 2, 3, and 4 deliverables completely omit token definitions, AST
   variants, type checking, and lowering for `?.` and `??`.
1. **Map Ergonomics Omissions in Type Checker (P37) [MAJOR]**: Track 3E omits bidirectional type
   inference for empty map literals (`Map {}`), method type checking for `map.get(k)`,
   `map.contains(k)`, `map.keys()`, and 2-tuple inference for destructuring iteration
   `for (k, v) in map`.
1. **Conflation of `E101` Index 0 Rejection with General Out-of-Bounds (P31) [MINOR]**: The plan
   claims in Invariant 4 and the Proposal Register that out-of-bounds accesses trigger `E101`.
   `E101` is strictly the static rejection of literal index `0`. Dynamic out-of-bounds subscripting
   is an execution event triggering `#Fail` in goal-directed contexts.
1. **Missing Slicing Edge Cases and Scanning Coordinate Primitives (P31, P33) [MINOR]**: Slicing
   edge cases ($end = start - 1$ producing empty slice, $end < start - 1$ failing) and the
   `tab_end()` primitive (required to replace `tab(0)` banned by `E101`) are missing from Phase 1A,
   2A, and 3A.
1. **Multi-Arm Plan Dead Code Verification (P36) [MINOR]**: The plan does not require the
   `default =>` fallback arm to be terminal, permitting dead arms to be declared after `default =>`.
1. **Event Header Payload Destructuring (P35) [MINOR]**: Track 2C mentions `on Event where Guard`,
   but omits event payload record destructuring `on Event { field: Pat } where Guard`.

Because of the three MAJOR issues, this review issues a verdict of **REVISE**.

______________________________________________________________________

## 2. Detailed Findings

### 2.1 [MAJOR] Garbled Four-Tier Operator Hierarchy in Track 2A & Register (P34)

#### Context & Specification

In `design/vagent-language-harmonization.md` (§1 Invariant 4, §5.2), the language harmonization
establishes the Four-Tier Evaluation Hierarchy to resolve the catastrophic collision between
procedural statement sequencing (`;`, `\n`) and Prolog logical disjunction (`;`):

```text
Tier 1: Procedural Sequencing     (\n, ;)          - Sequences along +d.spin
Tier 2: Pure Boolean Logic        (not, and, or)   - Short-circuiting, zero choice points
Tier 3: Goal-Directed Execution   (&, |, !)        - Conjunction, alternation (#Choice), cut (#Cut)
Tier 4: Relational Belief Queries (believes (...)) - Unification over d.clause ranks
```

Expression precedence inside terms is governed by an orthogonal 13-precedence level ladder (§4.3).

#### Defect in Implementation Plan

In `design/vagent-harmonization-and-missing-features-implementation-plan.md`, Track 2A Item 5 (lines
205–208) states:

> *"Banish `;` as Disjunction: Strictly banish `;` in plan guards and boolean expressions. Implement
> the 4-tier operator hierarchy in the lexer and parser: Tier 1 (`not`, `and`, `or`), Tier 2
> (relational), Tier 3 (concatenation `++`), Tier 4 (arithmetic)."*

This text is repeated verbatim in Section 8 Proposal Register P34 (line 532).

This text misidentifies the 4-tier hierarchy. It conflates expression arithmetic/relational
precedence with the language's four execution regimes, completely omitting Tier 1 (statement
sequencing), Tier 3 (goal-directed execution `&`, `|`, `!`), and Tier 4 (relational `believes`
queries). Furthermore, the core mechanism of P34—cutting choice points at procedural statement
boundaries (`#CutChoiceDepth` via `bounded()`)—is severed from the Tier 1 definition.

#### Required Remediation

Update Track 2A Item 5 and Register P34 to specify:

1. Semicolon (`;`) and newline (`\n`) are strictly and exclusively Tier 1 procedural statement
   separators, permanently banned from denoting logical disjunction.
1. The Four-Tier Evaluation Hierarchy governs execution regimes: Tier 1 (Procedural Sequencing),
   Tier 2 (Pure Boolean Logic: `not`, `and`, `or`), Tier 3 (Goal-Directed Execution: `&`, `|`, `!`),
   and Tier 4 (Relational Belief Queries: `believes (...)`).
1. Expression operator precedence is implemented across 13 distinct levels in `parser.cpp` per
   Harmonization Spec §4.3.
1. Bounded expressions cut remaining choice points at statement boundaries (`#CutChoiceDepth`).

______________________________________________________________________

### 2.2 [MAJOR] Omission of Safe Navigation (`?.`) and Null Coalescing (`??`) (P33)

#### Context & Specification

In `vagent-language-harmonization.md` (§3.3, §4.1, Register P33), Proposal P33 explicitly includes
safe navigation (`?.`) and null coalescing (`??`) as canonical operators for ergonomic `Opt<T>`
manipulation:

```vagent
let zip := user?.address?.zip_code         // Safe navigation: yields Opt<Str>
let display := user?.nickname ?? "Unknown"  // Null coalescing: yields Str
```

Lexer rules explicitly allocate `TokenKind::QuestionDot` (`?.`) and `TokenKind::DoubleQuestion`
(`??`). Precedence is strictly defined: `??` at Precedence 5, `?.` at Precedence 13.

#### Defect in Implementation Plan

In the implementation plan:

- Track 2A (Lexer Tokens, lines 197–208) omits `TokenKind::QuestionDot` and
  `TokenKind::DoubleQuestion`.
- Track 2B (AST Definitions, lines 220–227) omits AST expressions for `SafeNav` and `Coalesce`.
- Track 2C (Parser Extensions) does not parse `?.` or `??`.
- Track 3B (Canonical Option) covers only `opt?`, `opt!`, and `map.get(k)`, omitting `?.` and `??`.
- Track 4 (Lowering) omits operational lowering for `?.` and `??`.
- Proposal Register P33 (line 531) mentions only `?ident` and `expr?`.

#### Required Remediation

1. In Track 2A Item 3: Add `TokenKind::QuestionDot` (`?.`) and `TokenKind::DoubleQuestion` (`??`).
1. In Track 2B Item 3: Add `ExprKind::SafeNav` and `ExprKind::Coalesce` to `ast.hpp`.
1. In Track 2C: Add parser support for `base?.field`, `base?.method()`, and `expr ?? fallback`.
1. In Track 3B: Add type checking: `opt?.field` yields `Opt<FieldType>`; `opt ?? default` unifies
   `opt` inner type $T$ with `default` type $T$, producing $T$.
1. In Track 4: Lower `?.` to conditional branch on `Some`/`none`, propagating `none`; lower `??` to
   evaluation of left operand, unwrapping if `Some` or evaluating right operand if `none`.

______________________________________________________________________

### 2.3 [MAJOR] Map Ergonomics: Bidirectional Inference for `Map {}` & Built-in Method Typing (P37)

#### Context & Specification

In `vagent-metacircular-missing-features.md` (§8.4), `Map<K, V>` is specified as a first-class
collection supporting empty map literals (`Map {}`), tripartite lookup (`map[k]?`, `map[k]!`,
`map.get(k)`), key membership (`map.contains(k)`), and destructuring iteration:

```vagent
var table: Map<Str, Int> := Map {}
for (k, v) in table do {
  print(k ++ ": " ++ str(v))
}
```

#### Defect in Implementation Plan

In Track 3E (lines 330–342):

- Item 1 states: *"In `Checker::infer`, add `case ExprKind::Map:`: infer types of all entry keys and
  values, unify key types to $K$ and value types to $V$..."* If a map literal is empty (`Map {}`),
  bottom-up type inference fails because there are zero entries to unify! Track 3E fails to specify
  bidirectional type checking against contextual expected types.
- Track 3E only validates `map[key] := val` assignment in `Checker::place`. It completely omits type
  checking for:
  - `map.get(k) : Opt<V>`
  - `map.contains(k) : Bool`
  - `map.keys() : [K]`
  - `map.len() : Int`
  - `for (k, v) in map do`: destructuring loop iteration, where the loop iterator must infer the
    element type as a 2-tuple `(K, V)`.

#### Required Remediation

1. In Track 3E Item 1: Add bidirectional checking in `Checker::check`: when checking `ExprKind::Map`
   against expected type `Type{"Map", {K, V}}`, an empty literal `Map {}` succeeds with that type.
   If inferring without an expected type and the literal has 0 entries, emit diagnostic
   `E316: Cannot infer type of empty map literal; explicit type annotation required`.
1. In Track 3E: Add typing rules for built-in map methods: `.get(K) -> Opt<V>`,
   `.contains(K) -> Bool`, `.keys() -> [K]`, and `.len() -> Int`.
1. In Track 3E: Add typing rule for `Checker::forExpr`: iterating over `Map<K, V>` produces loop
   target type `(K, V)`.

______________________________________________________________________

### 2.4 [MINOR] Conflation of `E101` Index 0 Rejection with General Out-of-Bounds (P31)

#### Context & Specification

In `vagent-language-harmonization.md` (§2.1, §2.3), `E101` is strictly the static compile-time fatal
diagnostic for literal index `0`:

```text
error[E101]: 1-based indexing violation
  let head := payload[0]
                      ^ index 0 is invalid in 1-based indexing
```

Out-of-bounds subscripts where $i > N$ or $i < -N$ are NOT `E101` violations:

- In goal-directed contexts (or with `?` or `else`), dynamic out-of-bounds evaluation emits `#Fail`,
  allowing backtracking or `else` recovery.
- In deterministic contexts, it raises `IndexOutOfBounds` (or `ActorError::ContractViolation`).

#### Defect in Implementation Plan

In Section 1.1 Invariant 4 (lines 93–94):

> *"All user-facing sequences (strings, lists, grids) use 1-based indexing ($[1, N]$), with $N+1$
> designating the append point and out-of-bounds accesses raising `E101`."*

And in Section 8 Proposal Register P31 (line 529):

> *"`E101` out-of-bounds check across strings, lists, and arrays."*

Saying "out-of-bounds accesses raising `E101`" conflates literal `0` rejection with general
out-of-bounds access. If an author writes `arr[10]` on a 3-element list, reporting `E101` ("index 0
is invalid in 1-based indexing") is factually incorrect and misleading.

#### Required Remediation

Revise Invariant 4 and Register P31:

- `E101` is strictly the static rejection of literal index `0`.
- Out-of-bounds indexing ($i < -N$, $i > N$, or dynamic evaluated index $0$) fails goal-directed
  evaluation (`#Fail`) or raises runtime `IndexOutOfBounds` in assertive deterministic contexts.

______________________________________________________________________

### 2.5 [MINOR] Missing Slicing Edge Cases and Scanning Coordinate Primitives (P31, P33)

#### Context & Specification

In `vagent-language-harmonization.md` (§2.4, §2.5):

1. **Slicing Edge Cases**: For `coll[start..end]`:
   - Slice length is $L = end - start + 1$.
   - When $end = start - 1$, an empty sequence is produced (e.g. `text[2..1] == ""`).
   - When $end < start - 1$, slicing fails (`#Fail`).
   - Negative slice bounds resolve via $N + 1 + k$ (e.g. `text[2..-1]`).
1. **Scanning Boundary Coordinates & `tab_end()`**:
   - Element subscripts address discrete cells in $[1, N]$.
   - Scanning positions address boundaries in $[1, N+1]$.
   - Because `tab(0)` is strictly banned by `E101`, string scanning requires `tab_end()` (or
     `tab(subject().len() + 1)`) to advance to the end of the subject.

#### Defect in Implementation Plan

- Track 3A Deliverable 4 mentions only: *"Range slicing `coll[start..end]` is 1-based
  closed-interval $[start, end]$."* It omits the edge-case definitions for $end = start - 1$ and
  $end < start - 1$.
- Track 2A Item 4 mentions `subject()` and `pos()`, but omits `tab_end()`. Without `tab_end()`,
  existing scanning code that previously relied on Icon's `tab(0)` has no standard idiom to reach
  EOF.

#### Required Remediation

1. In Track 3A Item 4: Explicitly specify slice boundary semantics: $end = start - 1$ produces an
   empty sequence; $end < start - 1$ emits `#Fail`; negative bounds resolve via $N + 1 + k$.
1. In Track 1A and Track 2A: Explicitly include `tab_end()` alongside `subject()` and `pos()` as
   standard scanner built-ins.

______________________________________________________________________

### 2.6 [MINOR] Multi-Arm Plan Dead Code Verification & Goal Parameter Scope (P36)

#### Context & Specification

In `vagent-metacircular-missing-features.md` (§7.2, §7.3):

- Multi-arm plans permit `where Expr => Block`, `case Pattern ("where" Expr)? => Block`, and
  `default => Block`.
- `default =>` is an unconditional catch-all arm.
- Parameters bound in the plan goal header (`plan P for Goal(?x, ?y)`) are bound across all arms.

#### Defect in Implementation Plan

In Track 3D (Multi-Arm Plan Checking, lines 314–328):

1. The plan does not verify that `default =>` must be the final arm. If an author writes an arm
   after `default =>`, it is unreachable dead code.
1. The plan specifies that pattern variables in `case Pattern` are scoped to that arm, but does not
   explicitly affirm that goal header parameters remain in scope across all arms.

#### Required Remediation

1. In Track 3D: Add semantic rule: `Checker::declaration` verifies that `default =>` occurs at most
   once and is the terminal arm in `PlanArmList`. Any arm following `default =>` emits diagnostic
   `E317: Unreachable plan arm; 'default' arm must be terminal`.
1. In Track 3D: Affirm that goal parameters (`GoalPattern`) are populated into the parent
   environment before checking individual arms.

______________________________________________________________________

### 2.7 [MINOR] Event Header Payload Destructuring (P35)

#### Context & Specification

In `vagent-language-harmonization.md` (§6.2, §6.3), delimiter orthogonality frees square brackets
from declaration headers (`on Event where Guard`), and introduces deep record/event payload
destructuring in the header:

```vagent
on CollisionWarning { distance: ?d, obstacle_id: ?obs } as w where d < 10.0 {
  emit AvoidObstacle(obs)
}
```

#### Defect in Implementation Plan

In Track 2C Item 1:

> *"Parse `(...)` as `ExprKind::Tuple` ... and `on Event where Guard Block` for event handlers."*

This deliverable specifies replacing `[` with `where`, but omits parsing the optional record pattern
`{ field: Pat, ... }` between the event identifier and the `as` / `where` clauses.

#### Required Remediation

In Track 2C Item 1, clarify that `Parser::handler` parses optional record pattern destructuring
following the event name: `on Event (RecordPattern)? ("as" Ident)? ("where" Expr)? Block`.

______________________________________________________________________

## 3. Section-by-Section Plan Audit

| Plan Section                   | Focus & Proposal             | Status                   | Findings & Ergonomic Assessment                                                                                                                                                                    |
| :----------------------------- | :--------------------------- | :----------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **§1. Scope & Invariants**     | Invariants 1–4, P26–P37      | **Needs Text Fix**       | Invariant 4 conflates `E101` with general out-of-bounds (§2.4).                                                                                                                                    |
| **§2. Phase 1 (Substrate)**    | P26, P27, P37 (BST)          | **Clean with Minor Gap** | Add `tab_end()` to stdlib built-ins (§2.5). Tree BST on `d.left`/`d.right` is topologically sound.                                                                                                 |
| **§3. Phase 2 (Syntax & AST)** | P30, P33, P34, P35, P36, P37 | **Major Gaps**           | Item 5 mislabels 4-tier hierarchy (§2.1). Omission of `?.`, `??` (§2.2). Omission of event destructuring (§2.7).                                                                                   |
| **§4. Phase 3 (Semantics)**    | P30, P31, P32, P36, P37      | **Major Gaps**           | Track 3E lacks bidirectional inference for `Map {}` and map method typing (§2.3). Track 3D lacks terminal `default` check (§2.6). Slice edge cases need specification (§2.5).                      |
| **§5. Phase 4 (Lowering)**     | P29, P30, P36, P37           | **Clean**                | Deterministic decision trees (`#Equal`/`#Branch`) for pattern matching are excellent. Intra-turn `m_arm` rollback and untried-arm ladder are well designed. Add lowering for `?.` and `??` (§2.2). |
| **§6. Phase 5 (Stdlib)**       | P28, 11 Stdlib Modules       | **Clean**                | Module migration sequence is logical. 1-based indexing, pattern matching, and multi-arm plans will dramatically clean up `mind.vagent` and `checker.vagent`.                                       |
| **§7. Phase 6 (Testing)**      | Unit, Quine, Headless        | **Clean**                | Test matrix covers all 12 proposals. Headless invariants respected.                                                                                                                                |
| **§8. Proposal Register**      | P26–P37 Table                | **Needs Text Fix**       | Correct P31 and P34 summaries to match true specifications (§2.1, §2.4).                                                                                                                           |
| **§9. Refused Alternatives**   | Architectural Invariants     | **Clean**                | Clear Nelsonian justifications (no disk writes, no cross-actor raw CellRefs, no choice leaks).                                                                                                     |

______________________________________________________________________

## 4. Recommended Text Adjustments for the Implementation Plan

### Adjustment 1: Section 1.1, Invariant 4 (Lines 92–97)

*Current Text:*

```text
4. Universal 1-Based Indexing & Delimiter Orthogonality: All user-facing sequences (strings, lists,
   grids) use 1-based indexing ([1, N]), with N+1 designating the append point and out-of-bounds
   accesses raising E101. Delimiters are orthogonally partitioned: (...) for fixed-arity tuples,
   [...] for dynamic lists (with ... spread), and {...} for named records and event payloads.
```

*Replacement Text:*

```text
4. Universal 1-Based Indexing & Delimiter Orthogonality: All user-facing sequences (strings,
   lists, grids) use 1-based indexing ([1, N]), with N+1 designating the append point, literal
   index 0 statically rejected with E101, and out-of-bounds accesses failing goal-directed
   evaluation (#Fail) or raising IndexOutOfBounds. Delimiters are orthogonally partitioned:
   (...) for fixed-arity tuples, [...] for dynamic lists (with ... spread), and {...} for
   named records and event payloads.
```

### Adjustment 2: Section 3.1, Track 2A (Lines 197–208)

*Current Text:*

```text
1. Add reserved keyword tokens: TokenKind::KwMap (Map), TokenKind::KwMatch (match), ...
2. Add punctuation tokens: TokenKind::Spread (...) and TokenKind::FatArrow (=>).
3. Sigil ? Lexical Disambiguation: Prefix ? immediately followed by identifier emits
   TokenKind::LogicVariable (?ident). Postfix ? immediately following an expression or ] emits
   TokenKind::Question (?).
4. Scanning State Functions: Remove prefix & sigil from scanning; access scanning state via
   built-in functions subject() and pos().
5. Banish ; as Disjunction: Strictly banish ; in plan guards and boolean expressions. Implement
   the 4-tier operator hierarchy in the lexer and parser: Tier 1 (not, and, or), Tier 2
   (relational), Tier 3 (concatenation ++), Tier 4 (arithmetic).
```

*Replacement Text:*

```text
1. Add reserved keyword tokens: TokenKind::KwMap (Map), TokenKind::KwMatch (match),
   TokenKind::KwWith (with), TokenKind::KwDefault (default), TokenKind::KwCase (case).
2. Add punctuation tokens: TokenKind::Spread (...), TokenKind::FatArrow (=>),
   TokenKind::QuestionDot (?.), and TokenKind::DoubleQuestion (??).
3. Sigil ? Lexical Disambiguation: Prefix ? immediately followed by identifier emits
   TokenKind::LogicVariable (?ident). Standalone ? in infix position emits TokenKind::Scan (?).
   Postfix ? following an expression or ] emits TokenKind::Question (?). TokenKind::QuestionDot
   (?.) and TokenKind::DoubleQuestion (??) are recognized as distinct tokens.
4. Scanning State & Built-in Functions: Remove prefix & sigil from scanning; access scanning state
   via built-in functions subject(), pos(), and tab_end().
5. Banish ; as Disjunction & 4-Tier Evaluation Regimes: Strictly banish ; in plan guards and
   boolean expressions; semicolons and newlines are strictly Tier 1 procedural statement
   separators. Implement the 4-tier evaluation hierarchy: Tier 1 (Procedural Sequencing: ;, \n),
   Tier 2 (Pure Boolean Logic: not, and, or), Tier 3 (Goal-Directed Execution: &, |, !), and
   Tier 4 (Relational Belief Queries: believes (...)). Expression operators follow the 13-tier
   precedence table. Choice points are automatically cut at statement boundaries (#CutChoiceDepth).
```

### Adjustment 3: Section 4.5, Track 3E (Lines 336–342)

*Current Text:*

```text
1. In Checker::infer, add case ExprKind::Map:: infer types of all entry keys and values, unify
   key types to K and value types to V, synthesize Type{"Map", {K, V}}, and validate key type
   K in {Str, Int, CellRef}.
2. In Checker::place: validate map[key] := val assignment where map is a mutable local or
   field, key matches K, and val matches V.
```

*Replacement Text:*

```text
1. In Checker::infer and Checker::check, add Map literal typing: for non-empty Map literals,
   infer and unify key types to K and value types to V, synthesizing Type{"Map", {K, V}}. For empty
   literals Map {}, support bidirectional checking against contextual expected type Map<K, V>; if no
   expected type exists, emit diagnostic E316. Validate key type K in {Str, Int, CellRef}.
2. In Checker::place: validate map[key] := val assignment where map is a mutable local or
   field, key matches K, and val matches V.
3. In Checker::call and Checker::member: type built-in map methods: map.get(k) -> Opt<V>,
   map.contains(k) -> Bool, map.keys() -> [K], and map.len() -> Int.
4. In Checker::forExpr: validate iteration over Map<K, V>, binding the loop variable to a
   2-tuple (K, V).
```

______________________________________________________________________

## 5. Conclusion & Ergonomic Verdict

The engineering roadmap in `design/vagent-harmonization-and-missing-features-implementation-plan.md`
is sound in its architecture, Nelsonian invariants, and compilation phasing. Addressing the three
MAJOR issues (reinstating the true 4-tier evaluation hierarchy, adding `?.` and `??` deliverables,
and completing map type checker ergonomics) along with the four MINOR clarifications will ensure the
implementation delivers an uncompromised, highly ergonomic surface language.

```text
Findings Summary:
- BLOCKING: 0
- MAJOR:    3 (Hierarchy definition, Safe Nav/Coalesce operators, Map type ergonomics)
- MINOR:    4 (E101 conflation, Slice/Scan coordinates, Terminal default arm, Event destructuring)

VERDICT: REVISE
```
