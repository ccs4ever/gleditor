# Round 3 critique: Codebase Expert (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 4 MAJOR, 2 MINOR.

## BLOCKING

(none)

## MAJOR

### 1. (§7.4 & P31) Lowering of plan clauses misplaced in `compiler.cpp` instead of `decls.cpp`

Full title: Substrate lowering of multi-arm plans onto `+d.clause` ranks cited in `compiler.cpp`
instead of `decls.cpp`

**Problem:** §7.4 and Proposal P31 cite `apps/common/xanadu/vagent/compiler.cpp:1498–1530` for
lowering plan declarations onto `+d.clause` ranks. Lines 1498–1530 in `compiler.cpp` implement
`literalCell`, `resolved`, and `referencesParam` for expression compilation. `compiler.cpp` does not
lower plan declarations or interact with `+d.clause` ranks at all. In the VAgent compiler
architecture, declaration lowering is handled entirely by `apps/common/xanadu/vagent/decls.cpp`.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:617`:

  > `In the ZigZag manifold (apps/common/xanadu/vagent/parser.cpp:570–600, compiler.cpp:1498–1530):`

- In `design/vagent-metacircular-missing-features.md:722` (Proposal P31):

  > `apps/common/xanadu/vagent/parser.cpp:570–615, ast.hpp, compiler.cpp:1498–1530, ...`

- In `apps/common/xanadu/vagent/compiler.cpp:1498–1530`:

  ```cpp
  CellRef literalCell(const CellValue &value) {
    const auto cell = literal(value);
    recLiteral(cell, value);
    return cell;
  }

  CellRef resolved(const Expr &expression) const {
    const auto local = localReferences_.find(&expression);
    return local != localReferences_.end()
               ? localCells_.at(local->second)
               : externalReferences_.at(&expression);
  }

  bool referencesParam(const Expr &expression) const { ... }
  ```

  There is no plan lowering or `+d.clause` rank construction in `compiler.cpp`.

- In `apps/common/xanadu/vagent/decls.cpp:1268–1301` and `1979–2000`:

  ```cpp
  // decls.cpp:1294
  const auto clause = mintClause(host, clauseName, *test, *body, decl.priority);
  if (decl.body && bodyHasHostVerb(*decl.body))
    emitSpine(decl, clause, params, clauseName);
  return clause;

  // decls.cpp:1979
  case DeclKind::Plan: {
    ...
    auto host = symbols.contains(*decl.target) ? symbols[*decl.target]
                                               : mintSymbol(*decl.target);
    ...
    lowerClauseBody(decl, host, paramsOf(decl, *decl.target), name);
    return;
  }
  ```

  `decls.cpp` is the sole engine component responsible for minting clause cells, linking guards to
  `+d.grab`, bodies to `+d.spin`, and chaining clauses onto the goal host along `+d.clause`.

**Fix:** Update §7.4 and Proposal P31 to replace `compiler.cpp:1498–1530` with
`apps/common/xanadu/vagent/decls.cpp:1268–1301, 1979–2000`.

### 2. (§7.2, §7.4, & P31) Missing `PlanArm` AST node specification in `ast.hpp` and pipeline

Full title: Omission of `PlanArm` AST structure and its integration across `ast.hpp`,
`ast_clone.cpp`, `r1_ast.cpp`, `resolve.cpp`, and `checker.cpp`

**Problem:** §7.2 introduces the surface grammar for multi-arm plans (`PlanArm ::= ("case" Pattern)?
("where" Expr)? "=>" Block`), and P31 references `ast.hpp`. However, the document specifies no AST
representation for `PlanArm`. Currently, `Decl` (`ast.hpp:208–231`) only holds single optional
pointers `ExprPtr body` and `ExprPtr guard`. Without defining `PlanArm` and updating the AST
pipeline passes, multi-arm plans cannot be parsed, cloned, resolved, typechecked, or serialized.

**Evidence:**

- In `apps/common/xanadu/vagent/ast.hpp:208–231`:

  ```cpp
  struct Decl {
    DeclKind kind{DeclKind::Event};
    std::string name;
    ...
    ExprPtr body;
    ExprPtr guard;
    ...
  };
  ```

  `Decl` has no container for multiple plan arms.

- In `apps/common/xanadu/vagent/resolve.cpp:763–782`:

  ```cpp
  case DeclKind::Plan: {
    ...
    if (decl.guard) walk(*decl.guard, env, operand, 1);
    if (decl.body) walk(*decl.body, env, {owner, true}, 1);
    return;
  }
  ```

  `Resolver::declaration` inspects only `decl.guard` and `decl.body`. If multi-arm plans leave
  `decl.body` null or store arms elsewhere, arm bodies and arm pattern bindings are skipped.

- In `apps/common/xanadu/vagent/checker.cpp:2780–2782`:

  ```cpp
  if (decl.guard) condition(*decl.guard, env, operand(context));
  if (decl.body) infer(*decl.body, env, context);
  ```

  `Checker::declaration` only checks the top-level guard and body.

- In `apps/common/xanadu/vagent/ast_clone.cpp:62–63` and `r1_ast.cpp:498–502`:
  Cloning and R1 serialization only process `decl.body` and `decl.guard`.

**Fix:** Specify the AST node representation in §7 and P31:
1. Define `struct PlanArm { ExprPtr pattern; ExprPtr guard; ExprPtr body; SourceLocation loc{}; };`
   in `apps/common/xanadu/vagent/ast.hpp`.
2. Add `std::vector<PlanArm> planArms;` to `Decl` in `ast.hpp`.
3. Update Proposal P31 files to include `apps/common/xanadu/vagent/ast.hpp`, `ast_clone.cpp`,
   `r1_ast.cpp`, `resolve.cpp:763–782`, and `checker.cpp:2747–2782`.

### 3. (§8.4 & P32) Missing `ExprKind::Map` AST kind, parser routing, and literal typechecking

Full title: Missing `ExprKind::Map` expression kind in `ast.hpp`, record collision in `parser.cpp`,
and missing literal inference in `checker.cpp`

**Problem:** §8.4 defines `Map { "k": v, ... }` literal syntax and P32 specifies exposing `Map`
literals. However:
1. `ExprKind` in `ast.hpp:38–92` has no `Map` variant.
2. In `parser.cpp:1083–1084`, the parser encounters `Map` (PascalCase) followed by `{` and
   dispatches to `Parser::record(token)`. `Parser::record()` requires bare identifier field names
   (`anyWord()`) followed by `:`, causing a parse syntax error on literal keys
   (e.g. `"result": true`).
3. In `checker.cpp`, while `Map` type arity (`707`) and subscript indexing (`1391–1394`,
   `1975–1978`) are implemented, there is no type inference for `ExprKind::Map` in `Checker::infer`
   (lines 2060–2085 alongside `ExprKind::List`).
4. In `compiler.cpp`, P32 cites lines 1600–1625 (`Unary`/`Binary`), but `ExprKind::Index` and `Map`
   literals are completely unimplemented in `emitValue` (which handles terms and blocks around lines
   1729–1755).

**Evidence:**

- In `apps/common/xanadu/vagent/ast.hpp:38–92`:
  `ExprKind` defines `List`, `Cells`, and `Record`, but no `Map`.

- In `apps/common/xanadu/vagent/parser.cpp:1083–1084`:

  ```cpp
  if (pascalCase(token.text) && allowRecord_ && at(TokenKind::OpenBrace))
    return record(token);
  ```

  `record()` expects `result->names.push_back(anyWord().text); expect(TokenKind::Colon, ...);`.
  It cannot parse quoted string keys like `"result": true`.

- In `apps/common/xanadu/vagent/checker.cpp:2060–2070`:
  `Checker::infer` implements `case ExprKind::List:`, but has no `case ExprKind::Map:`.

**Fix:**
1. Add `ExprKind::Map` to `apps/common/xanadu/vagent/ast.hpp:38–92`.
2. Add dedicated `Parser::mapLiteral()` in `apps/common/xanadu/vagent/parser.cpp:880–915` and
   intercept `token.text == "Map"` before `record()` in `parser.cpp:1083`.
3. Add `case ExprKind::Map:` in `Checker::infer` (`checker.cpp:2060–2085`) to validate uniform key
   and value types and synthesize `Type{"Map", {keyType, valType}}`.
4. Update Proposal P32 line anchors to cite `parser.cpp:880–915, 1080–1088`,
   `checker.cpp:2060–2085`, and `compiler.cpp:1730–1760`.

### 4. (§7.4) Suffix-indexed clause naming (`PlanName$1`..`$M`) breaks `MetaRuntime::planNamed`

Full title: Symbolic naming `PlanName$1`..`PlanName$M` breaks exact-name lookup in `planNamed`

**Problem:** §7.4 states that a multi-arm plan with $M$ arms compiles directly into $M$ sequential
clause cells along `+d.clause` symbolically indexed as `PlanName$1`, `PlanName$2`, ...,
`PlanName$M`. However, `MetaRuntime::planNamed(std::string_view qualified)` in
`apps/common/xanadu/vagent/meta.cpp:267–287` resolves clauses by checking
`labelOf(clause).value_or("") == qualified` verbatim. If clauses are named with `$1`..`$M`
suffixes, lookups for `PlanName` (used by BDI intention scheduling, goal dispatch,
`genesis.cpp:320`, and meta-reflection) will fail to find any matching clause.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:620–622`:

  > `A multi-arm plan with M arms compiles directly into M sequential clause cells along +d.clause`
  > `(symbolically indexed as PlanName$1, PlanName$2, ..., PlanName$M).`

- In `apps/common/xanadu/vagent/meta.cpp:275–281`:

  ```cpp
  const auto match = [&](CellRef host) -> std::optional<CellRef> {
    for (const auto symbol :
         zigzag::rankAfter(arena(), host, core_.dims().vars))
      for (const auto clause : clausesOf(symbol))
        if (labelOf(clause).value_or("") == qualified) return clause;
    return std::nullopt;
  };
  ```

  Exact string equality against `qualified` will never match `"RouteCargo"` against
  `"RouteCargo$1"`.

- In `apps/common/xanadu/vagent/genesis.cpp:320`:

  ```cpp
  const auto clause = meta.planNamed(target);
  ```

  Any patch, unlearn, or introspection targeting the surface plan name fails with
  `"the patched clause '...' no longer resolves"`.

**Fix:** Clarify the naming and lookup contract in §7.4:
Either:
1. The primary (first) arm is labelled `PlanName` while auxiliary arms are labelled
   `PlanName$2`..`$M`, allowing `planNamed(PlanName)` to resolve the entry clause.
2. Or `MetaRuntime::planNamed` is updated to fall back to matching `${qualified}$1` when an exact
   match is not found.

## MINOR

### 1. (§9, line 723 P32) Line anchor for `vortex_stdlib.cpp` omits `mapKeyCell` search routine

Full title: Proposal P32 cites `vortex_stdlib.cpp:2650–2695` but omits `mapKeyCell` (2785–2791)

**Problem:** Proposal P32 cites `apps/common/xanadu/vortex/vortex_stdlib.cpp:2650–2695`
for replacing $O(N)$ linear scans with $O(\log N)$ traversal on `d.branch`.
While lines 2650–2695 contain `createMap`, `mapSet`, `mapGet`, `mapHas`, and `mapKeys`,
the key search helper `mapKeyCell` that performs the linear search along `+d.vars` is implemented
separately at lines 2785–2791.

**Evidence:**

- In `apps/common/xanadu/vortex/vortex_stdlib.cpp:2785–2791`:

  ```cpp
  std::optional<CellRef>
  VortexStdLib::mapKeyCell(const CellRef mapRoot,
                           const std::string_view key) const {
    return firstOf(rankAfter(core_.arena(), mapRoot, core_.dims().vars) |
                   std::views::filter([&](const CellRef cell) {
                     return core_.arena().textOf(cell) == key;
                   }));
  }
  ```

**Fix:** Update Proposal P32's citation to
`apps/common/xanadu/vortex/vortex_stdlib.cpp:2650–2695, 2785–2791`.

### 2. (§8.4, line 691 & §9 P32) Ungrammatical trailing `?` in `map[k]?` subscript example

Full title: Trailing question mark in `route_table[path]?` is unsupported by parser and P32

**Problem:** In §8.4 line 691, the example writes:

```vagent
let handler := route_table[path]?  // Returns handler; emits #Fail if key missing
```

In `apps/common/xanadu/vagent/parser.cpp:844–853`, postfix indexing parses `[` followed by an
expression and `]`. There is no grammar rule or AST representation for a trailing `?` after `]`.
Furthermore, Proposal P32 states that subscripting `m[k]` inherently emits `#Fail` on a missing key
in the Vortex VM, making a trailing `?` either redundant or an ungrammatical postfix operator.

**Evidence:**

- In `apps/common/xanadu/vagent/parser.cpp:844–853`:

  ```cpp
  } else if (accept(TokenKind::OpenBracket)) {
    auto indexed = make(ExprKind::Index, base->loc);
    indexed->children.push_back(std::move(base));
    indexed->children.push_back(expression());
    expect(TokenKind::CloseBracket, "']' after index");
    base = finish(std::move(indexed));
    last = TokenKind::OpenBracket;
  }
  ```

  There is no postfix `?` token consumed or handled.

- In Proposal P32:
  `subscripting m[k] with #Fail on missing key, and .get() : Opt<V>`

**Fix:** Change `route_table[path]?` in §8.4 to `route_table[path]`, or define an explicit postfix
try/unwrap operator `?` across the grammar, AST (`Expr`), and parser.

---

## VERDICT

VERDICT: UNCONVERGED (0 BLOCKING, 4 MAJOR, 2 MINOR)
