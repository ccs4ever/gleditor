# Common Briefing: VAgent Language Harmonization

**Document Under Review**: Proposed Language Harmonization for VAgent
(`apps/common/xanadu/vagent/`)\
**Context**: Metacircular compiler implementation and self-hosting VAgent language design\
**Goal**: Harmonize divergent influences from SARL, BDI, Icon/Vicon, Prolog/VProlog, and
Vortex/ZigZag into a coherent, elegant, and orthogonal language specification.

______________________________________________________________________

## 1. Background & Divergent Influences

VAgent has synthesized paradigms from multiple foundational systems:

1. **SARL (Agent-Oriented Programming)**:

   - Metamodel: Agents, events (`occurrence`), capacities (`uses`), skills (`install`), behaviors,
     spaces, contexts, holons.
   - Declarations: `on Event [guard] as name { ... }`, `agent Name { ... }`,
     `behavior Name { ... }`.
   - Structural conventions: Imperative/functional hybrid statements, 0-based indexing heritage
     (from Java/Xtend).

1. **BDI (Belief-Desire-Intention)**:

   - Cognitive primitives: Belief base (`believe`, `forget`, `revise`, `believes`), declarative
     goals (`adopt`, `achieve`), plan library (`plan PlanName for Goal(?args) [guard] { ... }`),
     intentions.
   - Unification & logic variables: `?var` in relational terms (e.g. `believes Position(?x, ?y)`).

1. **Icon / Vicon (Goal-Directed Execution & Scanning)**:

   - Evaluation regime: Expressions either succeed (producing 1 or more results) or fail.
     Backtracking across `#Choice`/`#Cut` marks.
   - Conjunction & Alternation: `&` evaluates both requiring success, producing the right value; `|`
     introduces a generator choice point producing left then right on backtracking.
   - String Scanning: Infix `subject ? scan_expr` sets `&subject` and `&pos`; scanning primitives
     `tab(n)`, `move(n)`, `upto(s)`, `many(s)`, `match(s)`, `find(s)`.
   - Indexing: 1-based indexing for strings and positions (`&pos` starts at 1; `tab(1)` is start of
     string).

1. **Prolog / VProlog (Logic Programming & Horn Clauses)**:

   - Horn clause logic: `head :- body`.
   - Conjunction: comma `,` means logical AND.
   - Disjunction: semicolon `;` means logical OR.
   - Backtracking and cut: `!` cuts choice points.

1. **Vortex & ZigZag (Hyperstructural Engine)**:

   - Data model: 2-degree orthogonal cell manifolds, Merkle-tree ops spool.
   - Sibling languages: VQL (1-based array/relation queries), VPL (1-based APL/J arrays), VProlog
     (1-based logic lists).
   - Execution: Actor turn isolation, zero sentinels (`Result<T, E>`, `Opt<T>`), determinism.

______________________________________________________________________

## 2. The Five Harmonization Axes

Each expert must address the following five specific conflicts:

### Axis 1: Indexing Harmonization (1-Based Everywhere)

- **Problem**: Vicon, VQL, and VProlog use 1-based indexing, whereas SARL/Java traditionally used
  0-based.
- **Direction**: Harmonize to 1-based indexing everywhere (`list[1]`, `str[1]`).
- **Questions**:
  1. What is the valid index range for a list/string of length $N$? (Is it $[1, N]$?)
  1. How do slice ranges work? (`arr[start..end]` — is `end` inclusive or exclusive? How does this
     align with Icon character positions $[1, N+1]$?)
  1. What is the semantic of index `0`? Does `arr[0]` produce an error/failure, or does it map to
     end-of-string as in Icon's `tab(0)`?
  1. How does negative indexing behave? (Does `arr[-1]` mean the last element $N$ or relative to
     end?)
  1. What is the syntax and semantics for loop ranges? (`for i in 1..n`).

### Axis 2: Option (`Opt<T>`) Consistency

- **Problem**: Option handling has fragmented into competing idioms:
  - Type spellings: `Opt<T>` vs `T?`.
  - Literals/Constructors: `none` vs `None` vs `Some(v)`.
  - Conditionals / Truthiness: `if opt[]` vs `if opt?` vs `if opt` vs `is_some(opt)`.
  - Safe navigation & unwrapping: `opt?.field`, `opt!`, `opt.unwrap()`, `opt ?? default`.
  - Pattern matching: `match opt with { Some(?x) => ..., none => ... }`.
