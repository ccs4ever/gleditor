# VAgent Language Harmonization: Indexing, Options, Operators & Delimiters

This document specifies the unified language harmonization for VAgent
(`apps/common/xanadu/vagent/`). VAgent synthesizes concepts from five foundational paradigms: SARL
(Agent-Oriented Programming), BDI (Belief-Desire-Intention cognitive agency), Icon/Vicon
(goal-directed execution and string scanning), Prolog/VProlog (Horn clause logic and unification),
and Vortex/ZigZag (2-degree hyperstructural cell manifolds and immutable Merkle ops spools).

As the language evolved, competing syntactic and semantic conventions produced severe dissonance:
conflicting 0-based and 1-based indexing, fragmented Option (`Opt<T>`) representations, lexical
overloading of `?`, catastrophic collision between procedural statement separators and Prolog
disjunction (`;`), and over-subscription of square brackets (`[...]`).

This specification resolves every conflict through a formal five-expert dialogue (Vortex/VAgent,
SARL, Prolog, Icon, and Programming Language Design & Ergonomics), establishing an orthogonal,
internally consistent, and mechanically sympathetic language standard.

______________________________________________________________________

## 1. Executive Summary & Harmonization Invariants

The harmonized language is governed by five architectural invariants:

1. **Strict 1-Based Indexing Everywhere (§2, P31)**: All sequences, lists, strings, and tuple
   elements adhere to discrete 1-based index domains $[1, N]$. Out-of-bounds subscripts fail
   goal-directed evaluation (`#Fail`). Index `0` is strictly invalid, triggering compile-time fatal
   diagnostic `E101` when literal. Negative indexing symmetrically addresses from the tail
   ($-1 \equiv N, -N \equiv 1$).
1. **Canonical `Opt<T>` and Goal-Directed Failure (§3, P32)**: Option is nominally typed as `Opt<T>`
   with constructors `Some(v)` and `none`. Zero sentinels (`""`, `-1`) are banished from the
   standard library. Evaluating `opt?` in a goal-directed expression yields $v$ if `Some(v)` or
   triggers VM `#Fail` if `none`, seamlessly bridging typed options into backtracking search.
1. **Lexical Disambiguation of `?` & Dual-Form Scanning (§4, P33)**: The prefix token `?ident` is
   reserved exclusively for logic and unification variables (`TokenKind::LogicVariable`). Safe
   navigation (`?.`) and null-coalescing (`??`) are distinct lexical tokens. String scanning
   provides both a structured block form (`scan subject with { ... }`) and an inline expression form
   (`subject ? scan_expr`), with scanning precedence corrected to bind tighter than assignment and
   looser than goal conjunction (`&`).
1. **Banishment of Prolog Semicolon & 4-Tier Operator Hierarchy (§5, P34)**: The semicolon (`;`) is
   strictly and exclusively a procedural statement separator/terminator; it is permanently banned
   from denoting logical disjunction. Four orthogonal tiers govern evaluation:
   - *Tier 1*: Statement sequencing (`;`, `\n`).
   - *Tier 2*: Pure Boolean logic (`not`, `and`, `or`).
   - *Tier 3*: Goal-directed execution (`&` conjunction, `|` alternation, `!` cut).
   - *Tier 4*: Relational belief queries (`believes (P(?x) & Q(?x))`).
1. **Orthogonal Delimiter Partitioning & Unified Matching (§6, P35)**: Square brackets `[...]` are
   freed from declaration headers: SARL's legacy `on Event [guard]` becomes `on Event where guard`
   (unifying with `plan for Goal where guard`). Delimiters are partitioned orthogonally: `(...)` for
   tuples, `[...]` for lists (with spread `[?head, ...?tail]`), and `{...}` for records. Pattern
   matching is unified under `match ... with`.

______________________________________________________________________

## 2. Axis 1: Indexing Harmonization (1-Based Everywhere)

### 2.1 Domain & Coordinates

Across mathematics, relational logic ($R(t_1, \dots, t_n)$), ISO Prolog (`arg/3`, `nth1/3`),
APL/VPL, VQL, Vicon, and ZigZag manifold ranks (`+d.step`), indexing is fundamentally 1-based:

```text
Sequence of length N:
Elements:      [  e_1  ,  e_2  ,  e_3  , ... ,  e_N  ]
1-Based Index:     1       2       3             N
Reverse Index:    -N    -(N-1)  -(N-2)          -1
```

