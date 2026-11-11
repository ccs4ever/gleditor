# VAgent Language Harmonization Review — Round 1

**Reviewer**: Critic 1: Vortex & VAgent Expert (`vortex_vagent`)\
**Focus**: Substrate Fidelity, Bytecode Lowering (`#Choice`, `#Cut`, `#ScanPush`, `#ScanPop`,
`#Unify`), ZigZag 2-Degree Manifold Representation (`d.vars`, `d.step`, `d.ast.*`), Spool-Mesh Actor
Turn Isolation, Transactional Trails, and Metacircular Compiler Pipeline Boundaries\
**Target Document**: `design/vagent-agent-language.md`\
**Reference Implementations**: `apps/common/xanadu/vagent/`, `apps/common/xanadu/vortex/`,
`tests/xuzz/vagent_vicon_scan_test.cpp`, `brief-common.md`\
**Date**: November 2026

______________________________________________________________________

## 1. Executive Summary

As the Vortex & VAgent Systems Architect in this harmonization review, my mandate is to ensure that
every syntactic and semantic construct in VAgent preserves strict fidelity to the underlying
hyperstructural substrate:

1. **ZigZag 2-Degree Orthogonal Manifold Representation**: All programmatic structures (ASTs, symbol
   environments, scopes, call frames, records, and percept queues) are concrete subgraphs within
   Nelsonian 2-degree cell manifolds (`vortex_core.hpp:34-57`). Dimensions such as `d.vars`,
   `d.step`, `d.spin`, `d.clause`, and `d.scan` have precise geometric topologies that cannot
   tolerate ambiguous or non-orthogonal semantics.
1. **Vortex VM Bytecode Lowering**: High-level language constructs must lower cleanly into native
   Vortex bytecode instructions (`vortex_vm.hpp:28-143`): `#Choice`, `#Cut`, `#Fail`, `#ChoiceDepth`
   for goal-directed execution; `#ScanPush`, `#ScanPop`, `#ScanTab`, `#ScanMove` for string
   scanning; `#Unify`, `#Deref` for logic relations; and `#Find`, `#FieldSet` for term member
   access.
1. **Spool-Mesh Actor Isolation & Turn Atomicity**: Each agent runs as an isolated actor
   (`mesh_actor.cpp:32-75`). Turn execution (`pumpOne`, line 550) operates within an `ArenaManifold`
   transactional mark trail (`arena_manifold.cpp:1506-1570`). State modifications must be
   rollback-safe under failure. Sentinels and raw pointers across actor boundaries violate the
   project's zero-sentinel invariants.
1. **Metacircular Pipeline Elegance**: The current parser (`parser.cpp`) contains ad-hoc state hacks
   (such as `allowScan_` flags toggling scanner precedence) caused by premature coupling between the
   lexer and parser. The AST (`ast.hpp`) and checker (`checker.cpp`) must be decoupled and cleanly
   stratified.

Below is my rigorous technical evaluation and concrete architectural recommendations across all five
harmonization axes.

______________________________________________________________________

## 2. In-Depth Substrate & Runtime Architecture

### 2.1 The ZigZag Cell Manifold as Language Substrate

In VAgent, code and data are not decoupled byte buffers; they are topological structures embedded in
an `ArenaManifold`. In `apps/common/xanadu/vortex/vortex_core.hpp:34-57`, `SystemDimensions` defines
the fundamental coordinates minted off `home`:

```cpp
struct SystemDimensions {
  DimRef grab{noCell};     ///< Parameter wings (-d.grab = outputs, +d.grab = inputs)
  DimRef step{noCell};     ///< Parameter chaining along rank
  DimRef spin{noCell};     ///< Process instruction stream
  DimRef stack{noCell};    ///< Call frame stack
  DimRef contract{noCell}; ///< Design by Contract (-d.contract = require, +d.contract = ensure)
  DimRef clone{noCell};    ///< Clone rank (shared identity)
  DimRef cursors{noCell};  ///< Process scheduler manifold
  DimRef cache{noCell};    ///< Memoization cache rank on pinned islands
  DimRef vars{noCell};     ///< Scope variable names
  DimRef values{noCell};   ///< Scope bound values
  DimRef clause{noCell};   ///< Clause database rank off predicate cells (Vlog)
  DimRef scan{noCell};     ///< String-scanning cursor rank off home; cursor's content
                           ///< is &pos, +d.grab is &subject (Vicon spec §6.1)
  DimRef stores{noCell};   ///< Multi-store connection rank off home
};
```

