# vagent review, round 4 - ergonomics and surface-language critic

Document: `design/vagent-agent-language.md` v3.1 (1858 lines). Line numbers are from that file.
Read: my round-3 critique, the round-3 disposition, §5.5-5.7, §6.1, §6.6-6.7, §7.1-7.2, §8 (all),
§10, §12 (all). Every example was checked by hand against the §8.9 grammar and the §8.1-8.3 rules.
SARL claims are from my own knowledge (SARL is not in the repo); the one I could not confirm is
marked.

Tally: 0 BLOCKING, 2 MAJOR, 14 MINOR.

## Part 1. Previous BLOCKING / MAJOR findings (round 3)

| R3 # | Topic                                    | Status                                                                                    |
| ---- | ---------------------------------------- | ----------------------------------------------------------------------------------------- |
| 1    | `Bool` rule rejects `sealed == false`    | RESOLVED: syntactic rule (l.882-885); `== true` now passes; §12.2 consistent              |
| 2    | reserved words as fields/params/`.`      | PARTLY: right for `.`, fields, record keys; wrong for parameters (see #2 below)           |
| 3    | belief terms and `?x` untyped            | RESOLVED: `predicate` declarations (l.954, 1061, 1027-1029); `?v` takes the argument type |
| 4    | `await` cannot read its event            | RESOLVED: `await E as e [g]`, `await t` binds (l.1011, 1127)                              |
| 5    | conditions may act; `believe`/`believes` | PARTLY: direct actions rejected (l.896-898); a called `def` bypasses it (#3)              |
| 6    | `var` without value; no `Opt`            | RESOLVED: initialiser required, `Opt<T>`, reads succeed or fail (l.902-910)               |
| 7    | scatter/gather broadcasts                | RESOLVED: per-worker `for [workers[i]]`, reply `for [t.source]`; arithmetic stated        |

## MAJOR

### 1. (§7.1 l.776, §8.7, §8.9 `Action`/`AnswerDef`, §12.4) `ask a.q(args)` has no …

Full title: 1. (§7.1 l.776, §8.7, §8.9 `Action`/`AnswerDef`, §12.4) `ask a.q(args)` has no static
type: `Agent` carries no interface

"Types are checked statically" (l.1028), but `ask warehouse.stock(item)` (l.1642) has an operand
`warehouse: Agent`, and `Agent` is only "an agent name" (l.1024). Nothing says where `stock`'s
parameter types or its `Int` result come from. `answer stock(...)` is declared inside
`agent Warehouse` (l.1625), the asker does not name `Warehouse`, and one name can be answered
differently by two agent types. So `let available := ask ... else 0` cannot be typed, `else 0`
cannot be checked against it, and the arguments cannot be checked at all. The document has solved
exactly this for beliefs (`predicate`), goals (global `goal` declarations) and events (global
`event` declarations); `ask` is the one cross-agent construct left with a bare string name. The same
hole exists for `Agent`-typed `tell` targets only in so far as terms are typed by predicates, which
is fine, so `ask` alone is the gap.

Evidence: grammar l.1126 `'ask' Postfix 'within' Postfix`; l.1070 `AnswerDef` is a `Member` only;
§7.1 says the receiver "run[s] `answer q`" with no declared interface.

Fix, in the house style of "more declarations, not conventions": add a top-level
`question stock(item: Str): Int` (same production family as `PredicateDecl`), make `AnswerDef` an
implementation of a declared question (checker: parameter and result types must match), and type
`ask a.q(args)` from the question `q`, not from `a`. A missing `answer` at the receiver is the
runtime fault the table already has (`remote_fault`). Alternative, costlier: typed agent references
`Agent<Warehouse>`. State the choice in §8.7 and add the one-line declaration to §12.4.

### 2. (§8.9 `ParamList` l.1139, §10 l.1274, §8.1 l.872-874) Parameters may be named …

Full title: 2. (§8.9 `ParamList` l.1139, §10 l.1274, §8.1 l.872-874) Parameters may be named with
reserved words, but then cannot be referenced

Round-3 #2 was fixed by making `Field`, `ParamList`, record keys and `.` take `Word`. That is right
where the name is only ever written after `.` or before `:` in a literal (event fields, record
keys), and wrong for `ParamList`, because a parameter becomes a variable and a variable is read as a
`Primary ::= Ident`. A parameter named `to`, `goal`, `plan`, `for`, `in` or `by` can be declared and
never read: `to` in an expression is the range operator, `goal` is a keyword. `ParamList` is used by
`agent`, `behavior`, `skill`, `goal`, `predicate`, `def`, `answer` and `capacity` methods (contracts
`requires speed >= 0.0` read them).

Worst case is the standard library's own signature: `Accepts` is `accepts(from: Agent, goal: Str)`
(l.1274). A user who overrides `accepts` (the only customisation point for delegation policy,
l.1659) writes `def accepts(from: Agent, goal: Str)` and has no way to say `goal` in the body. §12.3
already works around it (`from_point`, `to_point`, l.1491), which shows the authors hit it.

Fix: `ParamList ::= Ident ':' Type ...` (one word changed); keep `Word` in `Field`, in the record
literal key and after `.`. Rename the stdlib parameter to `goal_name`. Add to §8.1: "a parameter
declared with a keyword is a checker error, because it could never be read."

## MINOR

### 3. (§8.2 l.896-898, §7.1 l.783) "Conditions are pure" holds only syntactically

A condition may call a `def`, and a `def` body may contain `believe`, `emit`, `:=` (§8.3 table).
`if mark_seen(x) then ...` with `def mark_seen(x) { believe seen(x) }` is accepted, which is the bug
round-3 #5 closed for the literal spelling. The document already has the mechanism: `answer` bodies
are "read-only, transitively through every method they call" (l.783). Fix: say a method is *pure*
when its body, transitively, has no action, assignment or effect call (inferred, not declared),
conditions may call only pure methods, and reuse the checker pass for `answer`.

### 4. (§8.9 `Stmt`, `Action`, l.1097, 1017) `return` is a statement, not an expression …

Full title: 4. (§8.9 `Stmt`, `Action`, l.1097, 1017) `return` is a statement, not an expression, and
is never defined

`return` is in `Stmt` only. So the most common early exit, `if bad then return`, is a syntax error
(`then Expr`); `if bad then { return }` works. l.1017 says "`yield`, `consume`, `fail`, `return [e]`
... as §8.3", but §8.3 does not mention `fail` or `return`. Undefined: what `return` means in a
handler (succeed, keep writes?), in a plan (success of the plan, or of the statement?), and in a
`for`/`while` body. Also `fail` has no type: `if c then { emit ...; fail }` needs a `()` block
(l.893), and `let x := ask ... else fail` needs a value. Fix: move `return` to `Action`; add two
sentences to §8.3: `return e` ends the enclosing method or answer with `e`, a handler or plan
successfully (a plan's `succeed when` is still tested); `fail` and `return` have type `Never`, which
converts to any type.

### 5. (§8.1 l.843-845) Case convention is vague, and could remove the record-literal footgun

"PascalCase or snake_case as the examples show; the checker warns on a mismatch with the declared
kind" does not say which kind takes which. The examples are consistent: types, events, goals,
agents, behaviors, skills, capacities PascalCase; predicates, plans, defs, vars, params snake_case.
Make that normative. Then `Ident '{'` is a record literal iff the identifier is PascalCase, and the
whole `where ready { ... }` parenthesisation rule (l.1147-1149) disappears, along with its worst
error message ("expected block, found record literal"). The checker's case warning becomes a lexer
rule for one position only.

### 6. (§8.2 l.880-885, §8.4 l.939) `x == 1 | 2` compiles and always succeeds

Inherited from Icon and documented (l.939), but the Bool rule exists to stop silent always-true
conditions and this is the same trap in a language that is otherwise statically checked: for a
programmer from any other language `[x == 1 | 2]` means "1 or 2". Fix: a checker warning when the
right operand of `|` in a condition or guard is a literal or a name (it cannot fail), with the hint
`x == 1 | x == 2`. Cheap, and it removes the single most likely first-week bug.

### 7. (§8.9 `Primary` l.1112, §8.1 l.870-872) Contextual `parent`, `inner`, `outer` …

Full title: 7. (§8.9 `Primary` l.1112, §8.1 l.870-872) Contextual `parent`, `inner`, `outer`,
`occurrence` are also in `Primary`

They are listed as contextual ("reserved only in the position that uses them") yet each is a
`Primary` alternative beside `Ident`, and the position is every expression. So `let parent := 3` is
either legal and shadows, or illegal. Say which; I would reserve all four in expressions (they are
always read-only names, §8.8) and keep them free as field/record keys and after `.`.

### 8. (§8.9 `HandlerDef` l.1087) `as` is accepted after a belief trigger

`on +at(?who, ?x, ?y) as b { }` parses and binds nothing meaningful (the `?` variables already
bind). Restrict: `'on' ( Ident [ 'as' Ident ] | ( '+' | '-' ) Term ) [ guard ] [ priority ] Block`.
Also `priority IntLit` cannot be negative, so a behavior's handler cannot run *after* the agent's
default-0 handler for the same event; allow a signed literal.

### 9. (§7.2 l.789-792, §8.6, §12.1-12.2) The commonest send, one recipient, needs a …

Full title: 9. (§7.2 l.789-792, §8.6, §12.1-12.2) The commonest send, one recipient, needs a list:
`for [t.source]`

Replies and direct sends account for most `emit ... for r` in the examples (l.1389, 1406, 1439,
1443), and each writes a one-element list literal. Types are static, so `for r` can accept `Agent`
or `[Agent]`: `emit PartitionResult{...} for t.source`. A separate `reply E{...}` is tempting but
unnecessary. Also `source` and `space` are "absent" for host deliveries (l.481-483) but §8.8 does
not give them types; say `source: Opt<Agent>`, `space: Opt<Space>`, so the checker knows
`[t.source]` can fail.

### 10. (§8.10) SARL's default `emit` self-delivery is not in the divergence table

l.792 "The emitter does not receive its own emission." I believe SARL's default scope delivers to
every participant of the space, including the sender, which is why SARL code writes
`emit(e) [ it.UUID != ID ]` (not verifiable here; SARL is not in the repo). If so, it is a behaviour
a SARL reader relies on, and §8.10 claims fidelity row by row. Add a row either way, with the reason
(a self-emission would collide with the actor's own `remembered` entry, l.1357 (d)).

### 11. (§8.5 l.995-997, §8.9 `VarDecl`) When do `var` initialisers run, and may they act

"Member forms run before `on Initialize`: parameters, then `believe` members, `install` and
`behavior` members." `var` initialisers are not in the list, yet they may read parameters
(`var budget: Float := reserve_floor * 2.0`). Say: parameters, then `var` initialisers and member
forms in declaration order; initialisers are conditions in the §8.2 sense (pure).

### 12. (§12.1 l.1424-1425) Prose contradicts the semantics it explains

"The `if` has no `else`, so it succeeds for every partition but the last" reads as if it fails on
the last. A total `if` succeeds for every partition; its condition holds only on the last. Reword:
"the `if` is total, so the handler succeeds either way; its body runs only when the last partition
arrives".

### 13. (§8.7, §10) `Point` and `Map` are used or declared but cannot be had

`import std:math (Point)` (l.1476, 1515) and `Point{x: ..., y: ...}` (l.1501): `std:math` is
`abs, min, max, clamp, gcd, lcm, pow, sqrt` (`vortex_stdlib.hpp:15`) and §10 lists no `Point`.
Declare `record Point { x: Float, y: Float }` in `mars:hardware` and drop the import. Separately
`Map<K, V>` (l.1025) has no literal, no constructor and no function in §10, so a
`var m: Map<Str, Int> := ...` cannot be initialised under the required-initialiser rule. Either drop
`Map` (beliefs are the map) or add `Map{}` and `get`/`put` to §10. Also say that `xs[i]` out of
range fails (Icon), and that `==` on records is structural.

### 14. (§10, §12) Timers have a signature but no example, and `uses` for std capacities is unstated

The timer table row exists (l.1274) but no worked example uses `Schedules`, which is the other half
of SARL's `in`/`every`. Unspecified: is `uses Schedules` needed (SARL needs it)? What is `source` of
a timer event (self)? How does a presence test and cancel read? Add a six-line example to §12.4:
`uses Schedules`, `var tick: Opt<Timer> := none`, `tick := Schedules.every(...)` (or `periodic`),
`Schedules.cancel(tick)` (fails harmlessly when `none`), and a `on Heartbeat as h { ... h.time }`
handler. Also §8.6 should say once that an effect call is never legal inside a `def`, so a reusable
effect sequence is a goal with a plan, not a method.

### 15. (§8.1 l.857-859, §8.9) Newline rules outside blocks are unstated

`NEWLINE` appears only in `Sep`. Event and capacity bodies (`event E {` over several lines,
`capacity C { ... }`), goal clause lists and contract lists are not blocks, and their newline
treatment is unstated; only "a record literal's `{ }`" is named whitespace. Say: "outside a `Block`,
newlines are whitespace", which the declaration grammar can bear because every declaration starts
with a keyword.

### 16. (§8.9 `Primary`/`Term`, §8.2) Minor consistency

`_` as a `TermArg` is now an `Ident` expression, so the checker must special-case it in term
position; say so. `await E` without `as` (an event with no needed fields) is a syntax error; allow
`as` to be omitted. `Opt<Bool>` read in a condition succeeds with `false` when present; extend the
Bool rule to `Opt<Bool>` with the same message.

## Verdict basis

Round 3's contradictions are closed: the Bool rule, typed beliefs, `await as`, pure conditions in
their direct form, `Opt<T>`, and the scatter/gather example all now check against the grammar, and
every worked example parses. What is left is one real static-typing gap (`ask` has no declared
signature, #1) and one over-reach of the round-3 keyword fix (#2), each a one-paragraph change, plus
polish. With those two fixed from the ergonomics side nothing blocks.

VERDICT: NOT CONVERGED
