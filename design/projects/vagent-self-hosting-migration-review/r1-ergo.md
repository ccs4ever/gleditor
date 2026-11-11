# Round 1 critique: PL Design & Ergonomics Expert (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 4 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§4, line 149) Dynamic record construction and token representation paradox

**Problem:** In Phase 1 (Track 1.1, line 149), the migration plan states that `stdlib/lexer.vagent`
"Returns a cell list of Token records (`Token{kind, text, line, col}`)." Track 1.2 (line 155) lists
"Object construction: records `T{f: v}`", and Track 2.2 (line 174) lists "`Record<...>`". However,
in the existing VAgent compiler (`apps/common/xanadu/vagent/compiler.cpp:756-768`), record
instantiation `T{f: v, ...}` is statically marshaled at compile time via `marshalTermExpr` and is
restricted **strictly to static literal fields**. In `compiler.cpp:760-762`, the compiler specifies:
"The marshal is static — literals and nested terms/records only, emit's operand marshal holds the
same boundary — so the cell mints here and emit copies its ref out." Any non-literal expression in a
record field is rejected at lowering time in `apps/common/xanadu/vagent/marshal.hpp:101-112` with
`DiagnosticCode::InvalidPosition` ("host-step term argument does not lower"). Consequently, a
developer writing `stdlib/lexer.vagent` cannot dynamically instantiate
`Token{kind: k, text: t, line: l, col: c}` with variable arguments. Without dynamic record
construction or an in-manifold cell schema for tokens, the lexer cannot emit a token stream.

**Evidence:**

- `design/vagent-self-hosting-migration.md:149`: "Returns a cell list of Token records
  (`Token{kind, text, line, col}`)."
- `apps/common/xanadu/vagent/compiler.cpp:760-763`: "The marshal is static — literals and nested
  terms/records only, emit's operand marshal holds the same boundary — so the cell mints here and
  emit copies its ref out. auto marshaled = marshalTermExpr(core\_.arena(), core\_, expression);"
- `apps/common/xanadu/vagent/marshal.hpp:107-111`: "default: return std::unexpected(Diagnostic{
  DiagnosticCode::InvalidPosition, std::format("host-step term argument does not lower: kind {}",
  static_cast\<int>(expr.kind)), expr.loc});"

**Fix:** Either:

1. Extend VAgent's scalar compiler (`compiler.cpp`) to support dynamic record construction via
   `#NEW`, `#LINK`, or `#FIELD_SET` instructions at runtime, allowing arbitrary dynamic expressions
   in record field slots; or
1. Formally specify that tokens are in-manifold ZigZag cells linked on standard dimensions (e.g.,
   token cell with `+d.name` holding text, `+d.kind` link to token kind tag, `+d.vars` records for
   line/col, chained posward along `+d.step`), and define the constructor function
   `make_token(kind: Str, text: Str, line: Int, col: Int): Cell` in `std:compiler:ast` using
   `std:meta` (`make_cell`, `boot_linker`).

______________________________________________________________________

### 2. (§6, line 309) `Opt<Cell>`, sentinel zero, and goal-directed failure schism

**Problem:** Section 6 (lines 309–311) states: "3. Zero Sentinels: All missing nodes, lookup misses,
and optional branches use `Opt<Cell>` (`None` / bare cell), preserving the strict zero-sentinel rule
across both C++ and VAgent boundaries." This assertion contradicts the language semantics, the
compiler implementation, and runtime behavior:

1. The scalar compiler (`compiler.cpp`) does NOT support `none` in expressions.
   `tests/xuzz/vagent_compiler_test.cpp:321-333` explicitly asserts that `"none"` MUST be rejected
   before minting (`UnsupportedSyntaxAndNamesRefuseBeforeMinting`).
1. `checker.cpp:1794` automatically strips `Opt` via `read()` (`checker.cpp:111-115`) upon
   identifier resolution, implicitly coercing `Opt<T>` to `T` with no pattern matching (`match`),
   `if let`, or safe unwrapping operator.
