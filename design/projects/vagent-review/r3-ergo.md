# vagent review, round 3 - ergonomics and surface-language critic

Document: `design/vagent-agent-language.md` v3.0 (1600 lines). Line numbers are from that file.
Read: my round-2 critique, the round-2 disposition, §3, §5.5, §6.1, §6.6-6.7, §7.2, §8 (all), §10,
§12. SARL claims are from my own knowledge (SARL is not in the repo).

Tally: 0 BLOCKING, 7 MAJOR, 9 MINOR.

## Part 1. Previous BLOCKING / MAJOR findings

| R2 # | Topic                                           | Status                                                                    |
| ---- | ----------------------------------------------- | ------------------------------------------------------------------------- |
| 1    | `if` without `else` rolls the handler back      | RESOLVED: total `if` (l.787-789); §12.1 prose now correct                 |
| 2    | `import std:agent` unparseable                  | RESOLVED: contextual keywords, `Word` in `ModulePath` (l.762-768, l.936)  |
| 3    | block after `where`/`Contract` collides         | RESOLVED: parenthesise record literals there (l.1021-1024)                |
| 4    | ThrusterNav violated position rules             | RESOLVED: now `native skill` (l.857)                                      |
| 5    | belief triggers lose arguments; `believes_from` | RESOLVED: `Trigger ::= + Term`, `believes t from s`; but see #3 (types)   |
| 6    | timers have no signature                        | RESOLVED: §10 table; but see #6 (`var` without value)                     |
| 7    | `Bool` footgun, `not a == b`                    | PARTLY: `not` fixed; the Bool rule contradicts the comparison result (#1) |
| 8    | V11 vs vicon spellings                          | RESOLVED: V14 reworded, §8.11 rows added                                  |
| 9    | chained comparison contradiction                | RESOLVED: non-chaining, stated in §8.2                                    |

## MAJOR

### 1. (§8.2 l.777-779, l.784; §12.2 l.1330) The `Bool` rule cannot be applied to its …

Full title: 1. (§8.2 l.777-779, l.784; §12.2 l.1330) The `Bool` rule cannot be applied to its own
comparisons

Evidence: "`a == b` produces `b` or fails" and "A `Bool`-typed expression is rejected as a
condition, a guard, or an operand of `&`". `sealed == false` produces `false`, so its static type is
`Bool`, and it is an operand of `&` in the §12.2 guard `a.level > alert_level & sealed == false`
(l.1330). The checker as specified rejects the document's own example, and the suggested rewrite
"compare with `== true`" (l.778) is itself `Bool`-typed, so no spelling passes. Same for
`flag == true` anywhere.

Fix: say the rule applies to the expression's form, not to the value type: a comparison, a
`believes`, a call to a `def` declared `: ()`... is a "test"; a bare name, field, literal or call
whose type is `Bool` in test position is the error. Simplest wording: "a comparison has static type
`Test`, which has no values and is accepted wherever a condition is; the value it produces is
available only as `let v := (a == b)`, where its type is the right operand's." Then `== true` works.

### 2. (§8.1 reserved list, §8.9 `Postfix`/`Field`, §10 l.1152-1153) The standard …

Full title: 2. (§8.1 reserved list, §8.9 `Postfix`/`Field`, §10 l.1152-1153) The standard library
declares fields and parameters named with reserved words

`GoalAchieved{intention, goal}`, `GoalFailed{intention, goal, cause}`,
`PlanFailed{intention, plan, cause}`, `DeliveryFailed{to, kind, cause}` and
`accepts(from: Agent, goal: Str)` use `goal`, `plan` and `to`, all in the reserved-everywhere list
(l.756-760). `Postfix ::= ... '.' Ident` and `Field ::= Ident ':' Type` take `Ident`, so a handler
cannot write `e.goal`, `f.plan` or `d.to`, and the stdlib events cannot be declared in vagent
syntax. Round 2 #2 fixed module paths but not the other positions where a name follows a delimiter.

Fix: after `.`, in `Field`, in a record literal key and in `ParamList` accept `Word` (identifier or
keyword), as `ModulePath` does; the position, not the spelling, decides. Or rename (`goal_name`,
`plan_name`, `recipient`) in §10. The first costs one grammar line and also lets users name a field
`from` or `to` for a route event, which SARL users will.

### 3. (§8.7 l.911, §8.5 l.873, §6.1) Static typing stops at the term: belief arguments …

Full title: 3. (§8.7 l.911, §8.5 l.873, §6.1) Static typing stops at the term: belief arguments and
`?x` bindings have no type

"Types are checked statically; locals are inferred." But `on +at(?who, ?x, ?y)` binds `x` and `y`,
and the example then writes `emit Moved{x: x, y: y}` (l.873), `findall(?h, hit(...))` is assigned to
a `[Str]` event field (l.1274), and `let route := ...` uses `here`/`level` from
`believes position(?here) & believes battery(?level)` as `Point`/`Float` arguments (l.1394-1397).
Nowhere does the document say what type a `?v` has: no predicate declaration, no dynamic `Value`
type, no rule that the first use fixes it. A predicate is created by `believe` alone (grammar l.949,
`BeliefDecl` is a term), so `at/3` has no signature.

Fix (choose one, state it in §8.7): (a) a `predicate at(who: Agent, x: Float, y: Float)` top-level
declaration (same production as `RecordDecl`), checked at every `believe`/`believes`/`tell`/
`findall`/`Trigger`; it is also the natural place for a user to document a fact schema, and matches
the house preference for explicit structure over convention. Or (b) declare that term arguments are
the dynamic type `Value`, that a `?v` has type `Value`, and that using one where a static type is
required needs a cast `x as Float` that fails on mismatch. (a) is better; (b) is cheaper but then
"types are checked statically" must be qualified.

### 4. (§6.6 table l.640-642, §8.6 l.896, grammar `await`) `await E [g]` cannot read the …

Full title: 4. (§6.6 table l.640-642, §8.6 l.896, grammar `await`) `await E [g]` cannot read the
event it waited for, and its guard has no bound name

`await E [guard] within d` has value `()` (l.896). A plan that waits for `Acknowledgement` needs its
fields. The guard refers to what? `occurrence` is the enclosing handler's event (§8.8, and a plan
has none), and the grammar has no `as` on `await` although handlers have one. The same scoping
question exists for `consume` in a plan.

