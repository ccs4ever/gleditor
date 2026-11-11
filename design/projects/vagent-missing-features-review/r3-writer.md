# Round 3 critique: Technical Writer (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 3 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1, R1.B2, R1.M1, R1.M2, R1.M3: RESOLVED in Round 2 and remain resolved.
- Round 2 had 0 BLOCKING and 0 MAJOR issues.

## BLOCKING

None.

## MAJOR

### 1. (§8.4) Missing EBNF Grammar for Map Type, Literals, and Subscripting

Full title: Provide Formal EBNF Grammar Specification for `Map<K, V>` and Subscript Operations

**Problem:** Section 6 specifies algebraic pattern matching with an explicit EBNF subsection
(§6.1), and Section 7 specifies multi-arm plans with an explicit EBNF subsection (§7.2). Section 8,
however, introduces first-class map types (`Map<K, V>`), map literals (`Map { ... }`), fallible
subscripting with `#Fail` (`map[k]?`), safe option lookups (`map.get(k)`), and indexed assignment
(`map[k] := v`), but provides NO formal EBNF grammar rules. The Round 3 review brief explicitly
requires verification that "the EBNF grammar rules for `PlanBody`, `MultiArmPlanBody`, `PlanArm`,
and `Map` [are] unambiguous and consistent with existing syntax." Without formal productions, the
syntax remains under-specified for compiler front-end implementation.

**Evidence:** Lines 674–708 of `design/vagent-metacircular-missing-features.md` present bullet
points and code listings without an EBNF grammar block. In contrast, lines 441–462 (§6.1) and lines
552–563 (§7.2) contain comprehensive EBNF definitions.

**Fix:** Add a formal EBNF grammar subsection (e.g. `### 8.4 Surface Grammar (EBNF)`) specifying:

```ebnf
MapType        ::= "Map" "<" Type "," Type ">"
MapLiteral     ::= "Map" "{" MapEntryList? "}"
MapEntryList   ::= (MapEntry ("," | "\n"))* MapEntry?
MapEntry       ::= Expr ":" Expr
MapIndexExpr   ::= PostfixExpr "[" Expr "]" "?"?
MapIndexAssign ::= PostfixExpr "[" Expr "]" ":=" Expr
```

Explicitly define the semantics of `m[k]` vs. `m[k]?`: `m[k]?` performs goal-directed lookup
emitting `#Fail` if absent, while `m[k]` asserts key presence or aborts with `KeyNotFound`.

______________________________________________________________________

### 2. (§7.2) Syntax & Type Errors in Examples (`where _ =>` and Record Patterns)

Full title: Correct Invalid Guard Syntax in Example 1 and Add `RecordPattern` to EBNF

**Problem:** The worked code examples in §7.2 deviate from the grammar and contain two invalid
constructs:

1. In Worked Example 1 (line 575), the fallback arm is written as `where _ => { ... }`. According
   to `PlanArm ::= ("case" Pattern)? ("where" Expr)? "=>" Block`, the token following `where` must
   be an `Expr` evaluating to `Bool`. In VAgent, `_` is a `WildcardPattern` (§6.1, line 456), not
   an `Expr`. In expression positions, `_` is an invalid syntax error. In §7.1 (line 544), the
   equivalent BEFORE plan used `where true`.
1. In Worked Example 2 (lines 586–594), arms use record patterns
   (`Token { kind: Ident, text: ?t }`).
   However, the `Pattern` grammar defined in §6.1 (lines 449–460) only permits `LiteralPattern`,
   `WildcardPattern`, `BindingPattern`, `CtorPattern` (which uses parentheses `Ident(...)`), and
   `TuplePattern` (which uses brackets `[...]`). There is NO production for `RecordPattern`
   (`Ident "{" ... "}"`) anywhere in the document.

**Evidence:** From `design/vagent-metacircular-missing-features.md:575`:

```vagent
  where _ => {
    emit DeliveryDelayed(pkg.id)
  }
```

From `design/vagent-metacircular-missing-features.md:586–594`:

```vagent
  case Token { kind: Ident, text: ?t } where is_keyword(t) => {
    emit KeywordFound(t)
  },
  case Token { kind: Ident, text: ?t } => {
    emit IdentifierFound(t)
  },
  case Token { kind: Number, text: ?t } => {
    emit NumberFound(str_to_int(t))
  },
```

**Fix:**

1. In Worked Example 1 (line 575), replace `where _ =>` with `where true =>` (or `default =>` if
   supported in `PlanArm`).
1. Add `RecordPattern` to §6.1 (and document it in §7.2):

```ebnf
Pattern        ::= LiteralPattern
                 | WildcardPattern
                 | BindingPattern
                 | CtorPattern
                 | RecordPattern
                 | TuplePattern

RecordPattern  ::= Ident "{" (FieldPattern ("," FieldPattern)*)? "}"
FieldPattern   ::= Ident ":" Pattern
```

______________________________________________________________________

### 3. (§7.2, §7.3) Undefined `GoalPattern` and Unspecified `case Pattern` Target

Full title: Formally Define `GoalPattern` and Clarify Plan Arm Matching Target Semantics

**Problem:** The multi-arm plan specification leaves non-terminals undefined and match semantics
ambiguous:

1. In §7.2 (line 558), `PlanDecl` is specified as:
   `PlanDecl ::= "plan" Ident "for" GoalPattern PlanBody`.
   `GoalPattern` is never defined in §7.2, §6.1, or anywhere else in the document. In existing
   syntax, a plan binds parameters via `Target(?arg1, ?arg2)`. Without defining `GoalPattern`, the
   grammar is incomplete.
1. In §7.2 (Example 2) and §7.3 (Deliberation Semantics), `case Pattern` is introduced into
   `PlanArm`, but the specification never defines what value is being matched against. In
   `plan ProcessTokenPlan for ProcessToken(?tok)`, does `case Token { ... }` match the parameter
   `tok`, the goal argument tuple, or the entire goal term? If a goal has multiple parameters
   (e.g. `DeliverPackage(?pkg, ?dest)`), does `case Pattern` match a tuple `[?pkg, ?dest]`?

**Evidence:** From `design/vagent-metacircular-missing-features.md:558–562`:

```ebnf
PlanDecl         ::= "plan" Ident "for" GoalPattern PlanBody
PlanBody         ::= SinglePlanBody | MultiArmPlanBody
SinglePlanBody   ::= ("where" Expr)? Block
MultiArmPlanBody ::= "{" (PlanArm ("," | "\n"))* "}"
PlanArm          ::= ("case" Pattern)? ("where" Expr)? "=>" Block
```

From `design/vagent-metacircular-missing-features.md:603–606`: "The first arm whose pattern matches
and whose `where` guard evaluates to `true` is selected for execution."

**Fix:**

1. Define `GoalPattern` in §7.2:

```ebnf
GoalPattern      ::= Ident ("(" (TermPattern ("," TermPattern)*)? ")")?
TermPattern      ::= BindingPattern | Pattern
```

1. Add an explicit clause in §7.3 defining the match target: "When evaluating `case Pattern`, the
   pattern is matched against the goal invocation's argument value (for single-parameter goals)
   or the argument tuple `[arg1, ..., argN]` (for multi-parameter goals). Parameter bindings
   established in `GoalPattern` remain in scope across all arms."
1. Normalize `MultiArmPlanBody` delimiters to match §6.1 `MatchArmList`:

```ebnf
MultiArmPlanBody ::= "{" PlanArmList "}"
PlanArmList      ::= (PlanArm ("," | "\n"))* PlanArm?
```

______________________________________________________________________

## MINOR

### 1. (§8.4) Destructuring Iteration Uses Round Parens Instead of Bracketed Tuple

Full title: Use Standard VAgent Bracketed Tuple Syntax in Map Iteration Example

**Problem:** Line 707 specifies destructuring iteration as `for (k, v) in map do { ... }`. In
VAgent, tuples are strictly bracketed (`[k, v]`), as defined in `TuplePattern` (§6.1, line 459:
`"[" (Pattern ("," Pattern)*)? "]"`), and parentheses `(...)` represent Unit `()` or grouping.
Furthermore, `parser.cpp:1020` currently expects a single identifier (`for Ident in Expr do
Block`); pattern destructuring in `for` loops should be explicitly noted as part of the
destructuring extension.