Ranks along `+d.step` represent ordered sequences:

- Function and method arguments are chained along `+d.step`.
- Record and term members are chained along `+d.step`, where each member carries a label along
  `+d.name` or a schema link along `+d.recipe`.
- List elements are cells chained along `+d.step`.

Because stepping from a head cell $c_1$ along `+d.step` is fundamentally 1-based (the head is cell
1, the first forward step reaches cell 2), 0-based indexing is a foreign contrivance that violates
the geometry of the manifold.

### 2.2 Vortex VM Instruction Execution & Goal-Directed Lowering

The Vortex VM (`vortex_vm.hpp`, `vortex_vm.cpp`) executes opcodes linked along `+d.spin`. Unlike
conventional register or stack virtual machines, Vortex implements Warren Abstract Machine (WAM)
style choice points and Icon goal-directed backtracking directly over cell graphs:

1. **`OpcodeKind::Choice` (`vortex_vm.hpp:90`)**: Mints a `ChoicePoint` (`vortex_vm.hpp:160-169`)
   capturing the current cursor PC, alternative opcode target (`altOp`), arena transactional mark
   (`zigzag::Mark`), call stack frame, and cut barrier depth.
1. **`OpcodeKind::Cut` (`vortex_vm.hpp:92`)**: Truncates the VM's choice stack down to a specified
   barrier depth, discarding alternative choice points and committing all mutations executed since
   the corresponding mark.
1. **`OpcodeKind::ScanPush` & `OpcodeKind::ScanPop` (`vortex_vm.hpp:132-133`)**: Manages the string
   scanning environment stack along `core_.dims().scan`. `ScanPush` creates a scanning cursor cell
   holding `&pos` (initialized to `1`) with `+d.grab` pointing to the subject text cell
   (`tests/xuzz/vagent_vicon_scan_test.cpp:57-60`). `ScanPop` unwinds and restores the previous
   scanning cursor.
1. **`OpcodeKind::Find` & `OpcodeKind::FieldSet` (`vortex_vm.hpp:105, 114`)**: Walk the `+d.step`
   rank of a term cell to find or update the member whose `+d.name` matches the requested field
   identifier (`compiler.cpp:1586-1590`, `:1670-1675`).

### 2.3 Spool-Mesh Actor Turn Isolation

Each VAgent instance executes as a sovereign actor managed by `MeshActorRuntime`
(`apps/common/xanadu/vortex/mesh_actor.cpp:32-75`). An actor turn (`pumpOne`, line 550) is strictly
isolated:

- At the start of a turn, the actor takes a transactional mark (`arena.mark()`).
- Events, percepts, and goal invocations are dispatched within this turn mark.
- If a plan or handler encounters an unhandled failure, the turn rolls back cleanly via
  `arena.release(mark)` (`arena_manifold.cpp:1612-1700`), leaving the agent's persistent belief base
  and manifold state uncorrupted.
- Messages dispatched to other agents via `emit`, `tell`, or `ask` are held in the turn outbox
  (`executor.hpp:30-40`) and committed to the transport network only upon successful completion of
  the turn.

______________________________________________________________________

## 3. Evaluation and Recommendations Across the Five Axes

### Axis 1: Indexing Harmonization (1-Based Everywhere)

#### Substrate Analysis & The 1-Based Reality

The hyperstructural engine is uncompromisingly 1-based:

- **ZigZag Manifolds**: A rank has a head cell (rank index 1). Stepping 1 hop forward arrives at
  index 2. There is no "cell 0" preceding the rank head.
- **Vicon String Scanning**: In `tests/xuzz/vagent_vicon_scan_test.cpp:59`, `stdlib.scanPos()`
  begins at `1`. In `vortex_vm.cpp:1215`,
  `const std::int64_t curPos = core_.arena().asInt64(tail).value_or(1);`. Positions in strings are
  inter-character positions ranging from $1$ (before the first character) to $N+1$ (after the last
  character).
- **Sibling Query and Array Languages**: VQL (relational tuples), VPL (APL/J lattices), and VProlog
  (Horn clause lists) all use 1-based indexing.

However, an audit of `apps/common/xanadu/vortex/vortex_vm.cpp` reveals an internal contradiction:

- Line 1903: `OpcodeKind::ByteAt` checks `if (idx < 0 || static_cast<std::size_t>(idx) >= s.size())`
  — a legacy 0-based implementation.
