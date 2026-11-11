# Round 1 critique: Technical Writer (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 3 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§2.2, line 119) VAgent Code Examples Fail to Parse Under Live Grammar

Full title: VAgent Code Examples Across the Document Systematically Fail to Parse Under Live Grammar

**Problem:** Every VAgent code snippet in the document contains widespread syntactic non-conformance
with the live parser (`apps/common/xanadu/vagent/parser.cpp`), failing to parse:

1. Module declarations write `module std:io { ... }` with braces; VAgent grammar declares modules at
   file-scope without braces (`module std:io` or `module std:compiler:lexer`, `parser.cpp:373–377`,
   `ast.vagent:1`).
1. Method signatures write `def name(...) -> Type`; VAgent uses colon `: Type` after parameter
   parentheses (`parser.cpp:449`), not `->`.
1. Variable declarations and statements use `=` (`let x = e`, `var x = e`, and §6.1's
   `DestructureLet`); VAgent strictly uses `:=` for local and field assignment (`parser.cpp:616`,
   `parser.cpp:746`), reserving `=` exclusively for comparison (`TokenKind::Equal`).
1. In §4.2, `achieve Goal(...) by worker` omits `within <deadline>`. The parser enforces
   `expectWord("within")` whenever `by` is used (`parser.cpp:1138`), making every snippet in §4.2 an
   immediate parse error.
1. In §4.2, `spawn Worker() as worker` uses an invalid `as` clause; `spawn` only supports
   `in <space>` (`parser.cpp:1134`), and binding must be `let worker := spawn Worker()`.
1. In §4.2, `plan for Goal(...)` omits the mandatory plan identifier; `parser.cpp:563–565` requires
   `plan <PlanName> for <GoalName>(<params>)`.
1. In §6.2, `for [scope_id, bindings, parent_id] in stack {` omits the mandatory `do` token before
   the body (`parser.cpp:1024`), and uses tuple destructuring where the parser only permits a single
   identifier (`parser.cpp:1021`).
1. Default parameters `make_cell(text: string = "")` (§3.2) and `grant: Option[ActorGrant] = none`
   (§5.1) are invalid; `parser.cpp:406–428` does not support default parameter initializers.

**Evidence:** From `design/vagent-metacircular-missing-features.md:119–122`:

```vagent
module std:io {
  def read_file(path: string) -> Result[string, IoError]
```

From `design/vagent-metacircular-missing-features.md:342–346`:

```vagent
spawn LexerWorker() as lexer
achieve TokenizeSource(source_res.unwrap(), job_id) by lexer
```

```vagent
plan for HandleTokens(tokens, job_id) {
```

From `apps/common/xanadu/vagent/parser.cpp:449`:

```cpp
if (accept(TokenKind::Colon)) declaration.result = type();
```

From `apps/common/xanadu/vagent/parser.cpp:616`:

```cpp
expect(TokenKind::Assign, "':=' and an initial field value");
```

From `apps/common/xanadu/vagent/parser.cpp:746`:

```cpp
expect(TokenKind::Assign, "':=' after a local declaration");
```

From `apps/common/xanadu/vagent/parser.cpp:1138`:

```cpp
if (acceptWord("by")) {
  result->recipients = operand();
  expectWord("within");
  result->deadline = operand();
}
```

From `apps/common/xanadu/vagent/parser.cpp:564–565`:

```cpp
result.name = identifier().text;
expectWord("for");
result.target = identifier().text;
```

From `apps/common/xanadu/vagent/parser.cpp:1024`:

```cpp
expectWord("do");
```

**Fix:** Rewrite all VAgent code listings to strictly adhere to the real VAgent grammar:

1. Separate modules by file, using `module std:io` without enclosing braces.
1. Replace all `-> RetType` with `: RetType`.
1. Replace all `let x = ...` and `var x = ...` with `let x := ...` and `var x := ...`.
1. Add `within <duration>` to all `achieve ... by ...` calls (e.g., `within 5s`).
1. Replace `spawn ... as x` with `let x := spawn ...`.
1. Add plan names to `plan` declarations: `plan HandleTokensPlan for HandleTokens(tokens, job_id)`.
1. Add `do` to `for` loops, e.g. `for item in stack do { ... }`.
1. Remove default arguments from parameter signatures in §3.2 and §5.1, or specify an explicit
   grammar extension in Proposal P30 for default parameters.

