# VAgent Language Harmonization Review — Round 1

**Reviewer**: Critic 2: SARL Expert (`sarl`)\
**Focus**: Agent-Oriented Programming (AOP) Metamodel Purity, Reactive Handlers, Capacities &
Skills, BDI Plan Lifecycle, Spaces/Contexts, and Sibling Coherence\
**Target Document**: `design/vagent-agent-language.md`\
**Reference Implementations**: `apps/common/xanadu/vagent/stdlib/mind.vagent`,
`apps/common/xanadu/vagent/stdlib/arbitration.vagent`, `brief-common.md`\
**Date**: November 2026

______________________________________________________________________

## 1. Executive Summary

As the SARL / AOP representative in this harmonization review, my mandate is to ensure that VAgent
preserves the structural integrity and conceptual clarity of Agent-Oriented Programming:

1. **Agents as Autonomous Entities**: True encapsulation of mental state, reactive event handlers,
   and proactive goal-driven deliberation.
1. **Capacities and Skills**: Clean separation of interface contract from dynamic runtime
   implementation, with first-class support for hot-swapping and native host effects.
1. **BDI Deliberation & Intentions**: Disciplined single-minded commitment, hierarchical subgoaling,
   and failure recovery, free of low-level coroutine hacks or shared-memory leaks.
1. **Decoupled Interaction via Spaces and Events**: Pure asynchronous messaging across
   cryptographically addressed contexts and holons.

VAgent's architectural foundation is exceptionally solid. Grounding the agent in Vortex's turn
isolation, mapping spaces and holons into 13 dedicated dimensions on the arena manifold (Rule V10),
and treating native effects as asynchronous suspension points (Rule V6) solves the classic
concurrency and checkpointing hazards that have plagued Janus/SARL and JVM-based multi-agent
platforms for years.

However, VAgent's surface syntax currently suffers from severe syntactic schizophrenia. Influences
from SARL, Prolog, Icon, and procedural C++ collide head-on in the parser:

- SARL's awkward Xtend inheritance of square-bracket guards (`on Event [guard]`) collides with list
  literals and array subscriptions.
- The lack of coherent collection indexing and `Opt<T>` ergonomics forced the standard library
  (`mind.vagent`, `arbitration.vagent`) to devolve into manual variable unrolling (`b1_...` to
  `b8_...`, `i1_...` to `i4_...`), bitmask manipulation (`tried_mask`), and sentinel return values
  (`""`, `-1`, `0`), in direct violation of the project's zero-sentinel rule.
- The symbol `?` is dangerously overloaded between logic variables (`?var`), string scanning infix
  (`s ? scan`), optional types (`T?`), and question performatives (`ask`).
- Conjunction and disjunction are muddled: Prolog's `;` (OR) conflicts with procedural plan step
  sequencing (`;`).

Below is my comprehensive evaluation and set of concrete recommendations across all five
harmonization axes.

______________________________________________________________________

## 2. Evaluation Along the Five Harmonization Axes

### Axis 1: Indexing Harmonization (1-Based Everywhere)

#### AOP Context & Evaluation

In agent-oriented architectures, collections represent ordered observations (percept queues, sensor
arrays, LIDAR sweeps), priority-ranked candidate plans, time-sequenced episodic memory, and waypoint
lists.

SARL historically inherited 0-based indexing from the JVM, but this created cognitive dissonance in
domain models: an agent reasons about the "1st attempt", the "1st priority plan", or the "1st
waypoint". More critically, VAgent's sibling environments in the hypermedia workspace—VQL
(relational queries), VPL (array processing), and VProlog (logic facts)—are all strictly 1-based.
Requiring an agent to switch indexing conventions when processing a VQL projection or passing an
array to VPL is unacceptable.

Examining `mind.vagent` and `arbitration.vagent` reveals that VAgent's mental model is *already*
implicitly 1-based:

- In `mind.vagent`, `assert_fact` assigns slots numbered `1` through `8`.
- In `arbitration.vagent`, `next_intention_id` starts at `1`, and `is_plan_tried` addresses plans
  `1` through `8`. Standardizing on 1-based indexing formalizes what the stdlib authors already
  instinctively wrote.

#### Concrete Recommendations