- Line 1927: `OpcodeKind::StrSlice` clamps `start` and `end` as 0-based offsets.
- Meanwhile, `compiler.cpp` has not yet lowered `ExprKind::Index` (`ast.hpp:52`), leaving the
  surface syntax uncommitted.

#### Concrete Recommendations

1. **Element Index Range $[1, N]$**: For any list or string of length $N$, valid element indices are
   strictly $[1, N]$. Index `1` addresses the first element; index `N` addresses the last element.
1. **Subscript Semantics & Out-of-Bounds Behavior**: In goal-directed execution, `arr[i]` where
   $i \notin [1, N]$ is a **failure condition**, exactly as `ByteAt` currently attempts
   `if (backtrack(cursor))` in `vortex_vm.cpp:1905`. In a generator context, it backtracks to the
   enclosing `#Choice` point. In a deterministic statement, accessing an invalid index either
   produces goal failure or, if typed as returning `Opt<T>`, yields `None`.
1. **The Semantics of Index `0` vs `tab(0)`**: We must draw an absolute, non-negotiable
   architectural boundary between **element subscripting** and **string scanning positions**:
   - In element indexing (`arr[0]`), `0` is **strictly invalid**. There is no 0th element in a
     1-based collection. Subscripting `arr[0]` must fail / report out of bounds. In Icon itself,
     `s[0]` is an error; element subscripting only accepts non-zero integers.
   - In scanning positions (`tab(p)`, `move(p)`), $p$ is an inter-character position in $[1, N+1]$.
     Here, `0` is the conventional Icon idiom representing position $N+1$ (end-of-string). As
     verified in `vortex_vm.cpp:1218`, the VM translates non-positive scanning targets:
     `targetPos <= 0 ? (len + 1 + targetPos) : targetPos`. When `targetPos == 0`,
     `target = len + 1 + 0 = len + 1`. This scanning convention must remain confined to string
     scanning routines (`tab`, `move`) and must **never** leak into array or list subscripting
     `arr[0]`.
1. **Negative Indexing Formulation**: For negative indices $-k$ where $k \in [1, N]$, the effective
   index is $N + 1 - k$:
   - `arr[-1]` maps to $N + 1 - 1 = N$ (the last element).
   - `arr[-2]` maps to $N + 1 - 2 = N - 1$ (the penultimate element).
   - `arr[-N]` maps to $N + 1 - N = 1$ (the first element).
   - Notice that if $k = 0$, $N + 1 - 0 = N + 1$, which is out of bounds for an element of length
     $N$. This mathematical identity cleanly proves why `arr[0]` is invalid.
1. **Slice Ranges & Loop Ranges**:
   - Slices `arr[start..end]` use **inclusive bounds** $[start, end]$. `arr[1..3]` yields elements
     1, 2, 3 (a list of length 3). `arr[1..-1]` yields the full list $[1, N]$.
   - Loop ranges `for i in 1..n do` (`parser.cpp:1019`) iterate over the closed interval $[1, n]$.
     If $n < 1$, the loop executes 0 times and completes with `()`.
   - In `vortex_vm.cpp`, `ByteAt` and `StrSlice` must be updated to native 1-based parameters,
     eliminating the runtime conversion layer.

______________________________________________________________________

### Axis 2: Option (`Opt<T>`) Consistency

#### Substrate Analysis & The Zero-Sentinel Rule

In Xanadu and ZigZag, sentinel values are forbidden (Nelsonian rule: no reserved nulls or magic
numbers; see `GEMINI.md` and `AGENTS.md`). In `apps/common/xanadu/vagent/executor.cpp:661-666`:

```cpp
// A bare `none` arrives as position 0 — noCell is never a live
// referent, and marking it would harvest the Opt's empty state as
// a pointer. Route it through the scalar store, which also clears
// any mark a previous ref left on the slot.
if (referent == noCell) {
  storeVar(rec, CellValue{std::int64_t{0}});
}
```

Currently, `checker.cpp:25` spells the type as
`Type optional(Type element) { return {"Opt", ...}; }`, `token.cpp:43` defines `none` as a keyword,
and `parser.cpp:991` accepts `none`. However, code in the wild and proposals have oscillated between
`Opt<T>` and `T?`, `none` and `None`, and `Some(x)`.

#### Concrete Recommendations