______________________________________________________________________

### 2. (§6.1, line 430) Ambiguous Match Grammar and Missing Lexer Accounting in P30

Full title: Pattern Matching Grammar is Ambiguous, Lacks Arm Delimiters, and Omits Lexer Changes
from P30

**Problem:** The proposed EBNF grammar in §6.1 exhibits multiple ambiguities and specification gaps:

1. **Ambiguous `MatchExpr` Header**: `MatchExpr ::= "match" Expr "{" MatchArm* "}"`. In VAgent, an
   `Expr` can be a record literal (`Point { x: 1.0, y: 2.0 }`) or a block expression. Without a
   disambiguating keyword (such as `with` or `do`, following `if ... then` and `while ... do`), `{`
   is ambiguous between continuing a record literal and opening the match arm block.
1. **Missing Arm Delimiters**: The production `MatchArm*` has no separator (commas, semicolons, or
   newlines). In the worked example (§6.2), arms are comma-separated (`AgentDecl(...) => ...,`).
   Without explicit delimiters in the EBNF, arm boundaries cannot be parsed deterministically.
1. **Lexer and Token Header Omission**: `=>` is not in `TokenKind`
   (`apps/common/xanadu/vagent/token.hpp:15–53`) and is not tokenized in `lexer.cpp`. Proposal P30
   (§7) lists `parser.cpp, checker.cpp, compiler.cpp, stdlib/parser.vagent`, but omits `token.hpp`,
   `lexer.cpp`, and `stdlib/lexer.vagent`.
1. **`none` Missing from `LiteralPattern`**: `LiteralPattern` lists
   `IntLiteral | FloatLiteral | StringLiteral | BoolLiteral`. It omits `none` (`ExprKind::None`),
   making it impossible to match `none` literally.
1. **Assignment Token Contradiction in `DestructureLet`**: `"let" ... "=" Expr` uses `=` instead of
   VAgent's mandatory `:=`.

**Evidence:** From `design/vagent-metacircular-missing-features.md:430–446`:

```ebnf
MatchExpr      ::= "match" Expr "{" MatchArm* "}"
MatchArm       ::= Pattern ("where" Expr)? "=>" (Expr | Block)
LiteralPattern ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral
DestructureLet ::= "let" (TuplePattern | CtorPattern) "=" Expr
```

From `apps/common/xanadu/vagent/token.hpp:15–53`: `TokenKind` contains `Assign`, `Equal`, etc., but
no `FatArrow` (`=>`).

From `apps/common/xanadu/vagent/lexer.cpp:290–320`: No lexical rule for `=>`.

**Fix:**

1. Update `MatchExpr` to require a delimiter keyword, e.g.,
   `MatchExpr ::= "match" Expr "with" "{" MatchArmList "}"`.
1. Define `MatchArmList ::= (MatchArm ("," | ";")?)*` (or newline-delimited).
1. Include `NoneLiteral` in
   `LiteralPattern ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral | "none"`.
1. Change `DestructureLet` to `"let" (TuplePattern | CtorPattern) ":=" Expr`.
1. Update Proposal P30 in §7 to include `apps/common/xanadu/vagent/token.hpp`, `lexer.cpp`, and
   `apps/common/xanadu/vagent/stdlib/lexer.vagent`.

______________________________________________________________________

## MAJOR

### 1. (§2.2, line 122) Type Syntax, Names, and Fictitious Library APIs

Full title: Type Syntax, Primitive Type Names, and Standard Library Methods Disagree with Core
VAgent

**Problem:** The document uses type syntax and names from TypeScript/Rust rather than VAgent:

1. **Primitive Type Names**: VAgent primitives (`checker.cpp:47–54`) are `Str` (not `string`),
   `Int`, `Float`, `Bool`, and `()` (not `void`).