1. Because `none` has no representation in `zigzag::vortex::CellValue` (strictly
   `std::variant<std::string, double, std::int64_t, bool>`),
   `apps/common/xanadu/vagent/executor.cpp:1194, 1198, 1202` encodes `std::nullopt` by returning
   `CellValue{std::int64_t{0}}`. Integer `0` is a raw sentinel, violating `brief-common.md:17-18`:
   "No sentinels outside wire/on-disk formats: APIs use std::optional or std::expected, never
   noCell."
1. In Icon and Vortex, absent values and lookup misses are communicated via *goal-directed failure*
   (`fail` / unproduced, backtracking `|`), not through container types.

**Evidence:**

- `design/vagent-self-hosting-migration.md:309-311`: "3. Zero Sentinels: All missing nodes, lookup
  misses, and optional branches use `Opt<Cell>` (`None` / bare cell), preserving the strict
  zero-sentinel rule across both C++ and VAgent boundaries."
- `tests/xuzz/vagent_compiler_test.cpp:321-329`: "TEST(VAgentCompilerTest,
  UnsupportedSyntaxAndNamesRefuseBeforeMinting) { ... for (const auto source : {..., "none", ...})
  auto statement = h.compile(source); ASSERT_FALSE(statement); }"
- `apps/common/xanadu/vagent/executor.cpp:1194`: "auto g = meta.grabOf(asCell(metaArgs[0])); resVal
  = CellValue{g ? static_cast<std::int64_t>(\*g) : std::int64_t{0}};"
- `apps/common/xanadu/vagent/checker.cpp:112-114`: "if (type.name == "Opt" && type.arguments.size()
  == 1) return type.arguments.front();"

**Fix:** Standardize the optional/failure contract:

- If `Opt<T>` is the intended surface type: add `none` and `some(v)` to `compiler.cpp`, eliminate
  the automatic unwrapping in `checker.cpp:1794`, introduce safe unwrapping syntax
  (`if some(c) := opt`), and represent `none` in the runtime without integer `0` sentinels.
- If Icon-style goal-directed failure is the architectural standard: declare reflection lookups
  (`grab_of`, `step_of`, `symbol_named`, etc.) as failing operations (`fail` on lookup miss,
  returning bare `Cell` on match) so developers write idiomatic backtracking:
  `let next := step_of(c) | return fail`. Eliminate sentinel `0` from `executor.cpp`.

______________________________________________________________________

### 3. (§4, line 144) Lexer string scanning quadratic allocation bottleneck

**Problem:** Phase 1 (Track 1.1, line 144) specifies: "Implement full tokenization using VIcon
string scanning: ... Keywords ... Operators ... Literals". In VAgent's scanning implementation
(`apps/common/xanadu/vagent/compiler.cpp:1630-1653`), the `s ? e` construct emits `ScanPush` before
evaluating `e` and immediately emits `ScanPop` upon exit. Because the scan cursor is
expression-scoped, existing VAgent code (such as
`apps/common/xanadu/vagent/stdlib/parser.vagent:37-53`) repeatedly slices the source string on every
token (`let rest := str_slice(src, pos, len); let sp := rest ? (tab(many(" \t\n\u{0d}")) | "")`).
For a 15,000–20,000 LoC self-hosting compiler codebase (~500 KB to 1 MB source text), invoking
`str_slice` at every token to establish a new `?` scanning subject results in $O(N^2)$ transient
string allocations, churning gigabytes of memory and rendering the self-hosting compiler unusably
slow.

**Evidence:**

- `design/vagent-self-hosting-migration.md:144`: "Implement full tokenization using VIcon string
  scanning"
- `apps/common/xanadu/vagent/stdlib/parser.vagent:39-41`: "let rest := str_slice(src, pos, len); let
  sp := rest ? (tab(many(" \\t\\n\\u{0d}")) | ""); pos + str_len(sp)"
- `apps/common/xanadu/vagent/compiler.cpp:1637-1644`: `pushOp` is strictly paired with `popSuccess`
  and `popFail` enclosing `rightEntry`.

**Fix:** Specify a stateful scanner API or cursor primitive for streaming tokenization: Either:

1. Allow `s ? { ... }` to scope across an entire tokenizer loop where helper methods retain access
   to the active dynamic scan cursor linked at `core_.dims().scan` off `core_.home()` without
   re-slicing strings; or