1. **Canonical Type Spelling: `Opt<T>`**: The type is canonically spelled `Opt<T>`. We explicitly
   reject `T?`. In addition to aligning with `List<T>`, `Map<K, V>`, and `Code<T>`
   (`ast.hpp:28-36`), banning postfix `?` is essential to solve Axis 3 (eliminating `?`
   overloading).
1. **Canonical Constructors: `Some(x)` and `None`**:
   - Present value: `Some(x)`.
   - Absent value: `None`.
   - In the ZigZag manifold: `Some(x)` is represented as a 1-ary term: a cell with functor `"Some"`
     linked along `+d.step` to the value cell $x$. `None` is represented as a 0-ary atom: a cell
     with functor `"None"` and no `+d.step` children (or scalar integer 0 on an unreferenced slot as
     in `executor.cpp:666`).
   - For AST backwards compatibility, the lexer/parser will treat lowercase `none` as an alias for
     `None`.
1. **No Implicit Truthiness**: Expressions like `if opt then` or `if opt[]` are banned. In an actor
   system, implicit truthiness leads to cognitive errors. Options must be tested via:
   - Pattern matching: `if let Some(?x) := opt then ...` or `match opt with { Some(?x) => ... }`
   - Explicit guard method: `if opt.is_some() then ...`
1. **Safe Navigation & Null Coalescing**:
   - Null coalescing operator `??`: `opt ?? default`. Lowers to a branch: if `opt` is `Some(x)`,
     produce $x$; otherwise evaluate and produce `default`.
   - Safe navigation `?.`: `opt?.field`. If `opt` is `Some(x)`, navigates to field via `#Find`; if
     `None`, yields `None`.
1. **Goal-Directed Unwrapping (`opt!`)**: Unwrapping an option uses the postfix operator `opt!`.
   **Substrate Interaction with Goal-Directed Failure**:
   - In a generator/goal-directed context (evaluated under a `#Choice` point), if `opt` is
     `Some(x)`, `opt!` succeeds and produces $x$. If `opt` is `None`, `opt!` **fails the goal**,
     triggering backtracking to the previous `#Choice` mark. This directly unifies algebraic options
     with Icon success/failure semantics.
   - In a deterministic context outside choice points, `opt!` on `None` raises a typed contract
     violation (`ContractViolationKind`), rolling back the actor turn safely.

______________________________________________________________________

### Axis 3: Disambiguation of `?`

#### Substrate & Parser Analysis: The Root Cause of Grammar Ambiguity

In the current codebase, the character `?` is dangerously overloaded across five roles:

1. Pattern / Logic Variable: `?var` in `Parser::termArgument()` (`parser.cpp:918`) and
   `Parser::recordLiteral()` (`parser.cpp:902`).
1. String Scanning Infix Operator: `subject ? scan_expr` in `parser.cpp:48` and
   `compiler.cpp:1630-1658`.
1. Postfix Optional Type: `T?`.
1. Postfix Unwrapping / Chaining: `expr?.field` or `expr?`.
1. Performatives: `ask Agent question?`.

Because `lexer.cpp:309` naively emits `TokenKind::Scan` for *every* occurrence of `?`:

```cpp
case '?':
  return token(TokenKind::Scan, start);
```

the parser was forced to introduce a fragile modal state variable: `allowScan_`. Examining
`apps/common/xanadu/vagent/parser.cpp`:

- Line 705: `Mode scans(allowScan_, true);`
- Line 795: `if (!op || op->precedence < minimum || (at(TokenKind::Scan) && !allowScan_)) break;`
- Line 922: `Mode scans(allowScan_, false);` inside `termArgument()`
- Lines 736, 847, 876, 893, 980: constant toggling of `allowScan_`.

Whenever the parser encounters `?` in an expression, it cannot tell if it is starting a binary scan
operator or a pattern variable without inspecting surrounding context and modal flags. This is
unacceptable in a metacircular compiler.

#### Concrete Recommendations

1. **Lexer-Level Disambiguation of `?var`**: The lexer must distinguish prefix variable sigils from
   operators based on immediate character adjacency (zero whitespace):

   - When `?` is immediately followed by an identifier character `[a-zA-Z_]` (e.g. `?x`, `?name`),
     the lexer emits `TokenKind::LogicVariable` with string value `"x"` or `"name"`. This directly
     mirrors how VQL handles `$var` (`vql_parser.cpp`).
   - When `??` is encountered, the lexer emits `TokenKind::DoubleQuestion` (null-coalescing).
   - When `?.` is encountered, the lexer emits `TokenKind::QuestionDot` (safe navigation).
   - When `?` is standalone (surrounded by whitespace or preceded by an expression), it is lexed as
     `TokenKind::Scan`.