Fix: `await E as a [guard] within d [else e]` with value `a`'s event of type `E`; guard sees `a`.
For terms, `await t within d` should return the fact's bindings the same way
(`await at(?x, ?y) within d` binds `x`, `y` for the rest of the block, per the rule at l.1027). Add
`as` to the grammar and a sentence to §8.8.

### 5. (§8.2 l.789-790, §8.3 table l.808-810, §8.1 l.752-755) Conditions may have …

Full title: 5. (§8.2 l.789-790, §8.3 table l.808-810, §8.1 l.752-755) Conditions may have effects,
and `believe`/`believes` is a one-letter, opposite-effect pair

The table says `believe`, `emit`, `forget`, `spawn`, `adopt`... are allowed "in any position in
handlers, plans and methods". Guards, `where`, `succeed when` and `holds` are "bounded" (l.789-790)
but nothing says they are pure. So `if believe door(open) then ...` and `[forget p(_)]` compile: the
typo `believe` for `believes` asserts the fact, succeeds, and the branch runs. `believes` is the
language's most-written test and `believe` its most-written action; Jason avoids this with `+` and
`?`/`.`-less test syntax, SARL has no analogue.

Fix: (a) the checker rejects every action of §8.6 (and `:=`, `<-`, effect calls) inside a guard,
`where`, goal clause, `Contract` expression and the condition of `if`/`while`; say it in §8.3. (b)
Consider a visibly different test spelling (`holds`-style: `know`, or `believes?`) - cheap now. (a)
alone removes the dangerous case; guards that act are also an event-ordering hazard (a guard
evaluated for a handler that is then skipped by priority still ran).