1. **Generic Parameter Syntax**: In VAgent, generic type arguments use angle brackets `<T>`
   (`parser.cpp:398–402`). Square brackets `[T]` are strictly for list types (`parser.cpp:388–391`).
   `Result[string, IoError]`, `Option[string]`, and `list[string]` are invalid; they should be
   `Result<Str, IoError>`, `Option<Str>`, and `[Str]`.
1. **Fictitious APIs in §6.2**: The worked example in §6.2 invokes `members.len()`,
   `int_to_str(...)`, `bindings.contains(sym)`, and `bindings.get(sym)`. None of these methods exist
   in VAgent or its standard library (`apps/common/xanadu/vagent/stdlib/*.vagent`). In particular,
   `resolve.vagent` implements lexical scope as fixed slots (`s1`–`s8`), not a Map object.

**Evidence:** From `design/vagent-metacircular-missing-features.md:122`:

```vagent
def read_file(path: string) -> Result[string, IoError]
def print(text: string) -> void
def args() -> list[string]
```

From `apps/common/xanadu/vagent/checker.cpp:47–54`:

```cpp
type.name == "Int" || type.name == "Float" || type.name == "Str" ||
type.name == "Bool" || type.name == "()"
```

From `apps/common/xanadu/vagent/parser.cpp:388–402`:

```cpp
if (accept(TokenKind::OpenBracket)) {
  Type result{"List", {type()}};
  expect(TokenKind::CloseBracket, "']' after a list type");
  return result;
}
```

```cpp
if (accept(TokenKind::Less)) {
  result.arguments.push_back(type());
  expect(TokenKind::Greater, "'>' after type arguments");
}
```

**Fix:** Standardize all type annotations across §2.2, §3.2, §4.1, §5.1, and §6.2 to valid VAgent
types: `Str`, `[Str]`, `Option<Str>`, `Result<Str, IoError>`, and `()`. Replace fictitious Map and
string methods in §6.2 with valid operations or declare their interface in a helper module.

______________________________________________________________________

### 2. (§3.2, line 200) Promotion Mechanism Missing Store Context and Violates Rule 3

Full title: `std:zigzag:promote` Lacks Store Context Specification and Returns Bare Unchecked String

**Problem:**

1. **Missing Store Context**: In C++, `zigzag::promote` (`arena_manifold.hpp:930–934`) requires a
   mutable reference to `xanadu::Store &store` and a `parent` microversion ID. In `VortexStdLib`
   (`vortex_stdlib.hpp:505`), `boundStore_` is a `const xanadu::Store *`. In distributed or
   sandboxed `WorkerContext` execution, an arena may have no attached store (or only a folded
   read-only base). The specification does not explain how `std:zigzag:promote` acquires the target
   `Store` or determines the parent microversion when called inside `VortexStdLib`.
