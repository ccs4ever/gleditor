# Round 3 critique: PL & Ergonomics Expert (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 3 MAJOR, 3 MINOR.

## Status of previous findings (Round 2)

- R2.1 (Include `stdlib/parser.vagent` in P30): RESOLVED — `stdlib/parser.vagent` is explicitly
  enumerated in Proposal P30 (§9, line 721).
- R2.2 (Clarify refutable pattern diagnostics in `let`): RESOLVED — §6.1 (lines 470–473) explicitly
  requires irrefutable patterns or an `else` block, emitting
  `E313: Refutable pattern in let binding requires else clause`.
- R2.3 (Replace pseudo-call `str(members)`): RESOLVED — Replaced with `str(members)` / idiomatic
  conversions.

______________________________________________________________________

## BLOCKING

None.

______________________________________________________________________

## MAJOR

### 1. (§7.2, lines 561–562, 575) Ambiguous PlanArm grammar, bare => Block, and where _ =>

Full title: `PlanArm` grammar permits empty arm prefixes, creates LL(1) lookahead ambiguity with
`SinglePlanBody`, and introduces invalid `where _ =>` hybrid syntax

**Problem:** Section 7.2 defines the surface grammar for multi-arm plans as follows (lines 558–562):

```ebnf
PlanDecl         ::= "plan" Ident "for" GoalPattern PlanBody
PlanBody         ::= SinglePlanBody | MultiArmPlanBody
SinglePlanBody   ::= ("where" Expr)? Block
MultiArmPlanBody ::= "{" (PlanArm ("," | "\n")*)+ "}"
PlanArm          ::= ("case" Pattern)? ("where" Expr)? "=>" Block
```

This production contains three critical ergonomic and grammatical defects:

1. **Permits bare `=> Block`:** Because both `("case" Pattern)?` and `("where" Expr)?` are optional,
   `PlanArm` permits an arm with neither a pattern nor a guard: `=> Block`. An arm beginning with
   bare `=>` is confusing, visually resembles an anonymous lambda without parameters, and damages
   syntax clarity.

1. **Grammar lookahead ambiguity with `SinglePlanBody`:** An unconditional single-arm plan has the
   form `plan P for G { Stmt* }` (where `SinglePlanBody` matches `Block`). A multi-arm plan has the
   form `plan P for G { PlanArm+ }`. When the parser encounters `{` after the goal pattern, it
   cannot distinguish a single-arm statement block from a multi-arm plan body without lookahead. If
   `PlanArm` does not require a distinguishing leading keyword, disambiguation requires arbitrary
   lookahead past expressions.

1. **Invalid hybrid syntax `where _ =>`:** Worked Example 7.2 (line 575) writes:

   ```vagent
   where _ => {
     emit DeliveryDelayed(pkg.id)
   }
   ```

   In VAgent expression grammar, `where` expects an `Expr` (a boolean condition). The wildcard `_`
   is a `WildcardPattern` (§6.1), not an `Expr`. Writing `where _` is grammatically and semantically
   invalid. The natural, idiomatic way to declare an unconditional fallback arm is an explicit
   `default => Block` clause (or `where true => Block`).

**Evidence:** From `design/vagent-metacircular-missing-features.md:562, 575`:

```ebnf
PlanArm          ::= ("case" Pattern)? ("where" Expr)? "=>" Block
```

```vagent
  where _ => {
    emit DeliveryDelayed(pkg.id)
  }
```

**Fix:** Require every plan arm to begin with an explicit discriminator keyword (`where`, `case`, or
`default`), and add `default => Block` to the grammar:

```ebnf
PlanArm    ::= GuardedArm | CaseArm | DefaultArm
GuardedArm ::= "where" Expr "=>" Block
CaseArm    ::= "case" Pattern ("where" Expr)? "=>" Block
DefaultArm ::= "default" "=>" Block
```

With this grammar:

- Every arm has an unambiguous leading token (`where`, `case`, `default`).
- After `plan Ident for GoalPattern {`, the parser has an immediate LL(1) decision: if the first
  token is `where`, `case`, or `default`, it parses a `MultiArmPlanBody`; otherwise it parses a
  standard `SinglePlanBody` statement block.
- Update Worked Example 7.2 (line 575) to replace `where _ =>` with `default =>`.

______________________________________________________________________

### 2. (§7.2, lines 584–599) Undefined matching target and scope for case Pattern in plan arms

Full title: The match target and lexical scoping of `case Pattern` in multi-arm plan declarations
are unspecified

**Problem:** Section 7.2 introduces pattern-matching plan arms:

```vagent
plan ProcessTokenPlan for ProcessToken(?tok) {
  case Token { kind: Ident, text: ?t } where is_keyword(t) => {
    emit KeywordFound(t)
  },
  case Token { kind: Ident, text: ?t } => {
    emit IdentifierFound(t)
  },
  ...
}
```

The document never defines *what value* `case Pattern` matches against:

1. In `for ProcessToken(?tok)`, the goal has a single parameter `?tok`. Does `case Token` match
   `tok` implicitly because there is only one parameter?
1. What happens if a goal has multiple parameters:
   `plan RouteCargo for DeliverPackage(?pkg, ?dest)`? What does an arm like
   `case HeavyPackage(?w) =>` match? Does it match `pkg`, `dest`, or the argument tuple
   `(?pkg, ?dest)`?
1. What happens if a goal has zero parameters (`plan Heartbeat for Ping`)?
1. How do pattern bindings interact with variable scope? In the example, `?t` is bound in the arm.
   Are pattern bindings strictly local to that specific arm, or do they leak into sibling arms or
   outer scopes?

Without specifying the matching target and variable scoping, compiler implementors and developers
face semantic ambiguity.

**Evidence:** From `design/vagent-metacircular-missing-features.md:584–599`:

```vagent
plan ProcessTokenPlan for ProcessToken(?tok) {
  case Token { kind: Ident, text: ?t } where is_keyword(t) => {
...
```

Notice that `tok` is never referenced in the `case` clause; the target is entirely implicit.

**Fix:** Add explicit specification rules to §7.2 and §7.3:

1. **Matching Target**: For an $N$-ary goal ($N \ge 1$), `case Pattern` matches against the goal's
   argument payload:
   - If $N = 1$, `Pattern` matches the single argument value directly (e.g. `case Token { ... }`
     matches the token payload).
   - If $N > 1$, `Pattern` matches the tuple of arguments `[arg1, ..., argN]` (e.g.
     `case [HeavyPackage(?w), ?dest] =>`).
   - If $N = 0$, declaring a `case Pattern` arm is a static compile error
     (`E315: Cannot match pattern on nullary goal; use where or default`).
1. **Variable Scoping**: Variables bound by `?ident` within a `case Pattern` arm are lexically
   scoped strictly to that arm's guard and body block. They are inaccessible in other arms and do
   not shadow unconditionally across the enclosing plan.

______________________________________________________________________

### 3. (§8.4, lines 674–708) Missing formal EBNF grammar for Map types, literals, and mutation

Full title: Section 8 lacks formal EBNF grammar specifications for Map types, literals, entries, and
mutation statements

**Problem:** Sections 6 and 7 provide rigorous EBNF grammar productions (§6.1, §7.2). In contrast,
Section 8 introduces `Map<K, V>`, map literals `Map { "k": v }`, subscript access, and mutation
`map[k] := v` purely through prose and informal code snippets.

Without formal EBNF grammar rules:

1. The lexer status of the token `Map` is unformalized. To guarantee zero lookahead ambiguity
   between map literals (`Map { ... }`) and record literals (`RecordName { ... }`), `Map` must be a
   reserved keyword (`TokenKind::KwMap`) rather than an ordinary identifier.
1. The entry delimiters (commas vs. newlines) and empty map literal syntax (`Map {}`) are not
   formally specified.
1. Subscript assignment (`map[key] := val`) extends the grammar of `Stmt`, but this production is
   absent.

**Evidence:** From `design/vagent-metacircular-missing-features.md:674–708`, §8.4 contains only
bullet points:

- "Map Literals: Explicit `Map` keyword distinguished from record literals..."
- "Updates & Mutation: `map[key] := new_val` updates in place or inserts." No `::=` EBNF productions
  appear anywhere in §8.

**Fix:** Add a formal EBNF subsection to §8.4:

```ebnf
MapType             ::= "Map" "<" Type "," Type ">"
MapLiteral          ::= "Map" "{" MapEntryList? "}"
MapEntryList        ::= (MapEntry ("," | "\n")*)+
MapEntry            ::= Expr ":" Expr
SubscriptAssignStmt ::= Expr "[" Expr "]" ":=" Expr
```

Explicitly state that:

- `Map` is a reserved keyword (`TokenKind::KwMap`), guaranteeing that `Map {` parses unambiguously
  as a map literal with LL(1) lookahead.
- Empty map literals `Map {}` are valid and type-inferred from context or explicit `let` annotations
  (e.g. `let m: Map<Str, Int> := Map {}`).