- **Domain**: For any sequence of length $N$, valid indices belong to the closed integer set
  $\{1, 2, \dots, N\}$.

- **Subscript Access**: `arr[i]` accesses element $i$.

- **Goal-Directed Failure on Out-of-Bounds**: Subscripting out of bounds ($i < -N$, $i = 0$, or
  $i > N$) fails goal-directed evaluation (`#Fail`), cleanly participating in backtracking search or
  recovering via `else`:

  ```vagent
  let first := arr[1] else return none
  ```

### 2.2 Symmetrical Negative Indexing

Negative indexing addresses relative to the tail of the sequence:

$$
\text{effective}(k) = \begin{cases} k & \text{if } k > 0 \\ N + 1 + k & \text{if } k < 0 \end{cases}
$$

- `arr[-1]` yields element $N$ (the last element).
- `arr[-2]` yields element $N-1$ (the penultimate element).
- `arr[-N]` yields element $1$ (the first element).
- For $k = 0$, $N + 1 + 0 = N + 1 > N$, which is out of bounds.

### 2.3 Strict Rejection of Index `0`

Index `0` does not belong to either forward or reverse element domains:

1. **Compile-Time Static Diagnostics (`E101`)**: Subscripting with a literal integer `0` (e.g.
   `arr[0]`, `str[0]`) triggers a compile-time fatal diagnostic:

   ```text
   error[E101]: 1-based indexing violation
     --> telemetry.vagent:14:21
      |
   14 |   let head := payload[0]
      |                       ^ index 0 is invalid in 1-based indexing
      |
      = help: indices range from 1 to N (head is at [1], tail is at [-1])
   ```

1. **Runtime Evaluation**: Dynamic evaluation where an expression resolves to `0` triggers goal
   failure (`#Fail`) in goal-directed contexts, or raises `IndexOutOfBounds` in deterministic
   contexts.

### 2.4 Element Subscripts vs. Inter-Character Scanning Positions

A critical distinction exists between discrete element subscripting and continuous string scanning:

| Construct                       | Domain     | Coordinate Model                        | End-of-Sequence Meaning                           |
| :------------------------------ | :--------- | :-------------------------------------- | :------------------------------------------------ |
| **Element Subscript (`s[i]`)**  | $[1, N]$   | Addresses discrete characters/items     | `s[-1]` addresses element $N$; `s[0]` is invalid. |
| **Scanning Position (`pos()`)** | $[1, N+1]$ | Addresses boundaries between characters | `tab(0)` is an Icon idiom meaning position $N+1$. |

To eliminate confusion between the two models, scanning introduces explicit, readable position
primitives: `tab_end()` (or `tab(subject().len() + 1)`) replaces `tab(0)`. Icon's archaic prefix `&`
sigil is dropped entirely, exposing `pos()` and `subject()` as standard built-in functions.

### 2.5 Slices & Loop Ranges

- **Interval Slicing**: Slices use inclusive closed intervals $[start, end]$:

  ```vagent
  let sub := text[1..3]    // Yields characters 1, 2, and 3 (length 3)
  let tail := text[2..-1]  // Yields from character 2 to the end
  ```

  Slice length is $L = end - start + 1$. When $end = start - 1$, an empty sequence is produced. When
  $end < start - 1$, slicing fails (`#Fail`).

- **Loop Ranges**: Range iteration `for i in start..end do` iterates through inclusive values from
  `start` to `end`:

  ```vagent
  for i in 1..total_items do {
    process(items[i])
  }
  ```

______________________________________________________________________

## 3. Axis 2: Option (`Opt<T>`) Consistency & Goal-Directed Failure

### 3.1 Canonical Type & Constructors

- **Type Syntax**: The nominal generic type is strictly `Opt<T>` (e.g. `Opt<Int>`, `Opt<Agent>`).
  Postfix `T?` is rejected in type declarations to reserve `?` for expressions and pattern
  variables.

- **Constructors**:

  - `Some(v)`: Constructs a present option containing value $v$ (functor `"Some"` with 1-ary value
    cell along `+d.step`).
  - `none`: The lowercase keyword representing absence (0-ary unit term, zero-sentinel compliant).