### 6. (§8.5 `VarDecl`, §8.7, §10 `Schedules`) `var x: T` without an initialiser has no …

Full title: 6. (§8.5 `VarDecl`, §8.7, §10 `Schedules`) `var x: T` without an initialiser has no
meaning and the type system has no way to say "none"

`VarDecl ::= 'var' Ident ':' Type [ ':=' Expr ]` (l.947). The document never says what a field holds
before assignment, and §8.7 has no optional type (house rule: no sentinels). The common cases are
`var tick: Timer` (set in `Initialize`, cancelled later), `var current: Agent` (the peer being
helped), `var last_seen: Time`. Without a rule the implementer picks a zero value, which is a
sentinel.

Fix: require an initialiser on every `var` (drop `[ ... ]` from the grammar) and add `Opt<T>` with
`none` and `some(v)`-free surface: a read of an `Opt<T>` field in test position succeeds with the
value or fails, so `[timer]` is the presence test and `timer := Schedules.after(...)` stores it.
This composes with success/failure and is exactly `parent` and `r.space` (§5.5 "or none"), which are
already failing reads; type them `Opt<Agent>`/`Opt<Space>` so the checker agrees.

### 7. (§12.1 l.1262-1275, prose l.1285-1289) The fan-out example broadcasts every …

Full title: 7. (§12.1 l.1262-1275, prose l.1285-1289) The fan-out example broadcasts every sub-task
to every worker

`for i in 0 to partitions - 1 do emit SubTask{partition: i, ...} in inner` has no `for r`, so each
`SubTask` goes to all `partitions` workers (§7.2: `in s` without `for` is every member). Every
worker therefore runs every partition and answers `partitions` times: `partitions^2` results reach
the coordinator, and (the workers' default scope being `outer`, the parent's `inner`) every worker
also receives all `partitions^2` `PartitionResult`s from its siblings. The duplicate guard hides the
waste, and the mailbox sentence ("below half of `max_mailbox`") undercounts by a factor of
`partitions`. For the language's flagship common task (scatter/gather) the example teaches the wrong
thing.

Fix: keep the peer cells (`var workers: [Agent] := []`; in `Initialize`:
`for _ in 1 to partitions do workers := workers ++ [spawn Worker() in inner]`; then
`for i in 0 to partitions - 1 do emit SubTask{...partition: i...} for [workers[i]]`), and have the
worker reply `for [t.source]`... `source` is a peer cell, so `for [t.source]` works. State the
mailbox arithmetic for the corrected form. Note also that `to` ranges are never stated to be
inclusive (Icon: yes); say so once in §8.4.

## MINOR

### 8. (§8.1 statement termination) A line beginning with `-` or `*` continues the …

Full title: 8. (§8.1 statement termination) A line beginning with `-` or `*` continues the previous
statement