1. Introduce an explicit `ScanCursor` API (`scan_open(s: Str): Cell`,
   `scan_tab(c: Cell, n: Int): Str`, `scan_pos(c: Cell): Int`, `scan_close(c: Cell)`) that advances
   an integer index across an immutable source buffer without memory copies. Price the performance
   difference in allocations and cycles.

______________________________________________________________________

### 4. (§4, line 180) Missing error propagation, diagnostic spans, and multi-error reporting

**Problem:** Line 180 asserts: "Asserts that valid code passes with zero diagnostics and illegal
samples emit matching diagnostic codes (`TypeMismatch`, `InvalidPosition`, `UnknownName`,
`ArityMismatch`)." Line 288 adds: "yields checked annotations or diagnostics." However, the
migration document never specifies how diagnostics are represented in VAgent, how source spans are
preserved, or how errors are returned to the caller. In `apps/common/xanadu/vagent/token.hpp:64-85`,
C++ represents diagnostics with `DiagnosticCode`, `std::string detail`, and
`SourceLocation loc {file, line, column}`. In the initial VAgent prototype
(`apps/common/xanadu/vagent/stdlib/checker.vagent:85-121`), errors are returned as ad-hoc magic
strings (`"type_error"`, `"unknown_operator"`, `"reserved_descriptor_key"`) or stored in a single
scalar variable `var error_msg: Str := ""` (`checker.vagent:6`). There is no specification of:

1. What data type or cell structure represents a `Diagnostic`.
1. How source location coordinates (`line`, `col`, `span`) attached to R1 AST cells
   (`r1_ast.cpp:151-164`) are read and propagated by `checker.vagent`.
1. How multiple errors are accumulated rather than halting on the first error.
1. How the in-language compiler returns diagnostics to meta-agents or the host C++ runtime upon
   refusal.

**Evidence:**

- `design/vagent-self-hosting-migration.md:180-181`: "Asserts that valid code passes with zero
  diagnostics and illegal samples emit matching diagnostic codes (`TypeMismatch`, `InvalidPosition`,
  `UnknownName`, `ArityMismatch`)."
- `apps/common/xanadu/vagent/stdlib/checker.vagent:6`: `var error_msg: Str := ""`
- `apps/common/xanadu/vagent/stdlib/checker.vagent:100, 110`: `else "type_error"`,
  `else "unknown_operator"`
- `apps/common/xanadu/vagent/token.hpp:64-85`: C++ `Diagnostic` struct and `DiagnosticCode` enum.

**Fix:** Specify a canonical in-manifold diagnostic representation: Define a `Diagnostic` record or
cell structure (e.g. node carrying `+d.name` code, `+d.values` message, `+d.vars` line/col/span)
linked to `+d.diagnostics`. Specify that `std:compiler:checker` and `std:compiler:parser` collect a
`List<Cell>` of diagnostics, and specify how a failed compilation returns this list to the caller.

______________________________________________________________________

## MAJOR

### 1. (§3, line 103) Unspecified top-level compiler API and host integration interface

**Problem:** Section 3 enumerates 12 individual `.vagent` files under `stdlib/`, but nowhere defines
the top-level public API or entry points of the compiler. It is unclear whether an agent or runtime
tool imports a single facade `std:compiler` or must manually wire 7 disparate stages (`lexer` ->
`parser` -> `expand` -> `resolve` -> `checker` -> `emitter` -> `decls`). Furthermore, Section 4
(Phase 6, lines 238-241) states: "Replace C++ `parser.cpp`, `checker.cpp`, `compiler.cpp`,
`decls.cpp`, `expand.cpp`, `resolve.cpp` with thin shims invoking the Stage 1/2 VAgent compiler
image." The calling convention and parameter schema for these C++ shims is completely unspecified.
Today, `hostCompileFragment` (`apps/common/xanadu/vagent/host_compile.cpp:103-107`) expects
`(label, paramsCell, resultType, srcCell)` and invokes C++ compilation. If C++ invokes the VAgent
compiler image via `RunCall`, the argument registers, output result cells, error channel, and
fragment head extraction protocol must be explicitly specified.

**Evidence:**

- `design/vagent-self-hosting-migration.md:103-126`: Stdlib directory tree omitting
  `compiler.vagent` or top-level API.
- `design/vagent-self-hosting-migration.md:239-241`: "thin shims invoking the Stage 1/2 VAgent
  compiler image" without interface contract.