- **Subtype Promotion**: An expression of type $T$ implicitly promotes to `Opt<T>` where expected:

  ```vagent
  var current_target: Opt<Str> := "base_station"  // Promotes to Some("base_station")
  current_target := none                          // Resets to none
  ```

### 3.2 Goal-Directed Unwrapping (`opt?`) vs. Assertive Force-Unwrapping (`opt!` / `opt.unwrap()`)

Options provide two distinct unwrapping semantics to accommodate both goal-directed backtracking and
contract invariant assertion:

| Construct  | Category                          | Behavior on `Some(v)` | Behavior on `none`             | Runtime Impact                                                                                                               |
| :--------- | :-------------------------------- | :-------------------- | :----------------------------- | :--------------------------------------------------------------------------------------------------------------------------- |
| **`opt?`** | **Goal-Directed / Soft Unwrap**   | Yields $v$            | Emits VM **`#Fail`**           | Backtracks to enclosing choice point (`#Choice` / `\|`). Reverts speculative writes via trail. Never crashes the actor turn. |
| **`opt!`** | **Assertive / Hard Force-Unwrap** | Yields $v$            | Raises **`ContractViolation`** | Halts current turn immediately. Logs fatal diagnostic to outbox. Discards uncommitted turn state.                            |

#### Goal-Directed Unwrapping (`opt?`)

Options bridge directly into Vicon's success/failure execution regime via postfix `?`:

```vagent
let target := current_target?  // If Some(v), yields v; if none, fails (#Fail)
```

- In search, query chaining, and generator contexts, evaluating `none?` triggers VM `#Fail`, causing
  the runtime to backtrack to the next alternative:

  ```vagent
  let config := load_file("custom.cfg")? | load_file("default.cfg")? | fallback_config
  ```

- In statement contexts, `let` bindings pair with `else`:

  ```vagent
  let Some(?t) := current_target else {
    return Err("No active target")
  }
  ```

#### Assertive Force-Unwrapping (`opt!` / `opt.unwrap()`)

When an invariant guarantees that an option must be present (for example, after an explicit
`is_some()` check), the developer can force-unwrap using `opt!` or `opt.unwrap()`. Evaluating
`none!` is a fatal programmer error that raises `ActorError::ContractViolation`, halting the turn.
Where possible, pattern matching (`let Some(?x) := opt else { ... }`) or null-coalescing
(`opt ?? default`) is preferred over force-unwrapping.

### 3.3 Safe Navigation & Null Coalescing

- **Safe Navigation (`?.`)**: Traverses fields or calls methods, propagating `none` on absent bases:

  ```vagent
  let zip := user?.address?.zip_code  // Yields Opt<Str>
  ```

- **Null Coalescing (`??`)**: Provides a fallback value if the left operand is `none`:

  ```vagent
  let display_name := user?.nickname ?? "Anonymous"
  ```

### 3.4 Banishment of Sentinels in Stdlib

Sentinels (e.g. `""` for missing names, `-1` for invalid indices, `0` for inactive states) are
strictly prohibited. In `apps/common/xanadu/vagent/stdlib/mind.vagent` and `arbitration.vagent`, all
optional fields and return values are harmonized to `Opt<T>`:

```vagent
// stdlib/arbitration.vagent
def next_available_plan(rules: [PlanRule], tried: [Int]) : Opt<PlanRule> {
  for r in rules do {
    if not (tried.contains(r.id)) then return Some(r)
  }
  return none
}
```

______________________________________________________________________

## 4. Axis 3: Disambiguation of `?` & String Scanning

### 4.1 Lexical Tokenization Rules

The lexer (`lexer.cpp`) partitions the character `?` into mutually exclusive tokens based on
immediate lookahead without requiring parser modal states (`allowScan_` deleted):

| Token                       | Lexical Rule                                           | Meaning & Context                                               |
| :-------------------------- | :----------------------------------------------------- | :-------------------------------------------------------------- |
| `TokenKind::LogicVariable`  | `?` immediately followed by `[a-zA-Z_]`                | Logic/unification variable or pattern binding: `?x`, `?target`. |
| `TokenKind::QuestionDot`    | `?.`                                                   | Safe navigation operator: `obj?.field`.                         |
| `TokenKind::DoubleQuestion` | `??`                                                   | Null-coalescing operator: `a ?? b`.                             |
| `TokenKind::Question`       | `?` preceded by expression, followed by space/operator | Postfix goal-directed unwrap: `opt?`.                           |
| `TokenKind::Scan`           | Standalone `?` in binary infix expression position     | Infix string scanning operator: `subject ? scan_expr`.          |
| `TokenKind::Ellipsis`       | `...`                                                  | Spread pattern operator (`...?tail`) and range extension.       |