1. **Index Range**: For a list or string of length $N$, the valid index range is strictly $[1, N]$.
   Index `1` addresses the head element; index `N` addresses the tail.
1. **Subscript Semantics & Out-of-Bounds Behavior**:
   - In keeping with goal-directed execution and actor turn safety, evaluating `arr[i]` where
     $i \notin [1, N]$ or $arr$ is empty **fails** (in the Icon/Vicon sense). It does not panic or
     corrupt the actor turn.
   - For total lookups where failure is not desired, `arr.get(i)` returns `Opt<T>` (`none` if out of
     bounds).
1. **Index `0`**:
   - In 1-based indexing, `0` is strictly invalid. Subscripting `arr[0]` produces failure.
   - We firmly reject using `0` to denote end-of-string or end-of-array (an Icon scanning quirk). In
     an agent model, `0` is not an element. End-of-collection is addressed cleanly via negative
     indexing.
1. **Negative Indexing**:
   - `arr[-1]` denotes the last element ($N$); `arr[-2]` denotes the penultimate element ($N-1$).
   - In general, for $k \in [1, N]$, `arr[-k]` maps to index $N - k + 1$.
   - *AOP Justification*: Negative indexing is indispensable for agent percept and intention
     buffers. An agent queries `percept_inbox[-1]` to inspect the latest arrival, or
     `waypoint_queue[-1]` to inspect the final mission objective.
1. **Slices and Ranges**:
   - Range syntax `arr[start..end]` uses **inclusive bounds** $[start, end]$.
   - For example, `plan_candidates[1..3]` returns the top 3 candidates.
   - If `end < start`, it yields an empty collection `[]`.
   - Loop ranges: `for i in 1..n do { ... }` iterates from $1$ up to $n$ inclusive.

______________________________________________________________________

### Axis 2: Option (`Opt<T>`) Consistency

#### AOP Context & Evaluation

In multi-agent systems, partial observability is ubiquitous. An agent rarely possesses complete
information:

- Event fields may be omitted (e.g., `in_reply_to: Opt<Str>`, `delegator: Opt<Agent>`).
- Belief queries may yield no result for an unobserved entity.
- Capacities may or may not be dynamically installed at runtime (`skill_of<Navigation>()`).
- Top-level agents have no parent (`parent: Opt<Agent>` is `none`).

The absence of consistent `Opt<T>` mechanics has crippled the existing codebase. In `mind.vagent`:

```vagent
def query_belief_object(pred: Str, subj: Str): Str {
  ...
  else ""   // Sentinel empty string!
}
```

And in `arbitration.vagent`:

```vagent
def get_intention_step(id: Int): Int {
  ...
  else -1   // Sentinel -1!
}
def select_highest_priority_ready(): Int {
  ...
  best_id   // Sentinel 0!
}
```

This violates the core architecture mandate: *"No sentinels outside the wire and on-disk formats. In
an API, 'no cell', 'no index' and the like are `std::optional` (or `std::expected`), never a
reserved value such as `noCell`."*

#### Concrete Recommendations

1. **Canonical Type**: Strictly `Opt<T>`.
   - Reject `T?`. The postfix `?` clashes with logic variables (`?var`) and causes visual noise in
     complex signatures like `def search(target: Opt<Agent>, timeout: Opt<Duration>)`.
1. **Canonical Constructors**:
   - Present value: `Some(v)`.
   - Absent value: `none`.
   - *Rationale*: Lowercase `none` is already established across the VAgent specification (§5.5,
     §8.7, §8.8) and aligns with clean functional syntax without requiring uppercase capitalization
     for nullary states.
1. **Safe Navigation, Coalescing, and Unwrapping**:
   - **Default Coalescing**: `opt ?? default_expr`. Essential for agent configuration fallbacks
     (e.g., `event.priority ?? 0`, `event.deadline ?? (now + 10s)`).
   - **Goal-Directed Unwrap (Failure Integration)**:
     - An option value can be unpacked with `opt!`. If `opt` is `none`, evaluating `opt!` **fails**
       the current expression/goal-directed branch.
     - In an `if` or `where` guard: `where opt is Some(?val) & val > threshold`.
1. **Capacity & Skill Dynamic Inspection**:
   - Provide standard introspective reflection: `skill_of<C>() : Opt<C>`.
   - Calling a method on an uninstalled capacity without checking fails the action with an
     `UnboundCapacityFault`, keeping the actor turn safe.