1. **Violation of House Style Rule 3**: `zigzag::promote` can fail if the promotion budget is
   exceeded or if `root` is not a cell of the arena. Specifying `promote(root: CellRef) -> string`
   returning a bare hex string without an error wrapper violates House Style Rule 3 ("No Sentinels
   in APIs: Absent values use Option[T] or Result[T, E]").

**Evidence:** From `design/vagent-metacircular-missing-features.md:215`:

```vagent
promote(root: CellRef) -> string
```

From `apps/common/xanadu/zigzag/arena_manifold.hpp:930–934`:

```cpp
[[nodiscard]] std::optional<Promoted>
promote(xanadu::Store &store, const xanadu::MicroversionId &parent,
        const ArenaManifold &from, CellRef root, PromotionBudget budget = {},
        std::span<const CellRef> extraRoots = {});
```

From `brief-common.md:25`:

> "3. No Sentinels in APIs: Absent values use Option[T] or Result[T, E], never noCell or -1 in
> surface APIs."

**Fix:**

1. Define the signature of `promote` in `std:zigzag` as:
   `def promote(root: Cell, budget: Option[Int] = none): Result<Str, PromoteError>`
1. Clarify in §3.2 how `VortexStdLib` retrieves the active store and parent microversion (e.g., via
   the execution context's lineage store pin in `WorkerContext`).

______________________________________________________________________

### 3. (§4.2, line 306) Divergence Between Sequence Diagram Performatives and Coordinator Code

Full title: Swarm Sequence Diagram Performatives Inconsistent with Coordinator BDI Implementation

**Problem:** In §4.2, the sequence diagram and the `CompilerCoordinator` code listing contradict
each other:

1. The sequence diagram depicts workers replying with `tell TokensProduced(tokens)`,
   `tell AstConstructed(ast_root)`, etc.
1. In VAgent, `tell` asserts a relational belief in the recipient's mind. To react to a belief
   assertion or percept, an agent uses `on <Event>` or `for believes <Pattern>`.
1. The `CompilerCoordinator` listing defines plans for goals (`plan HandleTokens for ...`), but
   neither defines goal triggers nor declares event handlers matching `TokensProduced`,
   `AstConstructed`, etc.
1. An implementer cannot determine whether coordination is achieved via event dispatch (`emit` /
   `on`), belief revision (`tell` / `for believes`), or sub-goal delegation (`achieve`).

**Evidence:** From sequence diagram (`lines 317–318`):

```text
Coord->>Lex: achieve TokenizeSource(src) by LexerAgent
Lex-->>Coord: tell TokensProduced(tokens)
```

From coordinator listing (`lines 346–349`):

```vagent
plan for HandleTokens(tokens, job_id) {
  spawn ParserWorker() as parser
  achieve ParseTokens(tokens, job_id) by parser
}
```

**Fix:** Reconcile the sequence diagram and the code listing:

- Either change worker replies in the diagram to events (`emit TokensProduced(tokens, job_id)`), and
  implement `on TokensProduced as evt { ... }` in the coordinator,
- Or use `tell` in the diagram and `for believes TokensProduced(tokens, job_id) do { ... }` in the
  agent.

______________________________________________________________________

## MINOR

### 1. (§2.1, line 69) Citation Accuracy and Header Line Anchors

Full title: Correct Source File Line Anchors, Struct Field Names, and Proposal P26 Description

**Problem:**

1. Line 69 cites `user_permascroll.hpp:25–65` for `UserPermascroll`. Lines 25–65 define
   `DeviceDelegation`; `class UserPermascroll` begins at line 98 (definition at line 101).
1. Line 73 cites `segmented_ops_spool.hpp:42–88`. Lines 42–88 are constants and comments;
   `class SegmentedOpsSpool` is defined at line 220.
1. Line 411 claims `ActorPid` has fields `{node, name, epoch, sequence}`. In
   `identity_layout.hpp:304–309`, the actual fields are `{node, cursor, epoch, spawnSequence}`.
1. Proposal P26 table entry (line 499) omits `std:env:get_var`, which is specified in §2.2.

**Evidence:** From `apps/common/xanadu/user_permascroll.hpp:98–101`: `class UserPermascroll`.

From `apps/common/xanadu/segmented_ops_spool.hpp:220`: `class SegmentedOpsSpool`.

From `apps/common/xanadu/identity/identity_layout.hpp:304–309`:

```cpp
struct ActorPid {
  std::string node;
  std::string cursor;
  std::uint64_t epoch{};
  std::uint64_t spawnSequence{};
};
```

**Fix:** Update citations to `user_permascroll.hpp:98–290` and `segmented_ops_spool.hpp:216–280`.
Correct `ActorPid` field list to `{node, cursor, epoch, spawnSequence}`. Add `std:env:get_var` to
P26 description.

______________________________________________________________________

### 2. (§1, line 26) Missing Explicit Section Cross-References

Full title: Add Explicit Section Cross-References Between Summary, Proposal Register, and
Specification Body

**Problem:** The document contains no internal `§X.Y` section cross-references. §1 lists the 5
capability gaps without citing §§2–6, and §7 lists Proposals P26–P30 without citing the sections
that specify them.

**Evidence:** Grepping for `§` in `design/vagent-metacircular-missing-features.md` returns zero
results.

**Fix:** Add explicit cross-references in §1 (e.g. "1. Epistemic Ingestion (§2)...") and in the
Proposal Register table in §7 linking each proposal to its corresponding specification section
(§2.2, §3.2, §4, §5, §6).

______________________________________________________________________

VERDICT: NOT CONVERGED
