# Critic 3: Prolog & Logic Programming Review

**Document**: VAgent Language Harmonization Review — Round 1\
**Role**: Critic 3: Prolog Expert (`prolog`)\
**Focus**: Logic programming purity, Horn clause semantics, unification, relational queries against
belief bases, resolution, eliminating the catastrophic collision where `;` means OR in Prolog but
statement separator in procedural code, and harmonizing logic variables `?var` with patterns.\
**Date**: November 8, 2026

______________________________________________________________________

## 1. Executive Summary

VAgent attempts a bold and valuable synthesis: fusing SARL-style reactive agent workflows with BDI
cognitive deliberation, Icon/Vicon goal-directed execution, and Prolog/Vlog relational logic.
However, because these paradigms arose from fundamentally different mathematical foundations,
uncontrolled lexical borrowing has produced critical syntactic collisions and semantic ambiguities.

From the perspective of logic programming and relational purity, the most urgent findings are:

1. **The Catastrophic Semicolon Collision**: In ISO Prolog and Edinburgh syntax, `;` denotes logical
   disjunction (OR), introducing choice points and backtracking. In imperative and procedural code
   (C, Java, SARL, and VAgent plan blocks), `;` is a statement terminator or sequence separator.
   Allowing `;` to mean OR in query or guard contexts while meaning sequence in plan bodies is an
   intolerable cognitive trap that guarantees subtle semantic bugs and parser ambiguity. Semicolon
   MUST be strictly restricted to statement sequencing. Logical disjunction in VAgent must be
   expressed through the goal-directed alternation operator `|`, aligning relational queries with
   Icon-style choice points.
1. **Desecration and Overloading of `?`**: In logic programming and BDI systems where types use
   `PascalCase`, the prefix `?var` is the sacred notation for logic variables and unification
   binders. In VAgent, `?` was also drafted for infix string scanning (`subject ? scan_expr`),
   postfix optional types (`T?`), safe navigation (`?.`), and question performatives. The lexer
   currently dumps all `?` tokens into a generic `TokenKind::Scan`, forcing ad-hoc parser dispatch.
   We must reclaim `?ident` as a dedicated lexical token for logic variables, eliminate infix `?` in
   favor of `scan subject with { ... }`, and ban postfix `T?` in favor of `Opt<T>`.
1. **1-Based Indexing as Mathematical Orthogonality**: Positional argument access in logic terms has
   been 1-based since the inception of logic programming (`arg(1, Term, Arg)`, `functor/3`,
   `nth1/3`). Sibling systems (VQL, VPL, VProlog, and Vicon) are already 1-based. Harmonizing all
   lists, strings, and term arguments to 1-based indexing (`1..N`) restores algebraic purity.
   Subscript out-of-bounds must naturally fail under goal-directed evaluation rather than crash.
1. **Negation as Failure (NAF) vs. Explicit Options**: In relational logic, absence of a belief is
   represented by failure to prove, not by asserting a `none` sentinel into the closed-world belief
   base. For procedural values, `Opt<T>` must participate directly in goal-directed failure: reading
   an `Opt<T>` succeeds with its inner value or fails if `none`, bridging typed options and
   backtracking.
1. **Pattern Matching vs. Unification and Guard Cleanliness**: One-way structural matching in
   procedural `let` and `match` must be formally distinguished from two-way relational unification
   in belief queries (`believes Rel(...)`) and Horn clauses. To eliminate bracket overloading, event
   handler guards `on Event [guard]` must be renamed to `on Event where guard` (or `if`), reserving
   square brackets `[]` strictly for lists and indexing.

______________________________________________________________________

## 2. Foundational Architecture: Logic Programming in Vortex & VAgent

### 2.1 The Vlog Substrate

