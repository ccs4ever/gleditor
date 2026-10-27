# Round 1 critique: PL Design & Ergonomics Expert (epistemic-arbitration)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§7.1–§7.4, line 724; §4.1, line 488) Pervasive Syntax Errors in Grammar and Examples

Full title: Pervasive syntax errors in worked examples: nested predicates, `let` in query terms, and
missing `then` in conditionals

**Problem:** Every single worked example in §7 and query example in §4 violates the core grammar of
`vagent`:

1. **`predicate` nested in `agent` bodies**: In §7.1 (line 724), §7.2 (line 777), §7.3 (line 815),
   and §7.4 (line 853), `predicate` declarations are nested directly inside `agent Name { ... }`.
   Under `apps/common/xanadu/vagent/ast.hpp:453–456, 481–484` and
   `apps/common/xanadu/vagent/parser.cpp:435–436, 718–830`, `PredicateDecl` is strictly a `TopDecl`
   (module-level declaration). It is not a container `Member`. Attempting to parse `predicate`
   inside `agent` triggers `ParseErrorCode::NotAllowedHere`.
1. **`let` inside query terms**: In §1.1 (line 139), §4.1 (lines 488, 500, 513), §7.1 (line 753),
   §7.2 (line 788), §7.3 (line 827), and §7.4 (line 858), the document writes query pattern
   variables using `let`: `believes at("rover", let x, let y)`,
   `believes truest at(self, let cur_x, let cur_y)`, and
   `for believes ranked target_location(let loc)`. In `vagent`
   (`apps/common/xanadu/vagent/ast.hpp:47–50`, `parser.cpp:1004, 1361–1369`, and
   `design/vagent-agent-language.md:1295–1297, 1630`), query logic variables are unifiable logic
   variables prefixed with `?` (`?x`, `?y`, `?loc`), while `let` is strictly a statement-level
   binding keyword (`let name [: type] := expr`). Writing `let` inside a term argument fails with
   `ParseErrorCode::ExpectedExpression`.
1. **Invalid `if` syntax**: In §4.1 (lines 488, 501, 513) and §7.1 (lines 759, 792), conditions use
   `if <cond> do {` or bare `if <cond> {`. In `vagent` (`parser.cpp:1315–1328` and
   `design/vagent-agent-language.md:1293`), `if` strictly requires `then`, not `do` and not bare
   `{`. Furthermore, `if let landmark := detect_landmark(...) {` (line 759) does not exist in
   `vagent`; `Opt<T>` evaluation in goal position succeeds with unwrapped value or fails (§8.2).

**Evidence:**

- `apps/common/xanadu/vagent/parser.cpp:1319`:

  ```cpp
  expect(TokenKind::Then, "'then' after the condition");
  ```

- `apps/common/xanadu/vagent/parser.cpp:1361–1369`:

  ```cpp
  TermArg Parser::parseTermArg() {
    TermArg a;
    if (accept(TokenKind::QuestionMark)) {
      a.logicVar = expectName("a name after '?'");
      return a;
    }
    a.expr = parseExpr();
    return a;
  }
  ```

- `apps/common/xanadu/vagent/parser.cpp:718–725`: `parseMember` handles `Uses`, `Var`, `On`, `Def`,
  `Effect`, `Answer`, `Goal`, `Plan`, `Install`, `Behavior`. `Predicate` hits `default:` and calls
  `notHere(...)`.

**Fix:** Update all examples across §4 and §7 to valid `vagent` syntax:

1. Declare all predicates at top level (outside `agent` blocks), matching §4.2 and P18 §2.1.

1. Replace `let x`, `let y`, `let loc` with canonical logic variables `?x`, `?y`, `?loc` in all
   query terms (`believes truest at(self, ?cur_x, ?cur_y)`).

1. Replace all `if ... do {` and bare `if ... {` with `if ... then {`.

1. Replace `if let landmark := detect_landmark(...) {` with:

   ```vagent
   if landmark := detect_landmark("waypoint_beacon") then {
     confirm at(self, dest_x, dest_y)
   } else {
     refute at(self, dest_x, dest_y)
   }
   ```

______________________________________________________________________