1. **Preservation of Infix Scanning `subject ? scan_expr`**: String scanning *retains* infix `?`.
   The lowering in `compiler.cpp:1630-1658` is exceptionally clean:

   ```cpp
   if (op == "?") {
     const auto subjTemp   = temporary();
     const auto cursorTemp = temporary();
     const auto pushOp     = mint(OpcodeKind::ScanPush);
     bindIn(pushOp, subjTemp);
     bindOut(pushOp, cursorTemp);

     const auto depth     = scalar(std::int64_t{0});
     const auto readDepth = mint(OpcodeKind::ChoiceDepth);
     bindOut(readDepth, depth);

     const auto popSuccess = mint(OpcodeKind::ScanPop);
     bindIn(popSuccess, cursorTemp);
     spin(popSuccess, cut(depth, continuation));

     const auto popFail = mint(OpcodeKind::ScanPop);
     bindIn(popFail, cursorTemp);
     spin(popFail, jump(cells_->fail));

     const auto choice = mint(OpcodeKind::Choice);
     bindIn(choice, reference(popFail));

     const auto rightEntry = emit(right, output, popSuccess);
     spin(choice, rightEntry);
     spin(readDepth, choice);
     spin(pushOp, readDepth);

     return emit(left, subjTemp, pushOp);
   }
   ```

   Because `?var` is now a distinct token (`TokenKind::LogicVariable`), `subject ? scan_expr` and
   `?var` **never collide in the grammar**.

1. **Elimination of the `allowScan_` Parser Hack**: With `TokenKind::LogicVariable` recognized at
   the tokenization boundary, lines 705, 736, 795, 847, 876, 893, 922, and 980 in `parser.cpp` can
   be deleted. The grammar becomes pure LALR(1).

1. **Complete Elimination of `?` from Types and Performatives**:

   - Postfix optional types `T?` are prohibited in favor of `Opt<T>`.
   - `ask` performatives (`parser.cpp:1163`) do not use `?`; they use the clean syntax:
     `ask Question(...) within deadline else recovery`.

______________________________________________________________________

### Axis 4: Plan Operators & Conjunction/Disjunction Harmonization

#### Substrate Analysis & The Execution Regime Clash

VAgent synthesizes four distinct computational paradigms that have historically overloaded `,` and
`;`:

1. **Procedural Instruction Stream**: In imperative code and plan steps, statements are executed in
   sequence along `+d.spin`. Semicolons (`;`) and newlines (`\n`) are statement separators
   (`parser.cpp:141-143`).
1. **Boolean Value Logic**: Pure expressions evaluating to boolean values (`Bool`).
1. **Goal-Directed Generators (Icon/Vicon)**: Expressions that succeed or fail, backtracking across
   `#Choice` points.
1. **Horn Clause Relational Logic (Vlog/VProlog)**: Unification of predicate terms across `d.clause`
   ranks (`vortex_core.hpp:52`). In classic Prolog, `,` is AND and `;` is OR.

Allowing Prolog's `;` (OR) to exist in the same language where `;` is procedural statement
sequencing creates catastrophic ambiguity. For example:

```vagent
step1();
step2();
```

Does this mean "execute `step1` then execute `step2`", or "try `step1`, and if it fails, try
`step2`"? In an actor executing durable plan steps with side effects, this confusion is fatal.

#### Concrete Recommendations

We establish four strictly orthogonal syntactic and semantic tiers:

| Tier                             | Purpose                                | Operators                                            | Vortex VM Lowering                                                                         |
| :------------------------------- | :------------------------------------- | :--------------------------------------------------- | :----------------------------------------------------------------------------------------- |
| **1. Statement Sequencing**      | Procedural plan & handler steps        | `\n` or `;`                                          | Forward instruction chaining along `+d.spin` (`compiler.cpp:738`)                          |
| **2. Boolean Logic**             | Deterministic truth values (`Bool`)    | `and`, `or`, `not`                                   | `OpcodeKind::And`, `Or`, `Not` (`vortex_vm.hpp:45-47`)                                     |
| **3. Goal-Directed Generators**  | Success/failure generator backtracking | `&` (conjunction), `\|` (alternation)                | `&`: success chain (`compiler.cpp:1660`); `\|`: `OpcodeKind::Choice` (`compiler.cpp:1625`) |
| **4. Relational Belief Queries** | Pattern matching against belief base   | `and` / `,` (conjunction), `or` / `\|` (disjunction) | Unification over `d.clause` with choice branches                                           |