Logic programming in this repository is not an alien interpreter bolted onto an imperative runtime.
As defined in
\[vlog-logic-extension.md\](file:///home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan/design/vlog-logic-extension.md)
and implemented in
\[vlog.hpp\](file:///home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan/apps/common/xanadu/zigzag/vlog.hpp)
and
\[vlog.cpp\](file:///home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan/apps/common/xanadu/zigzag/vlog.cpp):

- **Terms are cells**: A compound term $f(t_1, \dots, t_n)$ is a manifold cell containing functor
  $f$; arguments hang off the Inputs Wing ($+d.grab$ for the first argument, chained along $+d.step$
  for subsequent arguments).
- **Variables are cells on the `d.vars` rank**: An unbound variable is a cell on `d.vars` whose
  clone master carries no value.
- **Unification is rank splicing**: Variable binding is a single link along $+d.clone$ joining two
  clone ranks. Dereferencing is `cloneMaster()`.
- **Backtracking is mark/release**: Choice points are pairs of high-water marks in `ArenaManifold`.
  Failing back to a choice point truncates the arena, immediately unlinking variable bindings
  without requiring a separate WAM heap or trail.

### 2.2 VAgent Belief Bases as Closed-World Extensional Databases

Under
\[vagent-agent-language.md\](file:///home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan/design/vagent-agent-language.md)
§6.1, an agent's belief base is a set of extensional ground facts indexed by predicate declarations:

```vagent
predicate at(key who: Agent, x: Float, y: Float)
```

Queries against the belief base (`believes at(?who, ?x, ?y)`) instantiate query terms with fresh
logic variables, walk the clause rank on $+d.grab$, and execute two-way unification against stored
facts using `Vlog::unify()`.

When designing the surface language, we must respect this underlying logic architecture. The syntax
must reflect Horn clause semantics, relational unification, and resolution search without corrupting
or being corrupted by procedural constructs.

______________________________________________________________________

## 3. Evaluation of the Five Harmonization Axes

### Axis 1: Indexing Harmonization (1-Based Everywhere)

#### Logic Programming Foundation

In standard mathematical logic and first-order logic, an $n$-ary relation $R(t_1, t_2, \dots, t_n)$
has arguments ordered from $1$ to $n$. There is no "0-th argument" in a mathematical relation.

This mathematical reality was codified in ISO Prolog (ISO/IEC 13211-1):

- `functor(Term, Functor, Arity)` reports $Arity \ge 0$.
- `arg(N, Term, Argument)` accesses arguments using 1-based indexing: $1 \le N \le Arity$. Calling
  `arg(0, Term, Arg)` raises a `domain_error(not_less_than_one, 0)`.
- Standard list indexing in logic programming libraries is 1-based: `nth1(1, [Head | Tail], Head)`.
  (`nth0/3` in SWI-Prolog is an acknowledged procedural concession).

In VAgent, predicates are declared positionally:

```vagent
predicate location(key target: Str, x: Float, y: Float, z: Float)
```

Argument 1 is `target`, argument 2 is `x`, argument 3 is `y`, and argument 4 is `z`. Accessing
arguments by position or decomposing terms via `arg(N, Term)` must therefore be 1-based.
Furthermore, VQL, VPL, VProlog, and Vicon are already 1-based. Standardizing VAgent to 1-based
indexing everywhere eliminates an arbitrary semantic fracture between siblings.

#### Recommendations for Axis 1

**Valid Range $[1, N]$**: For any list, string, or compound term of length or arity $N$, valid
indices are strictly $1 \le i \le N$.

**Goal-Directed Failure on Out-of-Bounds**: In procedural languages, accessing an invalid index
throws an uncaught exception. In relational logic programming, a query `nth1(I, List, X)` with an
out-of-bounds $I$ simply fails (producing no solutions and triggering backtracking). In VAgent's
goal-directed evaluation model, reading `arr[i]` where $i < 1$ or $i > N$ must fail. In a
speculative position, this failure triggers backtracking to the next choice point; in a bounded
statement, it fails the enclosing statement block unless handled by `else`.

**Index 0 is Strictly Invalid**: Index 0 has no referent in a 1-based sequence. Evaluating `arr[0]`
must fail (or produce a compile-time diagnostic if literal `0` is passed). We reject using index 0
as an end-of-sequence sentinel (Icon's `tab(0)`). End-of-sequence operations belong to explicit
functions (`last(arr)`, `end()`) or slicing.

**Inclusive Slice Ranges $[start..end]$**: In logic programming, ranges denote discrete closed
intervals $[A, B] = \{x \in \mathbb{Z} \mid A \le x \le B\}$, matching Prolog's
`between(Low, High, X)`. Therefore, slice syntax `arr[1..N]` must be fully inclusive. For a list of
length 5, `arr[1..5]` yields the entire list. If $start > end$, the slice produces an empty list
`[]` (or fails if strict non-empty matching is required).

**Negative Indexing**: In logic programming, negative indices in term argument inspection are domain
errors. If end-relative indexing is permitted in surface expressions, negative index $-k$ must
denote $N - k + 1$ (so `arr[-1]` is element $N$). When an index variable `?i` is unbound in a
relational subscript `arr[?i]`, it must generate solutions over the positive domain
$1 \le ?i \le N$.

**Loop Ranges**: `for i in 1..n do ...` evaluates $i$ across the closed interval $[1, n]$ in
ascending order, exactly mirroring Prolog's `between(1, n, I)`.

______________________________________________________________________

### Axis 2: Option (`Opt<T>`) Consistency

#### Negation as Failure (NAF) vs. Explicit Algebraic Options

A fundamental principle of logic programming is the Closed World Assumption (CWA) and Negation as
Failure (NAF):

- A proposition $P$ is false if every proof of $P$ fails.
- In Prolog: `\+ Goal` (or `not(Goal)`).

In VAgent's belief base (§6.1), facts represent truths known to the agent:

```vagent
predicate has_weapon(key agent: Agent, weapon_id: Str)
```

If an agent has no weapon, the belief base contains no fact for that agent. The query
`believes has_weapon(agent, ?w)` fails. One does NOT assert `has_weapon(agent, none)`! Asserting
explicit `none` tokens into a relational belief base pollutes the relational model, destroys
first-argument indexing in the ops spool, and degrades relational queries into tedious procedural
null checks.

#### The Bridge: Goal-Directed Failure of `Opt<T>`

In procedural records and local variables, optionality is inevitable (e.g. `parent: Opt<Agent>`).
How should this harmonize with logic programming?

VAgent §8.2 established an elegant rule:

> "A value of type `Opt<T>` read in an expression succeeds with its value or fails when it is
> `none`."

This rule is a masterpiece of semantic convergence. In logic programming terms, an optional value is
treated as an extensional predicate of arity 1:

- `Some(v)` represents the holding of the fact (succeeds, binding/yielding `v`).
- `none` represents failure of the fact (fails immediately).

#### Recommendations for Axis 2

**Canonical Type Name**: `Opt<T>`. Suffix shorthand `T?` is strictly banned (see Axis 3).

**Canonical Constructors**: Absence is `none`; presence is `Some(v)` (or implicit boxing when
assigning an expression of type `T` to a place of type `Opt<T>`).

**Safe Evaluation via Goal-Directed Backtracking**: Reading an `Opt<T>` in an expression succeeds
with `v` or fails if `none`. Thus, unwrap is just evaluation:

```vagent
let target := optional_target else return
```

No `opt!`, `opt.unwrap()`, or `opt?` operators are needed or permitted.

**Pattern Matching with Functors**: In `match` expressions or term decomposition, options are
matched as algebraic constructors:

```vagent
match parent with {
  Some(?p) => emit Notify{} for p,
  none     => emit AutonomousNotice{}
}
```

**Belief Bases Stay Ground and Positive**: Predicate declarations should reject `Opt<T>` parameters
where relational decomposition is more appropriate. If an attribute is optional, it should be
modeled as an auxiliary predicate rather than a nullable field in a fat predicate. If an `Opt<T>` is
stored in a term, `Some(?x)` unifies only with `Some(...)`, and `none` unifies only with `none`.

______________________________________________________________________

### Axis 3: Disambiguation of `?`

#### The Sanctity of `?var` in Logic and Relational Queries

In standard Prolog, variables are distinguished by starting with an uppercase letter: `X`, `Target`.
However, VAgent's lexical grammar (§8.1) strictly assigns `PascalCase` to nominal types, events,
goals, records, and agents:

```vagent
event TargetDetected { target_id: Str }
goal Reach(location: Point) achieve
```

Because uppercase identifiers represent types and constructors, logic variables cannot use
capitalization. Across the entire BDI literature (AgentSpeak, Jason, 2APL, Jadex, GOAL) and
relational query systems (SPARQL, KIF, Clojure `core.logic`, Datalog), the universal, battle-tested
standard is the prefix `?`:

```vagent
believes at(?who, ?x, ?y)
```

In this syntax, `?who`, `?x`, and `?y` are logic variables. In a query, an unbound `?var` unifies
with the corresponding argument in the belief base and binds the variable in the current
environment.

#### The Flaw in the Current Lexer and AST

In
\[lexer.cpp\](file:///home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan/apps/common/xanadu/vagent/lexer.cpp#L308):

```cpp
case '?':
  return token(TokenKind::Scan, start);
```

Every `?` character is blindly tokenized as `TokenKind::Scan`. Then, in
\[parser.cpp\](file:///home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan/apps/common/xanadu/vagent/parser.cpp#L917):

```cpp
ExprPtr Parser::termArgument() {
  if (accept(TokenKind::Scan)) {
    const auto name = identifier();
    return make(ExprKind::LogicVariable, name.loc, name.text);
  }
  Mode scans(allowScan_, false);
  return expression();
}
```

This is fragile and broken:

- `?` is simultaneously used for infix string scanning: `s ? expr`.
- If a developer writes `s ? ?v`, the parser must perform lookahead or toggle modal flags
  (`allowScan_ = false`).
- If `?` is also proposed for postfix `T?`, safe navigation `expr?.field`, or ask performatives
  `ask agent question?`, the grammar ceases to be LALR(1) and becomes an ambiguous lexical swamp.

#### Recommendations for Axis 3

**Dedicate `?` Strictly to Logic & Pattern Variables**: Prefix `?` is reserved exclusively for logic
variables and pattern binders.

**Lexical Tokenization Rule**: The lexer must distinguish `?` based on immediate character
adjacency:

- When `?` is immediately followed without whitespace by an identifier start character or `_`
  (`?[a-zA-Z_][a-zA-Z0-9_]*`), emit a dedicated token: `TokenKind::LogicVariable`.
- The token text carries the variable name (e.g. `"x"` or `"_"`).
- This completely removes the `TokenKind::Scan` hack from `Parser::termArgument()` and allows logic
  variables to be recognized unambiguously anywhere in patterns, terms, and queries.

**Eliminate Infix Binary `?` for String Scanning**: Icon's infix `?` operator was conceived in 1977
for an untyped language with no logic variables. In VAgent, infix `?` creates acute conflict with
logic variables. String scanning must be converted to a clear, keyword-delimited block construct:

```vagent
scan text with {
  tab(match("ID:")) & let id := tab(upto(";"))
}
```

Or `scan text do expr`. This isolates string scanning state (`&subject`, `&pos`), eliminates
precedence anomalies, and liberates `?` from expression grammar.

**Ban Postfix `T?`**: Optional types must be written `Opt<T>`. Postfix `?` on types is rejected.

**Safe Navigation as a Distinct Atomic Token**: If safe navigation is supported, `?.` must be lexed
as a single atomic token `TokenKind::QuestionDot`, requiring both characters with no intervening
whitespace.

**Questions Use Keywords, Not Punctuation**: Question declarations use `question heading(): Float`.
Invocations use `ask drone.heading()`. Trailing question marks on identifiers are prohibited.

______________________________________________________________________

### Axis 4: Plan Operators & Conjunction/Disjunction Harmonization

#### The Catastrophic Semicolon Clash

This is the most critical conflict in the VAgent language specification.

In standard Prolog:

- `,` (comma) represents logical conjunction (AND).
- `;` (semicolon) represents logical disjunction (OR / choice point).
- `.` (dot) terminates a clause.

In procedural programming and VAgent statement blocks:

- `;` (semicolon) and newlines represent statement sequencing.
- Statements in a block execute sequentially:

```vagent
step1()
step2()
```

Failure of a statement fails the block (or rolls back the turn/handler).

Now consider what happens if Prolog's `;` is allowed into VAgent:

```vagent
plan patrol for PatrolGoal where believes (threat(?x) ; low_battery(?level)) {
  prepare_patrol();
  execute_route();
}
```

Inside the `where` condition, `;` means OR (alternation / choice point). Inside the plan body, `;`
means THEN (procedural sequence).

If a programmer writes:

```vagent
plan resolve for HandleIssue {
  believes alert(?x);
  notify_supervisor();
}
```

Does `;` mean "query `alert(?x)`, and then proceed to `notify_supervisor()`"? Or does `;` mean
"either satisfy `alert(?x)` OR execute `notify_supervisor()`"?

In an agent language where plans execute procedural side-effects (emitting network packets, moving
hardware thrusters, committing database transactions), confusing sequencing with disjunction is
catastrophic. A procedural programmer who accidentally writes `;` expecting sequence will
unwittingly introduce a choice point with hidden backtracking; conversely, a logic programmer
expecting Prolog disjunction will write buggy procedural sequences.

#### The Comma Clash

Similarly, in Prolog, `,` is conjunction: `p(X) , q(X)`. In VAgent:

- `,` separates arguments in function and predicate calls: `at(self, ?x, ?y)`.
- `,` separates elements in list literals: `[1, 2, 3]`.
- `,` separates fields in record literals: `Point{x: 1.0, y: 2.0}`.

If a multi-predicate query uses comma:

```vagent
believes at(self, ?x, ?y), clear(?x, ?y)
```

The parser and reader cannot tell whether `clear(?x, ?y)` is a second predicate conjoined with `at`,
or whether `at` took four arguments, or whether this is an invalid expression sequence.

#### The Unification: Harmonizing with Goal-Directed `&` and `|`

VAgent already incorporates Icon's goal-directed evaluation operators (§8.2, §8.4):

- `&` is conjunction (evaluates right operand for each result of left; succeeds only if both
  succeed).
- `|` is alternation (produces left result; on backtracking, produces right result).
- `not` is negation (succeeds if operand fails).

Notice the exact semantic isomorphism:

- Icon `&` is logically identical to Prolog `,` (conjunction).
- Icon `|` is logically identical to Prolog `;` (disjunction / choice point).
- Icon `not` is logically identical to Prolog `\+` (negation as failure).

Therefore, the clean, elegant, and crash-proof solution is: **Eliminate Prolog `,` and `;` from all
query, guard, and rule contexts in VAgent. Standardize on `&`, `|`, and `not` across the entire
language.**

#### Recommendations for Axis 4

**Semicolon is Strictly Sequential**: Semicolon `;` (and significant newline) exclusively denotes
statement sequencing in blocks. It NEVER denotes logical OR anywhere in VAgent.

**Logical Conjunction is `&`**: In conditions, guards, plan `where` clauses, and multi-predicate
belief queries, conjunction is written with `&`:

```vagent
plan investigate for InvestigateGoal
  where believes target(?id, ?pos) & believes threat_level(?id, ?lvl) & lvl > 5 {
    approach(pos)
}
```

**Logical Disjunction is `|`**: Disjunction is written with `|`, introducing a generator choice
point:

```vagent
if believes low_battery(?v) | believes critical_damage(?v) then {
  abort_mission()
}
```

**Multi-Predicate Relational Queries**: Multi-predicate queries can be formulated in two isomorphic,
composable ways:

- **Chained Query Expressions**:

  ```vagent
  believes at(self, ?x, ?y) & believes clear(?x, ?y)
  ```

  Because `&` is goal-directed conjunction, the first `believes` unifies against the belief base and
  binds `?x` and `?y`. The second `believes` executes in an environment where `?x` and `?y` are
  already bound. If `clear(?x, ?y)` fails, `&` automatically backtracks into the first `believes` to
  find the next matching `at` fact! This is pure SLD-resolution without any special query syntax.

- **Compound Belief Query Block**: If enclosed in parentheses under a single `believes`:

  ```vagent
  believes (at(self, ?x, ?y) & clear(?x, ?y))
  ```

  Inside the parentheses, `&` conjoins predicates, `|` alternates predicates, and commas `,` are
  strictly reserved for argument lists within each predicate.

**Deductive Horn Clauses in VAgent**: When agents define intensional (deductive) rules rather than
extensional facts, rules should be declared with `predicate` and `:-`, using `&` and `|`:

```vagent
predicate connected(from: Node, to: Node) :-
  edge(?from, ?to) |
  (edge(?from, ?z) & connected(?z, ?to))
```

This gives agents the full power of Prolog Horn clauses while maintaining complete syntactic harmony
with procedural code.

______________________________________________________________________

### Axis 5: Pattern Matching & Destructuring Syntax (Disambiguating `[]`)

#### Unification vs. Structural Pattern Matching

In language design, it is vital to distinguish between two different mechanisms:

1. **One-Way Structural Pattern Matching** (as in ML, Haskell, Rust):
   - An existing, ground value is matched against a pattern.
   - Binder variables in the pattern are fresh local variables that receive copies or references of
     sub-components.
   - Matching is refutable: if the shape does not match, execution falls through to the next arm.
   - No logic variables, no two-way binding, and no trailing of variable-to-variable aliases.
1. **Two-Way Relational Unification** (as in Prolog, Vlog):
   - Both sides can contain logic variables (`?x = ?y`).
   - Symmetrical: variables on either side can bind to terms or to each other.
   - Requires choice points, occurs checks (or rational trees), and trailing for backtracking.

VAgent needs both, and must keep them distinct:

- **Procedural destructuring** belongs in `let` bindings and `match` expressions.
- **Relational unification** belongs in `believes` queries, Horn clause heads/bodies, and
  declarative event triggers (`on +at(?who, ?x, ?y)`).

#### Disambiguating Compound Data Constructors: Parentheses, Brackets, and Braces

Currently, `[` and `]` are overloaded in VAgent:

- List literals: `[1, 2, 3]`
- List types: `[Int]`
- Subscript indexing: `arr[1]`
- Destructuring patterns: `let [?a, ?b] := ...`
- Event handler guards: `on Event [guard] { ... }`

We establish clean syntactic orthogonality across compound structures:

| Structure  | Value Literal / Type                     | Destructuring Pattern | Unification Term      |
| :--------- | :--------------------------------------- | :-------------------- | :-------------------- |
| **Tuple**  | `(1, "bolt", 3.0)` / `(Int, Str, Float)` | `(?id, ?name, ?val)`  | `(?id, ?name, ?val)`  |
| **List**   | `[1, 2, 3]` / `[Int]`                    | `[?head, ...?tail]`   | `[?head, ...?tail]`   |
| **Record** | `Point{x: 1.0, y: 2.0}` / `Point`        | `Point{x: ?x, y: ?y}` | `Point{x: ?x, y: ?y}` |

#### List Deconstruction: `[?h | ?t]` vs. `[?h, ...?t]`

In ISO Prolog, list deconstruction is written `[Head | Tail]`. However, in VAgent:

- `|` is the binary alternation operator (`e1 | e2`).
- If `|` is used inside list patterns (`[?head | ?tail]`), the lexer and parser face a direct
  collision: does `[?head | ?tail]` represent a cons cell (head and tail), or does it represent a
  single-element list containing the alternation expression `?head | ?tail`?

In Icon, `[1 | 2]` produces a list containing 1 on initial evaluation and a list containing 2 on
resumption.

To prevent this collision, we recommend:

- In expression contexts, `[a | b]` is an alternation expression inside a list literal.
- In pattern and unification contexts, list decomposition is written:

```vagent
[?head, ...?tail]
```

This matches modern rest/spread notation (`...`), eliminates any ambiguity with the alternation
operator `|`, and aligns with typed array languages.

*Prolog Compatibility Note*: In pure VProlog source files compiled via `vprolog`, ISO
`[Head | Tail]` remains supported because VProlog does not have Icon expression syntax. In VAgent
source files, `[?head, ...?tail]` is the canonical list deconstruction syntax.

#### Liberating Square Brackets from Handler Guards

The most glaring misuse of square brackets in VAgent is SARL's legacy handler guard syntax:

```vagent
on TargetDetected [occurrence.distance < 50.0] { ... }
```

This syntax causes severe lexical friction:

- In `on Event [guard]`, `[` follows an identifier, mirroring array indexing `arr[i]`.
- Inside the brackets, newlines are suppressed, conflicting with statement layout.
- In BDI plans, VAgent already rejected brackets and adopted `where`:

```vagent
plan direct for Reach where believes battery(?level) & level > reserve_floor { ... }
```

- In goals, VAgent adopted `when`:

```vagent
succeed when ...
drop when ...
```

**Recommendation**: Replace `on Event [guard]` with `on Event where guard` (or `on Event if guard`):

```vagent
on TargetDetected where occurrence.distance < 50.0 {
  targets_seen := targets_seen + 1
}
```

This single change completely frees `[` and `]` for lists and indexing, restoring clean LALR(1)
grammatical predictability.

______________________________________________________________________

## 4. Summary Table of Expert Recommendations

| Axis                                  | Current State / Conflict                                              | Prolog Expert Recommendation                                                                                 | Architectural Justification                                                                            |
| :------------------------------------ | :-------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------- |
| **Axis 1: Indexing**                  | Mixed 0-based (SARL) vs 1-based (Vicon, VQL, VProlog).                | **1-based everywhere** (`1..N`). `arr[0]` fails. Slice `arr[1..N]` is inclusive. Out-of-bounds fails.        | Matches first-order logic relations, ISO Prolog `arg/3`, `nth1/3`, and sibling languages.              |
| **Axis 2: Options**                   | `Opt<T>` vs `T?`; null sentinels vs NAF; awkward unwrap.              | **`Opt<T>` canonical**. Presence: `Some(v)`, absence: `none`. Reading `Opt<T>` succeeds with `v` or fails.   | Bridges typed optionality with goal-directed backtracking. Belief bases stay positive via NAF.         |
| **Axis 3: Disambiguation of `?`**     | `?` overloaded for scanning, variables, types, navigation, questions. | **`?` reserved exclusively for `?var` logic variables**. Scanning becomes `scan s with { ... }`. Ban `T?`.   | Preserves logic variables as primary syntax. Eliminates modal lexer hacks and lookahead.               |
| **Axis 4: Conjunction & Disjunction** | Catastrophic collision: `;` is Prolog OR but procedural sequence.     | **`;` is sequence only**. Conjunction is `&`, disjunction is `\|`, negation is `not`. Ban `;` as OR.         | Completely eliminates the lethal procedural/logical semicolon trap. Unifies with goal-directed search. |
| **Axis 5: Patterns & Brackets**       | Overloaded `[]` on lists, indexing, tuples, and SARL guards.          | **Replace `on E [g]` with `on E where g`**. Tuples `()`, Lists `[]`, Records `{}`. List tail: `[?h, ...?t]`. | Unambiguous LALR(1) grammar. Separates one-way destructuring from two-way Vlog unification.            |

______________________________________________________________________

## 5. Concrete Code Examples Before and After Harmonization

### 5.1 Multi-Predicate Belief Query in a Plan Guard

#### Before Harmonization (Conflicted & Fragile)

```vagent
// Conflicted: uses Prolog comma or ambiguous semicolon in guard
plan attack for EngageTarget where believes target(?id), distance(?id, ?d) [d < 100] {
  target_lock(id);
  fire_thrusters();
}
```

#### After Harmonization (Orthogonal & Pure)

```vagent
// Harmonized: & for conjunction, where for guards, ?var cleanly tokenized
plan attack for EngageTarget
  where believes target(?id) & believes distance(?id, ?d) & d < 100.0 {
    target_lock(id)
    fire_thrusters()
}
```

### 5.2 Reactive Event Handler with Guard and Logic Belief Query

#### Before Harmonization (Bracket Overload & Scanning Infix Clash)

```vagent
// Bracket overload on [occurrence.threat > 5]
on AnomalyDetected [occurrence.threat > 5] {
  // Infix ? scanning colliding with ?who variable
  let code := occurrence.raw_payload ? tab(match("SIG:")) & tab(upto(";"))
  // Ad-hoc belief assertion
  revise threat_level(occurrence.target_id, occurrence.threat)
}
```

#### After Harmonization (Clean, Explicit, Unambiguous)

```vagent
// where replaces square brackets on handler guard
on AnomalyDetected where occurrence.threat > 5 {
  // Dedicated scan block liberates ?
  let code := scan occurrence.raw_payload with {
    tab(match("SIG:")) & tab(upto(";"))
  }
  revise threat_level(occurrence.target_id, occurrence.threat)
}
```

### 5.3 List Pattern Unification in a Deductive Predicate

#### Before Harmonization (Prolog Pipe Colliding with Icon Alternation)

```vagent
// Ambiguous: does [?h | ?t] mean cons or alternator expression?
predicate path_find(from: Str, to: Str, path: [Str])
```

#### After Harmonization (Unambiguous Spread Notation)

```vagent
// Pure Horn clause semantics with unambiguous list deconstruction
predicate path_find(from: Str, to: Str, path: [Str]) :-
  (edge(?from, ?to) & path == [?from, ?to]) |
  (edge(?from, ?next) &
   path_find(?next, ?to, ?rest) &
   path == [?from, ...?rest])
```

______________________________________________________________________

## 6. Conclusion

By strictly separating statement sequencing (`;`) from logical disjunction (`|`), establishing `&`
and `|` as the universal relational connectives, reserving prefix `?` exclusively for logic
variables, harmonizing on 1-based indexing, and replacing bracket guards with `where`, VAgent
achieves full logic programming purity without sacrificing the clarity and safety of its procedural,
agentic foundation.
