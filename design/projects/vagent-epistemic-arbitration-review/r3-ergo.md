# Round 3 critique: PL Design & Ergonomics Expert (vagent epistemic arbitration)

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Paths below are relative to `/data/git/gleditor`. Target:
`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` v1.2. The target has
pre-existing uncommitted edits; this review did not modify it. Tally: 2 BLOCKING, 2 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

Round 2 has no ergonomics report (resource exhaustion), so these are Round 1 statuses against v1.2:

- L1.1 (examples use invalid nesting, term variables, and conditional syntax): PARTLY — top-level
  predicate declarations, `?` variables, and `then` are now used; §7.1 retains unsupported `if let`
  syntax.
- L1.2 (ambiguous/incomplete BeliefQueryExpr): PARTLY — modifiers are now positional bare words, but
  `GoalTerm` remains undefined and the base grammar's `Term` nonterminal is not used.
- L1.3 (missing EBNF/AST additions): OPEN — §4.1 sketches productions but omits the AST shape and
  does not define how `believes truest` and `for believes ranked` map onto current AST/compiler
  forms.
- L1.4 (`by` already reserved/contextual keyword policy): RESOLVED — `by` is no longer listed as a
  new keyword; the new modifier words are described as contextual.
- L1.5 (unquoted model/fusion values typecheck as unresolved names): RESOLVED — §4.2 now uses quoted
  strings and describes policy parameter typing.
- L1.6 (missing diagnostics): PARTLY — `MissingClusterTolerance`, `MissingVariance`, and
  `UnknownContextFeature` are named, but the broader invalid-policy and directive cases remain
  unspecified.
- L1.7 (`reinforce`/`penalize` ad-hoc directives): RESOLVED — these directives were removed.

## BLOCKING

### 1. (§4.1, lines 582–596) The query grammar uses an undefined `GoalTerm`

**Problem:** The only grammar shown defines `BeliefQueryExpr` and `ForBelief` in terms of
`GoalTerm`, but never defines that nonterminal or says how it relates to the existing `Term`
production. This leaves the central parse boundary unspecified: after a modifier, does the parser
consume one predicate term, a general expression, or a conjunction? In the current parser,
`believes` calls `term()` and `for believes` calls `term()` (`parser.cpp:907–911`, `889–896`). The
new `ranked` and `consensus` variants need an explicit extension of that behavior, not an undefined
symbol.

**Evidence:** Target §4.1 lines 583–589 uses `GoalTerm`; lines 595–596 only say the productions
extend `Primary` and `Action`. Base parsing requires a predicate name and argument list in
`Parser::term` (`xanadu/vagent/parser.cpp:819–832`), and existing `believes` parsing immediately
invokes it (`parser.cpp:907–911`).

**Fix:** Define the grammar against the existing nonterminal, for example
`BeliefQuery ::= 'believes' [EpistemicModifier] Term [FromClause] [UncertaintyClause]`, and define
the modifier lookahead/contextual-word rule. For `ForBelief`, specify the same modifier slot and
whether the body is a goal or expression. Add parse examples for each form and state whether
conjunction belongs outside the query.

### 2. (§7.1, lines 942–948) The remaining `if let` example is not valid `vagent`

**Problem:** The example uses `if let landmark := detect_landmark(...) then`. The implemented
grammar parses `if` followed by an expression and `then`; it does not parse a `let` declaration in
the condition. `let` is only consumed at the start of a block statement. This means the example
fails despite the earlier Round 1 syntax fix and is the only syntax shown for handling the optional
landmark result.

**Evidence:** Target §7.1 lines 942–948. `Parser::prefix` parses `if` by parsing an expression, then
expecting `then` (`xanadu/vagent/parser.cpp:871–879`); `Parser::block` handles `let` only as a
statement (`parser.cpp:652–658`).

**Fix:** Replace this with a valid explicit local binding and a condition over the bound value, once
`Opt<Str>` unwrapping is specified, or formally add `if let` to grammar, AST, checker, compiler and
proposal P19.4. Do not keep it as a worked example without specifying those additions.

## MAJOR

### 3. (§4.1; §8.4, lines 586–589, 1082–1102) New constructs have no AST or compilation contract

**Problem:** The proposal adds query modifiers, `with uncertainty`, policy declarations, weighted
verification directives, source directives, and variance-bearing assertions, but does not specify
the AST representation for them or how they participate in checking/lowering. The current AST
distinguishes `Believes` and `ForBelieves` expressions but has no modifier/policy/uncertainty
fields; top-level declarations use a general `Decl` with no arbitration policy. Implementers cannot
infer whether the additions are expression nodes, action nodes, or attributes without designing the
language anew.

**Evidence:** §4.1 lines 582–589 lists new productions; P19.4 only says to parse them and lower
`believes truest` (§8.4 lines 1092–1102). Existing AST has `ExprKind::Believes` and `ForBelieves`
(`xanadu/vagent/ast.hpp:58–64`) and `Decl` fields with no policy slot (`ast.hpp:154–177`). Current
parser handles `Predicate` declarations without a trailing policy (`parser.cpp:561–572`).

**Fix:** Add an AST contract to P19.4: typed modifier enum and optional query metadata on belief
expressions, strategy on generator expressions, typed policy on predicate declarations, and fields
for uncertainty/variance/verification weight. List checker and compiler behavior for each node,
including rejected combinations.

### 4. Worked examples still use undeclared logging/failure APIs

**Full title and location:** 4. (§7.1–§7.4, lines 939–948, 973–980, 1021–1028) Worked examples still
use undeclared logging/failure APIs

**Problem:** The examples call `log(...)` and `fail("Beacon not found...")`, but the current
`vagent` action grammar treats `fail` as a no-argument action, and no `log` builtin or capability is
declared in the examples. These are presented as complete language examples, so readers cannot
compile them as written. This is a remaining Round 1 example-conformance finding.

**Evidence:** Target §7.1 lines 939–947 and §7.2 lines 976–979. `Parser::action` returns bare
`ExprKind::Fail` without parsing an argument (`xanadu/vagent/parser.cpp:936–946`). Calls are checked
against environment bindings/capabilities; no logging capability appears in these example agents.

**Fix:** Replace `fail("...")` with the bare `fail` action and remove logging calls or declare and
implement a `Logging` capacity/skill and include it in each example. Verify examples against the
language's actual parser/checker before marking them illustrative.

## MINOR

None.

VERDICT: NOT CONVERGED