#### Predictive Lookahead Resolution

In `Parser::postfix()`, the parser distinguishes postfix unwrap `opt?` from binary infix scanning
`s ? scan_expr` via a single-token predictive lookahead: if `?` is followed immediately by an
expression operand token (identifier, literal, open bracket, open parenthesis, or `scan` keyword),
it is treated as binary infix `TokenKind::Scan`; if followed by a statement separator, closing
delimiter, operator, or newline, it is treated as postfix unwrap `opt?`.

### 4.2 String Scanning: Scoped Block & Inline Infix

VAgent supports both a structured block form and a compact inline form for Icon string scanning:

#### Form A: Structured Block Form (`scan subject with { ... }`)

Recommended for multi-statement scanning workflows:

```vagent
scan packet with {
  tab(match("HEADER:"))
  let id := tab(upto(";"))
  move(1)
  let payload := tab_end()
  emit PacketParsed(id: id, body: payload)
}
```

The block pushes the scanning environment (initializing `subject()` to the packet text and `pos()`
to 1), executes the statements, and restores the previous scanning environment upon exit via
`#ScanPush` and `#ScanPop`. Inside the block, the current subject and cursor position are accessed
via `subject()` and `pos()`. Icon's archaic prefix `&` sigil is completely omitted.

#### Form B: Inline Infix Form (`subject ? scan_expr`)

Retained for compact scanner expressions:

```vagent
let id := packet ? (tab(match("ID:")) & tab(upto(";")))
```

### 4.3 Operator Precedence Hierarchy

To fix the parser bug where `?` bound tighter than `&`, operator precedence is strictly ordered:

```text
Precedence 1 (Lowest):  Assignment (:=, <-)
Precedence 2:           Infix String Scanning (?)
Precedence 3:           Alternation / Choice (|)
Precedence 4:           Goal Conjunction (&)
Precedence 5:           Null Coalescing (??)
Precedence 6:           Logical Disjunction (or)
Precedence 7:           Logical Conjunction (and)
Precedence 8:           Equality & Comparison (==, !=, <, <=, >, >=)
Precedence 9:           Range Construction (..)
Precedence 10:          Additive (+, -)
Precedence 11:          Multiplicative (*, /, %)
Precedence 12:          Unary (-, not, ?)
Precedence 13 (Highest): Primary (Subscript [], Member ., Call (), Safe Nav ?.)
```

Under this hierarchy, `s ? tab(a) & tab(b)` parses naturally as `s ? (tab(a) & tab(b))` without
requiring manual parentheses.

______________________________________________________________________

## 5. Axis 4: Plan Operators & Conjunction/Disjunction Harmonization

### 5.1 Banishment of the Prolog Semicolon

In procedural languages, the semicolon (`;`) separates sequential statements. In Prolog, the
semicolon (`;`) denotes disjunction (OR). In BDI plan bodies, this collision created intolerable
ambiguity:

```text
// AMBIGUOUS COLLISION (REFUSED):
plan MoveTo {
  check_battery; move_forward; report_status   // Sequential steps or alternative disjunctions?
}
```

**Architectural Rule**: The semicolon (`;`) and newline (`\n`) are strictly and exclusively
procedural statement separators. The semicolon NEVER denotes disjunction anywhere in VAgent.

### 5.2 The Four-Tier Evaluation Hierarchy

| Tier       | Category                      | Operators          | Semantics & VM Lowering                                                                                        |
| :--------- | :---------------------------- | :----------------- | :------------------------------------------------------------------------------------------------------------- |
| **Tier 1** | **Procedural Sequencing**     | `\n`, `;`          | Executes sequentially along `+d.spin`. Failure halts plan and triggers intention reconsideration.              |
| **Tier 2** | **Pure Boolean Logic**        | `and`, `or`, `not` | Operates on `Bool` values with short-circuiting. Emits no choice points.                                       |
| **Tier 3** | **Goal-Directed Execution**   | `&`, `\|`, `!`     | `&` chains successes; `\|` introduces backtracking choice points (`#Choice`); `!` cuts choice points (`#Cut`). |
| **Tier 4** | **Relational Belief Queries** | `believes (...)`   | Unification queries over `d.clause` ranks. Multi-predicate queries compose via Tier 3 `&` and `\|`.            |