- **Questions**:
  1. What is the canonical type syntax?
  1. What are the canonical constructors for present and absent values?
  1. How should options be unwrapped safely, defaulted, or checked in control flow?
  1. Should `Opt<T>` participate in goal-directed failure (e.g. absent fails)?

### Axis 3: Overloading and Disambiguation of `?`

- **Problem**: The symbol `?` is heavily overloaded:
  - Unification variable / pattern binding: `?var`.
  - String scanning binary operator: `subject ? scan_expr`.
  - Postfix optional type: `T?`.
  - Question / ask performatives: `ask Agent question?`.
  - Safe navigation or unwrap operator: `expr?.field` or `expr?`.
- **Questions**:
  1. How should the lexer and parser disambiguate `?` without ambiguity or arbitrary lookahead?
  1. Should string scanning retain infix `?`, or should it be a keyword / macro / distinct operator
     (e.g. `scan s with { ... }` or `s ? scan_expr`)?
  1. How does `?var` in pattern matching interact with unary `?` operators?

### Axis 4: Plan Operators & Conjunction/Disjunction Harmonization

- **Problem**:
  - Prolog uses `,` for conjunction and `;` for disjunction.
  - Icon uses `&` for conjunction and `|` for alternation/disjunction.
  - Procedural languages use `;` or newlines for statement sequencing, and `&&`/`||` or `and`/`or`
    for boolean logic.
  - In BDI plans, developers write sequential steps, applicability guards, and belief queries.
- **Questions**:
  1. In sequential plan and handler bodies, what separates steps? (Newlines / `;`?).
  1. In pure boolean expressions, what are the logical operators? (`and`, `or`, `not`).
  1. In goal-directed generator contexts, what are conjunction and alternation? (`&`, `|`).
  1. In relational belief queries (`believes ...`), how are multi-predicate conjunctions and
     disjunctions written? Can we eliminate the clash where `;` means OR in Prolog but statement
     separator in procedural code?

### Axis 5: Pattern Matching & Destructuring Syntax (Disambiguating `[]`)

- **Problem**: Square brackets `[` and `]` currently overload:
  - List literals: `[1, 2, 3]`.
  - List types: `[Int]`.
  - Subscript indexing: `arr[1]`.
  - Tuple/Record destructuring: `let [?a, ?b] := ...`.
  - SARL handler guards: `on Event [guard] { ... }`.
- **Questions**:
  1. How should tuples be represented vs lists? (e.g. `(?a, ?b)` for tuples, `[?a, ?b]` for lists).
  1. How should record destructuring be spelled? (e.g. `Point { x: ?x, y: ?y }`).
  1. How should handler guards be spelled to avoid bracket overload? (e.g. `on Event if guard` or
     `on Event where guard` instead of `on Event [guard]`).
  1. How should the unified `match ... with` syntax integrate with pattern destructuring?

______________________________________________________________________

## 3. Reviewer Roster & Assigned Perspectives

1. **Vortex & VAgent Expert** (`vortex_vagent`): Substrate fidelity, bytecode lowering (`#Choice`,
   `#Cut`, `#ScanPush`, `#ScanPop`), ZigZag manifold representation, actor turn atomicity, C++
   compiler boundaries.
1. **SARL Expert** (`sarl`): Agent-oriented metamodel purity, event-driven reactive handlers,
   capacity/skill interactions, BDI plan execution lifecycle, spaces/contexts.
1. **Prolog Expert** (`prolog`): Logic programming purity, Horn clause semantics, unification,
   relational queries against belief bases, avoiding confusion with `;` and `,`.
1. **Icon Programming Language Expert** (`icon`): Goal-directed evaluation purity, generator
   semantics, string scanning environments (`&subject`, `&pos`), position arithmetic,
   success/failure control flow.
1. **Programming Language Design & Ergonomics Expert** (`pl_ergo`): Grammar unambiguity (LALR(1)),
   cognitive load, operator precedence, type system elegance, consistency, error diagnosability.