### 2. (§4.1, lines 473–480) Shift-Reduce Ambiguity in BeliefQueryExpr EBNF

Full title: Lookahead ambiguity and shift-reduce conflict between EpistemicModifier and Term

**Problem:** The proposed EBNF in §4.1:

```ebnf
BeliefQueryExpr    = 'believes' [ EpistemicModifier ] GoalTerm [ 'from' Postfix ] ;
EpistemicModifier  = 'truest' [ '(' ArbitrationParams ')' ]
                   | 'ranked' [ '(' ArbitrationParams ')' ]
                   | 'consensus' [ '(' FusionParams ')' ] ;
ArbitrationParams  = ParamAssignment { ',' ParamAssignment } ;
ParamAssignment    = Ident ':' Expr ;
```

suffers from multiple grammatical and parsing defects:

1. **Lookahead / Term Collision**: When the parser encounters `believes` followed by `consensus`,
   `ranked`, or `truest`, it cannot distinguish whether `consensus(...)` is an `EpistemicModifier`
   with parameters or the predicate term itself (e.g. querying a predicate named `consensus(...)`)
   without scanning past arbitrary nested parentheses to check if a subsequent term follows.
   `vagent`'s recursive descent parser is designed for $k \le 2$ lookahead.
1. **Undefined Non-Terminals**: `GoalTerm` is not defined anywhere in the `vagent` grammar; the
   canonical production in `design/vagent-agent-language.md:1295–1297` is `Term`. `FusionParams` is
   referenced in line 477 but never defined in the EBNF.
1. **Dead Syntax in Queries**: Not a single query in the specification or examples ever uses inline
   parameters on `truest(...)` or `consensus(...)`. All queries write bare `believes truest Term`.
   Parameter blocks appear exclusively in declarative predicate policies
   (`predicate ... arbitrated by ...`).

**Evidence:**

- `design/vagent-agent-language.md:1295–1296`:

  ```ebnf
  | 'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr
  | 'believes' Term [ 'from' Postfix ]
  ```

- `apps/common/xanadu/vagent/parser.cpp:1285–1294`:

  ```cpp
  case TokenKind::Believes: {
    advance();
    EBelieves b;
    b.term = parseTerm();
  ...
  ```

**Fix:** Simplify and disambiguate query syntax. In query expressions, `EpistemicModifier` should be
a bare positional modifier without inline parentheses. If query-site parameter overrides are
desired, express them as a clean trailing clause matching `from Postfix`:

```ebnf
BeliefQueryExpr    ::= 'believes' [ EpistemicModifier ] Term [ 'from' Postfix ] [ 'by' '(' ArbitrationParams ')' ] ;
EpistemicModifier  ::= 'truest' | 'consensus' ;
ArbitrationParams  ::= ParamAssignment { ',' ParamAssignment } ;
ParamAssignment    ::= Ident ':' Expr ;
```

Under this grammar, after `believes`, if the next token is `truest` or `consensus` and `peek(1)` is
an `Identifier`, it is an `EpistemicModifier`. If followed immediately by `(`, it is the predicate
name `truest(...)` or `consensus(...)`. Parsing is strictly $k \le 2$ and unambiguous.

______________________________________________________________________

### 3. (§4.1–§4.3, §8.4) Missing EBNF and AST Productions for Core Language Features

Full title: Missing EBNF and AST definitions for `for believes ranked`, declarative policies, and
epistemic directives

**Problem:** Proposals P19.4 introduces major syntactic constructs but omits their formal EBNF
grammar productions and AST mappings:

1. **`for believes ranked`**: §4.1 (line 493) and §7.1 introduce `for believes ranked Term do Body`.
   However, the `ForExpr` production (`design/vagent-agent-language.md:1295`) is never updated in
   the EBNF. In `apps/common/xanadu/vagent/ast.hpp:186–192`, `EFor` contains `believesTerm` and
   `believesFrom`, but has no field for an epistemic modifier or strategy.
1. **Declarative Predicate Policies**: §4.2 specifies `predicate at(...) arbitrated by truest(...)`,
   but provides no EBNF production modifying `PredicateDecl`
   (`design/vagent-agent-language.md:1238`). In `ast.hpp:334–338`, `PredicateDecl` lacks any
   `policy` member.