### 5.3 Multi-Predicate Belief Queries

In BDI plans, applicability conditions and queries use `&` for conjunction and `|` for disjunction:

```vagent
plan EvacuatePlan for Evacuate(?zone)
  where believes At(self, ?x, ?y) & believes Danger(?zone, ?x, ?y)
{
  emit SoundAlarm(zone)
  achieve MoveToSafeZone(zone) within 60s
}
```

Compound queries inside a single `believes` block use `&` to conjoin predicates, while commas (`,`)
remain strictly argument separators inside terms:

```vagent
if believes (At(self, ?x, ?y) & Clear(?x, ?y)) then {
  log("Position verified clear at " ++ int_to_str(?x) ++ "," ++ int_to_str(?y))
}
```

### 5.4 Griswold's Principle of Bounded Expressions

Backtracking choice points created by generators (`|`, `many`, `upto`) are confined to the enclosing
expression. Upon reaching a statement boundary (`;` or `\n`), all remaining choice points in the
statement are automatically cut (`#CutChoiceDepth`), ensuring procedural actions never backtrack
across side-effecting statements.

______________________________________________________________________

## 6. Axis 5: Pattern Matching & Destructuring Syntax

### 6.1 Disambiguating Brackets: Delimiter Orthogonality

Square brackets `[...]` are restricted to lists and subscripting. Delimiters are partitioned:

```text
(...)  -> Heterogeneous fixed-arity Tuples:      (?x, ?y), (Int, Str)
[...]  -> Homogeneous dynamic Lists:             [?head, ...?tail], [Int], arr[1]
{...}  -> Structured Records & Event Payloads:   Point { x: ?x, y: ?y }, Event { ?payload }
```

### 6.2 Freeing Brackets from Event Handlers and Plan Guards

SARL's legacy bracket syntax `on Event [guard]` is deprecated. Declaration guards use the `where`
keyword, unifying event handlers and plan preconditions:

```vagent
// Reactive Event Handler
on CollisionWarning as w where w.distance < 10.0 {
  emit EmergencyBrake()
}

// BDI Plan Precondition
plan RescuePlan for Rescue(?target) where believes BatteryLevel(?level) & level > 25 {
  achieve Locate(target) within 30s
}
```

### 6.3 Deep Event & Record Destructuring

Handlers and plans can destructure event and record payloads directly in their signatures:

```vagent
on CollisionWarning { distance: ?d, obstacle_id: ?obs } where d < 10.0 {
  log("Imminent collision with obstacle " ++ obs)
  emit AvoidObstacle(obs)
}
```

### 6.4 Surface Grammar (EBNF)

```ebnf
MatchExpr        ::= "match" Expr "with" "{" (MatchArm ("," | "\n")*)* "}"
MatchArm         ::= Pattern ("where" Expr)? "=>" (Expr | Block)

Pattern          ::= LiteralPattern
                   | WildcardPattern
                   | VariablePattern
                   | CtorPattern
                   | TuplePattern
                   | ListPattern
                   | RecordPattern

LiteralPattern   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral | "none"
WildcardPattern  ::= "_"
VariablePattern  ::= "?" Ident
CtorPattern      ::= Ident ("(" (Pattern ("," Pattern)*)? ")")?
TuplePattern     ::= "(" (Pattern ("," Pattern)*)? ")"
ListPattern      ::= "[" (Pattern ("," Pattern)*)? (",..." Pattern)? "]"
RecordPattern    ::= Ident "{" (FieldPattern ("," FieldPattern)*)? "}"
FieldPattern     ::= Ident ":" Pattern | VariablePattern

DestructureLet   ::= "let" Pattern ":=" Expr ("else" Block)?
```

- **List Spread Destructuring**: `[?head, ...?tail]` decomposes a list into its first element and
  the remaining slice. The `...` spread avoids collision with alternation `|`.
- **Single-Element Tuples**: A 1-element tuple requires a trailing comma: `(?a,)` or `(val,)`,
  distinguishing it from parenthesized grouping expressions `(?a)` or `(val)`.