- `apps/common/xanadu/vagent/host_compile.cpp:103-107`: C++ interface requiring
  `(label, paramsCell, resultType, srcCell)`.

**Fix:** Specify `stdlib/compiler.vagent` (`std:compiler`) providing high-level entry points:

- `def compile_module(source: Str): Opt<Cell>`
- `def compile_fragment(label: Str, params: Cell, result_type: Str, source: Str): Opt<Code<Cell>>`
- `def compile_expression(source: Str): Opt<Code<Cell>>` Define the exact ABI and calling convention
  used by C++ shims to pass `source` via `RunCall` and retrieve the resulting fragment head cell and
  diagnostic records from `+d.vars`.

______________________________________________________________________

### 2. (§4, line 153) Grammar hallucination: unary `?` operator

**Problem:** In Phase 1 (Track 1.2, line 153), the migration document lists: "- Unary operators
(`-`, `not`, `?`)." In the VAgent grammar (`apps/common/xanadu/vagent/parser.cpp:818-828`), unary
operators are `-`, `not`, `!`, and `*`. `?` is NOT a unary operator:

1. `?` is the binary scanning operator `s ? e` (`TokenKind::Scan`, precedence 3,
   `parser.cpp:48-49`).
1. `?` is the prefix for logic variables in term expressions and belief patterns (`?name`,
   `parser.cpp:902-905, 918-920`). Introducing `?` as a unary operator in expressions creates severe
   grammar ambiguities against logic variables (`?var`) in pattern matching and against nested
   scanning expressions (`a ? ?b`). The document does not state what unary `?` would even compute.

**Evidence:**

- `design/vagent-self-hosting-migration.md:153`: "Unary operators (`-`, `not`, `?`)."
- `apps/common/xanadu/vagent/parser.cpp:820`:
  `if (word("not") || at(TokenKind::Minus) || at(TokenKind::Bang) || at(TokenKind::Star))`
- `apps/common/xanadu/vagent/parser.cpp:918-920`:
  `if (accept(TokenKind::Scan)) { const auto name = identifier();`
  `return make(ExprKind::LogicVariable, name.loc, name.text); }`

**Fix:** Remove `?` from the unary operators list in line 153 to conform with the existing VAgent
grammar, or provide a formal grammar disambiguation and semantics if a new unary operator is
intended.

______________________________________________________________________

### 3. (§4, line 158) Contradiction between R1 AST specification and canonical implementation

**Problem:** Line 158-159 states: "Builds R1 AST cells connected via `+d.step` and annotated with
`+d.name`, `+d.kind`, `+d.vars`." This statement contradicts the canonical R1 AST implementation in
`apps/common/xanadu/vagent/r1_ast.cpp:61-182`:

1. The node kind is NOT linked via `+d.kind`; the node cell's text itself IS the kind tag (e.g.
   `"binary"`, `"call"`, `"lit.int"`, `"ident"`), linked to `+d.name` for the token text
   (`r1_ast.cpp:141-143`).
1. Child AST nodes are NOT connected solely via `+d.step`. The parent links to its first child via
   `+d.grab Posward`, and child siblings chain posward along `+d.step` (`r1_ast.cpp:172-179`).
   Connecting children solely via `+d.step` from parent would violate the ZigZag 2-degree rank
   invariant whenever a node has both siblings and children.
1. Annotated types are linked along `+d.values Posward` (`r1_ast.cpp:146-149`), not `+d.vars`. If
   `stdlib/parser.vagent` implements the topology described in line 158, it will fail Validation
   Gate 1 (line 160-163) against C++ `marshalAstToR1`.

**Evidence:**

- `design/vagent-self-hosting-migration.md:158`: "Builds R1 AST cells connected via `+d.step` and
  annotated with `+d.name`, `+d.kind`, `+d.vars`."
- `apps/common/xanadu/vagent/r1_ast.cpp:141`: `const auto node = arena.makeCell(kindTag);`
- `apps/common/xanadu/vagent/r1_ast.cpp:173, 176`:
  `arena.link(node, core.dims().grab, Posward, child);` and
  `arena.link(prevChild, core.dims().step, Posward, child);`