- Keys in `MapEntry` are arbitrary expressions evaluating to `K`.
- `SubscriptAssignStmt` updates existing keys in place or inserts fresh entries along `d.branch`,
  evaluating to unit `()`.

______________________________________________________________________

## MINOR

### 1. (§8.4, lines 688–700) Formalize tripartite subscript lookup semantics for Map

Full title: Unify Map lookup semantics with the 4-tier operator hierarchy and option unwrapping
rules

**Problem:** Section 8.4 presents `route_table[path]?` (goal-directed failure via `#Fail`) and
`route_table.get(path)` (returning `Opt<V>`), but does not define the behavior of subscript indexing
without `?` (e.g. `let x := map[k]`) or assertive subscript indexing (`map[k]!`).

In `design/vagent-language-harmonization.md` (§3.2, lines 172–181), options establish a clear
duality:

- Postfix `?` is soft / goal-directed unwrap emitting `#Fail`.
- Postfix `!` is assertive hard unwrap raising `ContractViolation`.

The document should explicitly position `Map` access within this harmonized framework.

**Evidence:** From `design/vagent-metacircular-missing-features.md:688–700`:

```vagent
let handler := route_table[path]?  // Returns handler; emits #Fail if key missing
```

**Fix:** Document the tripartite lookup matrix in §8.4:

| Construct                | Mode            | Result on Hit | Result on Miss                 | Use Case                                           |
| :----------------------- | :-------------- | :------------ | :----------------------------- | :------------------------------------------------- |
| **`map[k]?`**            | Goal-directed   | `V`           | Emits VM **`#Fail`**           | Backtracking search, alternation `\|`, plan guards |
| **`map[k]` / `map[k]!`** | Assertive       | `V`           | Raises **`ContractViolation`** | Deterministic code asserting key presence          |
| **`map.get(k)`**         | Reified algebra | `Opt<V>`      | Yields **`none`**              | Exhaustive `match` destructuring, safe inspection  |

______________________________________________________________________

### 2. (§7.3, §10.6) Developer guidance on guards vs. side effects in multi-arm plan bodies

Full title: Add developer guidance on ordering guards and side effects in multi-arm plan bodies

**Problem:** Section 7.3 specifies that if an active plan arm fails during execution, the BDI
scheduler reconsiders and advances to the next untried arm. Section 10.6 correctly refuses
cross-statement backtracking of side effects.

However, from an ergonomics and developer-experience standpoint, if a developer writes procedural
side effects (`emit`, `spawn`) in an arm before encountering a failure (e.g. a failed subgoal
`achieve FlyTo(dest)`), those external messages have already been committed to the outbox. When the
scheduler advances to the next arm (e.g. `achieve DriveTo(dest)`), the earlier emitted messages
cannot be un-sent.

**Evidence:** From `design/vagent-metacircular-missing-features.md:606–613, 770–777`.

**Fix:** Add a note in §7.3 advising developers to place fallible preconditions in the `where` guard
or at the top of the arm body before executing external side effects (`emit`, `spawn`). When
fallible subgoals must be attempted after external actions, compensatory actions should be
structured via explicit BDI failure handling (`on GoalFailed`) rather than relying on silent arm
reconsideration.

______________________________________________________________________

### 3. (§9, Table row P32) Enumerate metacircular compiler stdlib files in Proposal P32

Full title: Proposal P32 file inventory omits metacircular VAgent compiler stdlib modules

**Problem:** Proposal P32 (§9, line 723) lists C++ engine files:

```text
apps/common/xanadu/vortex/vortex_core.hpp:45–65, vortex_core.cpp, vortex_stdlib.hpp:170–190,
vortex_stdlib.cpp:2650–2695, apps/common/xanadu/vagent/token.hpp, parser.cpp:840–860,
checker.cpp:1380–1400, 1965–1985, compiler.cpp:1600–1625
```

However, metacircular self-hosting requires that `stdlib/parser.vagent`, `stdlib/checker.vagent`,
and `stdlib/emitter.vagent` also parse, typecheck, and emit `Map` literals and subscript
expressions. These files were omitted from P32's inventory.

**Evidence:** From `design/vagent-metacircular-missing-features.md:723`.

**Fix:** Add `apps/common/xanadu/vagent/stdlib/parser.vagent`, `stdlib/checker.vagent`, and
`stdlib/emitter.vagent` to Proposal P32's file list.

______________________________________________________________________

VERDICT: NOT CONVERGED (0 BLOCKING, 3 MAJOR, 3 MINOR)