______________________________________________________________________

### Axis 3: Disambiguation of `?`

#### AOP Context & Evaluation

In SARL and BDI, `?` is the universal indicator of **pattern binding and unification variables**:
`?who`, `?x`, `?target`, `?level`. This syntax is foundational to logic-agent integration:

```vagent
on +at(?who, ?x, ?y) if who == self { ... }
plan direct for Reach where believes battery(?level) & level > reserve_floor { ... }
```

Allowing `?` to be simultaneously used as an infix binary operator for string scanning
(`subject ? scan_expr`), a postfix type modifier (`T?`), and a question performative marker
(`ask Agent question?`) introduces parser ambiguity, requires backtracking/lookahead in the lexer,
and generates immense cognitive fatigue.

#### Concrete Recommendations

1. **Reserve `?ident` Exclusively for Logic & Pattern Variables**:
   - Any token matching `\?[a-zA-Z_][a-zA-Z0-9_]*` is unambiguously a logic/unification variable
     used in belief terms, plan preconditions, and event destructuring.
1. **Eliminate Infix `?` for String Scanning**:
   - Replace Icon's cryptic `subject ? scan_expr` with an explicit block construct:

     ```vagent
     scan text with {
       tab(upto(":"))
       let key := move(1)
       ...
     }
     ```

   - *AOP Justification*: String scanning in agents is used for parsing protocol wire headers or
     unstructured sensory streams. An explicit `scan expr with { ... }` block scopes `&subject` and
     `&pos` clearly, matches agent block semantics (`on`, `plan`, `behavior`), and completely frees
     the `?` character from infix operator grammar.
1. **Question Performatives**:
   - Keep VAgent's existing declaration `question q(args): Type` and action
     `x := ask agent.q(args) within d else fallback`.
   - Never append `?` to `ask` statements. The keyword `ask` is already explicit and unambiguous.

______________________________________________________________________

### Axis 4: Plan Operators & Conjunction/Disjunction Harmonization

#### AOP Context & Evaluation

This is the most critical conflict in the multi-paradigm blend:

- In pure Prolog: `,` is logical AND; `;` is logical OR.

- In procedural languages (SARL, Java, C++): `;` is statement termination/sequencing.

- In Icon / Goal-Directed execution: `&` evaluates both sides requiring success; `|` introduces an
  alternation generator choice point.

- In BDI plan bodies: A plan is **procedural and intentional**, not a static Horn clause! It is a
  recipe of discrete steps, subgoals, and effects executed over hypertime:

  ```vagent
  step1()
  achieve Subgoal()
  effect move()
  step2()
  ```

If `;` were to mean disjunction (OR) inside a plan or handler body, it would destroy the core
procedural execution model of SARL and BDI.

#### Concrete Recommendations

1. **Regime Separation: Plan & Handler Bodies vs Declarative Conditions**: We must establish a
   strict syntactic firewall between **Imperative Step Sequences** and **Declarative
   Conditions/Queries**.

1. **Imperative Step Sequencing (Handlers, Plan Bodies, Methods)**:

   - Statements are separated by **newlines** or **semicolons** (`;`).
   - Semicolon `;` is **strictly a statement separator**. It NEVER means logical OR or alternation
     in statement position.
   - Sequential execution guarantees: Statement $N+1$ runs only after Statement $N$ succeeds.
   - If an intermediate action in a plan body fails (e.g. `achieve Subgoal()` fails with no
     fallback, or an effect raises an error), the entire plan fails, its writes in the live turn are
     unrolled according to Rule V2, and the deliberation cycle moves the plan to `tried` and selects
     an alternative plan.

1. **Declarative Conditions & Belief Queries (`where`, `if`, `holds`, `succeed when`)**:

   - Conjunction: `&` (goal-directed conjunction: left must succeed, then right runs, propagating
     unification bindings). Pure boolean conjunction can also be written `and`.

   - Disjunction / Alternation: `|` (generator choice point: try left; on backtrack try right). Pure
     boolean disjunction can also be written `or`.

   - Semicolons are strictly forbidden inside expressions and conditions.

   - Example of clean applicability query:

     ```vagent
     plan Evacuate for Safety
       where (believes fire(?room) | believes smoke(?room)) & believes at(self, ?room) {
       achieve Exit(room)
     }
     ```