**Fix:** Update line 158 to match `r1_ast.cpp`: Specify that each R1 AST node's cell text is its
kind tag, parent links first child via `+d.grab Posward`, siblings chain via `+d.step Posward`,
token text links via `+d.name Posward`, type annotations link via `+d.values Posward`, and source
coordinates attach via `varsRecord` on `+d.vars`.

______________________________________________________________________

### 4. (§1, line 12) Total absence of Proposal Register (`P<k>`) and cost/alternative pricing

**Problem:** The standing rules in `brief-common.md:12-13, 21-22` mandate:

- "Argued, not asserted: Every design choice or stage boundary must cite the code (`file:line`),
  price the cost in bytes/cycles/links/cells, and state what alternative was refused."
- "Proposal Register: Every change to existing code must be tracked in the numbered Proposal
  Register (`P<k>`)." `design/vagent-self-hosting-migration.md` contains NO Proposal Register (`P1`,
  `P2`, etc.). The document proposes modifying the build system, replacing C++ pipeline files with
  shims, deleting C++ source files from the `Makefile`, and adding ~12 new `.vagent` files, but
  tracks none of these in a proposal register. Furthermore, the document contains virtually zero
  code citations (`file:line`), zero pricing in bytes/cycles/links/cells, and zero refused
  alternatives.

**Evidence:**

- `brief-common.md:21-22`: "Proposal Register: Every change to existing code must be tracked in the
  numbered Proposal Register (`P<k>`)."
- `design/vagent-self-hosting-migration.md`: 0 occurrences of "Proposal" or "`P<k>`".

**Fix:** Add a dedicated Proposal Register section (`P1`–`Pn`) detailing every discrete code
modification, including `file:line` citations, cost pricing (arena cells/links and execution
cycles), and explicit alternatives refused.

______________________________________________________________________

## MINOR

### 1. (§4, line 148) Incomplete character classification and escapes in lexer specification

**Problem:** Track 1.1 (line 148) lists literal parsing with escape sequences (`\n`, `\t`,
`\u{...}`). In `apps/common/xanadu/vagent/lexer.cpp:142-220`, the lexer also supports `\"`, `\\`,
`\r`, `\0`, hex escapes `\xNN`, and multi-line strings. Furthermore,
`apps/common/xanadu/vagent/stdlib/parser.vagent:10-24` uses hand-coded ASCII strings
(`any("abcdefghijklmnopqrstuvwxyz...")`) that omit numeric base prefixes (`0x`, `0b`, `0o`) and
unicode identifiers.

**Evidence:**

- `design/vagent-self-hosting-migration.md:147-148`: lists only three escape sequences.
- `apps/common/xanadu/vagent/lexer.cpp:142-220`: full escape set including `\x`, `\"`, `\\`.

**Fix:** Specify the complete escape sequence table in Track 1.1 and provide standard character
classification helpers in `std:compiler:lexer`.

______________________________________________________________________

### 2. (§1, line 19) Signature discrepancy for `make_cell` in `prelude/meta.vagent`

**Problem:** `apps/common/xanadu/vagent/prelude/meta.vagent:109` declares:
`def make_cell(val: Cell): Cell` However, in `apps/common/xanadu/vagent/checker.cpp:1040`,
`make_cell` is typed as accepting `{any}`:
`if (name == "make_cell") return builtin({any}, named("Cell"), false);` And in
`apps/common/xanadu/vagent/executor.cpp:1080-1087`, `make_cell` accepts `int64_t`, `double`, `bool`,
and `string`. Declaring `val: Cell` in `meta.vagent` misleads developers and agent tools into
believing `make_cell` can only re-box an existing cell, whereas its primary purpose in Stage 4/5 is
boxing dynamic scalars.

**Evidence:**

- `apps/common/xanadu/vagent/prelude/meta.vagent:109`: `def make_cell(val: Cell): Cell`
- `apps/common/xanadu/vagent/checker.cpp:1040`: `builtin({any}, named("Cell"), false)`
- `apps/common/xanadu/vagent/executor.cpp:1080-1087`: `std::holds_alternative<std::int64_t>(val)`
  ...

**Fix:** Update `prelude/meta.vagent:109` to declare `def make_cell(val: any): Cell`.

______________________________________________________________________

VERDICT: NOT CONVERGED