- **Refutable Let Bindings**: Binding a refutable pattern without an `else` block triggers
  compile-time diagnostic `E313: Refutable pattern in let binding requires else clause`.

______________________________________________________________________

## 7. Stdlib Harmonization Case Study: `mind.vagent`

To illustrate the dramatic simplification achieved by this specification, we contrast the existing
`apps/common/xanadu/vagent/stdlib/mind.vagent` against the harmonized implementation.

### 7.1 Before: 48 Manual Scalar Slots & Cascading Conditionals

Because the language previously lacked 1-based lists, records, `Opt<T>`, and pattern matching,
`AgentMind` was forced to manually declare 48 scalar variables and 8-level `if/else` ladders:

```vagent
// BEFORE: apps/common/xanadu/vagent/stdlib/mind.vagent (fragment)
agent AgentMind {
  var b1_active: Bool := false
  var b1_pred: Str := ""
  var b1_subj: Str := ""
  var b1_obj: Str := ""
  var b1_weight: Int := 0
  var b1_source: Str := ""

  var b2_active: Bool := false
  var b2_pred: Str := ""
  // ... repeated 8 times up to b8_source (48 variables) ...

  def add_belief(pred: Str, subj: Str, obj: Str, weight: Int, source: Str) : Bool {
    if not b1_active then {
      b1_active := true; b1_pred := pred; b1_subj := subj; b1_obj := obj; b1_weight := weight; b1_source := source
      return true
    }
    if not b2_active then {
      b2_active := true; b2_pred := pred; b2_subj := subj; b2_obj := obj; b2_weight := weight; b2_source := source
      return true
    }
    // ... 8 levels of repetitive assignment cascades ...
    return false
  }
}
```

### 7.2 After: 35 Lines of Safe, Idiomatic VAgent Code

Under the harmonized language, the entire agent shrinks to clean, structured code:

```vagent
// AFTER: apps/common/xanadu/vagent/stdlib/mind.vagent
module std:agent:mind

record Belief {
  predicate: Str,
  subject: Str,
  object: Str,
  weight: Int,
  source: Str
}

agent AgentMind {
  var beliefs: [Belief] := []
  var max_beliefs: Int := 8

  def add_belief(pred: Str, subj: Str, obj: Str, weight: Int, source: Str) : Bool {
    if beliefs.len() >= max_beliefs then return false
    beliefs := beliefs ++ [Belief {
      predicate: pred,
      subject: subj,
      object: obj,
      weight: weight,
      source: source
    }]
    return true
  }

  def find_belief(pred: Str, subj: Str) : Opt<Belief> {
    for b in beliefs do {
      if b.predicate == pred and b.subject == subj then return Some(b)
    }
    return none
  }

  on QueryBelief { predicate: ?p, subject: ?s } as q {
    match find_belief(p, s) with {
      Some(?b) => emit BeliefFound(query_id: q.id, belief: b),
      none     => emit BeliefNotFound(query_id: q.id)
    }
  }
}
```

______________________________________________________________________

## 8. Numbered Proposal Register (`P31`–`P35`)

The harmonization requires the following isolated modifications to the engine and compiler:

| ID      | Files Touched & Line Anchors                                                                                                     | Specification & Mechanism                                                                                                                                                                                                                      | Cost & Mechanical Sympathy                                                                               | Fallback if Refused                                                     |
| :------ | :------------------------------------------------------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------- |
| **P31** | `apps/common/xanadu/vagent/checker.cpp:1964–1982`, `compiler.cpp:1601–1620`, `apps/common/xanadu/vortex/vortex_vm.cpp:1898–1937` | Unify indexing to 1-based ($[1, N]$). Implement $N + 1 + k$ negative indexing. Out-of-bounds yields `#Fail`. Add `E101` fatal static check for `[0]`. Update `ByteAt` and `StrSlice` opcodes to 1-based.                                       | 0 extra runtime bytes; 2 arithmetic ops ($N + 1 + k$) on negative indices. Eliminates off-by-one errors. | Sibling languages remain fragmented (VQL 1-based, VAgent 0-based).      |
| **P32** | `apps/common/xanadu/vagent/token.hpp`, `parser.cpp:665–715`, `checker.cpp:25, 673–678`, `compiler.cpp:1750–1786`                 | Standardize `Opt<T>` nominal type, `Some(v)` and `none` constructors. Implement postfix `opt?` goal-directed unwrap lowering to `#Fail` on `none`. Ban sentinels.                                                                              | Reuses existing 1-ary and 0-ary cell representations. Eliminates sentinel string memory leaks.           | Retain fragile sentinel values and inconsistent `is_some` checks.       |
| **P33** | `apps/common/xanadu/vagent/token.hpp:15–53`, `lexer.cpp:309–325`, `parser.cpp:705, 843–860`                                      | Lexer emits `TokenKind::LogicVariable` for `?ident`, `TokenKind::QuestionDot` for `?.`, and `TokenKind::DoubleQuestion` for `??`. Delete `allowScan_` parser modal state. Fix `?` precedence below `&`. Implement `scan subject with { ... }`. | Removes modal parser state. Guaranteed LALR(1) deterministic grammar with 0 conflicts.                   | Retain parser modal flag hacks and inverted scanning precedence.        |
| **P34** | `apps/common/xanadu/vagent/parser.cpp:440–580`, `compiler.cpp:1620–1665`                                                         | Enforce 4-tier operator hierarchy. Ban `;` as disjunction. Restrict `;` and `\n` to statement sequencing. Conjunction is `&`, alternation is `\|`. Automatically cut choice points at statement boundaries (`#CutChoiceDepth`).                | Prevents choice point leaks across procedural plan steps.                                                | Catastrophic ambiguity between plan steps and Prolog disjunctions.      |
| **P35** | `apps/common/xanadu/vagent/parser.cpp:491, 572`, `ast.hpp:38–92`, `r1_ast.hpp/.cpp`, `stdlib/*.vagent`                           | Free `[...]` from declaration headers: replace `on Event [guard]` with `on Event where guard`. Partition `(...)` tuples, `[...]` lists (with `...` spread), `{...}` records. Update stdlib files.                                              | Reduces bracket parsing ambiguity; conforms with ISO/BDI standards.                                      | Retain overloaded brackets in event handlers and pattern destructuring. |

______________________________________________________________________

## 9. Refused Alternatives & Negative Evidence

### 9.1 Refused: Preserving 0-Based Indexing in VAgent

- **Proposal**: Retain 0-based indexing for lists and collections in VAgent while VQL, VPL, VProlog,
  and Vicon use 1-based indexing.
- **Reason for Refusal**: Severe cognitive dissonance across the Vortex family. In an actor system
  where VQL queries, VPL array operations, and Vicon scanning interact within the same hypermedia
  space, having VAgent alone use 0-based indexing caused frequent off-by-one errors and translation
  overhead at foreign function boundaries. 1-based indexing is mathematically and structurally
  aligned with ZigZag rank heads ($+d.step$) and Merkle ops spool indices.

### 9.2 Refused: Retaining Prolog Semicolon (`;`) for Disjunction in Plans

- **Proposal**: Allowing `;` to denote logical OR inside plan applicability conditions or rule
  bodies.
- **Reason for Refusal**: Unacceptable grammatical and cognitive clash with procedural statement
  termination. In multi-line BDI plan bodies, developers instinctively use semicolons to separate
  sequential actions. Allowing `;` to mean OR in guards or conditions caused subtle bugs where
  sequential steps were misinterpreted as alternative plans. Banishment of `;` as OR restores
  complete orthogonality.

### 9.3 Refused: Postfix `T?` Sugar in Type Signatures

- **Proposal**: Permitting `Int?` as syntactic sugar for `Opt<Int>`.
- **Reason for Refusal**: Compromises lexical purity. In generic declarations, macro fragments, and
  pattern positions, `?` serves as the logic/unification variable marker (`?T`). Introducing postfix
  `?` in type positions caused parser lookahead ambiguities with logic variables in method
  signatures (e.g. `def check(?x: T?)`). Nominal `Opt<T>` is clear, self-documenting, and avoids
  token collision.

### 9.4 Refused: Implicit Truthiness for Options (`if opt then`)

- **Proposal**: Allowing `Opt<T>` to be evaluated directly as a boolean condition.
- **Reason for Refusal**: Violates static typing and causes type confusion with `Opt<Bool>`, where
  `Some(false)` would ambiguously evaluate as truthy (because present) or falsy (because false).
  Requiring explicit pattern matching (`if let Some(?x) := opt`), goal-directed unwrap (`opt?`), or
  explicit methods (`opt.is_some()`) eliminates the ambiguity.