1. **Relational Horn Clause Definitions (`rule` / `logic`)**:

   - If relational rules are embedded in VAgent, Horn clauses should use distinct clause heads
     (`clause Head if Body` or multiple clause definitions) rather than overloading `;` into the
     procedural space.

______________________________________________________________________

### Axis 5: Pattern Matching & Destructuring Syntax (Disambiguating `[]`)

#### AOP Context & Evaluation

In classic SARL, event handlers used Xtend's lambda bracket syntax for guards:

```sarl
on TargetDetected [ occurrence.distance < 50.0 ] { ... }
```

In VAgent's grammar (§8.9), this was carried over:

```ebnf
HandlerDef ::= 'on' ( Ident [ 'as' Ident ] | ( '+' | '-' ) Term )
               [ '[' Expr ']' ] [ 'priority' IntLit ] Block ;
```

This overloading of `[` and `]` is disastrous:

- `[` introduces a list literal: `[1, 2, 3]`
- `[` introduces a list type: `[Int]`
- `[` represents array indexing: `percepts[1]`
- `[` represents handler guards: `on Event [guard]`
- `[` represents await guards: `await Event [guard] within 10s`

When an identifier is followed by `[`, the parser cannot know whether it is looking at an indexed
access, a type specialization, or a guard without extensive lookahead. Furthermore, unpacking
complex event payloads currently requires manual field access (`occurrence.distance`).

#### Concrete Recommendations

1. **Eliminate Bracket Guards**:

   - Replace `on Event [guard]` with **`on Event if guard`** (or `where guard`).
   - Replace `await Event [guard]` with **`await Event if guard`**.
   - *AOP Metamodel Distinction*:
     - Use **`if`** for event handlers and await filters:
       `on TargetDetected as t if t.distance < 50.0`. The guard is an immediate reactive predicate
       evaluated against the incoming event instance.
     - Use **`where`** for BDI plan applicability and belief preconditions:
       `plan Patrol for AreaCoverage where believes battery(?b) & b > 20`. The guard queries the
       agent's internal belief base and context before committing to the plan.
   - This single change completely cleanses square brackets `[]` from control flow!

1. **Disambiguating Brackets, Parentheses, and Braces**:

   - **Lists**: Strictly square brackets `[...]` for literals (`[1, 2, 3]`), types (`[T]`), and
     subscripts (`list[1]`).
   - **Tuples**: Strictly parentheses `(...)` for literals (`(1, "ready")`), types (`(Int, Str)`),
     and tuple destructuring (`let (?id, ?status) := next_task()`).
   - **Records & Events**: Named braces `{...}` for literals (`Point{ x: 1.0, y: 2.0 }`), types, and
     record destructuring (`let Point{ x: ?x, y: ?y } := location`).

1. **First-Class Event and Belief Pattern Destructuring**: Enable deep pattern destructuring
   directly in event triggers and match statements:

   ```vagent
   // Deep destructuring in reactive handler
   on CollisionWarning{ obstacle_id: ?obs, distance: ?d } if d < 10.0 {
     stop_engines()
     emit ObstacleEvaded{ obstacle: obs }
   }

   // Pattern matching over sum types / events
   match evt with {
     Message{ sender: ?s, payload: Order{ id: ?id, amount: ?amt } } if amt > 1000 => {
       adopt ProcessLargeOrder(id, amt)
     }
     Message{ sender: ?s, payload: ?other } => {
       log("Unhandled message from " ++ s)
     }
   }
   ```

______________________________________________________________________

## 3. Case Study: Refactoring the VAgent Standard Library

To demonstrate the transformative impact of these harmonized recommendations, let us examine the
current implementation of `mind.vagent` and `arbitration.vagent`.

### 3.1 The Pathology of `mind.vagent`

Because VAgent lacked 1-based dynamic arrays, destructuring, and `Opt<T>`, the author of
`mind.vagent` had to manually allocate 8 fact slots across 48 discrete scalar variables:

```vagent
// apps/common/xanadu/vagent/stdlib/mind.vagent (lines 11-67)
var b1_active: Bool := false
var b1_pred: Str := ""
var b1_subj: Str := ""
var b1_obj: Str := ""
var b1_weight: Int := 0
var b1_source: Str := ""
...
var b8_active: Bool := false
...
```

