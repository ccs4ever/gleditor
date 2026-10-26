# vagent review, round 2 - ergonomics and surface-language critic

Document: `design/vagent-agent-language.md` v2.0 (1417 lines). Line numbers are from that file.
Siblings read: `vicon-goal-directed-language.md` (§7.2 comparison names, §8.2, l.599-602), my
round-1 critique, the round-1 disposition. SARL claims are from my own knowledge (not in repo).
Icon's exact operator-precedence table is from memory and I mark it unverified where it matters.

Tally: 1 BLOCKING, 8 MAJOR, 9 MINOR.

## Part 1. Round-1 BLOCKING / MAJOR findings

| R1 # | Topic                                           | Status                                                                                                            |
| ---- | ----------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| 1    | StructLiteral vs Block in headers               | PARTLY: `then`/`do` fixes `if`/`for`/`while`; `where Expr Block` and `Contract Expr Block` still collide (new #3) |
| 2    | `to` as range and emit target                   | RESOLVED: emit uses `in`/`for`; `to` is range only (but `for`/`in` are now overloaded, #13)                       |
| 3    | `commit;` in conjunction                        | RESOLVED: removed (V6)                                                                                            |
| 4    | Examples do the opposite of their comments      | RESOLVED: §12.3 `travel` is now correct                                                                           |
| 5    | `import std:agent` not in grammar               | PARTLY: grammar has `ImportDecl`, but `agent` is a reserved word (#2)                                             |
| 6    | `spawn` statement used as expression            | RESOLVED: `spawn` is an `Action` primary                                                                          |
| 7    | Named-field pattern vs typed binder             | RESOLVED: patterns removed, fields by name (but belief triggers lost their arguments, #5)                         |
| 8    | Examples invalid under grammar/type productions | PARTLY: new violations in §8.5 and §12.1 (#1, #4)                                                                 |
| 9    | Optional semicolons, no newline rule            | RESOLVED: §8.1 rule (one gap, #12)                                                                                |
| 10   | Booleans vs success/failure                     | PARTLY: documented, footgun remains (#7)                                                                          |
| 11   | Precedence surprising                           | PARTLY: table exists; chain contradiction and `not` (#6, #7)                                                      |
| 12   | Contracts, goal-plan link, lifecycle            | RESOLVED: `requires`/`ensures`, `plan ... for`, `Initialize`/`Destroy`                                            |
| 13   | `uses`/`requires`/`setSkill` vs SARL            | PARTLY: `install` explained; `requires` re-purposed silently (#10)                                                |
| 14   | Stdlib names are keywords                       | OPEN: `std:agent` still unparseable (#2)                                                                          |
| 15   | Block semantics under backtracking              | PARTLY: §8.2 states it, and it exposes #1                                                                         |
| 16   | Context vs space                                | RESOLVED: `inner`/`outer` are spaces (§2, §5.4)                                                                   |
| 17   | Example idiom/correctness                       | PARTLY: see #1, #4                                                                                                |
| 18   | Sibling inconsistency table                     | PARTLY: §8.11 exists but omits real gaps (#8)                                                                     |
| 19   | Prose syntax not in grammar                     | PARTLY: `believes_from`, `return`, `Schedules` (#5, #9)                                                           |

## BLOCKING

### 1. (§8.2, §8.9 `if`, §12.1) `if c then e` with no `else` fails when `c` fails; in a …

Full title: 1. (§8.2, §8.9 `if`, §12.1) `if c then e` with no `else` fails when `c` fails; in a
handler that rolls back the whole handler

Evidence: `Primary ::= 'if' Expr 'then' Expr [ 'else' Expr ]` (l.891). §8.2 l.683-684: "A failing
statement fails its block." §6.5 l.509-510: a handler whose body fails is `release(m)`d, "a failed
handler leaves no trace". vicon's `if` without `else` fails, as Icon's does.

The principal worked example hits this. §12.1 `on PartitionResult` (l.1134-1141) ends with
`if *findall(...) == partitions then { ... }`. For every partition but the last the condition fails,
so the `if` statement fails, the handler fails, and the mark is released: the
`believe reported(...)` and every `believe hit(...)` written earlier in the same handler are undone.
The count therefore never reaches `partitions` and `SearchCompleted` is never emitted. The example
does the opposite of what its prose says, which is exactly the class of defect round 1 found.

This is a design problem, not only an example problem. Any SARL/Kotlin programmer writes
`count := count + 1` followed by `if count > limit then emit Alert{}`, and the `if` silently erases
the increment. Same for `for h in xs do if keep(h) then believe ...`: the first rejected element
fails the loop and rolls back the handler. No diagnostic is produced (a failed handler is only
"unhandled").

Fix (pick one, state it in §8.2 and §8.6):

- `if c then e` without `else` in a bounded position is total: it is `if c then e else ()`. Icon's
  failing form remains available as `c & e`. This is the smallest change and removes the trap.
- Additionally log a failed handler on the `vortex.agent` category at debug level with the statement
  ordinal that failed, so a rollback is findable.
- Correct nothing in §12.1 except that it now works; add a one-line comment saying why the `if` is
  safe.

## MAJOR

### 2. (§8.1 reserved words, §8.9 `ModulePath`, every example) `import std:agent` cannot …

Full title: 2. (§8.1 reserved words, §8.9 `ModulePath`, every example) `import std:agent` cannot
parse: `agent` is reserved

`ModulePath ::= Ident { ':' Ident }` (l.833) and `agent` is in the reserved list (l.661). Every
example begins `import std:agent` (l.1105, l.1196). This is round-1 #14 and #5 not closed. Related:
`findall` is reserved although it is a function with a special argument form; `holds`, `succeed`,
`priority`, `perform`, `maintain`, `parent`, `inner`, `outer`, `answer`, `drop` are common words
that users will want as names. `return` is used in `Stmt` (l.873) but is not in the reserved list.

Fix: make `ModulePath ::= Name { ':' Name }` where `Name` is `Ident | Keyword` (a module path is
lexed after `import`/`module`). Make `holds`, `succeed`, `priority`, `perform`, `maintain`, `where`,
`by`, `within`, `as`, `answer`, `findall` contextual (reserved only in the clause position that uses
them). Add `return` to the list or delete it (see #9).

### 3. (§8.9 notes l.921-924, `PlanDecl`, `MethodSig`) The "no block after an …

Full title: 3. (§8.9 notes l.921-924, `PlanDecl`, `MethodSig`) The "no block after an identifier"
note is false for `where` and `requires`

The note claims "No expression production puts a block directly after an identifier". But
`PlanDecl ::= ... [ 'where' Expr ] Block` (l.868) and `MethodDef ::= MethodSig Block` with
`Contract ::= ( 'requires' | 'ensures' ) Expr` (l.843-845) put an `Expr` directly before a `Block`.
`plan p for G(x) where ready { ... }` or `def f() requires ok { ... }` parses `ready { ... }` as a
`RecordLit` and then wants `:`. `HandlerDef` is safe because it ends the guard with `]` and
`priority IntLit`.

Fix: delete the false claim and add the rule explicitly: in `where` and `Contract` position a
`RecordLit` is not recognised (require parentheses), the same restriction Rust applies in `if`
headers. One sentence in §8.9 notes; the examples already pass.

### 4. (§8.5 `ThrusterNav.move`) The example violates the document's own position rules

`s <= max_speed & { speed := s; thrusters.set(s, heading) }` (l.751). A `Block` that is an operand
of `&` is a speculative position (§8.3 l.699-701), but the block contains `:=` and an `effect` call,
both "bounded positions only" (l.708). The compiler as specified rejects the capacity example in the
section that introduces the rules. Also `thrusters` is not declared anywhere, so a reader cannot
tell what it is (a field? a capacity? a host object).

Fix:

```text
effect def move(s: Float, heading: Float) { if s <= max_speed then { speed := s; thrusters.set(s, heading) } else fail }
```

(with #1 settled, `else fail` is needed here to keep the intended refusal), and declare `thrusters`
(a `uses Hardware` or a field) or say it is a host binding.

### 5. (§6.1, §8.9 `Trigger`) Belief-triggered handlers have no way to read their …

Full title: 5. (§6.1, §8.9 `Trigger`) Belief-triggered handlers have no way to read their arguments;
`believes_from` has no syntax

`Trigger ::= ... ( '+' | '-' ) Ident [ '/' IntLit ]` (l.863) names a predicate and arity only.
`on +at/3 { ... }` cannot bind `rover`, `3`, `4`. §3 V12 and §8.5 l.781 forbid positional
destructuring, and §5.5 defines event fields only for declared events. The same gap covers
`tell`-induced `+fact` handlers (l.598) and the `GoalAchieved` family only because those are
declared events. Separately `believes_from(sender, at(...))` (l.421) is a call whose second argument
is a term, and §8.9 has no production for it (terms are only recognised after the listed keywords).

Fix: reuse the existing `Term`/`TermArg` grammar, which already carries `?x`:
`Trigger ::= ( '+' | '-' ) Term` so `on +at(rover, ?x, ?y) [x > 0] { ... }` binds `x`, `y` exactly
as `believes` does. For provenance add `believes at(...) from s` (a postfix clause on the `believes`
primary) and delete `believes_from`.

### 6. (§8.10, §10, §6.5) Timers: `Schedules.after/periodic/cancel` have no signature …

Full title: 6. (§8.10, §10, §6.5) Timers: `Schedules.after/periodic/cancel` have no signature and
the language has no lambda

SARL's `in(delay) [ ... ]` takes a closure. vagent has no closure (grammar has none), and the
document says only that the adapter "enqueues self-messages" (l.75) and that a `Timer(i)` message
continues an intention (l.513). Nothing says how a plain SARL-style handler agent does periodic
work, what `after` takes or returns, how a timer is cancelled, or what a firing looks like to a
handler. §8.7 lists no base `Event` type to pass as an argument. For the SARL audience this is the
first thing they reach for after `emit`.

Fix: specify `Schedules.after(d: Duration, e: Event): Timer`,
`periodic(d: Duration, e: Event): Timer`, `cancel(t: Timer)`, with firing as an ordinary event
delivered to `self` in a later turn; add `Event` (supertype of all declared events) and `Timer` to
§8.7. Say whether a firing counts as a `Timer` message for the executor (§6.5) or as a plain event.

### 7. (§8.2, §8.4) Two truth-value footguns remain: `if flag` with a `Bool`, and `not a == b`

(a) `if flag then ...` always succeeds when `flag` is a `Bool` (l.673-676). The document's own
examples are forced into `sealed == false` (l.1179) and the manual says "write `== true`". A
`Bool`-typed field in test position is a certain bug; the type exists to be misused.

(b) `not` is a level-2 unary (l.721), so `not x == y` is `(not x) == y`. `not x` yields `()` or
fails, and comparing `()` with `y` is meaningless. Python, Kotlin and SARL readers write
`not a == b` for negated equality. (I could not verify Icon's own precedence for `not` from the
repo; Icon documents it as a control-flow reserved word, not a tight unary.)

Fix: (a) the checker rejects a `Bool`-typed expression as the condition of `if`/`while`, as an
operand of `&`/`not`, or as a guard, with the message "use `== true`"; or drop `Bool` and keep
success/failure only. (b) parse `not` as `not Cmp`-level (below comparison, above `&`/`|`), or make
the checker reject a `not` whose operand is the left side of a comparison.

### 8. (§3 V11, §8.4, §8.11) V11's promise "a vicon reader and a vagent reader parse the …

Full title: 8. (§3 V11, §8.4, §8.11) V11's promise "a vicon reader and a vagent reader parse the
same text the same way" is false, and §8.11 hides it

vicon's comparisons are `<`, `<=`, `=`, `>=`, `>`, `~=` (vicon l.383) and string concatenation is
`||` (vicon l.599-602). vagent uses `==`, `!=` and `++` (l.882-883). vagent also gives `==` a single
meaning (Icon separates numeric `=` from lexical `==`), so `1 == 1.0` and `"1" == 1` are
unspecified. vagent has `return` but no `suspend`, so user `def`s cannot be generators, which is the
core of the sibling it claims to follow. §8.11 lists `assignment`, `loops`, `scanning`, `paths`,
`locals`, `logic variables` and omits comparison, concatenation and generator procedures.

Fix: either adopt vicon's spellings (`=`, `~=`, `||`) or amend V11 to "follows Icon's *structure*,
with Kotlin-style spellings", and add rows to §8.11 for comparison operators, concatenation, and
user-defined generators (state that vagent has none and why). Specify whether `==` is typed (`Int`
vs `Float`) or structural.

### 9. (§8.2 l.671, §8.4 l.726) Chained comparison is documented as working and …

Full title: 9. (§8.2 l.671, §8.4 l.726) Chained comparison is documented as working and specified as
impossible

§8.2: "A chain `a < b < c` works as in Icon." §8.4: "non-associative as written; chained comparison
is explicit". Grammar: `Cmp ::= Concat [ CmpOp Concat ]` (l.881) allows one operator. A reader gets
two answers and the grammar rejects the Icon chain. Fix: choose. To follow Icon use
`Cmp ::= Concat { CmpOp Concat }` (left-associative, each result feeds the next); to keep it
non-chaining delete the sentence in §8.2 and show `a < b & b < c`.

## MINOR

### 10. (§8.10) The SARL table omits the places where fidelity is claimed but broken

Rows to add: `requires` (SARL: a skill's required capacity; vagent: a contract precondition, l.743);
`on E as w` (not in SARL); event fields `{ x: T }` vs SARL's `var x`; agents take parameters and
receive `Initialize` (SARL has `Initialize.parameters`); `val` (absent); agent `extends` (absent);
`Schedules.in/every` renamed. The `wake` row (l.944) is garbled ("`wake` ->
`emit ... is not offered`"); say: "`wake` is not offered; an agent's own events come from beliefs,
goals and plans".

### 11. (§8.1) The newline rule does not let `else` start a line

`}` ends a statement and `else` is a keyword, not a binary operator, so

```text
if c then { ... }
else { ... }
```

splits. Add `else`, `then`, `do` to the line-continuation set, or say K&R style is mandatory. Also
state that newlines are significant again inside a `Block` nested within `( )`, `[ ]` or a record
literal (otherwise `f(x, { a\n b })` is undefined).

### 12. (§8.3) The `else` of `achieve`/`ask` is not a bounded position

The definition (l.699) lists only `if`/`while`/`for` bodies. §12.3
`achieve Reach(location) else { emit ...; fail }` uses `emit` and `fail`, which are allowed
anywhere, but a `:=` in that block (a likely compensation) would be rejected. Add `else` of
`achieve`/`ask` to the bounded list. Also say that `let x := ask ...` (not only `x := ask ...`) is a
suspension point.

### 13. (§8.9) `for` and `in` carry three meanings each

`for`: loop, `plan ... for Goal`, `emit ... for recipients`. `in`: loop, `emit ... in space`,
`spawn ... in space`. Both parse, but `for p in ps do emit E{} for [p]` reads badly and a line wrap
near `for` is a trap. Consider `emit e in s only r` (or `to r` is unavailable because of ranges).
Cheap now, expensive later.

### 14. (§5.5, §8.5, §8.7) Event subtyping and implicit fields are unspecified

`Alarm extends TargetDetected` (l.740) is declared but the dispatcher matches "the event type's
name" (l.373, §9.3). Does `on TargetDetected` fire for an `Alarm`? SARL says yes. The wire carries
only the name, so the receiver's program must supply the chain; say so. Say what happens if an event
declares a field named `source` or `space` (reserved, compile error). `Initialize{args}` (l.1037)
has no field list; define it (`args: [Value]`? named agent parameters?).

### 15. (§6.1, §8.9) Scope of `?x` bindings beyond the `believes` expression

"binding `x` and `y` for the rest of the expression" (l.408). In
`if believes at(?x, ?y) then use(x)` the `then` is another `Expr`; in a `where`, the binding is
"kept for the body" (l.480). State the rule once: bindings made in an `if`/`where`/`for` condition
are visible in the corresponding `then`/body. `_` appears both as `Ident` and as a `TermArg`
alternative (l.911), and `?` is both the scan operator and the variable sigil; say that `?` followed
immediately by an identifier inside a `Term` is a variable.

### 16. (§8.7) No user-defined data type but events, and the `Agent` handle is untyped

Records can only be declared as `event`s; `Point` is imported. A programmer modelling a map cell
must declare an event that is never emitted. Add `record Name { f: T }` (same production as
`EventDecl`, no wire kind). `ask a.q(args)` and `achieve G by a` cannot be type-checked because
`Agent` carries no interface (l.808); `Agent<PatrolDrone>` or `Agent<Navigation>` would let the
checker validate `q`, its arity and its result. Also the plan header binds goal arguments
positionally (`for Survey(target_id, location)`) while handlers bind by name (l.781); say that plan
parameters must repeat the goal's parameter names and are checked.

### 17. (§12.3, §12.2) Small example defects

`tab`, `match`, `upto` (l.1246) are `std:goal` names and only `std:agent` is imported; say whether
`std:agent` re-exports them or add `import std:goal`. §6.2 calls `reserve_floor()` as a function and
§12.3 uses a parameter `reserve_floor`. `install ThrusterNav(12.0)` (l.768) is a naked constant in
an AGENTS.md-governed codebase; fine for a language example but mark it as such. `BeliefDecl`
(`believe Term` as a member, l.859) has no stated timing (agent creation? before `Initialize`?).

### 18. (§1, §17) Testing remains U-A10

Deferred with a reason in the disposition; I do not re-raise it. I do disagree mildly with the
reason that "the language for it is not yet specified": the unit under test is already one handler
on one arena, so a three-line `test` form (`given`, `emit`, `expect emitted`) can be specified now
without the harness.

## Verdict basis

The revision closed most of round 1: commit is gone, the precedence table and newline rule exist,
the record/block ambiguity is largely cured, and the travel example is right. One BLOCKING item
remains (#1: a conditional statement silently rolls back the handler, and the main example is wrong
because of it) and eight MAJOR items, most of them cheap grammar fixes (#2-#5, #9) or one-paragraph
specifications (#6-#8).

VERDICT: NOT CONVERGED