`-` and `*` are binary operators and also the unary operators of level 2 (`*xs` is size). A block
ending in `*ys` after `let ys := f()` is `f() * ys`. Fix: only operators that cannot start an
expression continue a line (`&`, `|`, `++`, `\`, `.`, `?`, `to`, `by`, `then`, `else`, `do`,
comparisons, `+`); `-` and `*` continue only at line end, as in Kotlin.

### 9. (§8.1 lexing) `a<-1`

Maximal munch gives `<-` (assignment) for `a<-1`, not `a < -1`. Icon has the same trap. One sentence
plus a checker warning for `<-` without surrounding spaces, or require `x <- e` to be a place on the
left (already required), which makes the `a<-1` case an error rather than a surprise.

### 10. (§8.9 notes l.1028) `else` binds to the nearest `achieve`, which skips the `if` …

Full title: 10. (§8.9 notes l.1028) `else` binds to the nearest `achieve`, which skips the `if` that
visually owns it

`if ok then achieve G() else fail` is `achieve G() else fail` under a total `if`: the failure branch
is on the wrong construct and nothing complains. Fix: require braces around the `then` body when it
contains an `achieve`, `ask` or `await` that has no `else` of its own, or give the actions distinct
introducers (`or_else`), or warn.

### 11. (§8.2 l.787-789) Typing of `if c then e`

With no `else`, is its type `()` or `T | ()`? `let x := if c then 5` is undefined. Say: without
`else`, `e` must have type `()`. Also the failing form `c & e` cannot contain `:=` or `consume`,
because `e` is an operand of `&` and therefore speculative (l.797-801); state that the way to get
the failing conditional with assignment is `if c then { x := 1 } else fail`.

### 12. (§8.9 grammar) `for`, `in` and `by` are overloaded

Round 2 #13, not addressed and not refused. `by`: range step and `achieve G by a`. `in`: loop,
`emit in`, `spawn in`. The grammar parses (checked), but `for p in ps do emit E{} in s for [p]` has
three clause words from three constructs on a line. Not blocking; if a rename is ever wanted it is
cheapest before the first user program: `via a` for delegation, `into s` for spaces.

### 13. (§8.9 `PlanDecl`) Plan headers restate the goal's parameter list

`plan survey for Survey(target_id, location)` must repeat the names exactly (l.1031). A repeated
list that must be identical is noise and a typo source. Allow `plan survey for Survey { ... }`
binding all parameters by name, and `for Survey(location)` to bind a subset by name.

### 14. (§10 `Behaviors.mount`) `args: [Event]`

Behavior arguments are typed `[Event]`, which cannot carry an `Int` or a `Float`. Either mount is a
dynamic-`Value` list (see #3) or the signature is wrong. If `behavior B(args)` in an agent body is
the intended way, say `Behaviors` is for the dynamic case only and give it the `Value` type.

### 15. (§8.9 `Term`, `TermArg`) `_` and `?x` ambiguities

`_` is an `Ident` and so already an `Expr`; the separate `'_'` alternative in `TermArg` is redundant
and ambiguous. `?x` versus scan `s ?x`: the rule "immediately followed by an identifier" makes
whitespace significant inside a term, `f(s ?t)`. Say a term argument that starts with `?` is always
a variable; a scan inside a term needs parentheses.

### 16. (§8.8, §8.9 bare calls) Resolution order among agent `def`s, used capacities and …

Full title: 16. (§8.8, §8.9 bare calls) Resolution order among agent `def`s, used capacities and
stdlib functions

A bare `cost(...)` may be an agent `def`, a method of a used capacity (§8.5 l.887-889) or a stdlib
function (`text`, `intentions`). Only the two-capacity clash is specified. Add: agent and behavior
`def`s shadow capacity methods; a capacity method never shadows a stdlib function without
qualification.

### 17. (§8.6, §10) Small essentials

`event Tick` (no braces) is legal, but `emit Tick` is not an expression (`Ident` alone is a name):
allow `emit Tick` when the event has no fields or require `event Tick {}`. The stdlib `text(x)` and
`intentions()` are generic or special-cased; say the checker treats `text` as built in. The
roll-back log (round 2 #1 suggestion) was not taken: a handler that fails because of a `fail` in its
body is "unhandled" and counted; make sure that counter is per-event-type so a typo'd guard is
findable.

## Verdict basis

The revision closed everything BLOCKING from round 2 and most of the MAJOR items. What remains is a
set of contradictions and gaps that a first user hits within one program: the Bool rule rejects
`sealed == false` (#1), the stdlib's own events cannot be read with the grammar (#2), terms are
untyped under a "statically typed" language (#3), a plan cannot read the event it awaited (#4),
conditions can act (#5), `var` has no absent value (#6), and the flagship example scatters
incorrectly (#7). All have one-paragraph fixes.

VERDICT: NOT CONVERGED
