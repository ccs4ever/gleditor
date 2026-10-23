# vagent review, round 6 - ergonomics and surface-language critic (focused round)

Document: `design/vagent-agent-language.md` v3.3 (2075 lines). Read: my round-5 critique, the
disposition, and §5.3, §6.1, §6.6, §7, §8 (all), §9.1, §10, §11, §12.1-12.4. Every example in §12
was re-parsed by hand against the §8.9 grammar, the §8.1 newline rule, the §8.3 position table and
the §6.6 suspension rule; all four parse, and the suspension and speculative-call rules accept them.
SARL claims are from my own knowledge (SARL is not in the repo).

Tally: 0 BLOCKING, 2 MAJOR, 5 MINOR.

## Round-5 BLOCKING / MAJOR findings

| R5 # | Topic                                          | Status                                                                                    |
| ---- | ---------------------------------------------- | ----------------------------------------------------------------------------------------- |
| 1    | skills could not `uses`                        | RESOLVED: `UsesDecl` is in `SkillDecl` (l.1210); `effect` in a skill is rejected (l.1297) |
| 2    | settings, logging, build rules not in register | RESOLVED: P9, P10, P11 (l.1519-1521) with fallbacks                                       |
| 3    | §12.3 depends on P1 without saying so          | RESOLVED: item 9 of the walk-through (l.1787) and P1 marked load-bearing                  |

Two small remainders of R5 #1 are in the minor list.

## MAJOR

### 1. (§8.2 l.1017-1018, §8.3, §8.9 `Assign`, §12.3 l.1714) The type of `:=`, `<-` …

Full title: 1. (§8.2 l.1017-1018, §8.3, §8.9 `Assign`, §12.3 l.1714) The type of `:=`, `<-`, `for`
and `while` is never stated, and the rule that depends on it rejects or accepts the showcase example
by guesswork

§8.2: "`if c then e` without `else` is total ... and `e` must have type `()`." The only types the
document gives for expressions are comparisons (they produce `b`), `fail`/`return` (bottom), blocks
(last statement) and the action table (§8.6). Nothing says what `x := e`, `x <- e`, `for` or `while`
produce. §12.3 l.1714 is

```
if not charging then charging := adopt KeepCharged(reserve_floor)
```

If an assignment produces the assigned value (Icon's reading, and what
`Assign ::= Scan [ := Assign ]`, which chains right-associatively, suggests), `e` has type
`Intention` and the checker must reject the document's own example. If it produces `()`, then
`a := b := 3` (grammar-legal) is a type error and the grammar should not allow it. Likewise l.1720,
`if is_maintain(i) then { for r in restorers(i) do drop r } else drop i`, needs the `for` block and
`drop i` to have a common type, and no rule gives the arms of an `if ... else` a type relation at
all; `for`'s and `while`'s value (a loop "fails if body fails" and otherwise produces what?) is
unstated. An implementer of the checker has to invent all of this, and the choice decides whether
§12.3 type-checks.

Fix (three sentences in §8.2): "`x := e` and `x <- e` succeed with `()`; chaining assignments is a
type error (restrict `Assign ::= Scan [ ( ':=' | '<-' ) Scan-or-Alt-level Expr ]` accordingly, or
leave the grammar and let the checker reject). `for` and `while` succeed with `()`. The arms of
`if c then a else b` have a common type, where `fail` and `return` match any type; an `if` used as a
statement may have arms of any type, which are discarded." Then reword the total-`if` rule to "`e`
must have type `()`, or the `if` must be a statement (its value is discarded)" if the owner prefers
`if c then x := e` to need no wrapper. Either way, state which and make §12.3 consistent with it.

### 2. (§8.8 l.1175-1177, §12.4 l.1837-1838) The only documented way to start a program …

Full title: 2. (§8.8 l.1175-1177, §12.4 l.1837-1838) The only documented way to start a program
cannot express the program §12.4 shows

`vagent run module:Agent(args)` is the sole entry point, yet:

- the module path and the agent name share the separator `:` (`demo:depot:Warehouse(units)`), and
  `ModulePath ::= Word { ':' Word }` accepts a PascalCase final word, so `demo:depot:Warehouse` is a
  module path in a different reading; nothing says the last segment is the agent;
- `Warehouse(units)` shows the argument `units` as if it were an identifier; the arguments' grammar
  (literals only? `Expr`? Durations as `5s`, an `Agent` as ...) is not given;
- §8.8 says agents the program depends on "are started the same way and their names passed as
  arguments", and §12.4 relies on it for `Dispatcher(warehouse: Agent, deadline: Duration)`, but
  there is no syntax for an `Agent` argument. The names are minted at spawn (`$spawn_count`), so a
  user cannot type one, and a second `vagent run` is a different launcher and space.

So the one example with two cooperating programs cannot be launched as documented, and a program
needs an agent that spawns its dependencies instead (which is what the document does everywhere else
with `spawn`). This is carried from R4/R5 as a minor; it is raised now because every runnable claim
in §12 passes through it, and the fix is small.

Fix: (a) `vagent run [--module m] Agent(args)` or `vagent run module/Agent(args)`, so the agent name
is not a module segment; (b) arguments are `Literal`s and durations only; (c) either a
`--bind name=Agent` form that prints the started agent's name and accepts it as an argument, or,
simpler and in keeping with §4.5, say that an agent needing a peer receives it by `spawn` in its own
`on Initialize` and change §12.4 so `Dispatcher` spawns the `Warehouse` (and so passes `units` as a
literal). Delete the sentence at l.1175-1177 about passing names.

## MINOR (at most five)

- `effect def` is admitted by `MethodDef` in agents and behaviors (`Member ::= ... MethodDef`, and
  `MethodSig` carries `effect`), and only skills are told to reject it (l.1297). Split the grammar
  (`DefSig` for bodies, `MethodSig` for capacities) or say "`effect` is legal only in `capacity`".
  Also l.1122-1124 (bare-call resolution) still lists only an agent's or behavior's `def`s and used
  capacities; add "in a skill: its own `def`s, then capacities it `uses`".
- `let x := a & b` takes the whole `&`-chain (`Stmt ::= 'let' ... ':=' Expr`) while `x := a & b` is
  `(x := a) & b` (§8.4); one example (`r := ok() & compute()` versus `let r := ok() & compute()`)
  and a checker warning for `:=` whose right operand is followed by `&` would prevent a quiet trap.
- Timers still have a signature (l.1420) and no example: `Schedules.after(5s, Retry{})`, whether
  `uses Schedules` is implied (it is, by l.1124), what `source` of the timer event is (`none`?), and
  how a held `Opt<Timer>` is cancelled. Six lines in §12.4 close R4 #14 / R5 #11 for good.
- A plan has no early success exit: `return` is refused in plans and handlers (l.1299) and "early
  exit is a `while`" does not cover it, so `if done then return` must be written as a wrapping `if`.
  Say so in §8.6, or accept `return` in plans as "end the plan, then test `succeed when`".
- Newlines outside a `Block` (capacity, event, goal bodies) are still not stated to be whitespace
  (R5 #10); the §8.1 rule reads as context-free. One sentence.

## Verdict basis

The grammar is sound: every §12 example parses, precedence and the `Bool`, purity and suspension
rules agree with the examples, and the SARL divergences are argued. What remains is two gaps an
implementer would otherwise fill by guessing: the type of assignment and loops (which decides
whether the §12.3 rover type-checks) and the syntax of the program entry point (without which §12.4
cannot be started). Each is a few sentences.

VERDICT: NOT CONVERGED