**Evidence:** Line 707 vs. line 459 (`TuplePattern ::= "[" ... "]"`) and line 500
(`let [?scope_id, ?bindings, ?parent_id] := item`).

**Fix:** Change line 707 to:

```vagent
for [k, v] in map do { ... }
```

______________________________________________________________________

### 2. (§8.2) Topological Inconsistency on 2-Degree Invariant for `d.branch`

Full title: Clarify 2-Degree Manifold Topology for Binary Search Tree

**Problem:** Section 8.2 states that a binary search tree is represented natively along `d.branch`
where `NEG` links to the left subtree and `POS` links to the right subtree "without violating
2-degree rank limits." However, in a ZigZag manifold, links along any dimension are strictly
bidirectional (if A's POS is B, then B's NEG is A). In a binary search tree, an internal node has
3 edges (parent, left child, right child). If Root's POS is Right Child, then Right Child's NEG is
Root, which exhausts its negative link and leaves no NEG link for Right Child's own left subtree. A
binary tree cannot be embedded directly on a single ZigZag dimension where POS/NEG represent child
pointers without violating degree limits or losing parent links.

**Evidence:** Lines 646–663 of `design/vagent-metacircular-missing-features.md`.

**Fix:** Clarify the exact manifold topology: either use two orthogonal dimensions (e.g. `d.left`
and `d.right`, where parent POS links to child and child NEG links to parent), or represent the tree
as a first-child/next-sibling structure (`d.child` and `d.sibling`), or explain the intermediary
branching node topology that preserves degree <= 2 on all cells.

______________________________________________________________________

### 3. (§9, lines 722–723) File and Line Anchor Drift in Proposals P31 and P32

Full title: Correct Codebase File and Line Anchors in Proposal Register for P31 and P32

**Problem:** Codebase citations in the proposal register exhibit anchor drift and misallocated
responsibilities:

1. In **P31**, `parser.cpp:570–615` is cited, but `Parser::plan()` begins at line 560
   (`parser.cpp:560–615`). Furthermore, `compiler.cpp:1498–1530` is cited for plan lowering, but
   those lines contain unrelated helper functions (`literalCell`, `resolved`, `referencesParam`).
   Plan declaration lowering actually resides in `apps/common/xanadu/vagent/decls.cpp:1979–2000`
   and `genesis.cpp:296–315`.
1. In **P32**, `parser.cpp:840–860` is cited for Map syntax, but that range only handles subscript
   postfix indexing (`expr[...]`). Map literal parsing (`Map { ... }`) belongs in `primary()`
   (`parser.cpp:943–1085`).

**Evidence:**

- `apps/common/xanadu/vagent/parser.cpp:560–575` (`Parser::plan()`).
- `apps/common/xanadu/vagent/decls.cpp:1979–2000` (`DeclKind::Plan` lowering).
- `apps/common/xanadu/vagent/compiler.cpp:1498–1530` (`literalCell`).
- `apps/common/xanadu/vagent/parser.cpp:1080–1086` (`primary()` identifier/record handling).

**Fix:** Update the "Files Touched & Line Anchors" column in §9:

- P31: `apps/common/xanadu/vagent/parser.cpp:560–615`, `ast.hpp`, `decls.cpp:1979–2000`,
  `compiler.cpp:1498–1530`, `resolve.cpp:763–780`, `stdlib/checker.vagent`.
- P32: `apps/common/xanadu/vagent/vortex/vortex_core.hpp:45–65`, `vortex_core.cpp`,
  `vortex_stdlib.hpp:170–190`, `vortex_stdlib.cpp:2650–2695`, `apps/common/xanadu/vagent/token.hpp`,
  `parser.cpp:840–860, 943–1085`, `checker.cpp:1380–1400, 1965–1985`, `compiler.cpp:1600–1625`.

______________________________________________________________________

VERDICT: 0 BLOCKING, 3 MAJOR, 3 MINOR (NOT CONVERGED)