Operations like `assert_fact`, `retract_fact`, and `holds_fact` required unrolling 8 identical
`if-else` cascades stretching over 150 lines:

```vagent
def holds_fact(pred: Str, subj: Str, obj: Str): Bool {
  if b1_active & b1_pred == pred & b1_subj == subj & b1_obj == obj then true
  else if b2_active & b2_pred == pred & b2_subj == subj & b2_obj == obj then true
  ...
  else if b8_active & b8_pred == pred & b8_subj == subj & b8_obj == obj then true
  else false
}
```

### 3.2 The Harmonized `mind.vagent`

With 1-based indexing, record destructuring, `Opt<T>`, and generator expressions, the entire
300-line `mind.vagent` collapses into under 40 lines of clean, idiomatic AOP code:

```vagent
module std:agent:mind

record Fact {
  pred: Str,
  subj: Str,
  obj: Str,
  weight: Int,
  source: Str
}

agent AgentMind {
  var beliefs: [Fact] := []
  var active_intentions: Int := 0
  var inbox: Opt<Fact> := none

  def assert_fact(pred: Str, subj: Str, obj: Str, weight: Int, source: Str): Int {
    beliefs := beliefs ++ [Fact{ pred: pred, subj: subj, obj: obj, weight: weight, source: source }]
    beliefs.length()   // Returns 1-based index of newly inserted fact
  }

  def retract_fact(pred: Str, subj: Str, obj: Str): Bool {
    let initial_len := beliefs.length()
    beliefs := [f for f in beliefs if !(f.pred == pred & f.subj == subj & f.obj == obj)]
    beliefs.length() < initial_len
  }

  def holds_fact(pred: Str, subj: Str, obj: Str): Bool {
    for f in beliefs do {
      if f.pred == pred & f.subj == subj & f.obj == obj then return true
    }
    false
  }

  def query_belief_object(pred: Str, subj: Str): Opt<Str> {
    for f in beliefs do {
      if f.pred == pred & f.subj == subj then return Some(f.obj)
    }
    none   // Clean Opt<Str>, zero sentinels!
  }
}
```

### 3.3 The Pathology of `arbitration.vagent`

In `arbitration.vagent`, intention slots were similarly unrolled into 44 variables (`i1_id` through
`i4_tried`), and plan tracking resorted to raw bitwise arithmetic (`tried_mask % 2`,
`tried_mask / 2`):

```vagent
def is_plan_tried(tried_mask: Int, plan_id: Int): Bool {
  if plan_id == 1 then (tried_mask % 2) == 1
  else if plan_id == 2 then ((tried_mask / 2) % 2) == 1
  ...
}
```

With 1-based collections, `is_plan_tried` becomes `plan_id in tried_plans`, and the intention queue
becomes a typed list of records `[Intention]`.

______________________________________________________________________

## 4. Proposed Grammar Modifications (EBNF)

To lock in these changes, the grammar in `design/vagent-agent-language.md` (§8.9) should be updated
as follows:

```ebnf
/* ========================================================================= */
/* Top-Level & Member Declarations                                           */
/* ========================================================================= */

HandlerDef    ::= 'on' Trigger [ 'as' Pattern ] [ 'if' Expr ] [ 'priority' IntLit ] Block ;
Trigger       ::= Ident | Term | ( '+' | '-' ) Term ;

GoalDecl      ::= 'goal' Ident '(' [ ParamList ] ')' GoalKind [ '{' { GoalClause } '}' ] ;
GoalKind      ::= 'achieve' | 'perform' | 'maintain' ;
GoalClause    ::= 'succeed' 'when' Expr | 'drop' 'when' Expr | 'holds' Expr ;

PlanDecl      ::= 'plan' Ident 'for' Ident [ '(' [ IdentList ] ')' ] [ 'where' Expr ] Block ;

/* ========================================================================= */
/* Blocks & Statements                                                       */
/* ========================================================================= */

Block         ::= '{' [ Stmt { Sep Stmt } ] [ Sep ] '}' ;
Sep           ::= ';' | NEWLINE ;
Stmt          ::= 'let' Pattern [ ':' Type ] ':=' Expr
                | Expr
                | EffectRecovery ;

/* ========================================================================= */
/* Expressions & Operators                                                   */
/* ========================================================================= */

Expr          ::= Assign { '&' Assign } ;
Assign        ::= ScanExpr [ ( ':=' | '<-' ) Assign ] ;
ScanExpr      ::= 'scan' Expr 'with' Block | Alt ;
Alt           ::= NotExpr { '|' NotExpr } ;
NotExpr       ::= ( 'not' | '!' ) NotExpr | Cmp ;
Cmp           ::= Concat [ CmpOp Concat ] ;
CmpOp         ::= '<' | '<=' | '>' | '>=' | '==' | '!=' | 'is' ;
Concat        ::= Add { '++' Add } ;
Add           ::= Mul { ( '+' | '-' ) Mul } ;
Mul           ::= Coalesce { ( '*' | '/' | '%' ) Coalesce } ;
Coalesce      ::= Unary { '??' Unary } ;
Unary         ::= ( '-' | '*' ) Unary | Postfix ;
Postfix       ::= Primary { '(' [ ArgList ] ')' | '[' SliceIndex ']' | '.' Word | '!' } ;

SliceIndex    ::= Expr [ '..' Expr ] ;

/* ========================================================================= */
/* Patterns & Types                                                          */
/* ========================================================================= */

Pattern       ::= '?' Ident
                | Ident '{' [ Word ':' Pattern { ',' Word ':' Pattern } ] '}'
                | '(' [ Pattern { ',' Pattern } ] ')'
                | '[' [ Pattern { ',' Pattern } ] ']'
                | Literal
                | '_' ;

Type          ::= Ident [ '<' TypeList '>' ]
                | '[' Type ']'           /* List type: [Int] */
                | '(' [ TypeList ] ')'   /* Tuple type: (Int, Str) or () */
                ;
```

______________________________________________________________________

## 5. Summary Matrix for the Harmonization Committee

| Axis                         | Current Conflict                                  | SARL / AOP Expert Ruling                                                                                | Core Justification                                                                                                    |
| :--------------------------- | :------------------------------------------------ | :------------------------------------------------------------------------------------------------------ | :-------------------------------------------------------------------------------------------------------------------- |
| **1. Indexing**              | 0-based (SARL/Java) vs 1-based (VQL/VPL/Icon)     | **1-based everywhere** (`[1, N]`). Negative indices (`[-1]`). `end` inclusive.                          | Aligns with spatial/cognitive domain reasoning and sibling languages. Eliminates off-by-one errors in percept queues. |
| **2. Option**                | `Opt<T>` vs `T?`; sentinels in stdlib             | **Strictly `Opt<T>`**, `Some(v)`, `none`, `??` coalescing, goal-directed unwrap `!`.                    | Partial observability is an inherent agent condition. Zero sentinels allowed.                                         |
| **3. Disambiguation of `?`** | `?var` vs `s ? scan` vs `T?` vs `ask?`            | **Reserve `?var` strictly for logic/pattern variables**. Replace scanning with `scan s with { ... }`.   | Preserves BDI unification clarity and eliminates all parser ambiguities.                                              |
| **4. Plan Operators**        | Prolog `;` (OR) vs procedural step sequencing `;` | **`;` is strictly statement separator**. Conjunction is `&` / `and`. Disjunction is `\|` / `or`.        | BDI plans and handlers are intentional procedural recipes, not static Horn clauses.                                   |
| **5. Pattern Matching**      | Awkward `on Event [guard]`; list overload         | **`on Event if guard`**, `where` for plans. `(...)` for tuples, `[...]` for lists, `{...}` for records. | Eliminates bracket overload; enables deep pattern matching on event payloads.                                         |

______________________________________________________________________

## 6. Conclusion

By adopting these recommendations, VAgent will achieve what no previous agent language has managed:
a seamless synthesis of **SARL's reactive capacity/behavior model**, **BDI's cognitive
deliberation**, and **Icon's goal-directed evaluation**, fully aligned with the **1-based
hyperstructural substrate of Vortex and ZigZag**.

Most importantly, it will elevate VAgent from an esoteric academic hybrid into an ergonomic,
production-grade language capable of expressing complex autonomous multi-agent intelligence with
unrivaled elegance and safety.