#### Banishing Prolog Semicolon

- Semicolon `;` is **strictly and exclusively** a procedural statement separator/terminator.
- Semicolon `;` is **permanently banned** from representing logical disjunction anywhere in VAgent.
- In relational queries (`believes ...`):
  - Conjunction is written with `and` (or comma `,` between separate predicate terms).
  - Disjunction is written with `or` or `|`:

```vagent
believes (Target(?x) and Visible(?x))
believes Target(?x) | believes Candidate(?x)
```

______________________________________________________________________

### Axis 5: Pattern Matching & Destructuring Syntax (Disambiguating `[]`)

#### Substrate Analysis & The Overloading of `[]`

Square brackets currently overload five incompatible concepts:

1. List literals: `[1, 2, 3]` (`parser.cpp:979`).
1. List types: `[Int]` (`ast.hpp:23-25`).
1. Indexing: `arr[1]` (`checker.cpp:1964`).
1. Tuple / Record destructuring: `let [?a, ?b] := ...`.
1. SARL Event Handler Guards: `on Event [guard] { ... }` (`parser.cpp:491`).

In `apps/common/xanadu/vagent/parser.cpp:491`:

```cpp
if (accept(TokenKind::OpenBracket)) {
  Mode newlines(significantNewlines_, false);
  Mode records(allowRecord_, true);
  result.guard = expression();
  expect(TokenKind::CloseBracket, "']' after an event guard");
}
```

Meanwhile, in `parser.cpp:572`, `plan` declarations use `where`:

```cpp
if (acceptWord("where")) result.guard = headerExpression();
```

The use of `[guard]` in event handlers is an unnecessary relic of SARL's Xtend grammar that collides
with list literals and destructuring brackets.

#### Concrete Recommendations

1. **Eliminate `[guard]` in Event Handlers**: Replace `on Event [guard]` with `on Event if guard` or
   `on Event where guard`. Because `plan` declarations already use `where` (`parser.cpp:572`),
   standardizing on `where` (or `if`) completely removes square brackets from declaration headers:

   ```vagent
   on PacketReceived where occurrence.payload.size() > 0 {
     // handler body
   }
   ```

1. **Strict Orthogonality: Tuples `(a, b)` vs Lists `[a, b]`**:

   - **Lists**: Homogeneous collections of dynamic length, linked along `+d.step`.
     - Literal: `[1, 2, 3]`
     - Type: `List<T>` (canonical) or `[T]` (sugar)
     - Destructuring: `let [?head, ...?tail] := list`
   - **Tuples**: Heterogeneous collections of fixed arity, stored as compound term cells with
     positional children along `+d.step`.
     - Literal: `(1, "target", true)`
     - Type: `(Int, Str, Bool)`
     - Destructuring: `let (?id, ?name, ?active) := tuple`

1. **Record Pattern Destructuring**: Records represent named product types where members are linked
   along `+d.step` and labeled along `+d.name`.

   - Pattern syntax: `Point { x: ?x, y: ?y }`.
   - Variable punning: `Point { ?x, ?y }` binds field `x` to `?x` and field `y` to `?y`.
   - **Bytecode Lowering**: Record pattern matching lowers directly to `OpcodeKind::Find`
     (`vortex_vm.hpp:105`, `compiler.cpp:1586-1590`). The VM walks the term's `+d.step` rank to
     match the member labeled with `+d.name`, and binds or unifies the extracted cell with the
     pattern variable.

1. **Unified `match ... with` Syntax**: Pattern matching is unified into a first-class control
   expression:

   ```vagent
   match expr with {
     case Some(?val) => handle_val(val)
     case None => handle_empty()
     case [?head, ...?tail] => process_list(head, tail)
     case (?x, ?y) => process_pair(x, y)
     case Point { ?x, ?y } where x > 0 => process_point(x, y)
     case _ => default_handler()
   }
   ```

   **Substrate Lowering**: Each pattern arm lowers to:

   - A choice point (`OpcodeKind::Choice`), establishing the next arm as the backtrack target.
   - Unification of the subject with the pattern via `OpcodeKind::Unify` (`vortex_vm.hpp:85`).
   - If the arm has a `where` guard, evaluation of the guard expression.
   - Upon successful match and guard satisfaction, a cut instruction (`OpcodeKind::Cut`) commits the
     match and discards the choice points for the remaining arms.
   - Evaluation of the arm body expression.