1. **Directives `confirm` and `refute`**: §4.3 introduces `confirm Term` and `refute Term`, but
   provides no EBNF grammar under `Action` (`design/vagent-agent-language.md:1299–1301`). In
   `ast.hpp:216–226`, `EBeliefChange::Kind` has no `Confirm` or `Refute` enum tags.
1. **Omission in Proposal Register P19.4**: Lines 938–944 state what keywords to add, but do not
   define the AST additions to `EBelieves`, `EFor`, `EBeliefChange`, or `PredicateDecl`.

**Evidence:**

- `apps/common/xanadu/vagent/ast.hpp:186–192`:

  ```cpp
  struct EFor {
    std::string binder;
    ExprPtr iterable;
    std::optional<Term> believesTerm;
    ExprPtr believesFrom;
    ExprPtr body;
  };
  ```

- `apps/common/xanadu/vagent/ast.hpp:334–338`:

  ```cpp
  struct PredicateDecl {
    std::string name;
    std::vector<PredParam> params;
    SourceLocation loc{};
  };
  ```

**Fix:** Explicitly specify all modified EBNF grammar productions and AST additions in §4 and §8.4:

1. EBNF for `ForExpr`:

   ```ebnf
   ForExpr ::= 'for' ( Ident 'in' Expr | 'believes' [ 'ranked' ] Term [ 'from' Postfix ] ) 'do' Expr ;
   ```

1. EBNF for `PredicateDecl`:

   ```ebnf
   PredicateDecl     ::= 'predicate' Ident '(' [ PredParamList ] ')' [ 'arbitrated' 'by' ArbitrationPolicy ] ;
   ArbitrationPolicy ::= ( 'truest' | 'ranked' | 'consensus' ) [ '(' ArbitrationParams ')' ] ;
   ```

1. EBNF for `Action`:

   ```ebnf
   Action ::= ... | ( 'confirm' | 'refute' ) Term [ 'from' Postfix ] ;
   ```

1. Update `ast.hpp` in P19.4:

   - Add `std::optional<EpistemicModifier> modifier;` to `EBelieves`.
   - Add `bool ranked{false};` to `EFor`.
   - Add `Confirm` and `Refute` to `EBeliefChange::Kind`.
   - Add `std::optional<ArbitrationPolicy> policy;` to `PredicateDecl`.

______________________________________________________________________

## MAJOR

### 4. (§8.4, line 938) Keyword Collision with Existing `by` and Unjustified Reserved Keywords

Full title: Collision with existing keyword `by` and failure to follow contextual keyword policy for
`truest`, `ranked`, `consensus`, and `arbitrated`

**Problem:**

1. Proposal P19.4 states: "Add keywords `truest`, `ranked`, `consensus`, `confirm`, `refute`,
   `arbitrated`, `by`." However, `by` is already a reserved keyword in
   `apps/common/xanadu/vagent/token.hpp:30` (`X(By, "by")`), used in `achieve ... by ...` and
   `lo to hi by step`. Listing `by` as a new keyword reveals a lack of verification against
   `token.hpp`.

1. In `vagent`, `token.hpp:20–24` establishes the standing design rule for contextual keywords:

   ```cpp
   // Contextual words (`as`, `ensures`, `from`, `holds`, `inner`, `maintain`, `module`,
   // `occurrence`, `outer`, `parent`, `perform`, `preempts`, `priority`, `requires`,
   // `succeed`, `when`) are deliberately absent: they lex as identifiers and the parser
   // recognises them by position.
   ```

   Making `consensus`, `ranked`, and `arbitrated` hard reserved keywords needlessly breaks user code
   (e.g. declaring `predicate consensus(...)` in distributed systems, variables named `ranked`, or
   fields named `arbitrated`).

**Evidence:**

- `apps/common/xanadu/vagent/token.hpp:30`:

  ```cpp
  X(Believes, "believes") X(By, "by") X(Capacity, "capacity")
  ```

**Fix:**

