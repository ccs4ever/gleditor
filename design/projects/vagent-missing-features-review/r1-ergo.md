# Round 1 critique: PL & Ergonomics Expert (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 5 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§4.2, line 319) Cross-arena raw CellRef leak in Pattern B violates Ruling V1

Full title: Passing un-promoted in-arena CellRef across distributed actor boundaries violates Ruling
V1 and MeshActorRuntime invariants

**Problem:** In Pattern B (`agent CompilerCoordinator`), the compilation pipeline is divided across
independent distributed workers (`LexerWorker`, `ParserWorker`, `CheckerWorker`, `EmitterWorker`).
The sequence diagram (§4.2, lines 319–323) and coordinator code (§4.2, lines 346–365) pass in-arena
cell references (`ast_root: CellRef`, `image_root: CellRef`) directly between distinct actors via
asynchronous performatives (`tell AstConstructed(ast_root)`, `achieve ValidateAst(ast_root)`). In
`MeshActorRuntime`, each actor is instantiated with its own private, isolated `ArenaManifold`
(`apps/common/xanadu/vortex/mesh_actor.hpp:210–230`). A `CellRef` is a local integer slot index
within an actor's private arena; it is meaningless and unaddressable in any other actor's arena.
Passing raw `CellRef`s across actor boundaries directly violates Ruling V1 ("An agent is one actor;
there are no links between arenas. `MeshActorRuntime::spawn` gives each actor its own
`WorkerContext`") and causes undefined memory access or silent state corruption in the receiver.

**Evidence:** From `design/vagent-metacircular-missing-features.md:319–323`:

```text
Parse-->>Coord: tell AstConstructed(ast_root)
Coord->>Check: achieve ValidateAst(ast_root) by CheckerAgent
Check-->>Coord: tell AstValidated(ast_root)
Coord->>Emit: achieve LowerImage(ast_root) by EmitterAgent
Emit-->>Coord: tell ImageLowered(image_root)
```

From `design/vagent-agent-language.md:163–167` (Ruling V1): "V1. An agent is one actor; there are no
links between arenas. `MeshActorRuntime::spawn` gives each actor its own `WorkerContext`. An agent
names another only through a peer cell and affects another only by message. *Price:* a holon cannot
read its children's state, and every holon interaction is a message." Also
`apps/common/xanadu/vortex/mesh_actor.hpp:210–230`.

**Fix:** Reconcile Pattern B with Ruling V1 and the Spool-Mesh runtime architecture:

1. Clarify that fine-grained intra-file compiler passes (lexing, parsing into an AST manifold, type
   checking, opcode emission) **must execute synchronously within a single turn in a single agent's
   arena** (Pattern A, `behavior StagedCompiler` / compiler pipeline), where the entire ZigZag AST
   manifold (`d.grab`, `d.step`, `d.kind`) can be manipulated with zero IPC overhead and zero
   cross-arena leaks.
1. Reposition Pattern B (`agent CompilerCoordinator`) to operate at **module/file granularity**:
   independent worker actors compile whole `.vagent` files within their local arenas, seal their
   completed program images to the store via `std:zigzag:promote`, and return immutable
   `MicroversionId` strings or capability-sealed `Store` descriptors across the mesh rather than
   raw, un-promoted `CellRef` handles.

______________________________________________________________________

### 2. (§6.1, line 431) Ambiguous EBNF grammar for MatchExpr and missing arm separator

Full title: MatchExpr EBNF production lacks arm delimiters, creating shift/reduce grammar
ambiguities and conflicting with worked examples and record literals

**Problem:** The proposed surface grammar for `MatchExpr` (§6.1, lines 431–432) states:

```ebnf
MatchExpr ::= "match" Expr "{" MatchArm* "}"
MatchArm  ::= Pattern ("where" Expr)? "=>" (Expr | Block)
```

This specification suffers from two critical grammatical ambiguities:

1. `MatchArm*` contains no delimiter (such as `,`, `;`, or `NEWLINE`). Because `(Expr | Block)` can
   be an arbitrary expression, and subsequent `Pattern`s can start with unary operators (`-`, `+`)
   or identifiers, adjacent match arms cannot be partitioned deterministically. For example, in
   `0 => x` followed by `- 1 => y`, an LALR(1) or recursive descent parser greedily parses `x - 1`
   as a subtraction expression within the first arm. Furthermore, the author's own worked examples
   in §6.2 (lines 454–464) terminate every match arm with a comma `,`, which is a syntax error under
   the proposed EBNF rule.
1. Lookahead conflict with record literals: In VAgent (`apps/common/xanadu/vagent/parser.cpp:1083`),
   an identifier followed by `{` parses as a record literal (`RecordLit`) when `allowRecord_` is
   active. If `Expr` in `match Expr {` evaluates to or ends in an identifier (e.g.
   `match status {`), the parser encounters `status {` and attempts to parse a record literal,
   consuming `{` as the record body rather than the `match` expression body.

**Evidence:** From `design/vagent-metacircular-missing-features.md:431–432`:
`MatchExpr ::= "match" Expr "{" MatchArm* "}"`
`MatchArm  ::= Pattern ("where" Expr)? "=>" (Expr | Block)`

From `design/vagent-metacircular-missing-features.md:454–464`:

```vagent
AgentDecl(name, behaviors, members) =>
  "agent " ++ name ++ " { " ++ int_to_str(members.len()) ++ " members }",
PlanDecl(goal, guard, body) where guard != none =>
  ...
```

From `apps/common/xanadu/vagent/parser.cpp:431–434, 1083–1085`:
`if (pascalCase(token.text) && allowRecord_ && at(TokenKind::OpenBrace)) return record(token);`

**Fix:** Introduce mandatory or standard statement/comma delimiters in `MatchExpr`:

```ebnf
MatchExpr      ::= "match" Expr "{" [ MatchArmList ] "}"
MatchArmList   ::= MatchArm ( ( "," | Sep ) MatchArm )* [ "," ] [ Sep ]
MatchArm       ::= Pattern [ "where" Expr ] "=>" ( Expr | Block )
```

Disambiguate `Expr` from the opening brace `{`: either require a separating keyword (such as `with`,
matching ML/functional convention: `match Expr with "{" ... "}"`), or specify that the scrutinee
expression is parsed as a `headerExpression()` (`parser.cpp:431–434`), which sets
`allowRecord_ = false` and requires top-level record literals in scrutinees to be parenthesized
(e.g., `match (Point{x: 1, y: 2}) { ... }`).

______________________________________________________________________

### 3. (§6.1, line 441) VariablePattern identifier collision with nullary constructors and constants

Full title: Bare identifier VariablePattern captures nullary constructors and constants, causing
silent shadowing and unreachable match arms

**Problem:** The pattern grammar in §6.1 defines:

```ebnf
Pattern         ::= LiteralPattern | WildcardPattern | VariablePattern | CtorPattern | TuplePattern
VariablePattern ::= Ident
CtorPattern     ::= Ident "(" (Pattern ("," Pattern)*)? ")"
```

Under this definition, any bare identifier without parentheses is classified as `VariablePattern`.
Consequently, nullary constructors (e.g., `None`, `Tick`, `Empty`, `Eof`), boolean/unit tags, or
enumerated constants cannot be matched by value. For example, in:

```vagent
match result {
  Some(v) => v,
  None => 0
}
```

`None` is parsed as a `VariablePattern ::= Ident`. It matches *any* value unconditionally, binds a
new local variable named `None`, and prevents any subsequent arm from executing. Furthermore, this
directly contradicts VAgent's established pattern syntax in belief heads and VProlog/Vlog terms
(`design/vagent-agent-language.md:1206, 1390`, e.g. `on +at(?who, ?x, ?y)`), where pattern-binding
variables are explicitly prefixed with `?`.

**Evidence:** From `design/vagent-metacircular-missing-features.md:441–442`:
`VariablePattern::= Ident` `CtorPattern    ::= Ident "(" (Pattern ("," Pattern)*)? ")"`

From `design/vagent-agent-language.md:1206, 1390`: `TermArg ::= '?' Ident | Expr ;` "A belief
handler's trigger is a term whose `?` variables bind the fact's arguments; it takes no `as`."

**Fix:** Adopt an unambiguous lexical disambiguation rule for pattern variables versus constructor
tags:

- *Option A (VAgent / VProlog Family Consistency, Recommended)*: Require pattern-binding variables
  to use the `?` prefix (`VariablePattern ::= '?' Ident | "_"`), while bare identifiers `Ident`
  match nullary constructors, enum variants, and constants in scope.
- *Option B (Case-Sensitivity Convention)*: Specify that `Ident` starting with an uppercase letter
  (`PascalCase`) denotes constructors (both nullary `Ident` and parameterized `Ident(...)`), while
  `Ident` starting with a lowercase letter or underscore denotes a variable binding. Document the
  exact resolution semantics and diagnostic
  `E312: Unbound constructor or ambiguous pattern variable`.

______________________________________________________________________

## MAJOR

### 4. (§6.1, line 445) DestructureLet uses illegal = token and is omitted from statement grammar

Full title: DestructureLet introduces foreign assignment operator =, lacks integration into Stmt,
and omits refutable failure semantics

**Problem:** Section 6.1 defines: `DestructureLet ::= "let" (TuplePattern | CtorPattern) "=" Expr`
This introduces three language design defects:

1. In VAgent, variable declaration and assignment universally use `:=` (`let Ident ':=' Expr`,
   `var Ident ':' Type ':=' Expr`), never `=`. Introducing `=` violates the surface grammar of the
   entire language family (`design/vagent-agent-language.md:1344, 1351`).
1. `DestructureLet` is defined as a dangling production without being integrated into `Stmt`
   (`design/vagent-agent-language.md:1344`).
1. The specification fails to define failure semantics when destructuring a refutable pattern (e.g.,
   `let [x, y, z] := list_val` where `list_val` has 2 items). VAgent already supports `else Expr`
   fallback on statements (`EffectRecovery` in `vagent-agent-language.md:1345–1347`). Unhandled
   refutable pattern let-bindings will cause either runtime VM traps or silent corruption.

**Evidence:** From `design/vagent-metacircular-missing-features.md:445`:
`DestructureLet ::= "let" (TuplePattern | CtorPattern) "=" Expr`

From `design/vagent-agent-language.md:1344–1347`:
`Stmt ::= 'let' Ident [ ':' Type ] ':=' Expr | Expr | EffectRecovery ;`
`EffectRecovery ::= ... | 'let' Ident [ ':' Type ] ':=' CallExpr 'else' Expr ;`

**Fix:**

1. Replace `=` with `:=` and add optional type annotations and `else` fallback:
   `DestructureLet ::= "let" Pattern [ ":" Type ] ":=" Expr [ "else" Expr ]`
1. Update `Stmt` in `vagent-agent-language.md:1344`:
   `Stmt ::= 'let' ( Ident | Pattern ) [ ':' Type ] ':=' Expr [ 'else' Expr ]`
   `       | Expr | EffectRecovery ;`
1. Specify that destructuring `let` without an `else` branch is restricted to statically irrefutable
   patterns (tuples, single-variant records), and emits
   `E313: Refutable pattern in let binding requires else clause` if used with sum types.

______________________________________________________________________

### 5. (§6.2, line 472) Destructuring in for loops absent from grammar

Full title: Worked example relies on tuple destructuring in for loops, which is not supported by the
grammar and omits the mandatory do keyword

**Problem:** Section 6.2 (line 472) provides the following worked example for compiler scoping:

```vagent
def lookup_scope(stack: list[Scope], sym: string) -> Option[SymbolBinding] {
  for [scope_id, bindings, parent_id] in stack {
    ...
  }
}
```

This snippet is doubly invalid under VAgent's grammar:

1. The grammar in §6.1 only introduces `DestructureLet`; it does not amend `for` loop syntax. In
   VAgent (`vagent-agent-language.md:1368`), `for` is defined as:
   `'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr` It strictly requires a single `Ident`,
   refusing tuple or pattern destructuring.
1. The example omits the mandatory `do` keyword required by VAgent's grammar before the loop body.

**Evidence:** From `design/vagent-metacircular-missing-features.md:472`:
`for [scope_id, bindings, parent_id] in stack {`

From `design/vagent-agent-language.md:1368`: `'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr`

**Fix:** Generalize the `ForExpr` production to accept pattern destructuring:

```ebnf
ForExpr ::= "for" ( ( Ident | Pattern ) "in" Expr | "believes" Term ) "do" Expr
```

Update the worked example to include `do`:

```vagent
for [scope_id, bindings, parent_id] in stack do { ... }
```

______________________________________________________________________

### 6. (§4.2, line 346) Cognitive mismatch and invalid BDI syntax in CompilerCoordinator

Full title: CompilerCoordinator conflates reactive event handling with BDI plans and emits invalid
anonymous plan declarations

**Problem:** In §4.2 (lines 346–365), `agent CompilerCoordinator` defines:

```vagent
plan for HandleTokens(tokens, job_id) { ... }
plan for HandleAst(ast_root, job_id) { ... }
```

This is grammatically and semantically incorrect:

1. In VAgent grammar (`vagent-agent-language.md:1340`), plans require an explicit plan name:
   `PlanDecl ::= 'plan' Ident 'for' Ident [ '(' [ IdentList ] ')' ] [ 'where' Expr ] Block ;`
   Writing `plan for HandleTokens(...)` without a plan name (`plan Name for Goal(...)`) is a syntax
   error.
1. Semantically, plans in BDI are pursued to achieve goals (`achieve Goal(...)` or
   `adopt Goal(...)`). In the coordinator sequence diagram (§4.2, line 317), worker actors respond
   with asynchronous informative performatives (`tell TokensProduced(tokens)`). Receiving a `tell`
   message delivers an event; it does *not* adopt a goal! The coordinator cannot trigger a plan from
   an incoming `tell` message without either an `on` event handler or an explicit goal adoption
   step. The design confuses reactive event handlers with goal-directed planning.

**Evidence:** From `design/vagent-metacircular-missing-features.md:317, 346`:
`Lex-->>Coord: tell TokensProduced(tokens)` `plan for HandleTokens(tokens, job_id) {`

From `design/vagent-agent-language.md:1340`:
`PlanDecl ::= 'plan' Ident 'for' Ident [ '(' [ IdentList ] ')' ] [ 'where' Expr ] Block ;`

**Fix:** Rewrite `CompilerCoordinator` using proper VAgent reactive event handlers:

```vagent
on TokensProduced as e {
  spawn ParserWorker() as parser
  achieve ParseTokens(e.tokens, e.job_id) by parser
}
```

Or declare explicit goals and plans:

```vagent
goal CompilePipeline(job_id: Str, path: Str) achieve
plan StagedBuild for CompilePipeline(job_id, path) { ... }
```

______________________________________________________________________

### 7. (§2.2, §4.1, §5.1, §6.2) Pervasive type and signature divergence from VAgent surface syntax

Full title: Foreign syntax creep across stdlib definitions, types, function headers, and behavior
declarations

**Problem:** The design document uses syntax from Rust, TypeScript, or Python rather than conforming
VAgent syntax:

- Return type arrows: uses `-> Type` instead of `: Type` (`def f(...) -> T` vs `def f(...): T`).
- Unit type: uses `void` instead of `()` (`design/vagent-agent-language.md:1240`).
- Primitives: uses `string`, `bool`, `int` instead of `Str`, `Bool`, `Int`
  (`apps/common/xanadu/vagent/stdlib/ast.vagent:4–12`).
- Collections & generics: uses `list[T]`, `Option[T]`, `Result[T, E]` instead of `[T]`, `Opt<T>`,
  `Result<T, E>` (`vagent-agent-language.md:1240–1250, 1395`).
- Variable bindings: uses `var x: Type = []` instead of `var x: Type := []`.
- Internal types exposed: uses `CellRef` and `ActorPid` in surface code instead of `Cell` and
  `Agent` (`vagent-agent-language.md:1240–1248`).
- Default parameters: uses `grant: Option[ActorGrant] = none`, which is not supported by VAgent's
  parameter grammar (`vagent-agent-language.md:1392`).
- Behavior capacity misuse: `behavior StagedCompiler` specifies `uses VAgentLexer` (§4.1, line 272),
  but `Lexer` is declared as an `agent` in `stdlib/lexer.vagent:6`, not a `capacity`.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:122, 129, 137, 272, 279, 333, 386`
- `design/vagent-agent-language.md:1240–1250, 1313, 1328, 1332, 1392, 1395`

**Fix:** Standardize all module interfaces and examples to pure VAgent surface conventions:

- `def read_file(path: Str): Result<Str, IoError>`
- `def print(text: Str): ()`
- `def args(): [Str]`
- `def get_var(key: Str): Opt<Str>`
- `def compile_source_to_image(source: Str): Result<Cell, [Diagnostic]>`
- `var active_jobs: [CompileJob] := []`
- Replace `CellRef` with `Cell`, and `ActorPid` with `Agent`.
- Remove default parameter syntax and use function overloading or explicit `none` arguments.
- Declare compiler phases as modules or capacities implemented by skills rather than `uses Agent`.

______________________________________________________________________

### 8. (§7, line 503) Missing Proposal Register scope in P30 for lexer, tokens, and AST nodes

Full title: Proposal P30 omits token.hpp, lexer.cpp, and ast.hpp, preventing implementation of match
syntax and pattern AST structures

**Problem:** Proposal P30 (§7, line 503) lists:
`apps/common/xanadu/vagent/parser.cpp, checker.cpp, compiler.cpp, stdlib/parser.vagent`. However:

1. Pattern matching and match arms require the fat-arrow token `=>` (`TokenKind::FatArrow`). The
   current lexer (`apps/common/xanadu/vagent/token.hpp:15–53` and
   `apps/common/xanadu/vagent/stdlib/lexer.vagent:113–116`) does not tokenize `=>`. It treats `=`
   followed by `>` as `Assign` followed by `Greater`.
1. AST representation: `apps/common/xanadu/vagent/ast.hpp:38–70` defines `enum class ExprKind`,
   which currently has no `Match` node, nor any struct representations for `MatchArm` or `Pattern`.
   Without modifying `token.hpp`, `lexer.cpp`, and `ast.hpp`, `parser.cpp` cannot even be compiled.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:503`
- `apps/common/xanadu/vagent/token.hpp:15–53`
- `apps/common/xanadu/vagent/ast.hpp:38–70`
- `apps/common/xanadu/vagent/stdlib/lexer.vagent:113–116`

**Fix:** Expand P30's file list to include:

- `apps/common/xanadu/vagent/token.hpp` (add `TokenKind::FatArrow`)
- `apps/common/xanadu/vagent/lexer.cpp` and `stdlib/lexer.vagent` (scan `=>`)
- `apps/common/xanadu/vagent/ast.hpp` (add `ExprKind::Match`, `MatchArm`, and `Pattern` AST structs)

______________________________________________________________________

## MINOR

### 9. (§6.1, line 436) Lack of named record destructuring pattern syntax

Full title: Pattern grammar provides positional constructor matching but lacks named record
destructuring

**Problem:** VAgent records and events are declared with named fields
(`record Point { x: Float, y: Float }`). The proposed pattern grammar only provides positional
`CtorPattern ::= Ident "(" [PatternList] ")"`. It does not specify how named record fields map
positionally, nor does it provide named field matching
(`RecordPattern ::= Ident "{" [FieldPatternList] "}"`). **Evidence:**
`design/vagent-metacircular-missing-features.md:436, 442`. **Fix:** Add
`RecordPattern ::= Ident "{" [ FieldPattern ("," FieldPattern)* [","] ] "}"` where
`FieldPattern ::= Ident [ ":" Pattern ]`, or explicitly document that positional `CtorPattern`
destructures record fields in declaration order.

______________________________________________________________________

### 10. (§6.3, lines 488–490) Unspecified exhaustiveness with guards and runtime traps

Full title: Exhaustiveness algorithm does not specify treatment of guarded arms or runtime
exhaustion trap semantics

**Problem:** Section 6.3 states that `checker.vagent` checks exhaustiveness
(`E310: Non-exhaustive match`), but does not specify how arms with `where Expr` guards are treated.
In PL type theory, arbitrary guard expressions make exhaustiveness undecidable. Furthermore, if
guards fail at runtime, the document does not specify whether the VM executes `#Fail`, raises
`RuntimeError::MatchFailure`, or returns `none`. **Evidence:**
`design/vagent-metacircular-missing-features.md:488–490`. **Fix:** Specify that arms with guards
cannot satisfy exhaustiveness coverage (requiring an unguarded wildcard or fallback arm), and define
the runtime trap `RuntimeError::MatchFailure` (diagnostic `R310`) if all arms fail at runtime.

______________________________________________________________________

### 11. (§3.2, lines 199, 212) Ambiguous return type for std:zigzag:resolve_dim

Full title: std:zigzag:resolve_dim returns C++ internal DimRef rather than surface type

**Problem:** In §3.2, `resolve_dim(name: string) -> DimRef` returns `DimRef`. In VAgent surface
code, dimensions are integer IDs (`Int`). Exposing `DimRef` as an undeclared type creates surface
confusion. **Evidence:** `design/vagent-metacircular-missing-features.md:199, 212–214`. **Fix:**
Specify the return type as `Int` (or define `type DimRef = Int` in `std:zigzag`).

______________________________________________________________________

VERDICT: NOT CONVERGED