______________________________________________________________________

## 4. Concrete Implementation Plan for the Compiler Pipeline

To realize this specification in `metacircular_compiler_implementation_plan`, the following surgical
changes must be executed across the C++ implementation files:

### 4.1 `apps/common/xanadu/vagent/token.hpp` & `lexer.cpp`

- In `token.hpp`, add `TokenKind::LogicVariable`, `TokenKind::DoubleQuestion` (`??`), and
  `TokenKind::QuestionDot` (`?.`).
- In `lexer.cpp:308`:

```cpp
case '?': {
  if (peek() == '?') {
    advance();
    return token(TokenKind::DoubleQuestion, start);
  }
  if (peek() == '.') {
    advance();
    return token(TokenKind::QuestionDot, start);
  }
  if (isAlpha(peek()) || peek() == '_') {
    advance();
    while (isAlphaNumeric(peek()) || peek() == '_') advance();
    Token t = token(TokenKind::LogicVariable, start);
    t.stringValue = t.text.substr(1); // strip leading '?'
    return t;
  }
  return token(TokenKind::Scan, start);
}
```

### 4.2 `apps/common/xanadu/vagent/parser.cpp`

- **Delete `allowScan_`**: Remove all occurrences of `Mode scans(allowScan_, ...)` and the
  `allowScan_` boolean member from `Parser`.
- **Update `termArgument` (`parser.cpp:918`)**:

```cpp
ExprPtr Parser::termArgument() {
  if (at(TokenKind::LogicVariable)) {
    const auto token = take();
    return make(ExprKind::LogicVariable, token.loc, token.stringValue);
  }
  return expression();
}
```

- **Update Event Handler Guards (`parser.cpp:491`)**: Replace `accept(TokenKind::OpenBracket)` with
  `acceptWord("where") || acceptWord("if")`.

### 4.3 `apps/common/xanadu/vagent/compiler.cpp`

- **Lower `ExprKind::Index`**: Emit 1-based bounds verification:
  - Load index operand.
  - Check $1 \le \text{index} \le N$ (or apply negative index translation $N + 1 - k$).
  - For lists, walk the `+d.step` rank to the $k$-th cell.
  - For strings, invoke updated 1-based `OpcodeKind::ByteAt` or slice routine.
  - Out of bounds: emit jump to `cells_->fail`.
- **Lower Option Unwrapping (`opt!`)**:
  - Test if operand matches `Some(x)`.
  - If present: extract $x$ and continue.
  - If `None`: emit jump to `cells_->fail` (in goal-directed context) or fail opcode.

### 4.4 `apps/common/xanadu/vortex/vortex_vm.cpp`

- **Update `OpcodeKind::ByteAt` (line 1903)**: Change bounds check from 0-based to 1-based:

```cpp
const std::int64_t idx = toInt(inputs[1]);
if (idx < 1 || static_cast<std::size_t>(idx) > s.size()) {
  if (backtrack(cursor)) {
    jumped = true;
    break;
  }
  return ExecutionResult{.success = false, ...};
}
outputs.emplace_back(static_cast<std::int64_t>(
    static_cast<unsigned char>(s[static_cast<std::size_t>(idx - 1)])));
```

______________________________________________________________________

## 5. Conclusion & Verdict

By aligning VAgent with the physical geometry of the ZigZag manifold and the execution mechanics of
the Vortex VM:

- Indexing is unified to 1-based across the entire hypermedia platform (Vortex, Vicon, VQL, VPL,
  VProlog, VAgent).
- `Opt<T>` provides zero-sentinel safety with goal-directed backtracking on absent unwrap.
- The lexer cleanly segregates `?var` from infix `?`, completely eliminating the `allowScan_` parser
  hack.
- Conjunction and disjunction are strictly tiered across procedural, boolean, generator, and
  relational regimes, banishing the Prolog semicolon conflict.
- Pattern matching and destructuring are orthogonalized across lists, tuples, and records, removing
  bracket guards from declaration headers.

This specification achieves complete substrate fidelity and provides the foundation for self-hosting
the metacircular compiler.