1. Remove `by` from the list of newly added keywords in P19.4.
1. Add only `confirm` and `refute` to `XANADU_VAGENT_KEYWORDS` in `token.hpp`, as they are
   statement-initial action verbs (matching `believe`, `forget`, `revise`, `assert`, `retract`).
1. Classify `arbitrated`, `truest`, `ranked`, and `consensus` as positional contextual words
   (`isWord()` / `atWord()`), following `from`, `as`, and `priority`.

______________________________________________________________________

### 5. (§4.2, lines 523–540) Unsound Expression Typing in Declarative Policies

Full title: Unsound expression typing for policy model identifiers (`online_sgd`, `gaussian`, etc.)

**Problem:** In §4.2, declarative policies are written as:

```vagent
predicate at(key who: Agent, x: Float, y: Float)
  arbitrated by truest(decay: 2.5s, model: online_sgd, exploration: 0.15)

predicate temperature(key zone: Str, temp: Float)
  arbitrated by consensus(fusion: gaussian, min_confidence: 0.6)
```

The EBNF defines `ParamAssignment = Ident ':' Expr ;`. Under this rule, `online_sgd`, `gaussian`,
`beta_reputation`, and `native_skill` are parsed as general expression identifiers
(`EName{"online_sgd"}`). During static type checking (`apps/common/xanadu/vagent/checker.cpp`),
`infer()` resolves names against local and global scopes. Because `online_sgd` is neither a
variable, constant, nor declared type, the checker will reject it with
`CheckErrorCode::UnknownName: unknown identifier 'online_sgd'`. `vagent` does not have unquoted atom
literals or an `enum` declaration.

**Evidence:**

- `apps/common/xanadu/vagent/checker.cpp:557–568, 1547–1567`: No unquoted symbol or enum value type
  exists in `vagent` expressions.

**Fix:** Explicitly define valid option keys and enumerated values in the parser and AST:

```ebnf
ArbitrationParams  ::= ArbitrationParam { ',' ArbitrationParam } ;
ArbitrationParam   ::= 'decay' ':' DurationLiteral
                     | 'model' ':' EpistemicModel
                     | 'exploration' ':' FloatLiteral
                     | 'fusion' ':' FusionModel
                     | 'min_confidence' ':' FloatLiteral ;
EpistemicModel     ::= 'decay_only' | 'beta_reputation' | 'online_sgd' | 'native_skill' ;
FusionModel        ::= 'gaussian' | 'plurality' ;
```

In `ast.hpp`, parse these directly into strongly typed fields on `ArbitrationPolicy` rather than
arbitrary `ExprPtr` nodes.

______________________________________________________________________

### 6. (§8.4, lines 936, 943–945) Missing Static Diagnostic Codes in `CheckErrorCode`

Full title: Absence of static checking diagnostic codes in `CheckErrorCode` for epistemic rules

**Problem:** Proposal P19.4 promises that `checker.cpp` will "Typecheck arbitration parameters and
context vectors ($O(\text{params})$)." However, no diagnostic codes are added to `CheckErrorCode` in
`apps/common/xanadu/vagent/checker.hpp`. A production compiler requires explicit diagnostic codes
for all semantic refusal cases:

1. What error code is emitted if `arbitrated by consensus(fusion: gaussian)` is declared on a
   predicate whose non-key parameters are not `Float`? (Gaussian fusion cannot average `Str` or
   `Agent`).
1. What error code is emitted if a generator strategy `ranked` is declared on a predicate instead of
   an argmax strategy (`truest` / `consensus`)?
1. What error code is emitted if an invalid or out-of-range parameter value (e.g. negative
   exploration bonus or non-duration decay) is passed?
1. What error code is emitted if `confirm` or `refute` is executed against an unkeyed or undeclared
   predicate?

**Evidence:**

- `apps/common/xanadu/vagent/checker.hpp:27–84`: Lists all `CheckErrorCode` values; none exist for
  epistemic policies.

**Fix:** Add explicit diagnostic error codes to `CheckErrorCode` in `checker.hpp`:

```cpp
// Epistemic arbitration (§P19.4)
InvalidArbitrationStrategy,       ///< `ranked` on a predicate declaration
IncompatibleFusionType,           ///< `fusion: gaussian` on non-Float predicate arguments
UnknownArbitrationOption,         ///< unrecognized parameter key or model name
InvalidArbitrationParameterValue, ///< negative exploration bonus or zero decay
ConfirmRefuteUnkeyedPredicate,    ///< `confirm`/`refute` on a predicate without `key` parameters
```

______________________________________________________________________

### 7. (§4.3, lines 559–560) Ad-Hoc Directives `reinforce source` and `penalize source`

Full title: Ad-hoc statements `reinforce source` and `penalize source` violate language
orthogonality

**Problem:** In §4.3, the document introduces:

```vagent
reinforce source "sensor:gps", +0.1
penalize source "peer:scout_1", -0.5
```

1. Neither `reinforce`, `penalize`, nor `source` is listed in the Proposal Register P19.4 keywords
   or parser modifications.
1. The syntax is clunky and asymmetric: `penalize source "peer:scout_1", -0.5` passes a negative
   number to a verb named `penalize` (double negative).
1. In `vagent`, operational adjustments belong in standard library modules. High-level agent code
   operates on epistemic ground truth (`confirm P(...)`, `refute P(...)`). Low-level manual credit
   adjustment belongs in `std:epistemic`, not as dedicated surface syntax statements polluting the
   keyword table.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:938`: Lists keywords to add;
  omits `reinforce`, `penalize`, `source`.

**Fix:** Remove `reinforce source` and `penalize source` from surface statement syntax. Expose
manual credit nudging as procedural calls in `std:epistemic`:

```vagent
std:epistemic::reinforce("sensor:gps", 0.1)
std:epistemic::penalize("peer:scout_1", 0.5)
```

______________________________________________________________________

## MINOR

### 8. (§7.1, line 731; §7.4, line 847) Unsound Surface Types in Examples (CellRef & optional)

**Problem:**

1. Line 847 writes `def verify_scene_plausibility(image_ref: CellRef, candidate_text: Str): Float`.
   `CellRef` is an internal C++ engine type (`apps/common/xanadu/zigzag/manifold.hpp`), not a
   surface language type in `vagent` (`checker.cpp:557–568`). Exposing `CellRef` in user agent code
   violates the abstract machine model.
1. Line 731 writes `def detect_landmark(id: Str): optional<Record>`. In `vagent`, the optional type
   constructor is `Opt<T>`, not `optional<T>`. Furthermore, `Record` is an untyped keyword; records
   must name a declared record type.

**Fix:** Replace `image_ref: CellRef` with `image_ref: Str` (or an opaque capability handle).
Replace `optional<Record>` with `Opt<Landmark>` where `record Landmark { ... }` is declared.

______________________________________________________________________

### 9. (§4.1, line 514; §7.1, line 755) Non-Existent Logging and String Interpolation

**Problem:** Examples use Python-style `log.info(...)`, `log.warn(...)`, `fail("...")`, and string
interpolation `"{x}, {y}"`. None of these exist in `vagent`:

- `vagent` strings do not support interpolation `"{x}"`; strings concatenate via `++`.
- `fail` is an action keyword with no argument (`fail`), not a function taking a string message.
- Logging is handled via `emit` of diagnostic events or the `Logging` capability.

**Fix:** Adjust example code in §4 and §7 to use valid `vagent` string concatenation (`++`) and bare
`fail` or event emission.

______________________________________________________________________

### 10. (§4.1, lines 507–516; §5.3, lines 624–642) Unclear Semantics of Ground Values in `consensus`

**Problem:** `believes consensus at("drone1", ?x, ?y)` synthesizes continuous coordinates via
Gaussian fusion and binds them to `?x` and `?y`. What happens if a caller provides ground scalar
arguments instead of unbound logic variables: `believes consensus at("drone1", 10.0, 20.0)`? Does it
test exact equality against the floating-point fused coordinate (which almost always fails due to
precision)?

**Fix:** Specify in §4.1 and §5.3 that continuous arguments to `consensus` queries must be unbound
logic variables, or specify an explicit epsilon-tolerance matching rule.

______________________________________________________________________

VERDICT: NOT CONVERGED
